#!/usr/bin/env python3
"""
qemu_integration.py — boot the hypervisor WITH its three guests and verify the
system reaches a healthy running state.

This mirrors `make run-with-guests` (virtualization=on, iommu=smmuv3,
gic-version=3, and the Linux + RTOS + Android guest loaders) rather than the
old no-guest smoke boot. It drives the operator TOTP login non-interactively:
the TOTP self-test prints the current code to the console before the login
prompt, so we scrape it and feed it back with the default dev password.

Checks (all must pass):
  * VSE Phase 2 measures .text and .rodata as verified OK
  * Operator login succeeds ("Access granted")
  * The RTOS fuel ECU guest reaches the hypervisor with a NON-ZERO rpm
    (regression guard for the entry.S guest-x1 save bug)
  * The scheduler is time-slicing between guests (CTX[...] switches)

Exit 0 on success, non-zero on any failure.
"""
import sys, re, time, pexpect

RUN_SECONDS = 45       # login must be reached within this
COLLECT_SECONDS = 20   # wall-clock window to gather guest output after login
ELF   = "build/qemu/hypervisor.elf"
LINUX = "guests/linux/Image"
INITRD = "guests/linux/initramfs.cpio.gz"
RTOS  = "guests/rtos/rtos.bin"
ANDROID = "guests/android_stub/android.bin"
PASSWORD = "changeme"   # default dev password (SHA-256 baseline in vse/login.c)

QEMU = (
    "qemu-system-aarch64 "
    "-machine virt,gic-version=3,virtualization=on,iommu=smmuv3 "
    "-cpu cortex-a57 -m 2G -smp 4 -nographic -serial stdio -monitor none -no-reboot "
    "-d guest_errors "
    f"-kernel {ELF} "
    f"-device loader,file={LINUX},addr=0x41200000,force-raw=on "
    f"-device loader,file={INITRD},addr=0x47000000,force-raw=on "
    f"-device loader,file={RTOS},addr=0x60008000,force-raw=on "
    f"-device loader,file={RTOS},addr=0x90000000,force-raw=on "
    f"-device loader,file={ANDROID},addr=0x70200000,force-raw=on"
)

def main():
    buf = bytearray()
    child = pexpect.spawn("/bin/bash", ["-c", QEMU], timeout=RUN_SECONDS, encoding=None)
    child.logfile = None

    code = None
    try:
        child.expect(rb"TOTP selftest: PASS \(T=\d+, code=(\d+)\)", timeout=25)
        buf += child.before + child.after
        code = child.match.group(1).decode()
        print(f"[integration] captured TOTP code {code}")
        child.expect(rb"Password: ", timeout=10); buf += child.before + child.after
        child.sendline(PASSWORD)
        child.expect(rb"OTP code: ", timeout=10);  buf += child.before + child.after
        child.sendline(code)
    except (pexpect.TIMEOUT, pexpect.EOF) as e:
        print(f"[integration] FAIL: never reached login prompt ({type(e).__name__})")
        _dump(buf, child)
        return 1

    # Guests run continuously (Linux streams console output), so collect for a
    # bounded wall-clock window rather than waiting for EOF.
    deadline = time.time() + COLLECT_SECONDS
    while time.time() < deadline:
        try:
            buf += child.read_nonblocking(4096, timeout=1)
        except pexpect.TIMEOUT:
            continue
        except pexpect.EOF:
            break
    child.close(force=True)

    log = bytes(buf).decode(errors="replace")

    checks = [
        ("Phase 2 .text verified",   r"VSE Phase 2: component '\.text' verified OK"),
        ("Phase 2 .rodata verified", r"VSE Phase 2: component '\.rodata' verified OK"),
        ("operator login granted",   r"Access granted"),
        ("RTOS fuel rpm non-zero",   r"FUEL HVC: rpm=[1-9]\d*"),
        ("scheduler time-slicing",   r"CTX\[\d+\]:"),
    ]
    ok = True
    for name, pat in checks:
        if re.search(pat, log):
            print(f"PASS: {name}")
        else:
            print(f"FAIL: {name}  (pattern /{pat}/ not found)")
            ok = False

    if not ok:
        print("\n---- last 40 log lines ----")
        print("\n".join(log.splitlines()[-40:]))
    print("All integration checks passed." if ok else "Integration checks FAILED.")
    return 0 if ok else 1

def _drain(child, seconds=3):
    out = bytearray()
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            out += child.read_nonblocking(4096, timeout=1)
        except pexpect.TIMEOUT:
            continue
        except pexpect.EOF:
            break
    return out

def _dump(buf, child):
    buf += _drain(child)
    print("\n".join(bytes(buf).decode(errors="replace").splitlines()[-40:]))

if __name__ == "__main__":
    sys.exit(main())
