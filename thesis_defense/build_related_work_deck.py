#!/usr/bin/env python3
"""
build_related_work_deck.py — Generates a standalone supplementary slide deck
covering related work on LiFi/VLC security and presence-based authorization.

Kept deliberately separate from build_deck.py / lifi_thesis_defense.pptx so
the main defense deck is untouched. This is a defense-prep reference deck:
what prior work exists, how it compares to this thesis, and how to answer
"isn't this already done?" questions from the committee.

Run: venv/bin/python3 thesis_defense/build_related_work_deck.py
Output: thesis_defense/lifi_related_work_deck.pptx
"""
from pathlib import Path

from pptx import Presentation
from pptx.util import Inches, Pt, Emu
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN, MSO_ANCHOR
from pptx.enum.shapes import MSO_SHAPE

ROOT = Path(__file__).resolve().parent.parent
FIG = ROOT / "thesis_eval" / "figures"
OUT = Path(__file__).resolve().parent / "lifi_related_work_deck.pptx"

# ---- palette (matches build_deck.py for visual consistency) ----
SURFACE = "FCFCFB"
INK_PRIMARY = "0B0B0B"
INK_SECONDARY = "52514E"
INK_MUTED = "898781"
GRID = "E1E0D9"
BLUE = "2A78D6"
ORANGE = "EB6834"
AQUA = "1BAF7A"
VIOLET = "4A3AA7"
GOOD = "0CA30C"
WARNING = "FAB219"
CRITICAL = "D03B3B"

FONT = "Calibri"

SLIDE_W = Inches(13.333)
SLIDE_H = Inches(7.5)


def hexc(c):
    return RGBColor.from_string(c)


prs = Presentation()
prs.slide_width = SLIDE_W
prs.slide_height = SLIDE_H
BLANK = prs.slide_layouts[6]


def new_slide():
    slide = prs.slides.add_slide(BLANK)
    slide.background.fill.solid()
    slide.background.fill.fore_color.rgb = hexc(SURFACE)
    return slide


def set_notes(slide, text):
    slide.notes_slide.notes_text_frame.text = text


def add_title(slide, title, kicker=None, title_size=29, top=0.35):
    tb = slide.shapes.add_textbox(Inches(0.55), Inches(top), Inches(12.2), Inches(0.9))
    tf = tb.text_frame
    tf.word_wrap = True
    p = tf.paragraphs[0]
    r = p.add_run()
    r.text = title
    r.font.size = Pt(title_size)
    r.font.bold = True
    r.font.name = FONT
    r.font.color.rgb = hexc(INK_PRIMARY)
    if kicker:
        tb2 = slide.shapes.add_textbox(Inches(0.55), Inches(top + 0.62), Inches(12.2), Inches(0.5))
        tf2 = tb2.text_frame
        tf2.word_wrap = True
        p2 = tf2.paragraphs[0]
        r2 = p2.add_run()
        r2.text = kicker
        r2.font.size = Pt(13.5)
        r2.font.italic = True
        r2.font.name = FONT
        r2.font.color.rgb = hexc(INK_MUTED)


def add_bullets(slide, bullets, top=1.75, left=0.75, width=11.8, height=5.0,
                 font_size=17, sub_font_size=14.5, space_after=11):
    tb = slide.shapes.add_textbox(Inches(left), Inches(top), Inches(width), Inches(height))
    tf = tb.text_frame
    tf.word_wrap = True
    for i, item in enumerate(bullets):
        text, level = item if isinstance(item, tuple) else (item, 0)
        p = tf.paragraphs[0] if i == 0 else tf.add_paragraph()
        prefix = "●   " if level == 0 else "‒   "
        r = p.add_run()
        r.text = prefix + text
        r.font.size = Pt(font_size if level == 0 else sub_font_size)
        r.font.name = FONT
        r.font.color.rgb = hexc(INK_PRIMARY if level == 0 else INK_SECONDARY)
        p.space_after = Pt(space_after if level == 0 else space_after - 4)
        if level == 1:
            p.level = 1
    return tb


def add_image(slide, path, top=1.7, max_height=4.75, left_bound=0.5, right_bound=12.83):
    pic = slide.shapes.add_picture(str(path), Inches(left_bound), Inches(top), height=Inches(max_height))
    avail = Emu(int(Inches(right_bound - left_bound)))
    if pic.width > avail:
        ratio = avail / pic.width
        pic.width = avail
        pic.height = Emu(int(pic.height * ratio))
    pic.left = Emu(int((SLIDE_W - pic.width) / 2))
    return pic


def add_caption(slide, text, top=6.55, color=INK_MUTED, size=12.5, italic=True, bold=False):
    tb = slide.shapes.add_textbox(Inches(0.55), Inches(top), Inches(12.2), Inches(0.6))
    tf = tb.text_frame
    tf.word_wrap = True
    p = tf.paragraphs[0]
    p.alignment = PP_ALIGN.CENTER
    r = p.add_run()
    r.text = text
    r.font.size = Pt(size)
    r.font.italic = italic
    r.font.bold = bold
    r.font.name = FONT
    r.font.color.rgb = hexc(color)


def style_cell(cell, text, size=13, bold=False, color=INK_PRIMARY, fill=None, align=PP_ALIGN.LEFT):
    cell.text = ""
    tf = cell.text_frame
    tf.word_wrap = True
    p = tf.paragraphs[0]
    p.alignment = align
    r = p.add_run()
    r.text = text
    r.font.size = Pt(size)
    r.font.bold = bold
    r.font.name = FONT
    r.font.color.rgb = hexc(color)
    cell.vertical_anchor = MSO_ANCHOR.MIDDLE
    cell.margin_left = Inches(0.1)
    cell.margin_right = Inches(0.1)
    cell.margin_top = Inches(0.05)
    cell.margin_bottom = Inches(0.05)
    if fill:
        cell.fill.solid()
        cell.fill.fore_color.rgb = hexc(fill)


def add_table(slide, headers, rows, col_widths, top=1.85, left=0.75, height=4.3,
              row_fill_fn=None, header_fill=INK_PRIMARY, cell_size=12.5):
    n_rows = len(rows) + 1
    n_cols = len(headers)
    width = sum(col_widths)
    gtable = slide.shapes.add_table(n_rows, n_cols, Inches(left), Inches(top),
                                     Inches(width), Inches(height)).table
    for i, cw in enumerate(col_widths):
        gtable.columns[i].width = Inches(cw)
    for j, h in enumerate(headers):
        style_cell(gtable.cell(0, j), h, size=13.5, bold=True, color="FFFFFF",
                   fill=header_fill, align=PP_ALIGN.CENTER if j > 0 else PP_ALIGN.LEFT)
    for i, row in enumerate(rows):
        fill = row_fill_fn(i, row) if row_fill_fn else ("F0EFEC" if i % 2 else "FFFFFF")
        for j, val in enumerate(row):
            cell_color = INK_PRIMARY
            bold = False
            cf = fill
            if isinstance(val, tuple):
                val, cell_color, bold = val[0], val[1], val[2] if len(val) > 2 else False
            style_cell(gtable.cell(i + 1, j), val, size=cell_size, bold=bold, color=cell_color,
                       fill=cf, align=PP_ALIGN.CENTER if j > 0 else PP_ALIGN.LEFT)
    return gtable


