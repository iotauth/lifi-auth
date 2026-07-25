#!/usr/bin/env python3
"""
replay_test.py — automated G2 replay-resistance test against the debug
receiver (receiver_pico/src/main.c), driving its "replay"/"replaycap"
console commands. Implements thesis_plan.md's Experiment 3 procedure:

  1. Let one real (heartbeat) frame arrive — the firmware auto-snapshots it
     as `last_frame`, standing in for "capture one valid frame's raw bytes
     off the UART bridge."
  2. Send "replay" -> confirm immediate rejection (replay=YES).
  3. Set a small replaycap, let that many new legitimate frames cycle past
     to evict the captured nonce from the (now-small) window, then "replay"
     the original frame again -> confirm it's ACCEPTED — the documented
     boundary-wraparound limitation, not just the happy path.

Talks to the receiver over its serial console — must not be simultaneously
open elsewhere (dashboard RX panel, liveness_monitor.py, etc).
"""
import argparse
import csv
import os
import re
import sys
import time
from datetime import datetime

import serial

EVT_RE = re.compile(
    r'\[EVT\]\s+msg=(\d+)\s+type=0x([0-9A-Fa-f]+)\s+crc=(\w+)\s+replay=(\w+)\s+decrypt=(\w+)\s+text="([^"]*)"'
)


def send(conn, cmd):
    conn.write((cmd + '\n').encode('utf-8'))


def read_evt(conn, log_rows, timeout=15.0):
    """Read lines until the next [EVT] line or timeout; return its match groups or None."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        raw = conn.readline().decode('utf-8', errors='replace').rstrip()
        if not raw:
            continue
        print(f'  << {raw}')
        log_rows.append((time.time(), raw))
        m = EVT_RE.search(raw)
        if m:
            return m.groups()
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', default='/dev/ttyACM0')
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--boundary-cap', type=int, default=2,
                     help='replaycap value used for the wraparound test (default 2 — smallest useful window)')
    ap.add_argument('--logdir', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'logs'))
    args = ap.parse_args()

    os.makedirs(args.logdir, exist_ok=True)
    session_ts = datetime.now().strftime('%Y%m%d_%H%M%S')
    log_path = os.path.join(args.logdir, f'replay_test_{session_ts}.csv')
    log_rows = []

    conn = serial.Serial(args.port, args.baud, timeout=2)
    print(f'[replay_test] Connected to {args.port}')
    results = {}

    print('\n[Step 1] Waiting for a valid frame, then pinning it (frozen copy, immune to later auto-capture overwrites)...')
    evt = read_evt(conn, log_rows, timeout=15)
    if not evt or evt[4] != 'ok':
        print('FAIL: no valid frame observed to capture. Is the sender heartbeating and the key loaded on the receiver?')
        sys.exit(1)
    captured_msg = evt[0]
    send(conn, 'pin')
    time.sleep(0.2)
    print(f'Captured + pinned frame msg={captured_msg}.')

    print('\n[Step 2] Sending "replaypin" (immediate re-injection of the pinned frame)...')
    send(conn, 'replaypin')
    evt = read_evt(conn, log_rows, timeout=5)
    if evt and evt[3] == 'YES':
        print('PASS: immediate replay correctly REJECTED.')
        results['immediate_replay_rejected'] = True
    else:
        print(f'FAIL: expected replay=YES, got {evt}')
        results['immediate_replay_rejected'] = False

    cap = args.boundary_cap
    print(f'\n[Step 3] Setting replaycap {cap} and waiting for {cap} new legitimate frames to evict the pinned nonce...')
    send(conn, f'replaycap {cap}')
    time.sleep(0.5)
    for i in range(cap):
        evt = read_evt(conn, log_rows, timeout=15)
        if not evt:
            print('FAIL: did not observe expected legitimate traffic to cycle the window.')
            sys.exit(1)
        print(f'  cycled frame {i + 1}/{cap}: msg={evt[0]}')

    print(f'\nReplaying the PINNED frame (originally msg={captured_msg}) again '
          f'— should now be ACCEPTED (evicted from a {cap}-slot window)...')
    send(conn, 'replaypin')
    evt = read_evt(conn, log_rows, timeout=5)
    if evt and evt[3] == 'no':
        print(f'CONFIRMED LIMITATION: old nonce accepted again after {cap} newer frames evicted it from the window.')
        results['boundary_wraparound_reaccepted'] = True
    else:
        print(f'Unexpected: replay still rejected after {cap} cycles ({evt}) — window may not have wrapped as expected.')
        results['boundary_wraparound_reaccepted'] = False

    with open(log_path, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['unix_ts', 'raw_line'])
        for ts, line in log_rows:
            w.writerow([f'{ts:.6f}', line])
        w.writerow([])
        w.writerow(['result', 'value'])
        for k, v in results.items():
            w.writerow([k, v])

    print(f'\n=== SUMMARY (logged to {log_path}) ===')
    for k, v in results.items():
        print(f'{k}: {v}')


if __name__ == '__main__':
    main()
