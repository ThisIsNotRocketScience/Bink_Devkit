#!/usr/bin/env python3
"""Bink terminal monitor: inspect and validate the keybed sensor stream.

Live view (USB CDC or a UART bridge):

    python bink_monitor.py                     # auto-detect the Bink keybed by USB id
    python bink_monitor.py COM7                # live monitor, switches board to USB mode
    python bink_monitor.py COM7 --uart         # don't send 'u'; decode an existing UART stream
    python bink_monitor.py COM7 --log run.csv  # additionally log every message to CSV

Interactive keys while running:
    z / m / h   capture zero / mid / deep calibration levels
    c / r       compensated / raw data mode
    t           request temperatures        o  request octave count
    1..7        set octave count            E  save calibration to flash
    l           toggle CSV logging          q  quit

Offline plotting of a recorded CSV (requires matplotlib):

    python bink_monitor.py plot run.csv --id 17

Protocol (see docs/host_protocol.md): 3-byte messages, header byte 0x80|id, then two
data bytes: d1 = 0TTVVVVV (2-bit type, value bits 11..7), d2 = 0VVVVVVV
(value bits 6..0). The header is omitted when id == previous id + 1
(running status increments). Types: 0=value 1=note-on 2=note-off 3=stat.

Only dependency for the live view: pyserial  (pip install pyserial)
"""

import argparse
import collections
import csv
import os
import sys
import threading
import time

TYPE_NAMES = {0: "VAL", 1: "ON", 2: "OFF", 3: "STAT"}
# USB ids of the keybed. The product string is the same on every size of keybed.
BINK_VID = 0xB177
BINK_PID = 0x4009
BINK_PRODUCT = "Bink-25"
ID_OCTAVES = 90
ID_TEMP_BASE = 100
ID_TEMP_COUNT = 16


class Decoder:
    """Streaming decoder for the Bink serial protocol (incl. running status)."""

    def __init__(self, on_msg):
        self.on_msg = on_msg          # callback(id, type, value)
        self.last_id = None
        self._id = None
        self._d1 = 0
        self._state = 0               # 0 idle, 1 have id, 2 have id+d1
        self.resyncs = 0              # protocol errors seen (for validation)

    def feed(self, data: bytes):
        for b in data:
            if self._state == 0:
                if b & 0x80:
                    self._id = b & 0x7F
                    self._state = 1
                elif self.last_id is not None:
                    # running status: implicit id = last id + 1
                    self._id = (self.last_id + 1) & 0x7F
                    self._d1 = b
                    self._state = 2
                # else: mid-stream join, wait for an explicit header
            elif self._state == 1:
                if b & 0x80:          # unexpected header -> resync
                    self.resyncs += 1
                    self._id = b & 0x7F
                    continue
                self._d1 = b
                self._state = 2
            else:
                if b & 0x80:          # unexpected header -> resync
                    self.resyncs += 1
                    self._id = b & 0x7F
                    self._state = 1
                    continue
                value = ((self._d1 & 0x1F) << 7) | b
                mtype = (self._d1 >> 5) & 0x03
                self.last_id = self._id
                self._state = 0
                self.on_msg(self._id, mtype, value)