def add_divider(title, subtitle):
    slide = new_slide()
    rect = slide.shapes.add_shape(MSO_SHAPE.RECTANGLE, 0, 0, SLIDE_W, SLIDE_H)
    rect.fill.solid()
    rect.fill.fore_color.rgb = hexc(INK_PRIMARY)
    rect.line.fill.background()
    tb = slide.shapes.add_textbox(Inches(1.0), Inches(3.0), Inches(11.3), Inches(1.2))
    tf = tb.text_frame
    p = tf.paragraphs[0]
    r = p.add_run()
    r.text = title
    r.font.size = Pt(40)
    r.font.bold = True
    r.font.name = FONT
    r.font.color.rgb = hexc("FFFFFF")
    tb2 = slide.shapes.add_textbox(Inches(1.0), Inches(3.9), Inches(11.3), Inches(0.7))
    tf2 = tb2.text_frame
    p2 = tf2.paragraphs[0]
    r2 = p2.add_run()
    r2.text = subtitle
    r2.font.size = Pt(15)
    r2.font.italic = True
    r2.font.name = FONT
    r2.font.color.rgb = hexc("C3C2B7")
    return slide


# ==================================================================
# SLIDE 1 — Title
# ==================================================================
s = add_divider("Related Work",
                 "LiFi/VLC security & presence-based authorization — defense-prep reference deck")
set_notes(s, "This is a supplementary deck, not part of the main defense sequence. Purpose: have the "
             "literature landscape and the two closest prior-work papers ready if the committee asks "
             "'hasn't someone already done this?' The main deck's Related Work slide (the 4-row table) "
             "stays as the on-stage summary — this deck is the backup material behind it.")

# ==================================================================
# SLIDE 2 — Landscape table (expanded, 6 rows)
# ==================================================================
s = new_slide()
add_title(s, "The Landscape — Six Categories, One Gap")
rows = [
    ["Info-theoretic PLS", "Statistical eavesdropper disadvantage (Wyner wiretap model)", "No real-time authorization decision"],
    ["Physical-layer key generation", "Bootstraps a shared secret from channel measurements", "Key persists after the optical link ends"],
    ["Encrypted VLC implementations", "Protects data in transit (RSA / chaos-synchronized links)", "Optical channel not used for access control"],
    ["VLC-assisted device init (LISA)", "Uses light as an out-of-band channel to bootstrap IoT credentials", "One-time ceremony; light plays no role after setup"],
    ["VLC-gated network access (Suduwella et al.)", "Uses VLC to prove location before granting WiFi/RADIUS access", "One-time check at association; not re-verified afterward"],
    ["Commercial LiFi (Signify, Oledcomm)", "Markets spatial confinement as a security feature", "One-time authentication, not continuous"],
]
add_table(s, ["Category", "What it does", "What it doesn't do"], rows,
          col_widths=[3.3, 5.1, 3.5], top=1.7, height=4.4, cell_size=11.5)
add_caption(s, "Every row stops at the moment authorization is granted. None re-checks or revokes based on "
               "whether the optical link is still there.", top=6.35, bold=True, italic=False, color=INK_PRIMARY)
set_notes(s, "This is the 4-row main-deck table expanded to 6 by pulling LISA and the Suduwella et al. VLC "
             "location-authentication paper out of the paper's prose into their own rows — they're the two "
             "closest analogs and deserve to be named explicitly rather than folded into 'encrypted VLC "
             "implementations'. Everything else is unchanged from the main deck's framing.")

# ==================================================================
# SLIDE 3 — Closest prior work, head to head
# ==================================================================
s = new_slide()
add_title(s, "The Two Closest Prior-Work Papers",
          "Both use light for authorization — neither makes it continuous")
rows = [
    ["LISA (Perković et al., 2019)",
     "Smartphone-screen VLC bootstraps crypto credentials onto a constrained IoT device",
     "Once — light is a setup-time out-of-band channel",
     "Credential stands until administratively revoked"],
    ["Suduwella, Ranasinghe, de Zoysa (2017)",
     "VLC proves physical location; grants WiFi network access via modified RADIUS/WPA2",
     "Once — at network association",
     "Session stands until normal WiFi session end"],
    ["This thesis",
     "VLC frames are the ongoing authorization signal itself, not a one-time proof",
     "Continuously — every valid frame within Δ renews it",
     "Automatic revocation on signal loss (G4)"],
]
add_table(s, ["Prior work", "What light does", "When it checks", "When authorization ends"], rows,
          col_widths=[2.7, 4.4, 2.4, 3.4], top=1.9, height=3.8, cell_size=11,
          row_fill_fn=lambda i, r: ("EAF3E8" if i == 2 else ("F0EFEC" if i % 2 else "FFFFFF")))
add_caption(s, "Both prior papers answer ‘were you here at one moment?’ This thesis answers "
               "‘are you here right now?’ — and keeps re-answering it.",
            top=6.1, bold=True, italic=False, color=INK_PRIMARY)
set_notes(s, "If asked directly 'what about the Suduwella VLC/WiFi paper' or 'what about LISA' — this is "
             "the slide. Both are legitimate, closely related prior work; neither one binds authorization to "
             "continued optical reception. LISA's light channel is dead the moment provisioning finishes. "
             "Suduwella et al.'s VLC check happens once, at association — after that it's an ordinary WiFi "
             "session with no further optical dependency. The bottom row is the contrast, not a dismissal.")

# ==================================================================
# SLIDE 4 — Anatomy of a LiFi frame
# ==================================================================
s = new_slide()
add_title(s, "Anatomy of a LiFi Frame — What's Actually Sent",
          "Vocabulary for the G1/G2/G4 mechanisms on the following slides")
