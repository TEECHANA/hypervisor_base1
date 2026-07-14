# Historical logs — old synthetic-inclusive build

These captures predate commit 9c59d8f ("Retire synthetic IDS storm demo").
They were taken from a build that still compiled -DVSE_IDS_STORM_DEMO, so they
show a **synthetic VM3 fault storm** (`running STORM demo`, `quarantining VM3`,
injected address `0xBEEF...`) that the current hypervisor no longer produces.

Retained for historical reference only. They do NOT describe current behavior.
For the current, synthetic-free behavior see `run_current.log` at the repo
root, where the storm/enforcement is driven organically by VM2's own rogue DMA.

- `demo_boot_SAFE.log`  — old safe-baseline boot (shows VM3 synthetic storm)
- `demo_rogue_SAFE.log` — old rogue demo (VM3 synthetic + VM2 organic)

Byte-identical originals also exist outside the repo in ~/ and ~/Documents/.
