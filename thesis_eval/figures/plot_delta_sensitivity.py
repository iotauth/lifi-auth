#!/usr/bin/env python3
"""
plot_delta_sensitivity.py — Slide figure for the Delta-sensitivity-to-benign-traffic
result (thesis_plan.md Experiment 2). Reads thesis_eval/logs/delta_tuning_20260725.json
(produced by delta_tuning.py) directly — gaps and per-candidate-Delta false-revoke
rates are not retyped.

The honest finding is that this session is degenerate (a mechanically regular
2.5s heartbeat, not irregular real traffic), so every candidate Delta clears
every gap. The figure's job is to make *why* visually obvious: all 959 measured
gaps sit in a tight spike far below every candidate Delta.
"""
import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

LOGS = Path(__file__).resolve().parent.parent / "logs"
OUT = Path(__file__).resolve().parent / "delta_sensitivity.png"

BLUE = "#2a78d6"
BLUE_FILL = "#b7d3f6"
SURFACE = "#fcfcfb"
INK_PRIMARY = "#0b0b0b"
INK_SECONDARY = "#52514e"
INK_MUTED = "#898781"
GRID = "#e1e0d9"
BASELINE = "#c3c2b7"
GOOD = "#0ca30c"


def main():
    d = json.loads((LOGS / "delta_tuning_20260725.json").read_text())
    gaps = np.array(d["gaps"])
    results = d["results"]

    fig, ax = plt.subplots(figsize=(12, 5.4), dpi=200, facecolor=SURFACE)
    fig.subplots_adjust(left=0.08, right=0.96, top=0.80, bottom=0.16)

    bins = np.logspace(np.log10(gaps.min() * 0.98), np.log10(35), 70)
    counts, _ = np.histogram(gaps, bins=bins)
    ax.hist(gaps, bins=bins, color=BLUE_FILL, edgecolor=BLUE, linewidth=0.8, zorder=3)
    ax.set_xscale("log")
    ax.set_ylim(0, counts.max() * 1.14)
    ax.set_xlim(gaps.min() * 0.95, 35)

    # Delta reference lines/labels use data-x, axes-fraction-y so they never
    # clip against the histogram's own scale (the peak bin holds 954/959 gaps).
    blended = ax.get_xaxis_transform()
    for r in results:
        delta = r["delta"]
        is_prod = delta == 15.0
        color = INK_PRIMARY if is_prod else INK_MUTED
        ax.axvline(delta, color=color, linestyle=(0, (4, 3)), linewidth=1.8 if is_prod else 1.2, zorder=2)
        label = f"Δ={delta:.0f}s" + ("  (production)" if is_prod else "")
        ax.text(delta, 0.86, label, rotation=90, transform=blended,
                ha="right", va="top", fontsize=9.3,
                color=INK_PRIMARY if is_prod else INK_SECONDARY,
                fontweight="bold" if is_prod else "normal")
        ax.text(delta, 0.895, f"{r['false_revoke_rate']*100:.1f}%", rotation=0, transform=blended,
                ha="center", va="bottom", fontsize=8.6, color=GOOD)
    ax.set_xlabel("Inter-frame gap between valid frames (s, log scale)", fontsize=10.5, color=INK_SECONDARY)
    ax.set_ylabel("Count (of 959 gaps)", fontsize=10.5, color=INK_SECONDARY)
    ax.set_title("Δ-sensitivity to benign traffic — 40-min heartbeat-only session",
                 fontsize=13, color=INK_PRIMARY, loc="left", pad=44, fontweight="bold")

    ax.grid(axis="y", color=GRID, linewidth=0.9, zorder=0)
    ax.set_axisbelow(True)
    for spine in ("top", "right"):
        ax.spines[spine].set_visible(False)
    ax.spines["left"].set_color(BASELINE)
    ax.spines["bottom"].set_color(BASELINE)
    ax.tick_params(colors=INK_MUTED, labelsize=9.5)

    ax.annotate("All 959 gaps cluster at 2.41–2.59s\n(98% within ±20ms of the 2.5s heartbeat)",
                xy=(2.6, counts.max() * 0.55), xytext=(6, counts.max() * 0.62),
                fontsize=9.5, color=INK_SECONDARY,
                arrowprops=dict(arrowstyle="-", color=INK_MUTED, linewidth=1))

    fig.text(0.5, 0.965, "Measured false-revoke rate = 0.0% at every candidate Δ tested (3–30s) — "
                          "but this session cannot rule out a smaller crossover under degraded conditions",
              ha="center", fontsize=10, color=INK_SECONDARY, style="italic")

    fig.savefig(OUT, facecolor=SURFACE)
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