rows = [
    ["Preamble", "4 B", "0xAB 0xCD 0xEF 0x12 — fixed sync marker; any byte mismatch during matching "
                        "resets the receiver's detector to its hunting state"],
    ["Type", "1 B", "Message type identifier"],
    ["Length", "2 B", "Ciphertext length N, so the receiver knows where the tag/CRC begin"],
    ["Nonce", "12 B", "8 B random boot salt + 4 B monotonic counter — this is what G2's replay window checks"],
    ["Ciphertext", "N B", "AES-128-GCM output, same length as the plaintext payload"],
    ["GCM Tag", "16 B", "Authentication tag — verified before any plaintext is ever released"],
    ["CRC16", "2 B", "CRC16-CCITT, poly 0x1021, init 0xFFFF — cheap integrity check ahead of GCM"],
]
add_table(s, ["Field", "Size", "What it's for"], rows, col_widths=[2.3, 1.4, 8.1], top=1.85, height=3.9,
          cell_size=11.5)
add_bullets(s, [
    "Built in three steps before transmission: (1) AES-128-GCM encrypt with the active session key → "
    "ciphertext + tag, (2) construct the 12 B nonce (salt ‖ counter), (3) wrap with preamble/type/length "
    "in front and tag/CRC16 behind.",
    "Fixed 37 B of overhead per frame (4+1+2+12+16+2), independent of payload size N.",
], top=5.95, font_size=13, space_after=6)
add_caption(s, "Preamble hunting → nonce replay check → GCM tag verification, strictly in that order — "
               "a duplicate nonce is rejected before decryption is ever attempted.",
            top=6.95, bold=True, italic=False, color=INK_PRIMARY)
set_notes(s, "Reference slide for the wire-format question, and the vocabulary the next few slides lean on: "
             "t_last (G1/G4) is the timestamp of the last frame whose nonce and GCM tag both verified; G2's "
             "replay window operates purely on the 12-byte nonce field, before decryption is attempted at "
             "all, so a byte-identical resend is rejected essentially for free. The preamble's hunting-state "
             "behavior matters for G4 too: an occlusion doesn't produce garbled frames the receiver has to "
             "reject one at a time — it produces silence, because the detector never sees a matching preamble "
             "to begin accumulating a frame in the first place.")

# ==================================================================
# SLIDE 5 — What actually calls the SST C API (Pico sender + Pico 2 receiver)
# ==================================================================
s = new_slide()
add_title(s, "What Touches the SST C API — And What Doesn't",
          "For the Pico sender / Pico 2 receiver pair specifically: only one binary in the chain links libsst")
rows = [
    ["app.py (dashboard)", "Python, laptop", "None",
     "‘NEW KEY’ button → subprocess.run([pico_provisioner, rx_config, port]) — orchestrates, never calls SST itself"],
    ["pico_provisioner.c (host)", "C, laptop", "init_SST · get_session_key · free_session_key_list_t · free_SST_ctx_t",
     "The only SST C API caller in this whole chain — authenticates to Auth, fetches the session key, then exits"],
    ["lifi_session_sender.c (Pico)", "C, RP2040 firmware", "None",
     "Receives raw key bytes over UART/USB as a MSG_TYPE_KEY frame and stores them — no TLS stack, no libsst linked"],
    ["receiver_pico (Pico 2)", "C, RP2350 firmware", "None",
     "Same story — key is pasted via USB console (key <hex>); no network credentials, no SST client"],
]
add_table(s, ["Component", "Runs on", "SST C API calls", "Role"], rows,
          col_widths=[2.7, 1.7, 3.5, 4.1], top=1.85, height=3.9, cell_size=10.5,
          row_fill_fn=lambda i, r: ("EAF3E8" if i == 1 else ("F0EFEC" if i % 2 else "FFFFFF")))
add_bullets(s, [
    "init_SST(config_path) → loads the entity's config, its X.509 cert/key pair, and the distribution key; "
    "returns the SST_ctx_t used for every subsequent call.",
    "get_session_key(ctx, NULL) → does the actual authenticated request to Auth over the SST protocol and "
    "returns a session_key_list_t; s_key[0] holds { key_id[8], cipher_key[16], mac_key[32] }.",
    "That key is then written to session_key.json (for the dashboard/Pi4) and pushed to the Pico verbatim "
    "over UART as [PREAMBLE:4][MSG_TYPE_KEY][LEN:2][KEY_ID:8][CIPHER_KEY:16][MAC_KEY:32] — the exact frame "
    "apply_new_key() in the Pico firmware already knows how to parse.",
], top=5.9, font_size=12, space_after=6)
add_caption(s, "Refresh uses the identical two calls: get_session_key() run again returns a brand-new key, "
               "which overwrites session_key.json and gets re-pushed — there is no separate ‘refresh’ API.",
            top=6.9, bold=True, italic=False, color=INK_PRIMARY)
set_notes(s, "Scoped exactly to the Pico sender / Pico 2 receiver pair, since that's the pair the presence "
             "evaluation runs against — not the Pi4/dashboard path, which additionally uses "
             "get_session_key_by_ID() for its roaming lookup (that's the main deck's 'Auth's Role Doesn't Stop "
             "After Provisioning' slide, a different device). The point worth landing: exactly four SST C API "
             "functions are used for this pair, all inside one small host binary, pico_provisioner.c. Neither "
             "microcontroller links the library at all — confirmed by grep against both firmware sources, "
             "zero hits for init_SST/get_session_key in either. That's not an oversight, it's the paper's own "
             "framing of the receiver's key path (Section: Key Delivery to Receiver): the RP2040 and RP2350 "
             "are too constrained to run a TLS client, so the host does the SST handshake and hands the "
             "microcontroller only the raw key bytes it needs, in a format its own firmware already parses. "
             "'Refresh' isn't a distinct code path — clicking NEW KEY re-runs the identical init_SST() + "
             "get_session_key() sequence and re-pushes over the same UART frame.")

# ==================================================================
# SLIDE 6 — G4 mechanism, explained
# ==================================================================
s = new_slide()
add_title(s, "G4 — Revocation on Interruption",
          "One signal, five causes — there is no line-of-sight sensor")