class Monitor:
    BAR_MAX = 4095

    def __init__(self, port, log_path=None, send_usb_cmd=True):
        import serial  # pyserial
        self.ser = serial.Serial(port, 1000000, timeout=0.02)
        self.dec = Decoder(self._on_msg)
        self.values = {}                                   # id -> last value
        self.note_state = {}                               # id -> bool
        self.note_vel = {}                                 # id -> last velocity
        self.temps = {}                                    # sensor -> value
        self.octaves = None
        self.events = collections.deque(maxlen=12)
        self.msg_count = 0
        self.rate = 0.0
        self.t0 = time.time()
        self.lock = threading.Lock()
        self.running = True
        self.send_usb_cmd = send_usb_cmd
        self.log_path = log_path
        self.log_file = None
        self.log_writer = None
        if log_path:
            self._open_log(log_path)

    # ---------- logging ----------
    def _open_log(self, path):
        self.log_file = open(path, "w", newline="")
        self.log_writer = csv.writer(self.log_file)
        self.log_writer.writerow(["t", "id", "type", "value"])
        self.log_path = path

    def toggle_log(self):
        with self.lock:
            if self.log_writer:
                self.log_file.close()
                self.log_file = self.log_writer = None
                self.events.append((time.time(), "log stopped", "", ""))
            else:
                path = time.strftime("bink_log_%Y%m%d_%H%M%S.csv")
                self._open_log(path)
                self.events.append((time.time(), "logging to", path, ""))

    # ---------- protocol ----------
    def _on_msg(self, mid, mtype, value):
        now = time.time()
        with self.lock:
            self.msg_count += 1
            if self.log_writer:
                self.log_writer.writerow([f"{now - self.t0:.6f}", mid, TYPE_NAMES[mtype], value])
            if mtype == 3:  # STAT / keepalive / command echo
                return
            if mid == ID_OCTAVES:
                self.octaves = value
                return
            if ID_TEMP_BASE <= mid < ID_TEMP_BASE + ID_TEMP_COUNT:
                self.temps[mid - ID_TEMP_BASE] = value
                return
            if mtype == 0:
                self.values[mid] = value
            elif mtype == 1:
                self.note_state[mid] = True
                self.note_vel[mid] = value
                self.events.append((now, "NOTE ON ", f"key {mid:2d}", f"vel {value}"))
            elif mtype == 2:
                self.note_state[mid] = False
                self.note_vel[mid] = value
                self.events.append((now, "NOTE OFF", f"key {mid:2d}", f"vel {value}"))

    def _reader(self):
        # Block only for the first byte, then drain what's queued. Waiting to
        # fill a big buffer batches ~50 ms of messages under one timestamp,
        # which makes time-based plots look stepped.
        while self.running:
            data = self.ser.read(1)
            if data:
                n = self.ser.in_waiting
                if n:
                    data += self.ser.read(n)
                self.dec.feed(data)

    def send(self, ch):
        self.ser.write(ch.encode("ascii"))

    # ---------- UI ----------
    def _bar(self, v, width=24):
        v = max(0, min(self.BAR_MAX, v))
        n = round(v * width / self.BAR_MAX)
        return "#" * n + "." * (width - n)

    def _render(self):
        with self.lock:
            lines = []
            elapsed = time.time() - self.t0
            log = self.log_path if self.log_writer else "off"
            lines.append(
                f"Bink monitor | {self.ser.port} | {elapsed:7.1f}s | "
                f"{self.rate:7.0f} msg/s | octaves: {self.octaves} | "
                f"resyncs: {self.dec.resyncs} | log: {log}"
            )
            lines.append(
                "[z/m/h] cal zero/mid/deep  [c/r] comp/raw  [t]emps  [1-7] octaves  "
                "[E] save  [l]og  [q]uit"
            )
            lines.append("")
            ids = sorted(self.values)
            half = (len(ids) + 1) // 2
            for row in range(half):
                cells = []
                for mid in (ids[row], ids[row + half] if row + half < len(ids) else None):
                    if mid is None:
                        cells.append(" " * 46)
                        continue
                    v = self.values[mid]
                    on = "*" if self.note_state.get(mid) else " "
                    vel = self.note_vel.get(mid)
                    vel = f"{vel:4d}" if vel is not None else "   -"
                    cells.append(f"{mid:3d}{on}{v:5d} v{vel} |{self._bar(v)}|")
                lines.append("  ".join(cells))
            if self.temps:
                lines.append("")
                lines.append("temps: " + " ".join(f"{i}:{v}" for i, v in sorted(self.temps.items())))
            lines.append("")
            lines.append("events:")
            for t, a, b, c in list(self.events):
                lines.append(f"  {time.strftime('%H:%M:%S', time.localtime(t))} {a} {b} {c}")
        sys.stdout.write("\x1b[H\x1b[J" + "\n".join(lines) + "\n")
        sys.stdout.flush()

    def run(self):
        os.system("")  # enable ANSI escapes on Windows terminals
        threading.Thread(target=self._reader, daemon=True).start()
        if self.send_usb_cmd:
            time.sleep(0.2)
            self.send("u")  # switch board output to USB CDC
        last_count, last_t = 0, time.time()
        try:
            while self.running:
                self._render()
                for _ in range(6):  # ~15 Hz refresh, poll keys faster
                    ch = _getkey()
                    if ch:
                        self._handle_key(ch)
                    time.sleep(0.01)
                now = time.time()
                with self.lock:
                    self.rate = (self.msg_count - last_count) / (now - last_t)
                    last_count, last_t = self.msg_count, now
        except KeyboardInterrupt:
            pass
        finally:
            self.running = False
            if self.log_file:
                self.log_file.close()
            if self.send_usb_cmd:
                self.send("U")  # hand the stream back to the UART host
            self.ser.close()

    def _handle_key(self, ch):
        if ch == "q":
            self.running = False
        elif ch == "l":
            self.toggle_log()
        elif ch in "zmh":
            self.send({"z": "_", "m": "-", "h": "^"}[ch])
            self.events.append((time.time(), "sent", f"capture {'zero mid deep'.split()['zmh'.index(ch)]}", ""))
        elif ch in "crtoE1234567":
            cmd = {"c": "C", "r": "R", "t": "T", "o": "O", "E": "E"}.get(ch, ch)
            self.send(cmd)
            if ch.isdigit():
                self.send("O")  # the board does not announce a new octave count
            self.events.append((time.time(), "sent", f"command '{cmd}'", ""))


