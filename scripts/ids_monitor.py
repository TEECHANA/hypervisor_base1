#!/usr/bin/env python3
"""
ids_monitor.py — Tessolve VSE/IDS live monitor (Audit fix #6).

Tails the hypervisor log produced by run_qemu.sh (via `tee`) and renders:
  - per-VM trust state table (color-coded)
  - global counters (alerts / enforcements / faults / polls)
  - scrolling event feed (alerts, quarantines, trust transitions, heartbeats)

Works on a LIVE log (tail -f style) or a SAVED log (parse to end then stop).

Usage:
    python3 scripts/ids_monitor.py [/path/to/log]        # live tail
    python3 scripts/ids_monitor.py --once /path/to/log   # parse once, no follow

Default log path: /tmp/key.log  (matches the run_qemu.sh `tee` target).

No external dependencies — standard-library tkinter only. Parsing is tolerant
of the hypervisor's '0x0x' hex prefix quirk and the freestanding logger format.
"""

import sys
import os
import re
import time
import tkinter as tk
from tkinter import ttk, scrolledtext

DEFAULT_LOG = "/tmp/key.log"
POLL_MS = 400  # how often the GUI re-reads the log

# ── Trust level → display color ──
TRUST_COLORS = {
    "TRUSTED":    "#1a7f37",
    "DEGRADED":   "#bf8700",
    "QUARANTINE": "#cf222e",
    "REVOKED":    "#82071e",
    "UNKNOWN":    "#57606a",
}

# ── Log line patterns (match the hypervisor's real output) ──
RE_ALERT      = re.compile(r"VSE IDS: ALERT VM(\d+)\s+(.*)")
RE_ENFORCE    = re.compile(r"VSE IDS: ENFORCING\s+[—-]+\s*(.*)")
RE_QUARANTINE = re.compile(r"QUARANTINING VM(\d+)\s*'?([^']*)'?")
RE_TRUST_TX   = re.compile(r"VM(\d+)\s+trust\s+(\w+)\s*->\s*(\w+)")
RE_TRUST_DROP = re.compile(r"VSE IDS: VM(\d+) trust dropped\s+(\w+)\s*->\s*(\w+)")
RE_PHASE_TRUST= re.compile(r"VSE Phase 3: VM(\d+)\s+'([^']*)'\s*->\s*(\w+)")
RE_HEARTBEAT  = re.compile(r"VSE IDS: poll heartbeat\s*[—-]+\s*total_faults=(\d+)\s+new_anom=(\d+)")
RE_FAULT      = re.compile(r"VM(\d+)\s+(?:DMA violation|memory fault|PERMISSION fault|fault type)")