add_bullets(s, [
    "Mechanism: the receiver tracks t_last, the timestamp of the most recent frame to pass GCM tag "
    "verification and nonce validation. If t_last is more than Δ seconds old, authorization is false. "
    "No explicit revoke message exists — the absence of a fresh, valid frame is itself the revocation event.",
    "That one criterion — 'was a fresh, valid frame verified in the last Δ seconds' — goes false for "
    "several physically distinct reasons, and the receiver cannot tell them apart:",
    ("Physical occlusion, misalignment, or movement out of the cone — the condition the occlusion test "
     "actually exercises.", 1),
    ("A rotated key the receiver hasn't picked up yet — GCM tag verification fails even with a clean, "
     "unobstructed beam.", 1),
    ("A replayed frame rejected by G2's nonce window — bytes arrive, but don't count as fresh.", 1),
    ("A decode/CRC failure from noise, ambient-light saturation, or marginal SNR.", 1),
    ("Sender-side stoppage — power loss, crash, or an upstream key revocation at the Auth server.", 1),
    "By design, revocation is fail-closed and cause-agnostic: all five look identical from the outside — "
    "authorization drops within Δ regardless of which one occurred. That's also why the defense-in-depth "
    "claim against theft holds without qualification: theft isn't a special case the system recognizes, "
    "it's just one more way the same signal goes stale.",
], top=1.75, font_size=15.5, space_after=11)
set_notes(s, "This is the slide for 'does it really re-check based on whether the optical link is still "
             "there' — and the honest follow-up: 'besides literally being out of line of sight, what else "
             "ends it.' Answer: nothing distinguishes them. G4 isn't a LOS sensor bolted onto the protocol — "
             "it's a direct corollary of the same freshness check G1 already needs. There's no 'revoke' RPC "
             "anywhere in the system, and no diagnostic signal for why t_last went stale. This is the same "
             "underlying issue as G3's relay caveat (a live relay looks like genuine presence for the same "
             "reason) and is consistent with the paper's Non-Goals — no localization, no causal diagnosis, "
             "just fail-closed. The next slide is the measured proof for the occlusion case specifically: "
             "25 on-device trials, revocation firing at exactly Δ = 15.001s ± 0.000s every single time.")

# ==================================================================
# SLIDE 7 — G4 measured evidence (chart)
# ==================================================================
s = new_slide()
add_title(s, "G4 — Measured: Revocation Fires at Exactly Δ",
          "Occlusion (shadowing) test — 21 + 20 + 25 trials, receiver_pico platform")
add_image(s, FIG / "revocation_latency.png", top=1.55, max_height=4.55)
add_caption(s, "On-device [PRESENCE] transitions (n=25, zero external polling): time-to-revoke = "
               "15.001s ± 0.000s — every trial. That's Δ (15s) plus ~0.11s fixed overhead, enforced exactly.",
            top=6.25, bold=True, italic=False)
set_notes(s, "Three batches, same experiment, increasing rigor. Blue: as-deployed, 5s external-monitor "
             "polling, n=21 — bimodal (15.7s / 18.2s clusters), which is polling aliasing, not the mechanism "
             "misbehaving. Orange: fine-grained 0.2s polling, n=20 — collapses to 15.11s ± 0.06s, isolating "
             "the real latency from polling artifacts. Aqua: on-device, n=25 — reads the receiver's own "
             "[PRESENCE] state transitions directly, no external observation cadence at all. Every one of "
             "the 25 trials hit 15.001s exactly, because the on-device check runs every main-loop iteration "
             "and fires the instant the Δ timer crosses — this is the strongest evidence available that Δ is "
             "enforced exactly, not approximately. Right-hand panel: re-verification after unblocking is "
             "consistently ~1.3s across all three batches, bounded above by the 2.5s heartbeat interval as "
             "expected. If pressed on 'why isn't the main deck's G4 slide already framed this way' — the main "
             "deck's caption predates the on-device batch; this deck's version is the current, complete "
             "picture using the same source figure.")

# ==================================================================
# SLIDE 8 — G3, the point of it
# ==================================================================
s = new_slide()
add_title(s, "G3 — Location Confinement Is Not Distance Bounding",
          "What we're actually arguing, and why it's named rather than discovered")
add_bullets(s, [
    "The physics of light gives you ‘where’ for free: a valid frame proves the receiver shares the "
    "sender's optically-bounded space. It does not give you ‘how far’ or ‘how fast’ — nothing on the "
    "data path measures round-trip time.",
    "G1 (Δ freshness) bounds how long a captured, no-longer-live frame stays useful. G2 (nonce window) "
    "rejects a byte-identical resend. Neither examines WHEN a frame arrived relative to when it was sent.",
    ("A live optical relay — light→electrical→light, forwarding genuinely fresh frames with added "
     "latency δ_relay — passes every check the receiver runs: new nonce (G2 ✓), valid GCM tag (integrity "
     "✓), t_last continuously refreshed (G1 ✓). Verified directly against the receiver implementation, "
     "not inferred.", 1),
    "Named explicitly rather than left to be discovered: being precise about what the system does and "
    "doesn't defend against is more defensible at review than a gap a reviewer finds first.",
    "What closing it requires: a Brands–Chaum-style distance-bounding challenge-response over the "
    "existing bidirectional UART link — architecturally compatible, not implemented.",
    ("Feasibility evidence, not a guarantee: measured end-to-end frame latency is 3–6ms, leaving ample "
     "timing margin for a bounded RTT check without a hardware redesign.", 1),
    "Doesn't undercut the core RF-comparison claim — automatic revocation on physical departure is still "
    "structurally impossible for RF without environmental modification. G3 scopes out one specific "
    "attacker capability (live real-time optical relay), not the whole security model.",
], top=1.75, font_size=15, space_after=10)
add_caption(s, "The line this thesis draws: physics gives location confinement for free; distance "
               "bounding still has to be built.", top=6.85, bold=True, italic=False, color=INK_PRIMARY)
set_notes(s, "This slide isn't the mechanism walkthrough (that's the main deck's 'G3 — A Named Limitation, "
             "Not a Discovered Gap' slide, delivered in the same even tone as every positive result). This is "
             "the 'why does G3 exist at all' slide — the argumentative point behind it. The single sentence "
             "to have ready verbatim: G1's expiration window bounds a captured, no-longer-live frame; it does "
             "not bound relay latency, because a continuously operating relay keeps t_last refreshed "
             "indefinitely with genuinely fresh, just-late frames. That confusion — mistaking freshness for a "
             "relay bound — is the single most likely follow-up question. Second most likely: 'so does this "
             "mean the whole thing is broken against a determined attacker?' — no; the RF-comparison argument "
             "(automatic revocation on departure, which RF cannot do without environmental modification) "
             "survives G3 fully intact. G3 only concedes one specific capability, not the model.")

# ==================================================================
# SLIDE 9 — Trust assumptions
# ==================================================================
s = new_slide()
add_title(s, "Trust Assumptions — What We're Not Attacking",
          "The five things the security analysis takes as given, not as defended")
