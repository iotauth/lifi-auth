#!/usr/bin/env python3
"""
liveness_monitor.py — passive CSV logger for the debug receiver's own
on-device presence decision (receiver_pico/src/main.c's presence_mark_valid()/
presence_check_decay(), see LIVENESS_WINDOW_MS).

Watches the receiver's USB serial output for two machine-parseable line
families that receiver_pico/src/main.c emits:

  "[EVT] msg=N type=0xXX crc=ok|fail replay=no|YES decrypt=ok|... text=\"...\""
      — per-frame detail, logged verbatim as a 'frame' row for context.

  "[PRESENCE] verified=true" / "[PRESENCE] verified=false stale_ms=N"
      — the receiver's own VERIFIED/REVOKED transitions, checked every
      main-loop iteration on-device. This script no longer computes the
      decay itself (that used to mirror app.py's pi4_health_monitor() over
      a --poll-interval, which is exactly the polling-cadence jitter the
      firmware port was meant to remove) — it just timestamps the board's
      decisions as they arrive.

Usage:
    python3 liveness_monitor.py [--port /dev/ttyACM0] [--baud 115200]

While running, type any line + Enter (e.g. "occlude", "unblock") to drop a
timestamped manual marker into the same log — useful for correlating human
actions (physically blocking/unblocking the beam) with the logged state
transitions during the occlusion experiment.

Output CSV schema (unix_ts, iso_ts, event, detail) is unchanged from the
previous version, so analyze_occlusion.py works against these logs as-is —
REVOKED rows still carry a "<age>s since last valid frame" detail string.
"""

import argparse
import csv
import os
import re
import sys
import threading
import time
from datetime import datetime

EVT_RE = re.compile(
    r'\[EVT\]\s+msg=(\d+)\s+type=0x([0-9A-Fa-f]+)\s+crc=(\w+)\s+replay=(\w+)\s+decrypt=(\w+)\s+text="([^"]*)"'
)
PRESENCE_RE = re.compile(
    r'\[PRESENCE\]\s+verified=(true|false)(?:\s+stale_ms=(\d+))?'
)


class LivenessMonitor:
    def __init__(self, log_path):
        self.log_path = log_path
        self.lock = threading.Lock()

        self._log_f = open(log_path, 'w', newline='')
        self._csv = csv.writer(self._log_f)
        self._csv.writerow(['unix_ts', 'iso_ts', 'event', 'detail'])
        self._log_f.flush()

    def _log(self, event, detail=''):
        now = time.time()
        iso = datetime.fromtimestamp(now).isoformat(timespec='milliseconds')
        with self.lock:
            self._csv.writerow([f'{now:.6f}', iso, event, detail])
            self._log_f.flush()
        print(f'[{iso}] {event} {detail}'.rstrip())

    def handle_evt_line(self, line):
        m = EVT_RE.search(line)
        if not m:
            return
        msg_num, msg_type, crc, replay, decrypt, text = m.groups()
        is_valid = (crc == 'ok' and replay == 'no' and decrypt == 'ok')
        self._log('frame', f'msg={msg_num} type=0x{msg_type} crc={crc} replay={replay} decrypt={decrypt} valid={is_valid} text="{text}"')

    def handle_presence_line(self, line):
        m = PRESENCE_RE.search(line)
        if not m:
            return
        verified, stale_ms = m.groups()
        if verified == 'true':
            self._log('VERIFIED', 'first valid frame after gap (on-device)')
        else:
            age_s = int(stale_ms) / 1000.0 if stale_ms is not None else float('nan')
            self._log('REVOKED', f'{age_s:.3f}s since last valid frame (on-device decision)')

    def marker_loop(self):
        for line in sys.stdin:
            line = line.strip()
            if line:
                self._log('MARKER', line)

    def close(self):
        self._log_f.close()


def open_serial_with_retry(port, baud):
    import serial
    while True:
        try:
            s = serial.Serial(port, baud, timeout=2)
            print(f'[liveness_monitor] Connected to {port} at {baud} baud.')
            return s
        except serial.SerialException as e:
            print(f'[liveness_monitor] Could not open {port}: {e} — retry in 2s...')
            time.sleep(2)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', default='/dev/ttyACM0', help='Debug receiver serial port (default: /dev/ttyACM0)')
    ap.add_argument('--baud', type=int, default=115200, help='Serial baud for the USB CDC link (cosmetic for RP2 CDC-ACM, default 115200)')
    ap.add_argument('--logdir', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'logs'),
                     help='Directory for the timestamped CSV log (default thesis_eval/logs)')
    ap.add_argument('--duration', type=float, default=None,
                     help='Auto-exit after this many seconds (default: run until Ctrl+C). '
                          'Useful for unattended benign-session data collection.')
    args = ap.parse_args()

    os.makedirs(args.logdir, exist_ok=True)
    session_ts = datetime.now().strftime('%Y%m%d_%H%M%S')
    log_path = os.path.join(args.logdir, f'liveness_{session_ts}.csv')

    mon = LivenessMonitor(log_path)
    print(f'[liveness_monitor] Logging to {log_path}')
    print('[liveness_monitor] Watching for on-device [PRESENCE] verified=true/false lines (no local decay computation).')
    print('[liveness_monitor] Type a line + Enter anytime to drop a timestamped marker (e.g. "occlude", "unblock").')

    threading.Thread(target=mon.marker_loop, daemon=True).start()

    if args.duration:
        mon._log('SESSION_START', f'auto-exit after {args.duration}s')
        deadline = time.time() + args.duration
    else:
        deadline = None

    conn = open_serial_with_retry(args.port, args.baud)
    try:
        while True:
            if deadline is not None and time.time() >= deadline:
                mon._log('SESSION_END', f'reached --duration={args.duration}s')
                break
            try:
                raw = conn.readline().decode('utf-8', errors='replace').rstrip()
            except Exception as e:
                print(f'[liveness_monitor] Serial read error: {e} — reconnecting...')
                conn = open_serial_with_retry(args.port, args.baud)
                continue
            if raw:
                mon.handle_evt_line(raw)
                mon.handle_presence_line(raw)
    except KeyboardInterrupt:
        pass
    finally:
        mon.close()
        print('\n[liveness_monitor] Exiting.')


if __name__ == '__main__':
    main()
