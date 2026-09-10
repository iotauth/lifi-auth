#!/usr/bin/env python3
"""
plot_cone_geometry.py — Slide figure characterizing the sender LED / receiver
photodiode alignment tolerance: lateral offset (in) tolerated at a given
axial distance (in) before the receiver falls outside the LED's emission
cone. Six manually measured (distance, offset) pairs, fit two ways:
  1. free-intercept least squares (accounts for finite emitter/aperture size)
  2. forced-through-origin least squares (the naive point-source assumption)
The free-intercept fit is the honest model; the origin-forced fit is shown
only as the naive baseline comparison, with its systematic residual drift
called out explicitly.
"""
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

OUT = Path(__file__).resolve().parent / "cone_geometry.png"

BLUE = "#2a78d6"
ORANGE = "#eb6834"
SURFACE = "#fcfcfb"
INK_PRIMARY = "#0b0b0b"
INK_SECONDARY = "#52514e"
INK_MUTED = "#898781"
GRID = "#e1e0d9"
BASELINE = "#c3c2b7"

distance = np.array([6, 10, 12, 16, 20, 21], dtype=float)
offset = np.array([1.0, 1.25, 1.45, 1.75, 2.0, 2.1], dtype=float)


def main():
    m_free, b_free = np.polyfit(distance, offset, 1)
    pred_free = m_free * distance + b_free
    ss_res = np.sum((offset - pred_free) ** 2)
    ss_tot = np.sum((offset - offset.mean()) ** 2)
    r2_free = 1 - ss_res / ss_tot

    m_origin = np.sum(distance * offset) / np.sum(distance ** 2)
    pred_origin = m_origin * distance
    ss_res_o = np.sum((offset - pred_origin) ** 2)
    r2_origin = 1 - ss_res_o / np.sum(offset ** 2)

    half_angle_free = np.degrees(np.arctan(m_free))
    half_angle_origin = np.degrees(np.arctan(m_origin))

    fig, ax = plt.subplots(figsize=(12, 5.4), dpi=200, facecolor=SURFACE)
    fig.subplots_adjust(left=0.08, right=0.96, top=0.80, bottom=0.14)

    x_line = np.linspace(0, 23, 100)
    ax.plot(x_line, m_free * x_line + b_free, color=BLUE, linewidth=2.2, zorder=3,
            label=f"Free intercept: y = {m_free:.3f}x + {b_free:.2f}  (R²={r2_free:.3f}, half-angle {half_angle_free:.1f}°)")
    ax.plot(x_line, m_origin * x_line, color=ORANGE, linewidth=1.8, linestyle=(0, (5, 3)), zorder=2,
            label=f"Forced through origin: y = {m_origin:.3f}x  (half-angle {half_angle_origin:.1f}°)")
    ax.scatter(distance, offset, color=INK_PRIMARY, s=55, zorder=4, label="Measured (n=6)")

    ax.set_xlim(0, 23)
    ax.set_ylim(0, 2.5)
    ax.set_xlabel("Distance from LED (in)", fontsize=10.5, color=INK_SECONDARY)
    ax.set_ylabel("Lateral offset to cone edge (in)", fontsize=10.5, color=INK_SECONDARY)
    ax.set_title("LED / receiver alignment cone — measured lateral tolerance vs. distance",
                 fontsize=13, color=INK_PRIMARY, loc="left", pad=44, fontweight="bold")

    ax.grid(color=GRID, linewidth=0.9, zorder=0)
    ax.set_axisbelow(True)
    for spine in ("top", "right"):
        ax.spines[spine].set_visible(False)
    ax.spines["left"].set_color(BASELINE)
    ax.spines["bottom"].set_color(BASELINE)
    ax.tick_params(colors=INK_MUTED, labelsize=9.5)
    ax.legend(loc="upper left", fontsize=9.3, frameon=False)

    fig.text(0.5, 0.965, f"Free-intercept fit (R²={r2_free:.3f}) is the honest model — the origin-forced fit "
                          "overstates the cone angle and its residuals drift from + to -",
              ha="center", fontsize=10, color=INK_SECONDARY, style="italic")

    fig.savefig(OUT, facecolor=SURFACE)
    print(f"wrote {OUT}")
    print(f"free-intercept: slope={m_free:.4f} intercept={b_free:.4f} R2={r2_free:.4f} half_angle={half_angle_free:.2f}deg")
    print(f"origin-forced:  slope={m_origin:.4f} R2(uncentered)={r2_origin:.4f} half_angle={half_angle_origin:.2f}deg")


if __name__ == "__main__":
    main()