rows = [
    ["Trusted provisioning service", "IoTAuth and its X.509 keys are not compromised; it authenticates "
                                      "entities correctly and only distributes keys to authorized parties",
     "This is the root of trust — assumed, not defended. Every downstream key is only as good as this holding."],
    ["Trusted sender hardware", "The optical beacon is physically secured against tampering",
     "Physical access → key extraction from flash is treated as full compromise, out of scope by design "
     "(→ Limitations: no hardware key protection)."],
    ["Standard cryptographic assumptions", "AES-128-GCM IND-CPA/INT-CTXT, HMAC-SHA256 secure MAC, "
                                            "SHA-256 collision-resistant, CTR_DRBG indistinguishable output",
     "Generic, not thesis-specific — a break here breaks the primitives themselves, not this protocol."],
    ["Bounded clock drift", "RP2040/RP2350 local clocks support the Δ window without external time sync",
     "Millisecond resolution is adequate for Δ in the hundreds-of-ms-to-seconds range actually used."],
    ["No full client compromise", "Receiver hasn't been compromised by malware extracting keys or "
                                   "manipulating authorization logic; side-channels not considered",
     "A fully compromised receiver bypasses any software policy trivially — true of any system. Narrower: "
     "this covers runtime compromise, not provisioning-time key substitution (→ manual key-paste gap)."],
]
add_table(s, ["Assumption", "What it means", "What it does / doesn't cover"], rows,
          col_widths=[2.5, 4.7, 4.8], top=1.8, height=4.4, cell_size=10)
add_caption(s, "Assumptions 2 and 5 are the ones worth having ready — they're exactly where the receiver's "
               "manual key-paste limitation and lack of hardware key protection actually live.",
            top=6.4, bold=True, italic=False, color=INK_PRIMARY)
set_notes(s, "Pairs with the Adversary Model slide right after this one: that slide is what A can do, this "
             "slide is what the analysis assumes A cannot do or isn't trying to do. Worth stating plainly if "
             "asked to justify any one of these in isolation — they're standard assumptions for this class of "
             "system, not unusually generous ones. The one to actually engage with rather than wave off: "
             "assumption 5 sounds like it should cover the receiver's manual-key-paste weakness discussed "
             "earlier, and it doesn't — 'no full client compromise' is about an already-correctly-provisioned "
             "device staying uncompromised at runtime, not about whether the right key got typed in during "
             "provisioning in the first place. Those are genuinely different failure modes.")

# ==================================================================
# SLIDE 10 — Adversary model, full picture
# ==================================================================
s = new_slide()
add_title(s, "Adversary Model — Five Capabilities, Four Answers and One G3",
          "A computationally bounded A trying to get authorized without legitimate physical presence")
rows = [
    ["Network Control", "Full Dolev-Yao control of RF/wired links to the provisioning service — "
                         "eavesdrop, replay, delay, drop, reorder, inject",
     "TLS + mutual X.509 auth on that link. Explicitly excluded from the optical link — A cannot inject "
     "into the beam without physical access to the cone."],
    ["Optical Observation", "Passive cameras/photodiodes in the space capture ciphertext, tags, nonces "
                             "(e.g. an outdoor deployment visible from a distance)",
     "AES-128-GCM confidentiality — plaintext/key never exposed. A confidentiality answer, not a "
     "presence-authorization one."],
    ["Relay", "Capture the signal at the legit site, transport over an arbitrary medium (fiber/coax/RF), "
              "re-emit from a rogue source elsewhere, with latency δ_relay",
     "Not defended — this is G3, named explicitly as unachieved, not overlooked."],
    ["Optical Injection", "Rogue light source in the receiver's field of view, attempting crafted frames",
     "Fails GCM tag verification without the session key. Injecting a captured frame is a replay — "
     "caught by G2's nonce window."],
    ["Denial of Service", "Physical obstruction, ambient-light saturation of the front-end, or RF jamming "
                           "of the provisioning link",
     "Explicitly not prevented — by design. Fails closed: DoS produces revocation, not persistence."],
]
add_table(s, ["Capability", "What A can do", "How it's handled"], rows,
          col_widths=[2.0, 4.6, 5.4], top=1.75, height=4.35, cell_size=10,
          row_fill_fn=lambda i, r: ("FBEAEA" if i == 2 else ("F0EFEC" if i % 2 else "FFFFFF")))
add_bullets(s, [
    "Is this a good model? Two real strengths: it separates a fully adversarial network channel (standard "
    "Dolev-Yao) from a physically-bounded optical channel that needs physical presence to touch — that "
    "separation is the thesis's actual argument, not a checkbox threat list. And DoS is scoped as an "
    "accepted, named tradeoff (fail-closed) rather than silently ignored.",
    ("One real tension: the System Model states communication between ‘the receiver’ and the provisioning "
     "service happens over TCP/TLS — but receiver_pico, the hardware actually evaluated for G1/G2/G4, has "
     "no TLS client at all; its key arrives by hand over USB (Section: Key Delivery to Receiver). Not fatal, "
     "and the paper does caveat it — just not in the Threat Model section itself, where the general claim "
     "is made. Worth having the cross-reference ready.", 1),
], top=6.3, font_size=11, space_after=6)
set_notes(s, "The table is the full capability list from the paper's Threat Model section, condensed to one "
             "row each. The point of showing all five together, right after the G3 deep-dive: G3 is one "
             "specific, named gap in an otherwise mostly-answered list, not evidence the threat model is "
             "loosely reasoned. If asked to assess the model itself rather than just recite it — the two "
             "bullets are the honest answer. The tension bullet is a real inconsistency I found by "
             "cross-reading the System Model section against the System Architecture section and the actual "
             "receiver_pico/src/main.c source: the former states the receiver talks TCP/TLS to the "
             "provisioning service as part of the general three-participant model, the latter (correctly) "
             "documents that the evaluated receiver has no network credentials and gets its key pasted by an "
             "operator. It's an abstraction-vs-instance gap, not a contradiction about what was built — but a "
             "sharp reader who reads the Threat Model section in isolation could reasonably ask about it "
             "without ever having read the Key Delivery to Receiver caveat. Also worth noting out loud only "
             "if pressed: Trust Assumption #5 ('no full client compromise') is about runtime compromise of an "
             "already-correctly-provisioned device, not about provisioning-time key substitution — so it "
             "doesn't actually cover the manual-key-paste weakness either. That's a second, narrower version "
             "of the same gap, not a new one.")

