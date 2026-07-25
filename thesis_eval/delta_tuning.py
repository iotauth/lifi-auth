#!/usr/bin/env python3
"""
delta_tuning.py — Δ (liveness-window) tuning analysis, thesis_plan.md
Experiment 2.

Takes a benign-session liveness_monitor.py CSV log (no occlusion — just
normal operation, ideally with some varied alignment/ambient light over
30-60 min) and:

1. Extracts gaps between consecutive valid ("frame ... valid=True") events.
2. For each candidate Δ, computes the false-revoke rate = fraction of
   benign gaps that exceed Δ (i.e. how often normal operation would have
   been wrongly revoked at that window size).
3. Reports the attacker-dwell-time cost per Δ using the *measured*
   relationship from Experiment 1 (intrinsic revoke latency ≈ Δ + fixed
   overhead) rather than re-running physical occlusion trials at every
   candidate Δ — the fixed overhead is taken from the tight-poll-interval
   occlusion batch (thesis_eval/analyze_occlusion.py output), where
   time-to-revoke was measured at ~15.11s for Δ=15s, i.e. ~0.11s overhead.

Usage:
    python3 delta_tuning.py <benign_session.csv> [--overhead 0.11]
                             [--deltas 3,5,10,15,20,30]
"""
import argparse
import csv
import re
import sys

FRAME_RE = re.compile(r'msg=(\d+).*valid=(True|False)')


def load_valid_frame_gaps(csv_path):
    timestamps = []
    with open(csv_path, newline='') as f:
        for row in csv.DictReader(f):
            if row['event'] != 'frame':
                continue
            m = FRAME_RE.search(row['detail'])
            if m and m.group(2) == 'True':
                timestamps.append(float(row['unix_ts']))
    timestamps.sort()
    gaps = [b - a for a, b in zip(timestamps, timestamps[1:])]
    return timestamps, gaps


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('csv_path', help='Benign-session liveness_monitor.py CSV log')
    ap.add_argument('--overhead', type=float, default=0.11,
                     help='Fixed revoke-detection overhead in seconds, from the tight-poll occlusion batch (default 0.11s)')
    ap.add_argument('--deltas', default='3,5,10,15,20,30',
                     help='Comma-separated candidate Δ values in seconds (default 3,5,10,15,20,30)')
    ap.add_argument('--out-json', default=None, help='Optional path to also dump the results table as JSON')
    args = ap.parse_args()

    timestamps, gaps = load_valid_frame_gaps(args.csv_path)
    if not gaps:
        print('No valid-frame gaps found — is this a benign session log with real traffic?')
        sys.exit(1)

    deltas = [float(x) for x in args.deltas.split(',')]

    n = len(gaps)
    print(f'Benign session: {len(timestamps)} valid frames, {n} gaps, '
          f'spanning {timestamps[-1] - timestamps[0]:.1f}s (~{(timestamps[-1] - timestamps[0]) / 60:.1f} min)')
    print(f'Gap stats: min={min(gaps):.3f}s  max={max(gaps):.3f}s  mean={sum(gaps) / n:.3f}s')
    print()
    print(f'{"Delta(s)":>9} | {"false_revoke_rate":>18} | {"gaps_exceeding":>14} | {"attacker_dwell_cost(s)":>22}')
    print('-' * 72)

    results = []
    for d in sorted(deltas):
        exceeding = sum(1 for g in gaps if g > d)
        rate = exceeding / n
        cost = d + args.overhead
        results.append({'delta': d, 'false_revoke_rate': rate, 'gaps_exceeding': exceeding,
                         'n_gaps': n, 'attacker_dwell_cost': cost})
        print(f'{d:>9.1f} | {rate:>18.5f} | {exceeding:>14d} | {cost:>22.3f}')

    if args.out_json:
        import json
        with open(args.out_json, 'w') as f:
            json.dump({'gaps': gaps, 'results': results}, f, indent=2)
        print(f'\nWrote {args.out_json}')


if __name__ == '__main__':
    main()
