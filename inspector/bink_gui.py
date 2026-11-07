#!/usr/bin/env python3
"""Bink inspector: graphical inspection of the keybed sensor stream.

    python bink_gui.py           # auto-detect the keybed by USB id (B177:4009)
    python bink_gui.py COM7      # explicit port

See README.md and MANUAL.md in this folder.

Top:    connection controls and board commands (calibration, data mode, octaves).
Middle: live per-key displacement bars. Click a key to watch it in the scope,
        shift-click to add/remove keys so several traces overlay.
Scope:  scrolling traces of the selected keys with note-on/off markers and the
        keybed's 280/100 threshold lines, for validating velocity behavior.
Bottom: note event log and status line (msg rate, protocol resyncs, logging).

Only dependency: pyserial (tkinter ships with Python).
"""

import collections
import glob
import os
import shutil
import sys
import threading
import time
import tkinter as tk
from tkinter import ttk, filedialog, messagebox, simpledialog

from bink_monitor import (Decoder, TYPE_NAMES, ID_OCTAVES, ID_TEMP_BASE,
                          ID_TEMP_COUNT, find_bink_port)

SCOPE_SECONDS = 5.0
BAR_MAX = 4095
NOTEON_THRESHOLD = 280   # see "Note on and note off" in docs/host_protocol.md
NOTEOFF_THRESHOLD = 100
BLACK_KEYS = {1, 3, 6, 8, 10}
MONO = {"darwin": "Menlo", "win32": "Consolas"}.get(sys.platform, "DejaVu Sans Mono")

# trace colours for overlaid keys, in selection order
TRACE_COLORS = ["#8ab4f8", "#ffb74d", "#f48fb1", "#80cbc4", "#ce93d8",
                "#fff176", "#ffab91", "#a5d6a7"]

COMMANDS = [
    ("Cal zero", "_"), ("Cal mid", "-"), ("Cal deep", "^"),
    ("Compensated", "C"), ("Raw", "R"),
    ("Temps", "T"), ("Save EEPROM", "E"),
]