# ==================================================================
# SLIDE 11 — G1-G4 at a glance
# ==================================================================
s = new_slide()
add_title(s, "G1–G4 at a Glance", "Status, mechanism, and measured evidence for each — one table")
rows = [
    ["G1 — Freshness & Expiration", ("Achieved", GOOD, True), "t_last vs Δ=15s window",
     "960-frame / 40-min benign session: 0% false-revoke at every candidate Δ tested (3–30s)"],
    ["G2 — Replay Resistance", ("Achieved, bounded", WARNING, True), "64-entry nonce sliding window, "
     "checked before decryption",
     "25/25 replayed frames rejected. Boundary test (2-entry window): 25/25 accepted once the nonce aged "
     "out — real, disclosed edge case."],
    ["G3 — Relay-Bounded Authorization", ("Not achieved (named)", CRITICAL, True),
     "Would require a Brands–Chaum RTT challenge-response",
     "Verified against source: no timestamp/RTT field on the data path. 3–6ms frame latency margin "
     "suggests room to add one."],
    ["G4 — Revocation on Interruption", ("Achieved", GOOD, True), "Direct corollary of G1 — absence of a "
     "fresh frame is itself the revocation event",
     "25 on-device trials: 15.001s ± 0.000s time-to-revoke, every trial. ~1.3s time-to-reverify."],
]
add_table(s, ["Goal", "Status", "Mechanism", "Measured evidence"], rows,
          col_widths=[2.9, 1.9, 3.4, 4.8], top=1.85, height=4.1, cell_size=10.5)
add_caption(s, "Three goals achieved and measured, each with its own honestly-disclosed edge case. One goal "
               "explicitly not achieved, with a named reason and a concrete path to closing it. That's the "
               "whole security story in one table.", top=6.3, bold=True, italic=False, color=INK_PRIMARY)
set_notes(s, "Capstone table pulling together slides 6–10. Use this if someone asks for 'the whole picture' "
             "rather than walking one goal at a time. Note the color coding is deliberate, not decorative: "
             "green for clean achievement, amber for achieved-but-with-a-disclosed-boundary (G2), red for "
             "explicitly not achieved (G3) — all three colors are honest signals, not a pass/fail grade. The "
             "G2 row is easy to undersell — the boundary test used a deliberately shrunk 2-entry window to "
             "make the limitation observable quickly; the production 64-entry window means an adversary needs "
             "64 further legitimate frames to exchange before a captured frame ages back into acceptability, "
             "which at the minimum authenticated rate 1/Δ bounds how long the threat window stays open.")

# ==================================================================
# SLIDE 12 — Known limitations, ranked
# ==================================================================
s = new_slide()
add_title(s, "Known Limitations, Ranked",
          "Every row below is already in the paper's own text — none were found by a reviewer first")
rows = [
    ["1", "No relay resistance (G3)", "Threat Model §G3, Discussion, Limitations #2",
     "Already has 2 dedicated slides here — the single most likely question overall"],
    ["2", "Receiver key delivery is manual, not independently authenticated", "Limitations #6",
     "No second corroborating channel, unlike the sender's HMAC handshake (→ Trust Assumption #5's boundary)"],
    ["3", "No hardware key protection", "Limitations #5",
     "Session key extractable via physical access + debugger (→ Trust Assumption #2)"],
    ["4", "Replay window is bounded, not permanent", "Security Validation — replay window boundary test",
     "Demonstrated directly: 25/25 replayed frames accepted once a shrunk window aged the nonce out"],
    ["5", "Δ=15s validated only against heartbeat-only traffic", "Evaluation — Δ sensitivity",
     "Paper's own words: “degenerate rather than informative” — real degraded-signal validation is future work"],
    ["6", "Line-of-sight / ambient-light sensitivity", "Limitations #1, #4",
     "Dust, fog, vibration, or strong ambient light can trigger false revocation if Δ is set too aggressively"],
]
add_table(s, ["#", "Limitation", "Already acknowledged where", "Why it matters"], rows,
          col_widths=[0.5, 3.4, 3.4, 5.2], top=1.75, height=4.35, cell_size=10)
add_bullets(s, [
    "Lower-stakes, mention only if asked: single-channel reception (Limitations #3); single-sender "
    "assumption (Non-Goals — architecturally supported via the key-ID field, not implemented).",
    ("Not in the paper's text — my own cross-read finding, not a stated limitation: the System Model says "
     "the receiver talks TCP/TLS to the provisioning service, which doesn't match receiver_pico specifically "
     "(see the Adversary Model slide). Have the cross-reference ready if it comes up.", 1),
], top=6.25, font_size=10.5, space_after=5)
set_notes(s, "Ranked by (severity to the core claim) x (likelihood of being asked), not by section order in "
             "the paper. Rows 1-6 are all explicitly enumerated in the paper already — Limitations §1-6, plus "
             "the Δ-sensitivity prose and the G2 boundary-test prose in Security Validation — so 'what's the "
             "weakest part of this' is a safe question to invite rather than dread. The one item flagged "
             "separately at the bottom (System Model vs receiver_pico) is NOT in the paper anywhere — it's a "
             "finding from this session's own cross-reading, so don't present it as if the paper already "
             "owns it; frame it as 'here's something worth double-checking,' not 'here's a known limitation.'")

# ==================================================================
# SLIDE 13 — Why not just use UWB secure ranging?
# ==================================================================
s = new_slide()
add_title(s, "Why Not Just Use UWB Secure Ranging?",
          "The realistic ‘you're solving an already-solved problem’ question")
add_bullets(s, [
    "UWB (IEEE 802.15.4z HRP, commercialized via the FiRa Consortium — the tech behind Apple/Samsung "
    "‘precision finding’) already does cryptographically secured distance-bounding: a Scrambled Timestamp "
    "Sequence (STS) encrypts the ranging waveform itself, so round-trip time can't be spoofed the way an "
    "unauthenticated time-of-flight measurement could. That's exactly the capability G3 names as missing here.",
    "So why not just use UWB for presence-gated authorization instead of an optical system?",
    ("UWB proves distance, not confinement. It bounds ‘how far,’ not ‘which room’ — RF isn't blocked by a "
     "wall the way light is, so ranging to under a meter doesn't stop someone the right distance away in "
     "the next room.", 1),
    ("UWB needs purpose-built radio silicon — a dedicated UWB transceiver, antenna design, a certified "
     "FiRa-compliant stack. This system's entire ~$82 BOM uses COTS parts already doing a communications "
     "job, not a radio-ranging one.", 1),
    ("RF-denied or interference-sensitive environments (SCIFs, hospitals, EMI-heavy industrial floors) "
     "can't always add more RF radios. An optical channel doesn't compete for spectrum or need RF emissions "
     "certification.", 1),
    ("They're not mutually exclusive — G3's own ‘what would be required’ answer (a Brands–Chaum-style RTT "
     "challenge-response over the existing bidirectional link) is the same idea UWB implements in radio. "
     "Future work could borrow the pattern optically rather than adopting UWB wholesale.", 1),
], top=1.8, font_size=14.5, space_after=10)
add_caption(s, "Honest answer: UWB already solves distance-bounding, and does it better than this system "
               "currently does. This thesis was never trying to out-range UWB — it solves confinement, and "
               "G3 names the one place a UWB-style RTT check could still add value on top.",
            top=6.6, bold=True, italic=False, color=INK_PRIMARY)
