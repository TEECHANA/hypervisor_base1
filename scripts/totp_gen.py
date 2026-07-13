#!/usr/bin/env python3
"""
totp_gen.py — companion tool for the hypervisor's VSE TOTP login (vse/totp.c)

Must stay in sync with:
  - HMAC-SHA256 (matching vse/hotp.c which totp.c calls via hotp_compute)
  - The secret/pepper derivation in vse/keystore.c (vse_derive_secret):
        secret = HMAC-SHA256(master_key, label)[:len]
  - DEV_MASTER_KEY matches _dev_master_key[] in vse/keystore.c
  - PROVISIONED_EPOCH matches RTC_PROVISIONED_EPOCH in lib/rtc/rtc.h
  - TOTP_STEP_S = 30 (matches vse/totp.h)

The HOTP/TOTP secret is no longer hard-coded; it is DERIVED from the master key
via the same KDF the hypervisor uses. On real hardware the master key comes from
the OTP fuse, so codes become device-unique automatically.

The hypervisor synthesizes unix time as:
    unix_now = PROVISIONED_EPOCH + elapsed_seconds_since_boot

Usage:
  ./totp_gen.py                          # code for current step (uses real time)
  ./totp_gen.py --unix 1751328025        # code for explicit unix timestamp
  ./totp_gen.py --epoch 1751328000 --elapsed 25   # epoch + boot elapsed
  ./totp_gen.py --step 58377600          # code for explicit T step
  ./totp_gen.py --pwverify PASSWORD      # print _pw_verifier[] C array for PASSWORD
"""
import sys, hmac, hashlib, struct, time, argparse

# Must equal _dev_master_key[] in vse/keystore.c (dev/QEMU fallback key).
DEV_MASTER_KEY = bytes([
    0xa3, 0x7f, 0x2c, 0x91, 0xd4, 0x58, 0xbe, 0x06,
    0x1e, 0x82, 0x4a, 0xf3, 0x70, 0xc9, 0x55, 0x3d,
    0x08, 0xb1, 0xe7, 0x6f, 0x29, 0xda, 0x44, 0x9c,
    0x87, 0x5e, 0x13, 0xab, 0xfc, 0x60, 0x2b, 0x77,
])

# Domain-separation labels — must match vse/hotp.c and vse/login.c.
HOTP_SECRET_LABEL = b"vse-hotp-secret-v1"
LOGIN_PEPPER_LABEL = b"vse-login-pepper-v1"


def derive_secret(label, length, master_key=DEV_MASTER_KEY):
    """Mirror vse_derive_secret(): HMAC-SHA256(master_key, label)[:length]."""
    return hmac.new(master_key, label, hashlib.sha256).digest()[:length]


# Derived HOTP/TOTP secret (20 bytes), matching vse/hotp.c _ensure_secret().
SECRET = derive_secret(HOTP_SECRET_LABEL, 20)

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


def pwverify_bytes(password, master_key=DEV_MASTER_KEY):
    """Mirror vse/login.c: HMAC(pepper, password), pepper=derive(pepper_label,32)."""
    pepper = derive_secret(LOGIN_PEPPER_LABEL, 32, master_key)
    return hmac.new(pepper, password.encode(), hashlib.sha256).digest()


def pwverify(password, master_key=DEV_MASTER_KEY):
    """C-array body (four comma-separated rows) for _pw_verifier[]."""
    d = pwverify_bytes(password, master_key)
    lines = []
    for i in range(0, 32, 8):
        chunk = d[i:i+8]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    return "\n".join(lines)


def pw_header(password, master_key=DEV_MASTER_KEY, is_dev=True):
    """Full text of vse/pw_verifier.h defining VSE_PW_VERIFIER for `password`.

    login.c does `static const u8 _pw_verifier[HMAC_SIZE] = VSE_PW_VERIFIER;`
    so this header (never login.c) is what a deployment edits. The #ifndef guard
    also lets a build override it with -DVSE_PW_VERIFIER='{...}'.
    """
    d = pwverify_bytes(password, master_key)
    rows = []
    for i in range(0, 32, 8):
        rows.append("    " + ", ".join(f"0x{b:02x}" for b in d[i:i+8]) + ", \\")
    body = "\n".join(rows)
    # Describe the KEY source only — never echo the password (the whole point is
    # the plaintext is never stored). is_dev tracks dev-key vs device-key, not
    # whether the password is the "changeme" default.
    keysrc = ("dev/QEMU master key" if is_dev
              else "device master key (per-deployment)")
    return (
        "/*\n"
        " * pw_verifier.h — provisioned VSE operator password verifier.\n"
        " *\n"
        " * Defines VSE_PW_VERIFIER = HMAC(login-pepper, password), the value\n"
        " * login.c stores in _pw_verifier[]. Set the operator password per\n"
        " * deployment by REGENERATING THIS FILE (never edit login.c):\n"
        " *     scripts/provision_password.sh '<password>'\n"
        " * or override at build time with -DVSE_PW_VERIFIER='{...}'.\n"
        " *\n"
        f" * Derived with the {keysrc}. The committed default is HMAC(., \"changeme\").\n"
        " *\n"
        " * NOTE: this array lives in the Phase 2-measured .rodata. Changing it\n"
        " * changes the .rodata golden ONLY (never .text); reprovision slot [1]\n"
        " * of _golden_components[] (manual LEARN-mode boot) after a real change.\n"
        " */\n"
        "#ifndef VSE_PW_VERIFIER\n"
        "#define VSE_PW_VERIFIER { \\\n"
        f"{body}\n"
        "}\n"
        "#endif\n"
    )


def main():
    p = argparse.ArgumentParser(description="TOTP code generator for Tessolve hypervisor")
    p.add_argument("--unix",    type=int, help="Unix timestamp to use")
    p.add_argument("--epoch",   type=int, default=DEFAULT_EPOCH,
                   help=f"Provisioned epoch (default: {DEFAULT_EPOCH})")
    p.add_argument("--elapsed", type=int, default=0,
                   help="Seconds since boot (added to --epoch)")
    p.add_argument("--step",    type=int, help="Explicit T step (skips time computation)")
    p.add_argument("--pwverify", type=str, help="Print _pw_verifier[] C array for PASSWORD")
    p.add_argument("--pw-header", type=str, metavar="PASSWORD",
                   help="Print full vse/pw_verifier.h defining VSE_PW_VERIFIER for PASSWORD")
    p.add_argument("--master-key", type=str, metavar="HEX",
                   help="Device master key as 64 hex chars (default: dev/QEMU key). "
                        "Real deployments derive the pepper from the device key.")
    args = p.parse_args()

    master_key = DEV_MASTER_KEY
    is_dev = True
    if args.master_key:
        hexs = args.master_key.strip().replace(" ", "").replace("0x", "")
        if len(hexs) != 64:
            print("ERROR: --master-key must be 64 hex chars (32 bytes)", file=sys.stderr)
            sys.exit(2)
        master_key = bytes.fromhex(hexs)
        is_dev = False

    if args.pw_header is not None:
        print(pw_header(args.pw_header, master_key, is_dev), end="")
        return

    if args.pwverify:
        origin = "dev-pepper" if is_dev else "device-pepper"
        print(f"_pw_verifier = HMAC({origin}, {args.pwverify!r}) — paste into vse/pw_verifier.h:")
        print("static const u8 _pw_verifier[HMAC_SIZE] = {")
        print(pwverify(args.pwverify, master_key))
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