class BinkGui:
    def __init__(self, root, port=None):
        self.root = root
        root.title("Bink inspector")
        root.geometry("1000x720")

        self.ser = None
        self.dec = Decoder(self._on_msg)
        self.lock = threading.Lock()
        self.values = {}
        self.note_state = {}
        self.note_vel = {}
        self.temps = {}
        self.octaves = None
        self.pending_events = collections.deque()
        self.msg_count = 0
        self.rate = 0.0
        self._rate_ref = (0, time.time())
        self.t0 = time.time()
        self.selected = [0]                               # keys shown in the scope, in order
        self.scope = {}                                   # mid -> deque of (t, value)
        self.scope_marks = {}                             # mid -> deque of (t, on, vel)
        self._ensure_scope(0)
        self.log_file = None
        self.log_writer = None

        self._build_ui()
        self.root.protocol("WM_DELETE_WINDOW", self._close)
        self._refresh_ports()
        if port:
            self.port_var.set(port)
            self._connect()
        else:
            auto = find_bink_port()
            if auto:
                self.port_var.set(auto)
                self._connect()
        self._tick()

    # ---------------- UI construction ----------------
    def _build_ui(self):
        top = ttk.Frame(self.root, padding=(4, 4, 4, 0))
        top.pack(fill="x")
        ttk.Label(top, text="Port:").pack(side="left")
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(top, textvariable=self.port_var, width=28)
        self.port_combo.pack(side="left", padx=2)
        ttk.Button(top, text="Rescan", command=self._refresh_ports).pack(side="left", padx=2)
        self.connect_btn = ttk.Button(top, text="Connect", command=self._toggle_connect)
        self.connect_btn.pack(side="left", padx=6)
        self.log_btn = ttk.Button(top, text="Start log", command=self._toggle_log)
        self.log_btn.pack(side="left", padx=8)
        self.flash_btn = ttk.Button(top, text="Flash UF2", command=self._flash_uf2)
        self.flash_btn.pack(side="left", padx=2)

        board = ttk.Frame(self.root, padding=(4, 2, 4, 4))
        board.pack(fill="x")
        for label, cmd in COMMANDS:
            ttk.Button(board, text=label, width=len(label),
                       command=lambda c=cmd: self._send(c)).pack(side="left", padx=2)
        ttk.Label(board, text="Octaves:").pack(side="left", padx=(8, 0))
        for n in range(1, 9):
            ttk.Button(board, text=str(n), width=2,
                       command=lambda c=n: self._set_octaves(c)).pack(side="left")

        self.keys_canvas = tk.Canvas(self.root, height=240, bg="#181820",
                                     highlightthickness=0)
        self.keys_canvas.pack(fill="x", padx=4, pady=(4, 0))
        self.keys_canvas.bind("<Button-1>", self._on_key_click)
        self.keys_canvas.bind("<Shift-Button-1>", lambda ev: self._on_key_click(ev, toggle=True))

        ttk.Label(self.root, text="Scope (click a key above to select, shift-click to overlay more keys)").pack(anchor="w", padx=6)
        self.scope_canvas = tk.Canvas(self.root, height=220, bg="#101018",
                                      highlightthickness=0)
        self.scope_canvas.pack(fill="both", expand=True, padx=4)

        bottom = ttk.Frame(self.root, padding=4)
        bottom.pack(fill="x")
        self.events_list = tk.Listbox(bottom, height=6, font=(MONO, 9))
        self.events_list.pack(fill="x")
        self.status_var = tk.StringVar(value="disconnected")
        ttk.Label(self.root, textvariable=self.status_var, padding=4,
                  relief="sunken").pack(fill="x", side="bottom")

    # ---------------- connection ----------------
    def _refresh_ports(self):
        from serial.tools import list_ports
        ports = [p.device for p in list_ports.comports()]
        self.port_combo["values"] = ports
        if not self.port_var.get() and ports:
            auto = find_bink_port()
            self.port_var.set(auto or ports[0])

    def _toggle_connect(self):
        if self.ser:
            self._disconnect()
        else:
            self._connect()

    def _connect(self):
        import serial
        port = self.port_var.get()
        if not port:
            messagebox.showwarning("Bink inspector", "No port selected.")
            return
        try:
            self.ser = serial.Serial(port, 1000000, timeout=0.02)
        except Exception as e:
            messagebox.showerror("Bink inspector", f"Could not open {port}:\n{e}")
            self.ser = None
            return
        self.connect_btn.config(text="Disconnect")
        threading.Thread(target=self._reader, daemon=True).start()
        self.root.after(200, lambda: self._send("u"))  # switch board to USB output
        self.root.after(400, self._probe_octaves)      # ask for the octave count

    def _probe_octaves(self, attempt=0):
        """Ask the board how many octaves it has; retry until it answers.

        The board also announces this on DTR, but older firmware only replies
        to the explicit 'O' command, and a reply can be lost mid-resync.
        """
        if not self.ser:
            return
        with self.lock:
            known = self.octaves is not None
        if known:
            return
        if attempt >= 10:
            self._ask_octaves()
            return
        self._send("O")
        self.root.after(500, lambda: self._probe_octaves(attempt + 1))

    def _ask_octaves(self):
        """Board didn't answer the octave probe: ask the user, then program the board."""
        n = simpledialog.askinteger(
            "Bink inspector",
            "The board did not report its octave count.\n"
            "How many octaves does this keybed have? (1-8)",
            parent=self.root, minvalue=1, maxvalue=8, initialvalue=4)
        if n is None or not self.ser:
            self.pending_events.append("octave count unknown; showing keys as they report")
            return
        self._send(str(n))   # rebuilds the board's map from sensors to key IDs
        with self.lock:
            self.octaves = n
            self._layout_keys(n)
        self.pending_events.append(f"octaves set to {n} by user")

    def _set_octaves(self, n):
        """Set the octave count. The board does not announce it, so ask."""
        self._send(str(n))
        self._send("O")

    def _layout_keys(self, octaves):
        """Pre-populate the key strip with every key the board will report.

        The board sends octaves*12 keys plus two more IDs: the top C and the
        ambient field sensor. Values arrive only when they change, so without
        this the strip would stay empty for keys that sit at zero.
        """
        n = octaves * 12 + 2
        for mid in range(n):
            self.values.setdefault(mid, 0)
        # drop stale ids from a previous board with more octaves
        for mid in [m for m in self.values if m >= n]:
            self.values.pop(mid, None)
            self.note_state.pop(mid, None)
            self.note_vel.pop(mid, None)
        self.selected = [m for m in self.selected if m < n] or [0]
        for m in list(self.scope):
            if m >= n:
                self.scope.pop(m, None)
                self.scope_marks.pop(m, None)
        self._ensure_scope(self.selected[0])
        self.pending_events.append(f"board reports {octaves} octaves -> {n} keys")

    def _disconnect(self):
        ser, self.ser = self.ser, None
        if ser:
            try:
                ser.write(b"U")   # hand the stream back to the UART host
                ser.close()
            except Exception:
                pass
        self.connect_btn.config(text="Connect")
        with self.lock:
            self.octaves = None   # re-probe on next connect

    def _reader(self):
        while True:
            ser = self.ser
            if ser is None:
                return
            try:
                # first byte blocks (short timeout), then drain the queue so
                # message timestamps track actual arrival, not batch boundaries
                data = ser.read(1)
                if data and ser.in_waiting:
                    data += ser.read(ser.in_waiting)
            except Exception:
                self.root.after(0, self._disconnect)
                return
            if data:
                self.dec.feed(data)

    def _send(self, ch):
        if self.ser:
            try:
                self.ser.write(ch.encode("ascii"))
                self.pending_events.append(f"{time.strftime('%H:%M:%S')}  sent command '{ch}'")
            except Exception:
                self._disconnect()

    # ---------------- data ----------------
    def _on_msg(self, mid, mtype, value):
        now = time.time()
        with self.lock:
            self.msg_count += 1
            if self.log_writer:
                self.log_writer.writerow([f"{now - self.t0:.6f}", mid, TYPE_NAMES[mtype], value])
            if mtype == 3:
                return
            if mid == ID_OCTAVES:
                if value != self.octaves and 0 < value <= 8:
                    self.octaves = value
                    self._layout_keys(value)
                return
            if ID_TEMP_BASE <= mid < ID_TEMP_BASE + ID_TEMP_COUNT:
                self.temps[mid - ID_TEMP_BASE] = value
                return
            if mtype == 0:
                self.values[mid] = value
                if mid in self.scope:
                    self.scope[mid].append((now, value))
            else:
                on = mtype == 1
                self.note_state[mid] = on
                self.note_vel[mid] = value
                name = "ON " if on else "OFF"
                self.pending_events.append(
                    f"{time.strftime('%H:%M:%S')}  NOTE {name} key {mid:2d}  vel {value}")
                if mid in self.scope_marks:
                    self.scope_marks[mid].append((now, on, value))

    def _toggle_log(self):
        import csv
        with self.lock:
            if self.log_writer:
                self.log_file.close()
                self.log_file = self.log_writer = None
                self.log_btn.config(text="Start log")
                self.pending_events.append("logging stopped")
            else:
                path = time.strftime("bink_log_%Y%m%d_%H%M%S.csv")
                self.log_file = open(path, "w", newline="")
                self.log_writer = csv.writer(self.log_file)
                self.log_writer.writerow(["t", "id", "type", "value"])
                self.log_btn.config(text="Stop log")
                self.pending_events.append(f"logging to {path}")

    # ---------------- rendering ----------------
    def _ensure_scope(self, mid):
        self.scope.setdefault(mid, collections.deque(maxlen=20000))
        self.scope_marks.setdefault(mid, collections.deque(maxlen=200))

    def _on_key_click(self, ev, toggle=False):
        """Plain click: show only this key. Shift-click: add/remove it from the overlay."""
        with self.lock:
            ids = sorted(self.values)
        if not ids:
            return
        w = max(1, self.keys_canvas.winfo_width())
        idx = min(len(ids) - 1, int(ev.x * len(ids) / w))
        mid = ids[idx]
        with self.lock:
            if toggle:
                if mid in self.selected:
                    if len(self.selected) > 1:   # keep at least one trace
                        self.selected.remove(mid)
                        self.scope.pop(mid, None)
                        self.scope_marks.pop(mid, None)
                else:
                    self.selected.append(mid)
                    self._ensure_scope(mid)
            else:
                self.selected = [mid]
                self.scope.clear()
                self.scope_marks.clear()
                self._ensure_scope(mid)

    def _trace_color(self, mid):
        return TRACE_COLORS[self.selected.index(mid) % len(TRACE_COLORS)]

    def _tick(self):
        self._draw_keys()
        self._draw_scope()
        self._drain_events()
        self._update_status()
        self.root.after(33, self._tick)

    def _draw_keys(self):
        c = self.keys_canvas
        c.delete("all")
        with self.lock:
            items = sorted(self.values.items())
            note_state = dict(self.note_state)
            note_vel = dict(self.note_vel)
            selected = list(self.selected)
            ambient = self.octaves * 12 + 1 if self.octaves else None
        if not items:
            c.create_text(c.winfo_width() // 2, 110, fill="#888",
                          text="waiting for data… (connect the keybed)")
            return
        w = max(1, c.winfo_width())
        h = int(c["height"])
        bw = w / len(items)
        for i, (mid, v) in enumerate(items):
            x0, x1 = i * bw + 1, (i + 1) * bw - 1
            bar_h = min(1.0, v / BAR_MAX) * (h - 40)
            black = (mid % 12) in BLACK_KEYS
            if note_state.get(mid):
                fill = "#4caf50"
            elif mid in selected:
                fill = TRACE_COLORS[selected.index(mid) % len(TRACE_COLORS)]
            else:
                fill = "#405070" if black else "#6a85b5"
            c.create_rectangle(x0, h - 30 - bar_h, x1, h - 30, fill=fill, width=0)
            if mid in selected:
                c.create_rectangle(x0, 4, x1, h - 30,
                                   outline=TRACE_COLORS[selected.index(mid) % len(TRACE_COLORS)])
            c.create_text((x0 + x1) / 2, h - 20, fill="#aaa",
                          text="amb" if mid == ambient else str(mid),
                          font=(MONO, 7))
            vel = note_vel.get(mid)
            if vel is not None:
                c.create_text((x0 + x1) / 2, h - 8, fill="#7fc97f", text=str(vel),
                              font=(MONO, 7))

    def _draw_scope(self):
        c = self.scope_canvas
        c.delete("all")
        w = max(1, c.winfo_width())
        h = max(1, c.winfo_height())
        now = time.time()
        t_left = now - SCOPE_SECONDS

        def x(t):
            return (t - t_left) / SCOPE_SECONDS * w

        def y(v):
            return h - 4 - min(1.0, v / BAR_MAX) * (h - 8)

        # threshold guides
        for thr, col in ((NOTEON_THRESHOLD, "#2e7d32"), (NOTEOFF_THRESHOLD, "#b23b3b")):
            c.create_line(0, y(thr), w, y(thr), fill=col, dash=(3, 4))
            c.create_text(28, y(thr) - 7, fill=col, text=str(thr), font=(MONO, 8))
        with self.lock:
            selected = list(self.selected)
            traces = []
            for mid in selected:
                pts = [(t, v) for t, v in self.scope.get(mid, ()) if t >= t_left]
                marks = [(t, on, vel) for t, on, vel in self.scope_marks.get(mid, ()) if t >= t_left]
                traces.append((mid, pts, marks))
        # legend, right-aligned, one entry per trace in its colour
        lx = w - 8
        for mid in reversed(selected):
            col = TRACE_COLORS[selected.index(mid) % len(TRACE_COLORS)]
            item = c.create_text(lx, 12, anchor="e", fill=col, text=f"key {mid}",
                                 font=(MONO, 10, "bold"))
            lx = c.bbox(item)[0] - 10
        for mid, pts, marks in traces:
            col = TRACE_COLORS[selected.index(mid) % len(TRACE_COLORS)]
            if len(pts) > 1:
                coords = []
                for t, v in pts:
                    coords += [x(t), y(v)]
                c.create_line(*coords, fill=col, width=1)
                # hold last value to the right edge
                c.create_line(x(pts[-1][0]), y(pts[-1][1]), w, y(pts[-1][1]),
                              fill=col, width=1)
            for t, on, vel in marks:
                mcol = "#4caf50" if on else "#ef5350"
                c.create_line(x(t), 0, x(t), h, fill=mcol)
                c.create_text(x(t) + 2, 20 if on else h - 20, anchor="w", fill=col,
                              text=f"k{mid} v{vel}", font=(MONO, 8))

    def _drain_events(self):
        while self.pending_events:
            self.events_list.insert(0, self.pending_events.popleft())
        if self.events_list.size() > 200:
            self.events_list.delete(200, "end")

    def _update_status(self):
        now = time.time()
        with self.lock:
            cnt = self.msg_count
            resyncs = self.dec.resyncs
            octaves = self.octaves
            temps = dict(self.temps)
        ref_cnt, ref_t = self._rate_ref
        if now - ref_t >= 1.0:
            self.rate = (cnt - ref_cnt) / (now - ref_t)
            self._rate_ref = (cnt, now)
        conn = self.ser.port if self.ser else "disconnected"
        tstr = " ".join(f"{i}:{v}" for i, v in sorted(temps.items())) or "-"
        self.status_var.set(
            f"{conn} | {self.rate:7.0f} msg/s | octaves: {octaves} | "
            f"resyncs: {resyncs}\ntemps: {tstr}")

    # ---------------- firmware flashing ----------------
    def _flash_uf2(self):
        """Copy a chosen uf2 to every RP2 drive in BOOTSEL mode.

        The board reboots into the new firmware as soon as the copy lands,
        drops off as a mass-storage drive and comes back as a serial port,
        which we then pick up with a rescan/auto-connect.
        """
        drives = find_rp2_drives()
        if not drives:
            messagebox.showwarning("Bink inspector",
                                   "No RP2350/RP2354 drive found.\n"
                                   "Hold BOOTSEL while plugging in the board and try again.")
            return
        uf2 = filedialog.askopenfilename(parent=self.root, title="Firmware to flash",
                                         initialdir=firmware_build_dir(),
                                         filetypes=[("UF2 firmware", "*.uf2")])
        if not uf2:
            return
        self.flash_btn.config(state="disabled")
        self.status_var.set(f"flashing {os.path.basename(uf2)} to {', '.join(drives)} ...")
        threading.Thread(target=self._flash_worker, args=(uf2, drives), daemon=True).start()

    def _flash_worker(self, uf2, drives):
        results = []
        for d in drives:
            try:
                shutil.copyfile(uf2, os.path.join(d, os.path.basename(uf2)))
                results.append(f"{d}: ok")
            except OSError as e:
                # The drive vanishes mid-write when the board reboots; that is
                # the normal success path on macOS/Linux, not a failure.
                if e.errno in (5, 6, 19, 28, 30, 2) or not os.path.exists(d):
                    results.append(f"{d}: ok (rebooted)")
                else:
                    results.append(f"{d}: FAILED {e}")
        self.root.after(0, self._flash_done, uf2, results)

    def _flash_done(self, uf2, results):
        self.flash_btn.config(state="normal")
        summary = "; ".join(results)
        self.status_var.set(f"flashed {os.path.basename(uf2)} -> {summary}")
        self.events_list.insert("end", f"flash: {summary}")
        self.events_list.see("end")
        # give the board a moment to enumerate as a serial device, then attach
        self.root.after(2500, self._reconnect_after_flash)

    def _reconnect_after_flash(self):
        if self.ser:
            return
        self._refresh_ports()
        auto = find_bink_port()
        if auto:
            self.port_var.set(auto)
            self._connect()
        else:
            self.status_var.set("flashed; waiting for board serial port (press Rescan)")

    def _close(self):
        self._disconnect()
        if self.log_file:
            self.log_file.close()
        self.root.destroy()


