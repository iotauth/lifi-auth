#!/usr/bin/env python3
"""
plot_sst_auth_role.py — Sequence diagram for how Auth (SST/IoTAuth) participates
at runtime, beyond initial provisioning. Two phases, same three actors:

  Phase 1 — Roaming discovery: a device broadcasts only a non-secret key ID
  optically; if the receiver doesn't recognize it locally, it asks the Auth
  server directly (get_session_key_by_ID), which can accept (valid device) or
  reject (unknown/revoked) — this is Auth's identity-layer revocation path,
  distinct from G4's physical-layer revocation.

  Phase 2 — Runtime verification: the receiver can challenge the sender live,
  at any later point, with a fresh HMAC challenge-response over the same
  optical/UART link — proof of live key possession, not just "a frame that
  happened to decrypt."

Source: diagrams/room_communication.md, diagrams/ask_receiver.md,
diagrams/hmac_challenge.md (mermaid diagrams already in the repo, redrawn
here to match the rest of the deck's figure style since PowerPoint can't
render mermaid natively).
"""
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch
from matplotlib.lines import Line2D

OUT = Path(__file__).resolve().parent / "sst_auth_role.png"

SURFACE = "#fcfcfb"
INK_PRIMARY = "#0b0b0b"
INK_SECONDARY = "#52514e"
INK_MUTED = "#898781"
GRID = "#e1e0d9"
BASELINE = "#c3c2b7"

BLUE = "#2a78d6"     # Sender
AQUA = "#1baf7a"      # Receiver / Detector
VIOLET = "#4a3aa7"    # Auth (SST/IoTAuth)
ORANGE = "#eb6834"    # optical channel
GOOD = "#0ca30c"
CRITICAL = "#d03b3b"


def lifeline(ax, x, y_top, y_bot, color, label, sublabel):
    box = FancyBboxPatch((x - 1.55, y_top - 0.35), 3.1, 0.7,
                          boxstyle="round,pad=0.02,rounding_size=0.08",
                          linewidth=2.0, edgecolor=color, facecolor=SURFACE, zorder=5)
    ax.add_patch(box)
    ax.text(x, y_top + 0.06, label, ha="center", va="center", fontsize=12,
            fontweight="bold", color=INK_PRIMARY, zorder=6)
    ax.text(x, y_top - 0.16, sublabel, ha="center", va="center", fontsize=8.7,
            color=INK_SECONDARY, zorder=6)
    ax.plot([x, x], [y_top - 0.35, y_bot], color=BASELINE, linewidth=1.4,
            linestyle=(0, (5, 3)), zorder=1)


def hmsg(ax, x0, x1, y, text, color, lw=2.2, style="-", label_dy=0.14, fontsize=9.0, bold=False):
    a = FancyArrowPatch((x0, y), (x1, y), arrowstyle="-|>", mutation_scale=15,
                        linewidth=lw, linestyle=style, color=color, zorder=3,
                        shrinkA=0, shrinkB=0)
    ax.add_patch(a)
    xm = (x0 + x1) / 2
    ax.text(xm, y + label_dy, text, ha="center", va="bottom", fontsize=fontsize,
            color=INK_PRIMARY if bold else INK_SECONDARY, fontweight="bold" if bold else "normal")


def selfnote(ax, x, y, text, color=INK_MUTED, fontsize=8.6, width=3.3):
    ax.text(x, y, text, ha="center", va="center", fontsize=fontsize, color=color,
            style="italic",
            bbox=dict(boxstyle="round,pad=0.35", fc="#f0efec", ec=GRID, lw=1.0))


def phase_label(ax, y, text, color):
    ax.plot([0.2, 12.9], [y, y], color=GRID, linewidth=1.1, zorder=1)
    ax.text(0.2, y - 0.16, text, fontsize=11, fontweight="bold", color=color, ha="left")


