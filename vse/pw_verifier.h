/*
 * pw_verifier.h — provisioned VSE operator password verifier.
 *
 * Defines VSE_PW_VERIFIER = HMAC(login-pepper, password), the value
 * login.c stores in _pw_verifier[]. Set the operator password per
 * deployment by REGENERATING THIS FILE (never edit login.c):
 *     scripts/provision_password.sh '<password>'
 * or override at build time with -DVSE_PW_VERIFIER='{...}'.
 *
 * Derived with the dev/QEMU master key. The committed default is HMAC(., "changeme").
 *
 * NOTE: this array lives in the Phase 2-measured .rodata. Changing it
 * changes the .rodata golden ONLY (never .text); reprovision slot [1]
 * of _golden_components[] (manual LEARN-mode boot) after a real change.
 */
#ifndef VSE_PW_VERIFIER
#define VSE_PW_VERIFIER { \
    0x21, 0x34, 0xf2, 0xdf, 0x9d, 0x60, 0xa8, 0x41, \
    0x3f, 0xb2, 0x15, 0x89, 0x56, 0xfa, 0x02, 0x7a, \
    0x19, 0xf4, 0x2f, 0xfd, 0x4b, 0xe7, 0x3a, 0x9e, \
    0x9e, 0x17, 0xdb, 0x24, 0x77, 0xdc, 0xed, 0xf5, \
}
#endif
