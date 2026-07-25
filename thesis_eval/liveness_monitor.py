#!/usr/bin/env python3
"""
liveness_monitor.py — standalone liveness/revocation state machine for the
debug receiver (receiver_pico/src/main.c), mirroring pi4_health_monitor() /
_set_mac_key_verified() in sender/dashboard/app.py so results are directly
comparable if this logic is later ported to the real Pi4 path.

Watches the receiver's USB serial output for the machine-parseable
"[EVT] msg=N type=0xXX crc=ok|fail replay=no|YES decrypt=ok|... text=\"...\""
lines that receiver_pico/src/main.c emits per frame (see process_complete_frame()).

A frame counts as "valid" (refreshes the liveness clock) only if:
  crc=ok AND replay=no AND decrypt=ok
— i.e. it actually decrypted with the current key, matching what an HMAC
match on /pi4_frame proves on the real system.

State-change logging mirrors _set_mac_key_verified()'s "only log on actual
change" guard exactly, so a steady stream of valid frames doesn't spam the
log every poll tick.

Usage:
    python3 liveness_monitor.py [--port /dev/ttyACM0] [--baud 115200]
                                 [--poll-interval 5.0] [--liveness-window 15.0]

While running, type any line + Enter (e.g. "occlude", "unblock") to drop a
timestamped manual marker into the same log — useful for correlating human
actions (physically blocking/unblocking the beam) with the logged state
transitions during the occlusion experiment.
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


class LivenessMonitor:
    def __init__(self, port, baud, poll_interval, liveness_window, log_path):
        self.port = port
        self.baud = baud
        self.poll_interval = poll_interval
        self.liveness_window = liveness_window
        self.log_path = log_path

        self.lock = threading.Lock()
        self.verified = False
        self.last_valid_frame_time = None
        self.running = True

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

        self._log('frame', f'msg={msg_num} type=0x{msg_type} crc={crc} replay={replay} decrypt={decrypt} valid={is_valid}')

        if not is_valid:
            return

        with self.lock:
            self.last_valid_frame_time = time.time()
            changed = not self.verified
            self.verified = True
        if changed:
            self._log('VERIFIED', f'first valid frame after gap (msg={msg_num}, text="{text}")')

    def decay_loop(self):
        while self.running:
            time.sleep(self.poll_interval)
            with self.lock:
                verified = self.verified
                last = self.last_valid_frame_time
            if verified and last is not None:
                age = time.time() - last
                if age > self.liveness_window:
                    with self.lock:
                        # re-check under lock in case a frame arrived between
                        # the read above and now
                        if self.verified and self.last_valid_frame_time == last:
                            self.verified = False
                            changed = True
                        else:
                            changed = False
                    if changed:
                        self._log('REVOKED', f'{age:.3f}s since last valid frame (window={self.liveness_window}s)')

    def marker_loop(self):
        for line in sys.stdin:
            line = line.strip()
            if line:
                self._log('MARKER', line)

    def close(self):
        self.running = False
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
    ap.add_argument('--poll-interval', type=float, default=5.0,
                     help='Decay-check cadence in seconds (default 5.0, matches pi4_health_monitor exactly)')
    ap.add_argument('--liveness-window', type=float, default=15.0,
                     help='Seconds without a valid frame before revoking (default 15.0, matches LIVENESS_WINDOW_S)')
    ap.add_argument('--logdir', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'logs'),
                     help='Directory for the timestamped CSV log (default thesis_eval/logs)')
    ap.add_argument('--duration', type=float, default=None,
                     help='Auto-exit after this many seconds (default: run until Ctrl+C). '
                          'Useful for unattended benign-session data collection.')
    args = ap.parse_args()

    os.makedirs(args.logdir, exist_ok=True)
    session_ts = datetime.now().strftime('%Y%m%d_%H%M%S')
    log_path = os.path.join(args.logdir, f'liveness_{session_ts}.csv')

    mon = LivenessMonitor(args.port, args.baud, args.poll_interval, args.liveness_window, log_path)
    print(f'[liveness_monitor] Logging to {log_path}')
    print(f'[liveness_monitor] poll_interval={args.poll_interval}s liveness_window={args.liveness_window}s')
    print('[liveness_monitor] Type a line + Enter anytime to drop a timestamped marker (e.g. "occlude", "unblock").')

    threading.Thread(target=mon.decay_loop, daemon=True).start()
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
    except KeyboardInterrupt:
        pass
    finally:
        mon.close()
        print('\n[liveness_monitor] Exiting.')


if __name__ == '__main__':
    main()
