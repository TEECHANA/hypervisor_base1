# Tessolve Hypervisor — Manual Run Runbook

Follow this yourself. Every step lists the **command**, **what it does**, the **expected
output** (the key line to look for), and whether it **dirties the git tree**.

Run everything from the repo root (the directory containing the `Makefile`).

Prerequisites (one-time):
```bash
sudo apt-get update
sudo apt-get install -y gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu \
                        qemu-system-arm python3 make gcc git
```

---

## SAFETY NOTES — read first

Most commands are read-only w.r.t. git (`build/` and `*.bak*` are git-ignored). **Only
these touch tracked files:**

| Command | Modifies (tracked) | How to revert |
|---|---|---|
| `scripts/provision_password.sh '<pw>'` | `vse/pw_verifier.h` | `git checkout -- vse/pw_verifier.h` |
| `make reprovision-goldens` | `vse/component_check.c` (only if goldens changed) | `git checkout -- vse/component_check.c` |
| `boot_capture … run_current.log` (regenerating the proof log) | `run_current.log` | `git checkout -- run_current.log` |

- **`run_current.log` is git-tracked** (it is the committed proof log). Any boot capture
  that writes to it will show up in `git status`. Restore with `git checkout -- run_current.log`.
- After any step above, check `git status --porcelain`. If you only wanted to *test*,
  revert with the matching `git checkout --` line so the tree is clean again.
- The integration sub-tests (`rodata_wp_verify.sh`, `trust_promote_verify.sh`,
  `password_provision_verify.sh`) build into throwaway temp dirs and restore anything
  they touch — they leave the tree **clean**.

Quick clean-tree check at any time:
```bash
git status --porcelain          # empty output == clean
```

---

## 1. Build

```bash
make qemu
```
- **Does:** compiles + links the hypervisor into `build/qemu/hypervisor.elf`.
- **Expect:** `✓ Hypervisor built: build/qemu/hypervisor.elf`
- **Dirties tree:** No (`build/` is git-ignored).

Optional clean rebuild:
```bash
make clean && make qemu         # clean removes build/ only; committed guest blobs survive
```

---

## 2. Full verification suite

```bash
make check-guests
```
- **Does:** verifies the committed guest images (linux, rtos, android) against their Phase 2 goldens (no rebuild).
- **Expect:** `✓ All guest images present and match goldens`
- **Dirties tree:** No.

```bash
make test-unit
```
- **Does:** runs the host-side unit tests (compiled with the native gcc).
- **Expect:** `Passed:10  Failed:0`
- **Dirties tree:** No.

```bash
make clean && make test-integration
```
- **Does:** from a fully clean tree, builds the ELF, then boots QEMU and runs the whole
  end-to-end suite (attestation, 2FA login, rogue-DMA → quarantine → failover, console
  restore, `.rodata` write-protect, trust auto-promote, password provisioning). Takes a
  few minutes.
- **Expect:** `All integration checks passed.` (46 PASS, 0 FAIL)
- **Dirties tree:** No (sub-tests restore what they touch).

---

## 3. Individual proof scripts

Each builds its own throwaway image and boots QEMU. Run standalone to isolate one proof.

```bash
bash tests/integration/rodata_wp_verify.sh
```
- **Proves:** a store to `.rodata` triggers an EL2 permission fault and the boot halts at
  the probe before Phase 1 — i.e. `.rodata` is write-protected at runtime.
- **Expect:** `.rodata write-protection verified at runtime.` (preceded by
  `PASS: store to .rodata FAULTED at the probe (halted before Phase 1).`)
- **Dirties tree:** No.

```bash
bash tests/integration/trust_promote_verify.sh
```
- **Proves:** a DEGRADED VM that stays quiet for the clean period is auto-promoted back to
  TRUSTED by the real `ids_poll → trust_auto_promote_tick` path.
- **Expect:** `Auto-promotion verified at runtime.` (preceded by
  `PASS: VM3 auto-promoted DEGRADED -> TRUSTED after clean period`)
- **Dirties tree:** No.

```bash
bash tests/integration/password_provision_verify.sh
```
- **Proves:** login succeeds with a newly provisioned password and is denied with the old
  `changeme`; the committed default is left untouched.
- **Expect:** `Password provisioning verified: new password works, changeme rejected.`
- **Dirties tree:** No (it provisions into a temp build and restores `pw_verifier.h`).

