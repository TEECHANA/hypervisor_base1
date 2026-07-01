#!/usr/bin/env python3
"""
totp_gen.py — companion tool for the hypervisor's VSE TOTP login (vse/totp.c)

Must stay in sync with:
  - HMAC-SHA256 (matching vse/hotp.c which totp.c calls via hotp_compute)
  - SECRET matches _hotp_secret[] in vse/hotp.c
  - PROVISIONED_EPOCH matches RTC_PROVISIONED_EPOCH in lib/rtc/rtc.h
  - TOTP_STEP_S = 30 (matches vse/totp.h)

The hypervisor synthesizes unix time as:
    unix_now = PROVISIONED_EPOCH + elapsed_seconds_since_boot

To generate the correct code, pass --epoch PROVISIONED_EPOCH and
--elapsed SECONDS_SINCE_BOOT (or just --unix UNIX_TIMESTAMP directly).

Usage:
  ./totp_gen.py                          # code for current step (uses real time)
  ./totp_gen.py --unix 1751328025        # code for explicit unix timestamp
  ./totp_gen.py --epoch 1751328000 --elapsed 25   # epoch + boot elapsed
  ./totp_gen.py --step 58377600          # code for explicit T step
  ./totp_gen.py --pwhash PASSWORD        # print SHA-256(PASSWORD) C array
"""
import sys, hmac, hashlib, struct, time, argparse

# Must equal _hotp_secret[] in vse/hotp.c
SECRET = bytes([
    0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x21, 0xde, 0xad,
    0xbe, 0xef, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc,
    0xde, 0xf0, 0x11, 0x22,
])

# Must equal RTC_PROVISIONED_EPOCH in lib/rtc/rtc.h (2025-07-01 00:00:00 UTC)
DEFAULT_EPOCH = 1751328000

DIGITS   = 6
STEP_S   = 30   # TOTP_STEP_S


def hotp(secret, counter):
    """RFC 4226 HOTP with HMAC-SHA256 (matching vse/hotp.c)."""
    msg = struct.pack(">Q", counter)
    mac = hmac.new(secret, msg, hashlib.sha256).digest()
    off = mac[-1] & 0x0f
    bincode = struct.unpack(">I", mac[off:off + 4])[0] & 0x7fffffff
    return f"{bincode % (10 ** DIGITS):0{DIGITS}d}"


def totp(secret, unix_time, step=STEP_S):
    """RFC 6238 TOTP: T = floor(unix_time / step)."""
    T = unix_time // step
    return T, hotp(secret, T)


def pwhash(password):
    d = hashlib.sha256(password.encode()).digest()
    lines = []
    for i in range(0, 32, 8):
        chunk = d[i:i+8]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    return "\n".join(lines)


def main():
    p = argparse.ArgumentParser(description="TOTP code generator for Tessolve hypervisor")
    p.add_argument("--unix",    type=int, help="Unix timestamp to use")
    p.add_argument("--epoch",   type=int, default=DEFAULT_EPOCH,
                   help=f"Provisioned epoch (default: {DEFAULT_EPOCH})")
    p.add_argument("--elapsed", type=int, default=0,
                   help="Seconds since boot (added to --epoch)")
    p.add_argument("--step",    type=int, help="Explicit T step (skips time computation)")
    p.add_argument("--pwhash",  type=str, help="Print SHA-256 of PASSWORD as C array")
    args = p.parse_args()

    if args.pwhash:
        print(f"SHA-256({args.pwhash!r}) — paste into _pw_hash[] in vse/login.c:")
        print("static const u8 _pw_hash[32] = {")
        print(pwhash(args.pwhash))
        print("};")
        return

    if args.step is not None:
        T = args.step
        code = hotp(SECRET, T)
    else:
        if args.unix is not None:
            unix_time = args.unix
        else:
            unix_time = args.epoch + args.elapsed
        T, code = totp(SECRET, unix_time)

    print(f"TOTP code: {code}  (T={T}, step={STEP_S}s)")
    print(f"  Valid for T-1={T-1} to T+1={T+1} on the hypervisor (±30s window)")
    print(f"  Hypervisor epoch: {args.epoch}  "
          f"(set RTC_PROVISIONED_EPOCH in lib/rtc/rtc.h)")


if __name__ == "__main__":
    main()