set_notes(s, "This is the one question in the whole deck nobody had prepped for before this pass — a "
             "security-literate committee member who knows Apple/Samsung UWB precision-finding could "
             "reasonably ask 'isn't this already solved?' The honest answer isn't to argue optical beats UWB "
             "— it's to relocate the claim: UWB answers 'how far' with more maturity than this system's G3 "
             "attempt would; this thesis answers 'which physically-confined space' for free, from the physics "
             "of light, which UWB's RF signal fundamentally cannot do without environmental modification — "
             "the same RF-comparison argument used elsewhere in this deck, just against a much stronger, "
             "modern RF opponent than plain WiFi. Sourced from FiRa Consortium technical material and the "
             "IEEE 802.15.4z STS mechanism description — verified via web search this session, not from the "
             "codebase.")

# ==================================================================
# SLIDE 14 — Physical-layer authentication is an active, adjacent thread
# ==================================================================
s = new_slide()
add_title(s, "Physical-Layer Challenge-Response is a Live 2025 Research Thread",
          "Cetindere Vela, Vela & Tomasin — “Physical Layer Authentication With Colored RIS in Visible Light Communications” (arXiv:2504.13666, Apr. 2025)")
add_bullets(s, [
    "A colored reconfigurable intelligent surface (dichroic-mirror RIS) reflects VLC light in a "
    "multicolor pattern that depends on a dynamic, verifier-known configuration.",
    "The verifier compares received multicolor power profiles to the expected pattern — "
    "a challenge-response scheme built entirely at the physical layer.",
    "Randomized, dynamic configurations authenticate best; static ones are easiest to spoof.",
    ("Confirms the field is actively moving toward PHY-layer challenge-response for VLC — "
     "but it authenticates individual messages against a known reflector configuration, not a "
     "device's continued physical presence in a coverage area.", 1),
    ("No revocation model: it verifies one exchange is genuine, not that the link is still live "
     "a second later.", 1),
], top=2.4, font_size=16.5)
add_caption(s, "Adjacent, concurrent, and a useful citation for ‘this is a live area’ — "
               "but it targets message authenticity, not session-level presence enforcement.",
            top=6.5, bold=True, italic=False, color=INK_PRIMARY)
set_notes(s, "Good to have on hand if asked 'is anyone doing PHY-layer VLC authentication right now' — yes, "
             "and here's a April 2025 example. It strengthens rather than threatens the gap claim: even the "
             "newest PHY-authentication work verifies one message against a reflector configuration, it "
             "doesn't enforce that a device stays authorized only while it keeps receiving.")

# ==================================================================
# SLIDE 15 — Survey foundation
# ==================================================================
s = new_slide()
add_title(s, "This Sits on a Well-Surveyed Foundation", "Three surveys, three years apart, same conclusion")
rows = [
    ["Blinowski (2019)", "Physical Communication",
     "First comprehensive VLC security review — eavesdropping, jamming, node compromise; PLS/keygen/steganography/crypto taxonomy"],
    ["Arfaoui et al. (2020)", "IEEE Communications Surveys & Tutorials",
     "Unified info-theoretic + signal-processing treatment of VLC physical-layer security; channel models, precoding, secrecy capacity"],
    ["Zhang, Klevering, Lei, Hu, Xiao, Tu (2023)", "ACM Computing Surveys",
     "Broader optical wireless security survey spanning VLC, LiFi, optical camera comms, free-space optical, and LiDAR"],
]
add_table(s, ["Survey", "Venue", "Scope"], rows, col_widths=[3.4, 3.4, 5.6], top=1.9, height=3.4, cell_size=12)
add_caption(s, "All three conclude the same thing: light's spatial confinement is real, but it is not itself "
               "an authorization mechanism — something has to be built on top of it.",
            top=5.7, bold=True, italic=False, color=INK_PRIMARY)
set_notes(s, "Zhang et al. 2023 is not currently cited in the paper's bibliography — it's a newer, broader "
             "ACM Computing Surveys piece that widens the lens from VLC specifically to optical wireless "
             "comms generally (adds OCC, FSOC, LiDAR). Worth citing alongside Blinowski/Arfaoui if the paper's "
             "related-work section gets a revision pass — flagged here, not yet added to the .tex.")

# ==================================================================
# SLIDE 16 — The gap, restated with named comparisons
# ==================================================================
s = new_slide()
add_title(s, "The Gap, Restated Against Named Prior Work",
          "“Authorized because you are here, now, in the light”")
add_bullets(s, [
    "Not LISA — light is not a one-time bootstrap channel here; it is the standing authorization signal.",
    "Not Suduwella et al. — the optical check is not a gate you pass once at association; it must keep passing.",
    "Not the CRIS challenge-response work — the unit being verified is not a single message; it is "
    "continued presence over time.",
    "Not the commercial LiFi products — spatial confinement alone is not enforcement; this thesis adds "
    "the enforcement layer (automatic revocation on signal loss, G4).",
    ("No prior work formalizes, implements, and evaluates continuous optical-presence authorization with "
     "automatic revocation — that is the gap this thesis fills.", 1),
], top=1.85, font_size=16.5, space_after=13)
set_notes(s, "This is the closing slide of the supplementary deck — same gap claim as the main deck's "
             "caption, but now defended against each named piece of prior work individually rather than left "
             "as a category-level assertion. Use this ordering if the Q&A goes deep on 'why isn't this "
             "already solved.'")

# ==================================================================
# SLIDE 17 — References
# ==================================================================
s = new_slide()
add_title(s, "References — New to This Deck")
add_bullets(s, [
    "C. P. Suduwella, Y. S. Ranasinghe, and K. de Zoysa, “Visible light communication based "
    "authentication protocol designed for location based network connectivity,” in Proc. 2017 "
    "Seventeenth Int. Conf. Advances in ICT for Emerging Regions (ICTer), Sept. 2017, "
    "doi:10.1109/ICTER.2017.8257800.",
    "B. Cetindere Vela, S. Vela, and S. Tomasin, “Physical Layer Authentication With Colored RIS in "
    "Visible Light Communications,” arXiv:2504.13666, Apr. 2025.",
    "X. Zhang, G. Klevering, X. Lei, Y. Hu, L. Xiao, and G.-H. Tu, “The Security in Optical Wireless "
    "Communication: A Survey,” ACM Computing Surveys, vol. 55, no. 14s, Article 329, 2023, "
    "doi:10.1145/3594718.",
], top=1.85, font_size=15.5, space_after=16)
add_caption(s, "All other citations (Wyner, Brands & Chaum, Haas, Blinowski, Arfaoui et al., LISA, and the "
               "commercial vendors) are already in the paper's bibliography — see conference_101719.tex.",
            top=5.4, bold=False, italic=True, color=INK_MUTED)