---

## 4. Interactive login run

```bash
make run-with-guests
```
- **Does:** boots the hypervisor + 3 guests in QEMU and drops you at the operator login.
- **At the prompts:**
  - `Password:` → type `changeme`
  - `OTP code:` → get the current TOTP code (next command)
- **Expect (after correct 2FA):** `Access granted.`
- **Dirties tree:** No.
- **Exit QEMU:** `Ctrl-a` then `x`.

Get the TOTP code (in another terminal):
```bash
python3 scripts/totp_gen.py --epoch 1751328000 --elapsed 5
```
- **Does:** prints the TOTP code for the hypervisor's provisioned epoch.
- **Expect:** `TOTP code: 734795  (T=..., step=30s)` → use `734795`.
- **Dirties tree:** No.

---

## 5. Password provisioning (per-deployment) + undo

```bash
scripts/provision_password.sh 'YourStrongPassword!'
```
- **Does:** regenerates `vse/pw_verifier.h` for the new password (plaintext is never stored).
- **Expect:** `==> wrote .../vse/pw_verifier.h`
- **Dirties tree:** **YES — modifies `vse/pw_verifier.h`.**

```bash
make reprovision-goldens && make qemu
```
- **Does:** re-derives both Phase 2 goldens (a new password moves `.rodata`), then rebuilds.
- **Expect:** `both goldens re-provisioned — Phase 2 verifies (.text + .rodata).`
- **Dirties tree:** **YES — may modify `vse/component_check.c`.**

**Undo — restore the committed `changeme` default and goldens:**
```bash
git checkout -- vse/pw_verifier.h vse/component_check.c
make qemu
git status --porcelain          # confirm empty (clean)
```

---

## 6. Golden reprovisioning (when & why)

**When:** after you intentionally change any *measured* hypervisor source (anything in the
`.text`/`.rodata` of the image — e.g. code in `boot/`, `core/`, `vre/`, `vse/`, or a new
log string). The Phase 2 goldens then no longer match and the image would panic at boot.

```bash
make reprovision-goldens
```
- **Does:** boots once in learn mode and re-derives BOTH goldens (`.text` slot 0 + `.rodata`
  slot 1) in `vse/component_check.c`.
- **Expect:** `both goldens re-provisioned — Phase 2 verifies (.text + .rodata).`
- **Dirties tree:** **YES — modifies `vse/component_check.c`** (idempotent: unchanged if the
  goldens already matched). Commit it as part of your source change, or
  `git checkout -- vse/component_check.c` to discard.

Optional local drift guard (rebuild guests and re-verify — not run in CI):
```bash
make check-guests-rebuild
```
- **Expect:** `✓ All guest images present and match goldens`
- **Dirties tree:** No.

---

## 7. IDS monitor

Headless (no display needed):
```bash
python3 scripts/ids_monitor.py --headless --once run_current.log
```
- **Does:** parses the captured runtime log and prints a trust/alert/failover summary.
- **Expect:** `=== VSE / IDS Headless Report ===` followed by the per-VM trust table and
  `[QUARANTINE] VM2` / `[FAILOVER] VM2 …` events.
- **Dirties tree:** No.

GUI (requires a display / X server):
```bash
python3 scripts/ids_monitor.py run_current.log
```
- **Does:** opens the tkinter live monitor window (per-VM trust table, counters, event feed).
- **Expect:** a window titled `Tessolve VSE / IDS Monitor`.
- **Dirties tree:** No.

> Regenerating the proof log (`boot_capture … run_current.log`) **dirties `run_current.log`**
> (it is tracked). Restore with `git checkout -- run_current.log`.

---

## 8. Cross-platform builds (COMPILE-ONLY)

```bash
make rpi4        # Raspberry Pi 4
make s32g        # NXP S32G
```
- **Does:** cross-compiles the hypervisor for the target platform.
- **Expect:** `✓ Hypervisor built: build/<platform>/hypervisor.elf`
- **Dirties tree:** No (`build/` is git-ignored).

> ⚠️ **These only COMPILE. They do NOT boot on hardware.** There is no verified Pi4/S32G
> hardware boot in this repo — the SoC fuse-key path (`plat_read_fuse_key`) and a real
> hardware boot log require actual Pi4/S32G silicon and are **not** exercised by QEMU.
