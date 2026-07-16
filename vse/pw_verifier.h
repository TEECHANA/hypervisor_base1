/*
 * pw_verifier.h — VSE operator password verifier (FAIL-CLOSED, no default).
 *
 * VSE_PW_VERIFIER = HMAC(login-pepper, password) is the value login.c stores in
 * _pw_verifier[]. There is deliberately NO shipped default password: a bare
 * `make qemu` / `make all` FAILS to compile with the #error below unless
 * VSE_PW_VERIFIER is supplied. Provide it one of two ways:
 *
 *   1. Provision a header (per deployment; regenerates THIS file, never login.c):
 *          scripts/provision_password.sh '<password>'
 *      then `make reprovision-goldens && make qemu`.
 *
 *   2. Define it at build time (no file change):
 *          make qemu EXTRA_CFLAGS=-DVSE_PW_VERIFIER=$(python3 \
 *              scripts/totp_gen.py --pw-define '<password>')
 *
 * Dev/test/CI builds inject a KNOWN test verifier for the "changeme" dev
 * password via option 2 — see the Makefile (TEST_PW_VERIFIER, applied to
 * run-with-guests/debug/qemu-run/test-integration) and .github/workflows/.
 * That is a build-time TEST credential, NOT a committed default.
 *
 * NOTE: _pw_verifier[] lives in the Phase 2-measured .rodata, so changing the
 * password changes the .rodata golden ONLY (never .text); reprovision with
 * `make reprovision-goldens` after a real (option 1) change.
 */
#ifndef VSE_PW_VERIFIER
#error "VSE_PW_VERIFIER is unset: no default operator password is shipped (fail-closed). Provision one with scripts/provision_password.sh '<password>', or pass -DVSE_PW_VERIFIER='{...}' (python3 scripts/totp_gen.py --pw-define '<password>'). See vse/pw_verifier.h and docs/RUNBOOK.md."
#endif
