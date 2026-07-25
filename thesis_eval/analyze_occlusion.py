#!/usr/bin/env python3
"""
analyze_occlusion.py — pulls time-to-revoke / time-to-reverify stats out of a
liveness_monitor.py CSV log (thesis_plan.md Experiment 1's deliverable).

time-to-revoke: read directly from each REVOKED row's own logged age
    ("<age>s since last valid frame") — liveness_monitor.py already computes
    this as (revoke_timestamp - last_valid_frame_time), which is effectively
    time-since-occlusion (frames stop the instant the beam is blocked).

time-to-reverify: (next VERIFIED timestamp) - (preceding "unblock" MARKER
    timestamp) — depends on human reaction time typing the marker, so this
    is an upper bound on the true reverify latency, not a lower one.

Usage:
    python3 analyze_occlusion.py thesis_eval/logs/liveness_<ts>.csv
"""
import csv
import re
import statistics
import sys

REVOKE_AGE_RE = re.compile(r'([\d.]+)s since last valid frame')


def main():
    if len(sys.argv) != 2:
        print(f'Usage: {sys.argv[0]} <liveness_csv_path>')
        sys.exit(1)

    rows = []
    with open(sys.argv[1], newline='') as f:
        for r in csv.DictReader(f):
            rows.append(r)

    revoke_latencies = []
    for r in rows:
        if r['event'] == 'REVOKED':
            m = REVOKE_AGE_RE.search(r['detail'])
            if m:
                revoke_latencies.append(float(m.group(1)))

    # Reverify latency: for each 'unblock' marker, find the next VERIFIED row.
    reverify_latencies = []
    pending_unblock_ts = None
    for r in rows:
        if r['event'] == 'MARKER' and r['detail'].strip().lower() == 'unblock':
            pending_unblock_ts = float(r['unix_ts'])
        elif r['event'] == 'VERIFIED' and pending_unblock_ts is not None:
            reverify_latencies.append(float(r['unix_ts']) - pending_unblock_ts)
            pending_unblock_ts = None

    n_revoked = sum(1 for r in rows if r['event'] == 'REVOKED')
    n_verified = sum(1 for r in rows if r['event'] == 'VERIFIED')

    print(f'Log: {sys.argv[1]}')
    print(f'Total rows: {len(rows)}  |  REVOKED events: {n_revoked}  |  VERIFIED events: {n_verified}')
    print()

    def report(name, vals):
        if not vals:
            print(f'{name}: no data')
            return
        print(f'{name} (n={len(vals)}):')
        print(f'  mean   = {statistics.mean(vals):.3f}s')
        print(f'  stdev  = {statistics.stdev(vals):.3f}s' if len(vals) > 1 else '  stdev  = n/a (only 1 sample)')
        print(f'  min/max= {min(vals):.3f}s / {max(vals):.3f}s')
        print(f'  values = {[round(v, 3) for v in vals]}')
        print()

    report('Time-to-revoke', revoke_latencies)
    report('Time-to-reverify (from "unblock" marker to VERIFIED)', reverify_latencies)


if __name__ == '__main__':
    main()
