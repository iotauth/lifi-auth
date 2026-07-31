#!/usr/bin/env python3
"""
plot_revocation_latency.py — Slide figure for the G4 occlusion/shadowing test.

Reads the two analysis outputs produced by analyze_occlusion.py
(as-deployed 5.0s poll vs fine-grained 0.2s poll batches) and renders a
strip-plot comparison of time-to-revoke and time-to-reverify, with a
reference line at the production Delta = 15s and the heartbeat interval
2.5s. Source numbers are parsed directly from the *_analysis.txt files,
not retyped, so the figure always matches thesis_eval/logs/.
"""
import re
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

LOGS = Path(__file__).resolve().parent.parent / "logs"
OUT = Path(__file__).resolve().parent / "revocation_latency.png"

# --- palette (validated categorical order, see dataviz skill) ---
BLUE = "#2a78d6"
ORANGE = "#eb6834"
SURFACE = "#fcfcfb"
INK_PRIMARY = "#0b0b0b"
INK_SECONDARY = "#52514e"
INK_MUTED = "#898781"
GRID = "#e1e0d9"
BASELINE = "#c3c2b7"

CONDITIONS = [
    ("As-deployed (5.0s poll), n=21", LOGS / "liveness_20260725_144547_poll5.0_win15.0_analysis.txt", BLUE),
    ("Fine-grained (0.2s poll), n=20", LOGS / "liveness_20260725_145930_poll0.2_win15.0_analysis.txt", ORANGE),
]


def parse_block(text, header):
    m = re.search(header + r".*?mean\s*=\s*([\d.]+)s\s*\n\s*stdev\s*=\s*([\d.]+)s.*?values = \[(.*?)\]",
                   text, re.S)
    mean, stdev, raw_vals = float(m.group(1)), float(m.group(2)), m.group(3)
    vals = [float(v) for v in raw_vals.split(",")]
    return np.array(vals), mean, stdev


def load(path):
    text = path.read_text()
    revoke_vals, revoke_mean, revoke_std = parse_block(text, r"Time-to-revoke \(n=\d+\):")
    reverify_vals, reverify_mean, reverify_std = parse_block(text, r"Time-to-reverify.*?\(n=\d+\):")
    return dict(revoke=revoke_vals, revoke_mean=revoke_mean, revoke_std=revoke_std,
                reverify=reverify_vals, reverify_mean=reverify_mean, reverify_std=reverify_std)


def strip_plot(ax, panel_key, mean_key, std_key, xref, xref_label, xlabel, title, xmax):
    rng = np.random.default_rng(7)
    y_positions = [1, 0]
    for (label, path, color), y in zip(CONDITIONS, y_positions):
        d = load(path)
        vals = d[panel_key]
        jitter = rng.uniform(-0.16, 0.16, size=len(vals))
        ax.scatter(vals, np.full_like(vals, y) + jitter, s=26, color=color,
                   alpha=0.75, linewidths=0, zorder=3)
        mean, std = d[mean_key], d[std_key]
        ax.scatter([mean], [y], marker="D", s=70, color=color, edgecolors=INK_PRIMARY,
                   linewidths=1.1, zorder=4)
        ax.text(mean, y + 0.34, f"μ={mean:.2f}s ±{std:.2f}s",
                ha="center", va="bottom", fontsize=9.5, color=INK_SECONDARY)

    ax.axvline(xref, color=INK_MUTED, linestyle=(0, (4, 3)), linewidth=1.4, zorder=1)
    ax.text(xref, 1.62, xref_label, ha="center", va="bottom", fontsize=9.5,
            color=INK_SECONDARY, fontweight="bold")

    ax.set_yticks(y_positions)
    ax.tick_params(axis="y", labelleft=False, length=0)
    ax.set_xlabel(xlabel, fontsize=10.5, color=INK_SECONDARY)
    ax.set_title(title, fontsize=12.5, color=INK_PRIMARY, loc="left", pad=14, fontweight="bold")
    ax.set_xlim(0, xmax)
    ax.set_ylim(-0.6, 1.9)
    ax.grid(axis="x", color=GRID, linewidth=0.9, zorder=0)
    ax.set_axisbelow(True)
    for spine in ("top", "right", "left"):
        ax.spines[spine].set_visible(False)
    ax.spines["bottom"].set_color(BASELINE)
    ax.tick_params(axis="x", colors=INK_MUTED, labelsize=9.5)


def main():
    fig, axes = plt.subplots(1, 2, figsize=(13, 4.6), dpi=200, facecolor=SURFACE)
    fig.subplots_adjust(left=0.20, right=0.97, top=0.80, bottom=0.16, wspace=0.35)

    strip_plot(axes[0], "revoke", "revoke_mean", "revoke_std",
               xref=15.0, xref_label="Δ = 15s (production window)",
               xlabel="Time to revoke (s)",
               title="G4 — Time-to-revoke on occlusion", xmax=20)

    strip_plot(axes[1], "reverify", "reverify_mean", "reverify_std",
               xref=2.5, xref_label="2.5s heartbeat interval",
               xlabel="Time to re-verify (s)",
               title="Time-to-reverify on unblock", xmax=4.5)

    fig.suptitle("Occlusion (shadowing) test — 21 + 20 trials, receiver_pico debug platform",
                 fontsize=11, color=INK_MUTED, x=0.20, ha="left", y=0.95)

    fig.text(0.02, 0.02,
              "Bimodal as-deployed spread (≈ 15.7s / ≈ 18.2s) is 5s-poll aliasing, not a protocol property — "
              "fine-grained polling collapses to Δ + ≈ 0.11s fixed overhead.",
              fontsize=8.3, color=INK_MUTED, style="italic")

    handles = [plt.Line2D([0], [0], marker="o", color="none", markerfacecolor=c, markersize=8, label=l)
               for l, _, c in CONDITIONS]
    fig.legend(handles=handles, loc="upper right", bbox_to_anchor=(0.97, 1.0), ncol=1,
               frameon=False, fontsize=9.5, labelcolor=INK_SECONDARY)

    fig.savefig(OUT, facecolor=SURFACE)
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
