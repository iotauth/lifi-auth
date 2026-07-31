#!/usr/bin/env python3
"""
plot_system_block_diagram.py — System architecture slide figure.

Top row matches the paper's System Model (Sec. IV.A) exactly: Sender,
Receiver, Verifier, and the two channel types (optical vs TLS) that the
threat model is built on. Bottom row is the real reporting-plane wiring
used in the deployed prototype (Pi4 dash_receiver.c -> WiFi/HMAC ->
dashboard) — shown de-emphasized/dashed because it is implementation,
not part of the security boundary the thesis argues about.
"""
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch, Rectangle
from matplotlib.lines import Line2D

OUT = Path(__file__).resolve().parent / "system_block_diagram.png"

SURFACE = "#fcfcfb"
INK_PRIMARY = "#0b0b0b"
INK_SECONDARY = "#52514e"
INK_MUTED = "#898781"
GRID = "#e1e0d9"

BLUE = "#2a78d6"
AQUA = "#1baf7a"
VIOLET = "#4a3aa7"
ORANGE = "#eb6834"


def box(ax, cx, cy, w, h, title, subtitle, border, fill=SURFACE, lw=2.0, title_color=None):
    b = FancyBboxPatch((cx - w / 2, cy - h / 2), w, h,
                        boxstyle="round,pad=0.02,rounding_size=0.10",
                        linewidth=lw, edgecolor=border, facecolor=fill, zorder=3)
    ax.add_patch(b)
    ax.text(cx, cy + h * 0.16, title, ha="center", va="center", fontsize=11.5,
            fontweight="bold", color=title_color or INK_PRIMARY, zorder=4)
    ax.text(cx, cy - h * 0.24, subtitle, ha="center", va="center", fontsize=9.0,
            color=INK_SECONDARY, zorder=4, linespacing=1.6)


def arrow(ax, p0, p1, color, style="-", lw=2.0, mutation=14, connectionstyle="arc3,rad=0.0"):
    a = FancyArrowPatch(p0, p1, arrowstyle="-|>", mutation_scale=mutation,
                         linewidth=lw, linestyle=style, color=color, zorder=2,
                         connectionstyle=connectionstyle, shrinkA=2, shrinkB=2)
    ax.add_patch(a)