def find_bink_port():
    """Return the CDC serial port of a connected Bink keybed, or None.

    Matches on USB VID:PID first; falls back to the product string in case
    the PID ever changes with a different interface mix.
    """
    from serial.tools import list_ports

    candidates = []
    for p in list_ports.comports():
        if p.vid == BINK_VID and p.pid == BINK_PID:
            candidates.insert(0, p)  # exact match wins
        elif p.vid == BINK_VID or BINK_PRODUCT.lower() in (p.product or "").lower():
            candidates.append(p)
    if not candidates:
        return None
    p = candidates[0]
    print(f"found {p.product or 'Bink keybed'} on {p.device} "
          f"(VID:PID {p.vid:04X}:{p.pid:04X}, s/n {p.serial_number or '?'})")
    if len(candidates) > 1:
        others = ", ".join(c.device for c in candidates[1:])
        print(f"note: {len(candidates) - 1} more candidate(s) ignored: {others}")
    return p.device


def _getkey():
    if os.name == "nt":
        import msvcrt
        if msvcrt.kbhit():
            c = msvcrt.getwch()
            return c if c.isprintable() else None
        return None
    import select
    r, _, _ = select.select([sys.stdin], [], [], 0)
    return sys.stdin.read(1) if r else None


def plot_log(path, key_id):
    import matplotlib.pyplot as plt

    t_vals, vals, ons, offs = [], [], [], []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            if int(row["id"]) != key_id:
                continue
            t, v = float(row["t"]), int(row["value"])
            if row["type"] == "VAL":
                t_vals.append(t)
                vals.append(v)
            elif row["type"] == "ON":
                ons.append((t, v))
            elif row["type"] == "OFF":
                offs.append((t, v))

    if not t_vals and not ons:
        print(f"no data for key {key_id} in {path}")
        return

    fig, ax = plt.subplots(figsize=(12, 5))
    ax.step(t_vals, vals, where="post", lw=0.8, label=f"key {key_id} displacement")
    for i, (t, v) in enumerate(ons):
        ax.axvline(t, color="g", alpha=0.6)
        ax.annotate(f"ON v{v}", (t, ax.get_ylim()[1]), color="g",
                    rotation=90, va="top", fontsize=8)
    for t, v in offs:
        ax.axvline(t, color="r", alpha=0.6)
        ax.annotate(f"OFF v{v}", (t, 0), color="r", rotation=90, va="bottom", fontsize=8)
    ax.set_xlabel("time [s]")
    ax.set_ylabel("compensated displacement [counts]")
    ax.set_title(f"{path}, key {key_id}: {len(ons)} note-ons, {len(offs)} note-offs")
    ax.legend()
    plt.tight_layout()
    plt.show()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("port", nargs="?",
                    help="serial port (e.g. COM7) or 'plot'; omit to auto-detect by USB id")
    ap.add_argument("logfile", nargs="?", help="CSV file (plot mode)")
    ap.add_argument("--log", help="log all traffic to this CSV file")
    ap.add_argument("--uart", action="store_true",
                    help="passive: don't send the USB-mode command on connect")
    ap.add_argument("--id", type=int, default=0, help="key id to plot (plot mode)")
    args = ap.parse_args()

    if args.port == "plot":
        if not args.logfile:
            ap.error("plot mode needs a CSV file: bink_monitor.py plot run.csv --id N")
        plot_log(args.logfile, args.id)
        return

    port = args.port or find_bink_port()
    if not port:
        sys.exit(f"no Bink keybed found (looked for USB {BINK_VID:04X}:{BINK_PID:04X} "
                 f"/ product '{BINK_PRODUCT}'); pass a port explicitly, e.g. COM7")

    Monitor(port, log_path=args.log, send_usb_cmd=not args.uart).run()


if __name__ == "__main__":
    main()