set_notes(s, "These three are verified via Crossref (Suduwella et al. DOI), arXiv (Cetindere Vela et al.), and "
             "the ACM DOI page (Zhang et al.) — not yet added to the thesis paper's .bib/thebibliography. "
             "If the paper gets one more revision pass, these are the citations to fold in.")

# ==================================================================
# SLIDE 18 — G2: found and fixed during evaluation
# ==================================================================
s = new_slide()
add_title(s, "G2 — Found and Fixed During Evaluation",
          "Unauthenticated window eviction: a gap the existing 'bounded window' disclosure didn't cover")
add_bullets(s, [
    "Known Limitations #4 already discloses that the replay window is bounded, not permanent — a nonce "
    "ages out once enough traffic cycles through a shrunk window (25/25 accepted once aged out, by design, "
    "using legitimate re-transmission to demonstrate the boundary).",
    "That disclosure assumes only legitimate, key-holding traffic drives eviction. It didn't: insertion into "
    "the window happened on CRC-pass alone, before GCM verification ever ran — process_complete_frame() in "
    "receiver_pico/src/main.c added a nonce to Seen regardless of whether the frame later decrypted.",
    ("CRC-16 is a public, unkeyed checksum — computing one requires no session key. So an attacker holding "
     "zero cryptographic material could inject 64 CRC-valid junk frames to deliberately evict a genuinely "
     "captured nonce on their own schedule, then replay the captured (genuine) frame once its slot was "
     "freed — bypassing the |Seen|·T_beacon ≥ Δ margin entirely, since that margin assumed eviction paced "
     "by the 2.5s heartbeat, not by an attacker's injection rate.", 1),
    "Fix: nonce insertion now happens only after GCM authentication succeeds (dec == DEC_OK) — eviction "
    "requires possessing the session key, matching what the Novelty Predicate slide already assumed "
    "('on accept'). Firmware rebuilt (receiver_pico/build/lifi_pico2_rx.uf2); reflash and G2 trial re-run "
    "against the fixed firmware pending.",
    "Found via source review during defense prep, not by an external reviewer — same posture as the two "
    "protocol bugs already disclosed in the main deck's Limitations backup slide.",
], top=1.85, font_size=14.5, space_after=10)
add_caption(s, "The 25/25 replay-rejection result is unaffected — it never depended on this gap. What changes "
               "is the eviction-bound argument's soundness against an unauthenticated attacker, not the headline number.",
            top=6.7, bold=True, italic=False, color=INK_PRIMARY)
set_notes(s, "This slide only exists because of source-level review, not because a test failed — worth saying "
             "that plainly if it comes up. The distinction to keep sharp under questioning: the ALREADY-disclosed "
             "boundary limitation (#4) is about a nonce eventually aging out under real traffic, which is "
             "expected and harmless (the traffic that evicts it also independently keeps presence current). "
             "This is different — it's about an attacker with no key forcing that eviction on demand, which "
             "the original |Seen|·T_beacon≥Δ margin never accounted for. If asked whether this invalidates the "
             "measured 25/25 replay numbers: no — that result is about immediate replay (δ=200ms), where "
             "nothing has had time to evict anything regardless of which insertion rule is active. The rerun "
             "still needs to happen before citing this as closed, and should add a new trial that specifically "
             "exercises the old eviction path to demonstrate it's now closed.")

# ==================================================================
# SLIDE 19 — G3: correcting the "existing bidirectional UART" claim
# ==================================================================
s = new_slide()
add_title(s, "G3 Future Work — Correcting an Architecture Claim",
          "The proposed distance-bounding transport doesn't exist on the evaluated receiver")
add_bullets(s, [
    "Both the paper (Section VI, future work) and this deck's G3 slides describe closing G3 via a "
    "Brands–Chaum-style challenge-response 'over the existing bidirectional UART link between sender and "
    "receiver' — called 'architecturally compatible... not implemented.'",
    ("Verified against source: receiver_pico/src/main.c — the RP2350, the receiver actually evaluated for "
     "G1/G2/G4 — initializes zero UART peripherals (grep for uart_init returns nothing). Its only interfaces "
     "are the PIO photodiode RX and a USB console for a human operator. There is no wired return path to "
     "the sender at all.", 1),
    ("The bidirectional UART that does exist — lifi_session_sender.c's UART_ID/uart1, carrying the "
     "HS1/HS2/HS3 SST handshake — connects the sender to the Pi4, not to receiver_pico. That link belongs "
     "to the retired Pi4/dashboard stack (the same commit that cut the roaming/live-challenge claims), not "
     "the two-Pico system this thesis evaluates.", 1),
    "Consequence: closing G3 for the evaluated system requires adding a new physical return channel — a "
    "UART line, or a second LED/photodiode pair for the reverse direction. It is not 'architecturally "
    "compatible... without a hardware redesign' as currently written; it requires one.",
    "Separately: even with a return channel, the achievable margin needs framing as a processing-time bound "
    "(Hancke–Kuhn style), not a propagation-time bound — light-speed transit at ~15cm is sub-nanosecond, "
    "negligible against microcontroller response jitter. The measured 3–6ms end-to-end frame-processing "
    "figure is a defensible starting point for that argument, but only once framed correctly.",
], top=1.75, font_size=14, space_after=9)
add_caption(s, "Correction needed in both the paper and this deck: 'architecturally compatible, not "
               "implemented' → 'requires a new physical return channel, not present in the evaluated hardware.'",
            top=6.85, bold=True, italic=False, color=CRITICAL)
set_notes(s, "This is an internal inconsistency, not a new limitation — Slide 5 of this same deck already "
             "states correctly that receiver_pico has no SST client and gets its key pasted by an operator, "
             "which implies no UART; the G3 slides just never carried that fact forward into the future-work "
             "framing. Good news if asked: this doesn't weaken the G3 discussion, it sharpens it — 'not "
             "achieved, and closing it needs new hardware' is a cleaner, more defensible claim than 'not "
             "achieved, but the wiring is already there,' which was never true for the system actually being "
             "evaluated. If pressed on the 3-6ms figure specifically: that number was measured on the "
             "receiver's own frame-processing pipeline (preamble lock to CRC-validated frame), so it's still "
             "a legitimate data point — it just needs to be sold as evidence that processing jitter is tight "
             "enough to bound, not as evidence that light-speed propagation is measurable at this range.")

prs.save(OUT)
print(f"Wrote {OUT} ({len(prs.slides)} slides)")