def main():
    fig, ax = plt.subplots(figsize=(12.4, 9.6), dpi=200, facecolor=SURFACE)
    ax.set_xlim(0, 13.1)
    ax.set_ylim(0, 13.4)
    ax.axis("off")
    fig.subplots_adjust(left=0.01, right=0.99, top=0.99, bottom=0.01)

    ax.text(0.2, 13.0, "How Auth (SST / IoTAuth) Participates at Runtime",
            fontsize=16.5, fontweight="bold", color=INK_PRIMARY)
    ax.text(0.2, 12.6, "Two phases, same three actors — not just a one-time boot-up handshake",
            fontsize=10.5, color=INK_MUTED)

    xs, xr, xa = 2.1, 6.5, 10.9
    lifeline(ax, xs, 11.7, 0.5, BLUE, "Sender", "Pico (optical beacon)")
    lifeline(ax, xr, 11.7, 0.5, AQUA, "Receiver", "Pi4 (detector)")
    lifeline(ax, xa, 11.7, 0.5, VIOLET, "Auth Server", "SST / IoTAuth")

    # ---------------- Phase 1 ----------------
    phase_label(ax, 10.75, "PHASE 1 — ROAMING DISCOVERY  (device unknown locally)", VIOLET)

    hmsg(ax, xs, xr, 10.15, "Key ID only (8B, non-secret)  —  optical", ORANGE, lw=2.6, bold=True)
    selfnote(ax, xr, 9.55, "Check local cache → NOT FOUND", width=3.6)
    hmsg(ax, xr, xa, 8.85, "get_session_key_by_ID(keyID)  —  mutual TLS", BLUE)
    selfnote(ax, xa, 8.25, "Auth database lookup", width=3.0)

    hmsg(ax, xa, xr, 7.55, "Valid device → session key\n(cipher key + MAC key)", GOOD, label_dy=0.05, bold=True)
    ax.text(xa - 0.05, 7.05, "alt", fontsize=8, color=INK_MUTED, style="italic", ha="right")
    hmsg(ax, xa, xr, 6.75, "Unknown / revoked → rejected", CRITICAL, label_dy=0.05, bold=True, style=(0, (4, 2)))

    ax.text(xr, 5.95, "Auth's identity-layer revocation: deny a key request outright.\n"
                       "Different from G4's physical-layer revocation (signal interruption).",
            ha="center", fontsize=8.8, color=INK_MUTED, style="italic", linespacing=1.4)

    # ---------------- Phase 2 ----------------
    phase_label(ax, 5.15, "PHASE 2 — RUNTIME VERIFICATION  (on demand, any time after)", AQUA)

    hmsg(ax, xr, xs, 4.55, "MSG_TYPE_CHALLENGE (32B random)  —  optical/UART", ORANGE, lw=2.6, bold=True)
    selfnote(ax, xs, 3.95, "HMAC-SHA256(session_mac_key, challenge)\n→ AES-128-GCM encrypt", width=3.8)
    hmsg(ax, xs, xr, 3.25, "ENCRYPTED(\"HMAC:...\")", BLUE)
    selfnote(ax, xr, 2.65, "Decrypt → recompute HMAC locally → compare", width=3.9)

    hmsg(ax, xr, xr, 1.85, "", GOOD)  # placeholder not used
    ax.text(xr, 1.85, "MATCH → live proof of key possession, not just a\nframe that happened to decrypt",
            ha="center", fontsize=9.3, color=INK_PRIMARY, fontweight="bold", linespacing=1.4,
            bbox=dict(boxstyle="round,pad=0.4", fc="#e6f7ea", ec=GOOD, lw=1.4))

    # legend
    handles = [
        Line2D([0], [0], color=ORANGE, linewidth=2.6, label="Optical / UART channel"),
        Line2D([0], [0], color=BLUE, linewidth=2.2, label="TLS / network channel"),
        Line2D([0], [0], color=GOOD, linewidth=2.2, label="Accepted outcome"),
        Line2D([0], [0], color=CRITICAL, linewidth=2.2, linestyle=(0, (4, 2)), label="Rejected outcome"),
    ]
    ax.legend(handles=handles, loc="lower center", bbox_to_anchor=(0.5, -0.015), ncol=4,
              frameon=False, fontsize=9.2, labelcolor=INK_SECONDARY)

    fig.savefig(OUT, facecolor=SURFACE)
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