def main():
    fig, ax = plt.subplots(figsize=(13, 7.6), dpi=200, facecolor=SURFACE)
    ax.set_xlim(0, 13)
    ax.set_ylim(0, 9.6)
    ax.axis("off")
    fig.subplots_adjust(left=0.01, right=0.99, top=0.99, bottom=0.01)

    ax.text(0.3, 9.25, "System Architecture", fontsize=17, fontweight="bold", color=INK_PRIMARY)
    ax.text(0.3, 8.82, "Three participants of the security model (top) and the deployed reporting path (bottom)",
            fontsize=10.5, color=INK_MUTED)

    # ---- Verifier (top center) ----
    vx, vy, vw, vh = 6.5, 8.0, 4.6, 1.15
    box(ax, vx, vy, vw, vh,
        "Key Provisioning Service (Verifier)",
        "IoTAuth Server · TLS + X.509 mutual auth",
        border=VIOLET)

    # ---- Sender / Receiver (security model row) ----
    sx, sy, sw, sh = 2.6, 5.4, 4.1, 1.9
    rx, ry, rw, rh = 10.4, 5.4, 4.1, 1.9

    box(ax, sx, sy, sw, sh,
        "Optical Beacon (Sender)",
        "Pico H · RP2040\nAES-128-GCM encrypt + nonce + frame\nPIO 4-channel LED TX (1 Mbps)",
        border=BLUE)

    box(ax, rx, ry, rw, rh,
        "Client Device (Receiver)",
        "Pico 2 H · RP2350\nPhotodiode + TIA + comparator\n64-entry nonce window, GCM verify",
        border=AQUA)

    # Linux host intermediary for sender-side key delivery
    hx, hy, hw, hh = 2.6, 7.0, 3.0, 0.85
    box(ax, hx, hy, hw, hh, "Linux Host", "runs keys_receiver",
        border=INK_MUTED, lw=1.4)

    # Verifier -> Linux host (TLS) -> Sender (UART)
    arrow(ax, (vx - vw / 2 + 0.3, vy - vh / 2), (hx + 0.2, hy + hh / 2 + 0.05), BLUE, lw=1.6,
          connectionstyle="arc3,rad=-0.15")
    ax.text(4.55, 7.85, "TLS + X.509", fontsize=8.6, color=INK_SECONDARY, ha="center", style="italic")
    arrow(ax, (hx, hy - hh / 2), (sx, sy + sh / 2), INK_MUTED, lw=1.6)
    ax.text(2.85, 6.2, "UART\n(key delivery)", fontsize=8.6, color=INK_SECONDARY, ha="left", style="italic")

    # Verifier -> Receiver directly (TLS, optical key-ID rendezvous mode)
    arrow(ax, (vx + vw / 2 - 0.3, vy - vh / 2), (rx - 0.2, ry + rh / 2), BLUE, lw=1.6,
          connectionstyle="arc3,rad=0.15")
    ax.text(9.35, 6.9, "TLS + X.509\n(key-ID rendezvous\nfetches session key)",
            fontsize=8.6, color=INK_SECONDARY, ha="center", style="italic")

    # Sender -> Receiver optical channel (the headline arrow)
    arrow(ax, (sx + sw / 2 + 0.05, sy), (rx - rw / 2 - 0.05, ry), ORANGE, lw=3.0, mutation=20)
    ax.text((sx + rx) / 2, sy + 0.62, "LiFi optical link — one-way",
            fontsize=11, color=ORANGE, ha="center", fontweight="bold")
    ax.text((sx + rx) / 2, sy + 0.30, "AES-128-GCM encrypted frames, 64-nonce replay window",
            fontsize=8.8, color=INK_SECONDARY, ha="center")
    ax.text((sx + rx) / 2, sy - 0.55, "Δ = 15s freshness window (G1) — no round-trip timing check (G3)",
            fontsize=8.3, color=INK_MUTED, ha="center", style="italic")

    # ---- Divider ----
    ax.plot([0.3, 12.7], [3.55, 3.55], color=GRID, linewidth=1.2, zorder=1)
    ax.text(0.3, 3.35, "IMPLEMENTATION / REPORTING PLANE  —  not part of the security boundary above",
            fontsize=8.8, color=INK_MUTED, fontweight="bold")

    # ---- Reporting plane (bottom row) ----
    py = 1.9
    p1x, p1w = 2.6, 2.9
    p2x, p2w = 6.5, 3.1
    p3x, p3w = 10.4, 2.9
    ph = 1.3

    box(ax, p1x, py, p1w, ph, "Receiver MCU", "RP2350 (same box as above)",
        border=INK_MUTED, fill="#f0efec", lw=1.4)
    box(ax, p2x, py, p2w, ph, "Pi4 Host", "dash_receiver.c\ndecode · decrypt · verify",
        border=INK_MUTED, fill="#f0efec", lw=1.4)
    box(ax, p3x, py, p3w, ph, "Dashboard", "app.py (Flask)\npi4_health_monitor()",
        border=INK_MUTED, fill="#f0efec", lw=1.4)

    arrow(ax, (p1x + p1w / 2, py), (p2x - p2w / 2, py), INK_MUTED, lw=1.6)
    ax.text((p1x + p2x) / 2, py + 0.28, "UART", fontsize=8.6, color=INK_SECONDARY, ha="center", style="italic")
    arrow(ax, (p2x + p2w / 2, py), (p3x - p3w / 2, py), INK_MUTED, lw=1.6)
    ax.text((p2x + p3x) / 2, py + 0.28, "WiFi + HMAC\nHTTP", fontsize=8.6, color=INK_SECONDARY, ha="center", style="italic")

    ax.text(0.3, 0.55,
            "Evaluation results in Section V were measured on receiver_pico/src/main.c — identical wire\n"
            "format, freshness state machine, and 64-entry nonce window to dash_receiver.c, omitting only the\n"
            "WiFi/HMAC layer shown here. Confirming on this Pi4 path is listed as pending (Limitations).",
            fontsize=8.0, color=INK_MUTED, style="italic", linespacing=1.5)

    # Legend
    legend_items = [
        (ORANGE, "Optical channel (security-relevant)"),
        (BLUE, "TLS / network channel"),
        (INK_MUTED, "Reporting plane (implementation)"),
    ]
    ly = 8.9
    lx = 9.4
    for i, (c, label) in enumerate(legend_items):
        yy = ly - i * 0.32
        ax.add_line(Line2D([lx, lx + 0.45], [yy, yy], color=c, linewidth=2.6))
        ax.text(lx + 0.58, yy, label, fontsize=8.3, color=INK_SECONDARY, va="center")

    fig.savefig(OUT, facecolor=SURFACE)
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
