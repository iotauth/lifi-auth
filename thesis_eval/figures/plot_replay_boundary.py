#!/usr/bin/env python3
"""
plot_replay_boundary.py — Slide figure for the G2 replay test (thesis_plan.md
Experiment 3): immediate-replay rejection, then the documented window-boundary
limitation once the pinned nonce ages out of a deliberately shrunk 2-slot
window. Event timestamps are parsed directly from
thesis_eval/logs/replay_test_20260725_151700.csv (produced by replay_test.py),
not retyped.
"""
import csv
import re
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

LOGS = Path(__file__).resolve().parent.parent / "logs"
SRC = LOGS / "replay_test_20260725_151700.csv"
OUT = Path(__file__).resolve().parent / "replay_boundary.png"

SURFACE = "#fcfcfb"
INK_PRIMARY = "#0b0b0b"
INK_SECONDARY = "#52514e"
INK_MUTED = "#898781"
GRID = "#e1e0d9"
BASELINE = "#c3c2b7"
BLUE = "#2a78d6"
GOOD = "#0ca30c"
WARNING = "#fab219"

EVT_RE = re.compile(r'\[EVT\] msg=(\d+) type=0x(\w+) crc=(\w+) replay=(\w+) decrypt=(\w+)')


def load_events():
    rows = []
    with open(SRC, newline="") as f:
        for row in csv.reader(f):
            if len(row) != 2:
                continue
            ts, line = row
            try:
                ts = float(ts)
            except ValueError:
                continue
            m = EVT_RE.search(line)
            if m:
                rows.append((ts, *m.groups()))
            elif "Pinned frame" in line:
                rows.append((ts, "PIN", None, None, None, None))
            elif "capacity set to" in line:
                rows.append((ts, "CAPSET", None, None, None, None))
    return rows


def main():
    events = load_events()
    t0 = events[0][0]

    # Hand-pick the events that tell the story, in order, with display metadata.
    def find(msg_tag, want):
        for ts, msg, *_rest in events:
            if msg == msg_tag and (want is None or _rest == want):
                return ts - t0
        return None

    # (label, color, marker, side, height, fontsize, bold) — side/height assigned
    # explicitly (not alternated) so the two closely-timed early events (frame
    # received @ t+0.00 and window-cap-set @ t+0.71) stack instead of colliding.
    picks = []
    for ts, msg, mtype, crc, replay, decrypt in events:
        if msg == "51" and replay == "no" and decrypt == "ok" and not picks:
            picks.append((ts - t0, "Frame #51 received\n(heartbeat, captured+pinned)", BLUE, "o", 1, 0.45, 9.3, False))
        elif msg == "51" and replay == "YES":
            picks.append((ts - t0, "Replay of #51 →\nREJECTED (nonce seen)", GOOD, "P", -1, 0.55, 9.6, True))
        elif msg == "CAPSET":
            picks.append((ts - t0, "window cap → 2", INK_MUTED, "D", 1, 0.78, 8.2, False))
        elif msg == "52" and replay == "no":
            picks.append((ts - t0, "Frame #52 arrives\n(cycles window 1/2)", BLUE, "o", 1, 0.45, 9.3, False))
        elif msg == "53" and replay == "no" and len(picks) == 4:
            picks.append((ts - t0, "Frame #53 arrives\n(cycles window 2/2 —\n#51's nonce evicted)", BLUE, "o", 1, 0.45, 9.3, False))
        elif msg == "53" and replay == "no" and len(picks) == 5:
            picks.append((ts - t0, "Replay of #51 again →\nACCEPTED (boundary limitation)", WARNING, "X", -1, 0.55, 9.6, True))

    fig, ax = plt.subplots(figsize=(13, 5.2), dpi=200, facecolor=SURFACE)
    fig.subplots_adjust(left=0.04, right=0.97, top=0.78, bottom=0.10)

    tmax = picks[-1][0]
    ax.plot([-0.2, tmax + 0.3], [0, 0], color=BASELINE, linewidth=1.6, zorder=1)

    for t, label, color, marker, side, height, fontsize, bold in picks:
        ax.plot([t, t], [0, height * side], color=GRID, linewidth=1.3, zorder=1)
        ax.scatter([t], [0], s=220, color=color, edgecolors=INK_PRIMARY, linewidths=1.3,
                   marker=marker, zorder=4)
        ax.text(t, height * side + 0.05 * side, label, ha="center",
                va="bottom" if side > 0 else "top",
                fontsize=fontsize, color=INK_PRIMARY, fontweight="bold" if bold else "normal",
                linespacing=1.4)
        ax.text(t, -0.08 if side > 0 else 0.08, f"t+{t:.2f}s", ha="center",
                va="top" if side > 0 else "bottom", fontsize=8, color=INK_MUTED)

    ax.set_xlim(-0.3, tmax + 0.5)
    ax.set_ylim(-1.05, 1.05)
    ax.axis("off")

    ax.text(0.0, 1.02, "G2 — Replay rejection, then the documented window-boundary limitation",
            fontsize=13.5, color=INK_PRIMARY, fontweight="bold", transform=ax.transAxes)
    ax.text(0.0, 0.94, "Single trial, receiver_pico debug platform — nonce window shrunk to 2 slots to make the boundary observable quickly",
            fontsize=9.8, color=INK_MUTED, transform=ax.transAxes)

    # legend
    handles = [
        plt.Line2D([0], [0], marker="o", color="none", markerfacecolor=BLUE, markeredgecolor=INK_PRIMARY, markersize=11, label="Legitimate frame"),
        plt.Line2D([0], [0], marker="P", color="none", markerfacecolor=GOOD, markeredgecolor=INK_PRIMARY, markersize=11, label="Replay rejected (G2 holds)"),
        plt.Line2D([0], [0], marker="X", color="none", markerfacecolor=WARNING, markeredgecolor=INK_PRIMARY, markersize=11, label="Replay accepted (boundary limitation)"),
    ]
    ax.legend(handles=handles, loc="lower center", bbox_to_anchor=(0.5, -0.12), ncol=3,
              frameon=False, fontsize=9.5, labelcolor=INK_SECONDARY)

    fig.savefig(OUT, facecolor=SURFACE)
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
