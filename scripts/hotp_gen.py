#!/usr/bin/env python3
"""
hotp_gen.py — companion tool for the hypervisor's VSE login (vse/hotp.c, vse/login.c)

This MUST stay in sync with vse/hotp.c:
  - HMAC-SHA256 (not SHA-1) — the hypervisor reuses its in-tree HMAC-SHA256.
  - 6-digit codes, RFC 4226 dynamic truncation.
  - The default secret below matches _hotp_secret[] in hotp.c.

Usage:
  ./hotp_gen.py code [COUNTER]      # print the HOTP code for COUNTER (default 0)
  ./hotp_gen.py codes N             # print the first N codes (counters 0..N-1)
  ./hotp_gen.py --pwhash PASSWORD   # print SHA-256(PASSWORD) as a C byte array
                                    #   (paste into _pw_hash[] in vse/login.c)

Because there is no RTC on the target, codes are COUNTER-based (HOTP), not
time-based. The device advances its counter on each successful login; generate
the next code with the matching counter. The device also accepts a small
look-ahead window (HOTP_LOOKAHEAD in hotp.h) so being a few steps ahead is fine.
"""
import sys, hmac, hashlib, struct

# Must equal _hotp_secret[] in vse/hotp.c
SECRET = bytes([
    0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x21, 0xde, 0xad,
    0xbe, 0xef, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc,
    0xde, 0xf0, 0x11, 0x22,
])
DIGITS = 6


def hotp(counter: int, digits: int = DIGITS) -> str:
    mac = hmac.new(SECRET, struct.pack(">Q", counter), hashlib.sha256).digest()
    off = mac[-1] & 0x0F
    binc = ((mac[off] & 0x7F) << 24 | mac[off + 1] << 16
            | mac[off + 2] << 8 | mac[off + 3])
    return str(binc % (10 ** digits)).zfill(digits)


def pwhash_c_array(password: str) -> str:
    d = hashlib.sha256(password.encode()).digest()
    rows = []
    for i in range(0, 32, 8):
        rows.append("    " + ",".join(f"0x{b:02x}" for b in d[i:i + 8]) + ",")
    body = "\n".join(rows)
    return (f'/* SHA-256("{password}") = {d.hex()} */\n'
            f"static const u8 _pw_hash[32] = {{\n{body}\n}};")


def main() -> int:
    a = sys.argv[1:]
    if not a:
        print(__doc__); return 1
    if a[0] == "--pwhash":
        if len(a) < 2:
            print("usage: hotp_gen.py --pwhash PASSWORD"); return 1
        print(pwhash_c_array(a[1])); return 0
    if a[0] == "code":
        c = int(a[1]) if len(a) > 1 else 0
        print(hotp(c)); return 0
    if a[0] == "codes":
        n = int(a[1]) if len(a) > 1 else 10
        for c in range(n):
            print(f"{c}: {hotp(c)}")
        return 0
    print(__doc__); return 1


if __name__ == "__main__":
    sys.exit(main())
