#!/usr/bin/env bash
#
# provision_password.sh — set the VSE operator password for a deployment.
#
# Regenerates vse/pw_verifier.h (the VSE_PW_VERIFIER macro consumed by
# vse/login.c) from a password, WITHOUT editing login.c. The derivation
# (HMAC(login-pepper, password), pepper = derive(master_key, label)) is done by
# scripts/totp_gen.py so it exactly mirrors login.c / config_check.c.
#
# Usage:
#   scripts/provision_password.sh '<password>'                 # dev/QEMU key
#   scripts/provision_password.sh '<password>' --master-key HEX # real device key
#
# The plaintext password is NEVER written anywhere — only its HMAC verifier.
#
# IMPORTANT — Phase 2 golden: the verifier lives in the measured .rodata.
# Changing the password changes the .rodata golden ONLY (never .text). After
# running this you MUST rebuild and reprovision slot [1] of _golden_components[]
# by hand (LEARN-mode boot) — reprovision_golden.sh only handles .text (slot 0).
# The dev default ("changeme") is measurement-neutral and needs no reprovision.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
HDR="$ROOT/vse/pw_verifier.h"

if [[ $# -lt 1 || "$1" == "-h" || "$1" == "--help" ]]; then
    grep -E '^#( |$)' "${BASH_SOURCE[0]}" | sed -E 's/^# ?//'
    exit 2
fi

PASSWORD="$1"; shift
MK_ARGS=()
if [[ "${1:-}" == "--master-key" ]]; then
    [[ -n "${2:-}" ]] || { echo "ERROR: --master-key needs a 64-hex value" >&2; exit 2; }
    MK_ARGS=(--master-key "$2")
    shift 2
fi

command -v python3 >/dev/null || { echo "ERROR: python3 required" >&2; exit 1; }

# Back up any existing header (timestamped) before overwriting.
if [[ -f "$HDR" ]]; then
    cp "$HDR" "$HDR.bak.$(date +%Y%m%d-%H%M%S)"
fi

# Write to a temp file first; only move into place if generation succeeded.
TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT
python3 "$ROOT/scripts/totp_gen.py" --pw-header "$PASSWORD" "${MK_ARGS[@]}" > "$TMP"
grep -q "VSE_PW_VERIFIER" "$TMP" || { echo "ERROR: header generation failed" >&2; exit 1; }
mv "$TMP" "$HDR"
trap - EXIT

echo "==> wrote $HDR"
if [[ ${#MK_ARGS[@]} -eq 0 ]]; then
    echo "    (derived with the DEV/QEMU master key — for a real unit pass --master-key HEX)"
fi
echo
echo "Next steps:"
echo "  1. make qemu           # rebuild with the new verifier"
echo "  2. reprovision the Phase 2 .rodata golden (slot [1]) by hand:"
echo "       - boot once, copy the Phase 2 .rodata 'Computed:' block"
echo "       - paste into _golden_components[1] in vse/component_check.c"
echo "       - rebuild"
echo "     (reprovision_golden.sh only auto-fixes .text / slot [0].)"