def firmware_build_dir():
    """Where the file picker starts: the DevKit firmware build, if there is one."""
    here = os.path.dirname(os.path.abspath(__file__))
    build = os.path.normpath(os.path.join(here, "..", "firmware", "build"))
    return build if os.path.isdir(build) else here


def find_rp2_drives():
    """Mounted RP2040/RP2350/RP2354 BOOTSEL drives, identified by INFO_UF2.TXT."""
    roots = []
    if sys.platform == "darwin":
        roots = glob.glob("/Volumes/*")
    elif sys.platform.startswith("win"):
        import string
        roots = [f"{c}:\\" for c in string.ascii_uppercase if os.path.exists(f"{c}:\\")]
    else:
        roots = glob.glob("/media/*/*") + glob.glob("/run/media/*/*") + glob.glob("/mnt/*")
    drives = []
    for r in roots:
        info = os.path.join(r, "INFO_UF2.TXT")
        try:
            if not os.path.isfile(info):
                continue
            with open(info, "r", errors="replace") as f:
                txt = f.read()
            if "RP2" in txt or "RPI-RP2" in os.path.basename(r):
                drives.append(r)
        except OSError:
            continue
    return drives


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else None
    root = tk.Tk()
    try:
        ttk.Style().theme_use("clam")
    except Exception:
        pass
    BinkGui(root, port)
    root.mainloop()


if __name__ == "__main__":
    main()
