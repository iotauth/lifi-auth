#!/usr/bin/env python3
"""
plot_baud_sweep.py — Slide figure for Communication Performance / the
bandwidth-range tradeoff (paper Sec. V.C-D, Table IV/V). Values are the
published per-baud success rates from test_results/RESULTS_SUMMARY.md
(itself computed from test_results/rx_20260327_175020.log and
rx_20260327_182703.log — 50-packet samples per baud rate, ~15cm, white LED),
reproduced here as the table's own literal numbers, not re-derived.
"""
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

OUT = Path(__file__).resolve().parent / "baud_sweep.png"

SURFACE = "#fcfcfb"
INK_PRIMARY = "#0b0b0b"
INK_SECONDARY = "#52514e"
INK_MUTED = "#898781"
GRID = "#e1e0d9"
BASELINE = "#c3c2b7"
BLUE = "#2a78d6"
ORANGE = "#eb6834"
CRITICAL = "#d03b3b"

# From test_results/RESULTS_SUMMARY.md
RF_470K = {
    "label": "R_f = 470kΩ (2.0V swing)",
    "color": BLUE,
    "measured_baud": [9600, 50000, 100000, 150000, 200000, 250000, 300000],
    "measured_pct": [100, 100, 100, 100, 100, 100, 100],
    "collapse_baud": [350000],
    "ceiling": 300000,
}
RF_100K = {
    "label": "R_f = 100kΩ (400mV swing)",
    "color": ORANGE,
    "measured_baud": [9600, 50000, 100000, 150000, 200000, 250000, 300000, 350000],
    "measured_pct": [100, 100, 100, 100, 100, 100, 96, 86],
    "collapse_baud": [400000, 500000],
    "ceiling": 250000,
}


def main():
    fig, ax = plt.subplots(figsize=(13, 5.6), dpi=200, facecolor=SURFACE)
    fig.subplots_adjust(left=0.07, right=0.97, top=0.80, bottom=0.14)

    for cfg in (RF_470K, RF_100K):
        mb, mp = cfg["measured_baud"], cfg["measured_pct"]
        ax.plot(mb, mp, color=cfg["color"], linewidth=2.4, marker="o", markersize=7,
                markerfacecolor=cfg["color"], markeredgecolor=SURFACE, markeredgewidth=1.2,
                zorder=4, label=cfg["label"])
        # dashed bridge to first collapse point, then a critical marker at each collapse baud
        first_collapse = cfg["collapse_baud"][0]
        ax.plot([mb[-1], first_collapse], [mp[-1], 0], color=cfg["color"], linewidth=1.6,
                linestyle=(0, (3, 2)), zorder=2)
        for cb in cfg["collapse_baud"]:
            ax.scatter([cb], [0], marker="X", s=140, color=CRITICAL, edgecolors=INK_PRIMARY,
                       linewidths=1.0, zorder=5)
        ax.annotate("COLLAPSE\n(control msgs corrupted)" if cfg is RF_470K else "COLLAPSE",
                    xy=(first_collapse, 0), xytext=(first_collapse, 14),
                    ha="center", fontsize=8, color=INK_MUTED, style="italic")

        ax.axvline(cfg["ceiling"], color=cfg["color"], linestyle=(0, (1, 2)), linewidth=1.2,
                   alpha=0.55, zorder=1)

    ax.text(RF_470K["ceiling"], 104, "470kΩ clean ceiling\n300 kbaud", ha="center", va="bottom",
            fontsize=8.6, color=BLUE, fontweight="bold")
    ax.text(RF_100K["ceiling"], 104, "100kΩ clean ceiling\n250 kbaud", ha="right", va="bottom",
            fontsize=8.6, color=ORANGE, fontweight="bold")

    ax.set_xlim(0, 520000)
    ax.set_ylim(-8, 118)
    ax.set_xlabel("Baud rate", fontsize=10.5, color=INK_SECONDARY)
    ax.set_ylabel("Frame success rate (%, n=50/point)", fontsize=10.5, color=INK_SECONDARY)
    ax.set_title("Communication performance — TIA feedback resistor sweep",
                 fontsize=13.5, color=INK_PRIMARY, loc="left", fontweight="bold", pad=16)

    xticks = [9600, 100000, 200000, 300000, 400000, 500000]
    ax.set_xticks(xticks)
    ax.set_xticklabels([f"{x//1000}k" if x >= 1000 else str(x) for x in xticks])

    ax.grid(axis="y", color=GRID, linewidth=0.9, zorder=0)
    ax.set_axisbelow(True)
    for spine in ("top", "right"):
        ax.spines[spine].set_visible(False)
    ax.spines["left"].set_color(BASELINE)
    ax.spines["bottom"].set_color(BASELINE)
    ax.tick_params(colors=INK_MUTED, labelsize=9.5)

    ax.legend(loc="lower left", frameon=False, fontsize=9.8, labelcolor=INK_SECONDARY)

    fig.text(0.5, 0.965,
              "Higher gain (470kΩ) outperforms higher bandwidth (100kΩ) at 15cm — SNR margin, not TIA bandwidth, is the limiting factor at close range",
              ha="center", fontsize=10, color=INK_SECONDARY, style="italic")

    fig.savefig(OUT, facecolor=SURFACE)
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
