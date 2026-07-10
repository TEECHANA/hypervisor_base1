#!/usr/bin/env python3
"""
verify_guest_goldens.py — guard against guest-golden drift at build time.

Parses the guest-genuineness golden table (_guests[]) out of
vse/guest_measure.c and, for each guest, hashes the first `image_size` bytes of
the built image (exactly what the hypervisor measures at load) and compares it
to the embedded golden. Exits non-zero on any mismatch or missing file.

This catches the failure mode where a guest's source changes but its golden is
not re-provisioned: a fresh build then hashes differently and boot-time
attestation would reject the guest. Wired into `make check-guests`.
"""
import hashlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "vse" / "guest_measure.c"

# guest name -> built image path
IMAGES = {
    "linux":   ROOT / "guests" / "linux" / "Image",
    "rtos":    ROOT / "guests" / "rtos" / "rtos.bin",
    "android": ROOT / "guests" / "android_stub" / "android.bin",
}

# One _guests[] entry: { "name", 0x..ULL, <size>ULL, <bool>, { <bytes> } }
ENTRY_RE = re.compile(
    r'\{\s*"([^"]+)"\s*,\s*0x[0-9A-Fa-f]+ULL\s*,\s*(\d+)ULL\s*,\s*'
    r'(true|false)\s*,\s*\{(.*?)\}\s*\}',
    re.DOTALL,
)
BYTE_RE = re.compile(r'0x([0-9A-Fa-f]{2})')


def parse_goldens(text):
    # Restrict to the _guests[] initializer so we don't match anything else.
    m = re.search(r'_guests\[\]\s*=\s*\{(.*?)\n\};', text, re.DOTALL)
    if not m:
        sys.exit("verify_guest_goldens: could not locate _guests[] in guest_measure.c")
    out = []
    for name, size, prov, body in ENTRY_RE.findall(m.group(1)):
        golden = bytes(int(h, 16) for h in BYTE_RE.findall(body))
        out.append((name, int(size), prov == "true", golden))
    return out


def main():
    entries = parse_goldens(SRC.read_text())
    if not entries:
        sys.exit("verify_guest_goldens: parsed zero guest entries")

    fails = 0
    for name, size, provisioned, golden in entries:
        if not provisioned:
            print(f"  SKIP {name}: golden not provisioned (learn mode)")
            continue
        if len(golden) != 32:
            print(f"  FAIL {name}: golden is {len(golden)} bytes, expected 32")
            fails += 1
            continue
        img = IMAGES.get(name)
        if img is None:
            print(f"  FAIL {name}: no image path mapping")
            fails += 1
            continue
        if not img.exists():
            print(f"  FAIL {name}: missing image {img}")
            fails += 1
            continue
        data = img.read_bytes()
        if len(data) < size:
            print(f"  FAIL {name}: image {len(data)} B < declared size {size} B")
            fails += 1
            continue
        measured = hashlib.sha256(data[:size]).digest()
        if measured == golden:
            print(f"  PASS {name}: {size} B  sha256={measured.hex()[:16]}…")
        else:
            print(f"  FAIL {name}: golden drift — rebuild ≠ embedded golden")
            print(f"       golden  ={golden.hex()}")
            print(f"       measured={measured.hex()}")
            print(f"       (re-provision the '{name}' golden in vse/guest_measure.c)")
            fails += 1

    if fails:
        print(f"verify_guest_goldens: {fails} guest golden check(s) FAILED")
        return 1
    print("verify_guest_goldens: all guest goldens match their built images")
    return 0


if __name__ == "__main__":
    sys.exit(main())