class IDSMonitor:
    def __init__(self, root, log_path, follow=True):
        self.root = root
        self.log_path = log_path
        self.follow = follow
        self._fh = None
        self._inode = None

        # state
        self.vm_trust = {}      # vm_id -> (level, name)
        self.counts = {"alerts": 0, "enforcements": 0, "faults": 0, "polls": 0}
        self.last_faults_total = 0

        self._build_ui()
        self._open_log()
        self.root.after(POLL_MS, self._tick)

    # ── UI ──
    def _build_ui(self):
        self.root.title("Tessolve VSE / IDS Monitor")
        self.root.geometry("860x620")
        self.root.configure(bg="#0d1117")

        hdr = tk.Label(self.root, text="VSE / IDS Intrusion Monitor",
                       bg="#0d1117", fg="#e6edf3",
                       font=("DejaVu Sans Mono", 16, "bold"))
        hdr.pack(pady=(12, 2))

        self.src_lbl = tk.Label(self.root, text=f"source: {self.log_path}",
                                bg="#0d1117", fg="#57606a",
                                font=("DejaVu Sans Mono", 9))
        self.src_lbl.pack()

        # counters row
        cf = tk.Frame(self.root, bg="#0d1117")
        cf.pack(fill="x", padx=16, pady=10)
        self.counter_lbls = {}
        for key, label in [("alerts", "ALERTS"), ("enforcements", "ENFORCEMENTS"),
                           ("faults", "FAULTS"), ("polls", "POLLS")]:
            cell = tk.Frame(cf, bg="#161b22", bd=1, relief="solid")
            cell.pack(side="left", expand=True, fill="x", padx=4)
            tk.Label(cell, text=label, bg="#161b22", fg="#8b949e",
                     font=("DejaVu Sans Mono", 9)).pack(pady=(6, 0))
            v = tk.Label(cell, text="0", bg="#161b22", fg="#e6edf3",
                         font=("DejaVu Sans Mono", 20, "bold"))
            v.pack(pady=(0, 6))
            self.counter_lbls[key] = v

        # per-VM trust table
        tk.Label(self.root, text="Guest Trust State", bg="#0d1117", fg="#e6edf3",
                 font=("DejaVu Sans Mono", 11, "bold")).pack(anchor="w", padx=16)
        self.vm_frame = tk.Frame(self.root, bg="#0d1117")
        self.vm_frame.pack(fill="x", padx=16, pady=(4, 10))
        self.vm_rows = {}

        # event feed
        tk.Label(self.root, text="Event Feed", bg="#0d1117", fg="#e6edf3",
                 font=("DejaVu Sans Mono", 11, "bold")).pack(anchor="w", padx=16)
        self.feed = scrolledtext.ScrolledText(
            self.root, height=16, bg="#161b22", fg="#e6edf3",
            insertbackground="#e6edf3", font=("DejaVu Sans Mono", 9),
            wrap="none", bd=0)
        self.feed.pack(fill="both", expand=True, padx=16, pady=(4, 14))
        self.feed.tag_config("alert", foreground="#ff7b72")
        self.feed.tag_config("enforce", foreground="#cf222e")
        self.feed.tag_config("quarantine", foreground="#cf222e")
        self.feed.tag_config("trust", foreground="#d29922")
        self.feed.tag_config("beat", foreground="#3fb950")
        self.feed.tag_config("info", foreground="#8b949e")

    def _set_vm(self, vm_id, level=None, name=None):
        cur_level, cur_name = self.vm_trust.get(vm_id, ("UNKNOWN", f"vm{vm_id}"))
        if level: cur_level = level
        if name:  cur_name = name
        self.vm_trust[vm_id] = (cur_level, cur_name)
        self._render_vm_rows()

    def _render_vm_rows(self):
        for w in self.vm_frame.winfo_children():
            w.destroy()
        for vm_id in sorted(self.vm_trust):
            level, name = self.vm_trust[vm_id]
            color = TRUST_COLORS.get(level, "#57606a")
            row = tk.Frame(self.vm_frame, bg="#161b22")
            row.pack(fill="x", pady=2)
            tk.Label(row, text=f"VM{vm_id}", width=6, bg="#161b22", fg="#e6edf3",
                     font=("DejaVu Sans Mono", 10, "bold")).pack(side="left", padx=6)
            tk.Label(row, text=name, width=14, anchor="w", bg="#161b22",
                     fg="#8b949e", font=("DejaVu Sans Mono", 10)).pack(side="left")
            badge = tk.Label(row, text=level, bg=color, fg="white",
                             font=("DejaVu Sans Mono", 10, "bold"), padx=10)
            badge.pack(side="right", padx=6, pady=3)

    def _feed(self, text, tag="info"):
        self.feed.insert("end", text + "\n", tag)
        self.feed.see("end")

    def _refresh_counts(self):
        for k, lbl in self.counter_lbls.items():
            lbl.config(text=str(self.counts[k]))

    # ── log handling ──
    def _open_log(self):
        try:
            self._fh = open(self.log_path, "r", errors="replace")
            self._inode = os.fstat(self._fh.fileno()).st_ino
        except FileNotFoundError:
            self._fh = None
            self._feed(f"[waiting for {self.log_path} ...]", "info")

    def _tick(self):
        if self._fh is None:
            self._open_log()
        if self._fh is not None:
            # handle log rotation (run_qemu.sh recreated the file)
            try:
                if os.stat(self.log_path).st_ino != self._inode:
                    self._fh.close()
                    self._open_log()
            except FileNotFoundError:
                pass
            if self._fh:
                for line in self._fh:
                    self._parse(line.rstrip("\n"))
                self._refresh_counts()
        if self.follow:
            self.root.after(POLL_MS, self._tick)

    def _parse(self, line):
        m = RE_HEARTBEAT.search(line)
        if m:
            total, new = int(m.group(1)), int(m.group(2))
            self.counts["polls"] += 1
            self._feed(f"poll  heartbeat  total_faults={total} new_anom={new}", "beat")
            return

        m = RE_ENFORCE.search(line)
        if m:
            self.counts["enforcements"] += 1
            self._feed(f"ENFORCE  {m.group(1)}", "enforce")
            return

        m = RE_TRUST_DROP.search(line)
        if m:
            vm, a, b = m.group(1), m.group(2), m.group(3)
            self._set_vm(vm, level=b)
            self._feed(f"VM{vm}  trust dropped {a} -> {b}", "trust")
            return

        m = RE_ALERT.search(line)
        if m:
            self.counts["alerts"] += 1
            self._feed(f"ALERT  VM{m.group(1)}  {m.group(2)}", "alert")
            return

        m = RE_QUARANTINE.search(line)
        if m:
            vm, name = m.group(1), (m.group(2) or "").strip()
            self._set_vm(vm, level="QUARANTINE", name=name or None)
            self._feed(f"VM{vm}  QUARANTINED", "quarantine")
            return

        m = RE_PHASE_TRUST.search(line)
        if m:
            vm, name, level = m.group(1), m.group(2), m.group(3)
            self._set_vm(vm, level=level, name=name)
            return

        m = RE_TRUST_TX.search(line)
        if m:
            vm, a, b = m.group(1), m.group(2), m.group(3)
            self._set_vm(vm, level=b)
            self._feed(f"VM{vm}  trust {a} -> {b}", "trust")
            return

        if RE_FAULT.search(line):
            self.counts["faults"] += 1
            return


def main():
    args = [a for a in sys.argv[1:]]
    follow = True
    if "--once" in args:
        follow = False
        args.remove("--once")
    log_path = args[0] if args else DEFAULT_LOG

    root = tk.Tk()
    IDSMonitor(root, log_path, follow=follow)
    root.mainloop()


if __name__ == "__main__":
    main()
