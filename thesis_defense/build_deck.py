#!/usr/bin/env python3
"""
build_deck.py — Generates the thesis defense slide deck (.pptx) from the
paper (thesis_paper/conference_101719.tex), the eval figures
(thesis_eval/figures/*.png), and the speaker-note scripts already drafted
in thesis_eval/figures/speaker_notes.txt.

Run: venv/bin/python3 thesis_defense/build_deck.py
Output: thesis_defense/lifi_thesis_defense.pptx
"""
from pathlib import Path

from pptx import Presentation
from pptx.util import Inches, Pt, Emu
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN, MSO_ANCHOR
from pptx.enum.shapes import MSO_SHAPE

ROOT = Path(__file__).resolve().parent.parent
FIG = ROOT / "thesis_eval" / "figures"
PCB = ROOT / "thesis_paper"
OUT = Path(__file__).resolve().parent / "lifi_thesis_defense.pptx"

# ---- palette (same as the figure scripts, for visual consistency) ----
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
              row_fill_fn=None, header_fill=INK_PRIMARY):
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
            style_cell(gtable.cell(i + 1, j), val, size=12.5, bold=bold, color=cell_color,
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
s = new_slide()
bar = s.shapes.add_shape(MSO_SHAPE.RECTANGLE, 0, 0, Inches(0.22), SLIDE_H)
bar.fill.solid()
bar.fill.fore_color.rgb = hexc(ORANGE)
bar.line.fill.background()

tb = s.shapes.add_textbox(Inches(0.9), Inches(2.15), Inches(11.5), Inches(2.3))
tf = tb.text_frame
tf.word_wrap = True
p = tf.paragraphs[0]
r = p.add_run()
r.text = "Physical Presence-Based Authentication and\nAuthorization for Cyber-Physical Systems and\nIoT using LiFi"
r.font.size = Pt(30)
r.font.bold = True
r.font.name = FONT
r.font.color.rgb = hexc(INK_PRIMARY)

tb2 = s.shapes.add_textbox(Inches(0.9), Inches(4.55), Inches(11.5), Inches(0.5))
p2 = tb2.text_frame.paragraphs[0]
r2 = p2.add_run()
r2.text = "M.S. Thesis Defense"
r2.font.size = Pt(17)
r2.font.italic = True
r2.font.name = FONT
r2.font.color.rgb = hexc(ORANGE)

tb3 = s.shapes.add_textbox(Inches(0.9), Inches(5.35), Inches(11.5), Inches(1.4))
tf3 = tb3.text_frame
tf3.word_wrap = True
p3 = tf3.paragraphs[0]
r3 = p3.add_run()
r3.text = "Jose Felix"
r3.font.size = Pt(15)
r3.font.bold = True
r3.font.name = FONT
r3.font.color.rgb = hexc(INK_PRIMARY)
p4 = tf3.add_paragraph()
r4 = p4.add_run()
r4.text = "with Dongha Kim, Hokeun Kim, Michel Kinsy, Gail-Joon Ahn — KIM Labs, Arizona State University"
r4.font.size = Pt(12.5)
r4.font.name = FONT
r4.font.color.rgb = hexc(INK_SECONDARY)
p5 = tf3.add_paragraph()
p5.space_before = Pt(10)
r5 = p5.add_run()
r5.text = "github.com/asu-kim/lifi-auth"
r5.font.size = Pt(11.5)
r5.font.name = FONT
r5.font.color.rgb = hexc(INK_MUTED)

set_notes(s, "Title slide. State the title once clearly, introduce yourself, thank the committee for their "
             "time, and give a one-sentence roadmap: what the system is, and that you'll cover motivation, "
             "architecture, threat model, and then spend the middle of the talk on measured evaluation results "
             "before discussing limitations and future work.")

# ==================================================================
# SLIDE 2 — Motivation
# ==================================================================
s = new_slide()
add_title(s, "Motivation", "RF security is an architecture problem. Optical confinement is a physics problem.")
add_bullets(s, [
    "RF wireless (WiFi, Bluetooth, Zigbee) propagates through walls, floors, and physical barriers by design",
    "An attacker need not be physically present to intercept, relay, or replay an RF authorization credential",
    "Enforcing a physical boundary on RF requires costly countermeasures — shielding, Faraday enclosures, attenuation infrastructure",
    "LEDs have quietly become the dominant lighting technology in nearly every indoor environment — hospitals, schools, warehouses, offices",
    "That installed, already-powered, already-trusted infrastructure can modulate data imperceptibly to the human eye",
    "Light does not pass through opaque walls or travel around corners without deliberate redirection — confinement by construction, not by policy",
])
set_notes(s, "Open with the RF problem in one sentence: propagation is the enemy of physical-boundary security. "
             "Then pivot to LEDs — they're everywhere already, and the communicative potential is untapped. Land "
             "on the physics point: light is confined by construction. That's the one sentence you want the "
             "committee to remember from this slide.")

# ==================================================================
# SLIDE 3 — Thesis statement / gap
# ==================================================================
s = new_slide()
add_title(s, "The Gap This Thesis Addresses", "“Authorized because you are here, now, in the light.”")
add_bullets(s, [
    "No existing work binds authorization to continuous, time-bounded optical reception with automatic revocation",
    "This thesis reframes the optical signal as an active security primitive — not just a higher-bandwidth data channel",
    "The optical signal is continuously asserted, continuously verified, and automatically revoked the instant the optical path breaks",
    "Formalized as four concrete security goals:",
    ("G1 — Freshness & Expiration", 1),
    ("G2 — Replay Resistance", 1),
    ("G3 — Relay-Bounded Authorization", 1),
    ("G4 — Revocation on Interruption", 1),
])
set_notes(s, "This is the thesis statement slide. Say directly: the literature has information-theoretic secrecy, "
             "physical-layer key generation, encrypted VLC pipes, and commercial one-time-auth products — but "
             "nothing that continuously binds authorization to ongoing optical presence with automatic revocation. "
             "Then introduce G1 through G4 as the four goals the rest of the talk is structured around — you'll "
             "come back to this exact list on the security-goals table slide.")

# ==================================================================
# SLIDE 4 — Related work landscape
# ==================================================================
s = new_slide()
add_title(s, "Related Work — Where the Literature Stops Short")
rows = [
    ["Info-theoretic PLS", "Statistical eavesdropper disadvantage (Wyner wiretap model)", "No real-time authorization decision"],
    ["Physical-layer key generation", "Bootstraps a shared secret from channel measurements", "Key persists after the optical link ends"],
    ["Encrypted VLC implementations", "Protects data in transit (RSA/chaos-synchronized links)", "Optical channel not used for access control"],
    ["Commercial LiFi (Signify, Oledcomm)", "Markets spatial confinement as a security feature", "One-time authentication, not continuous"],
]
add_table(s, ["Category", "What it does", "What it doesn't do"], rows,
          col_widths=[2.9, 4.9, 4.1], top=1.85, height=3.6)
add_caption(s, "Gap: no work formalizes, implements, and evaluates continuous optical-presence authorization "
               "with automatic revocation on signal loss.", top=5.75, bold=True, italic=False, color=INK_PRIMARY)
set_notes(s, "Walk the four rows quickly — the point isn't to teach the literature, it's to show you've placed "
             "this work precisely. Each row has one thing it does and one thing it stops short of. Land on the "
             "caption: nobody formalizes and evaluates continuous optical-presence authorization with automatic "
             "revocation. That's the gap.")

# ==================================================================
# SLIDE 5 — System Architecture (image)
# ==================================================================
s = new_slide()
add_title(s, "System Architecture")
add_image(s, FIG / "system_block_diagram.png", top=1.55, max_height=5.55)
set_notes(s,
          "This is the system architecture. Three participants: the optical beacon (Sender, RP2040) encrypting "
          "every message with AES-128-GCM and driving four LED channels in parallel; the client device (Receiver, "
          "RP2350) with a photodiode front-end checking each frame against a 64-entry nonce window before "
          "decryption; and the key provisioning service (IoTAuth), which both sides authenticate to independently "
          "over TLS with X.509 certs — the IoTAuth host is used only at provisioning time, not in the runtime "
          "data path. The orange arrow is the one that matters: the optical channel, one-way, is the whole "
          "security argument. Below the divider is the on-device monitoring path — the receiver makes its own "
          "revocation decision in firmware and reports it over USB serial — drawn separately and grayed out on "
          "purpose, because it is implementation, not part of the security model above it. Flag here that the "
          "reported occlusion-test numbers predate this on-device decision logic — you'll return to why on the "
          "methodology slide.")

# ==================================================================
# SLIDE 6 — Sender
# ==================================================================
s = new_slide()
add_title(s, "Optical Beacon (Sender)", "Pico H · RP2040")
add_image(s, PCB / "lifi_sender_pcb.png", top=1.55, max_height=3.35, left_bound=0.55, right_bound=6.6)
add_bullets(s, [
    "AES-128-GCM encrypt → nonce construction → frame → modulate",
    "Nonce = 8-byte random boot salt (CTR_DRBG) + 4-byte monotonic counter",
    "PIO state machine drives 4 LED channels in lockstep, 1 Mbps, zero CPU overhead during TX",
    "Frame: preamble · type · length · nonce · ciphertext · 16B GCM tag · CRC16",
    "Dual-slot A/B flash key storage, SHA-256 integrity hash, secure_zero() RAM clearing",
], top=1.7, left=6.9, width=6.0, height=5.4, font_size=14.5, sub_font_size=13)
set_notes(s, "Sender hardware. Emphasize the crypto pipeline order — encrypt, construct nonce, frame, modulate — "
             "and that the PIO transmitter needs zero CPU cycles once armed, which is what makes the 1 Mbps "
             "figure achievable on a microcontroller with no dedicated radio hardware. If asked about nonce "
             "reuse: counter exhaustion at 2^32 messages triggers an immediate reboot to regenerate the salt.")

# ==================================================================
# SLIDE 7 — Receiver
# ==================================================================
s = new_slide()
add_title(s, "Client Device (Receiver)", "Pico 2 H · RP2350")
add_image(s, PCB / "lifi_receiver_pcb.png", top=1.55, max_height=3.35, left_bound=0.55, right_bound=6.6)
add_bullets(s, [
    "Photodiode (SFH 2400) → OPA381 TIA → TLV3501 comparator → PIO UART RX",
    "Programmable threshold via MCP4725 12-bit DAC, tuned each boot (~0.8V)",
    "64-entry nonce window checked before decryption is ever attempted",
    "Robotic integration: Pololu 3pi+ headers, LiPo + buck-boost, USB-C charging",
    "Same board can operate untethered aboard a mobile CPS platform",
], top=1.7, left=6.9, width=6.0, height=5.4, font_size=14.5, sub_font_size=13)
set_notes(s, "Receiver hardware and analog front-end. Walk the signal chain left to right: photodiode current, "
             "transimpedance amplification, comparator digitization, then a custom PIO UART receiver on the "
             "RP2350. Mention the replay window is checked before decryption is attempted at all — a duplicate "
             "nonce never reaches the GCM primitive. The robot header is a real, populated connector, not a "
             "conceptual placeholder — flag that this feeds the robotic-platform future-work item later.")

# ==================================================================
# SLIDE 8 — SST / IoTAuth: the security foundation
# ==================================================================
s = new_slide()
add_title(s, "SST / IoTAuth — the Security Foundation",
          "Not a homebrew crypto stack — a published, decade-long IoT authorization research line")
add_bullets(s, [
    "SST (Secure Swarm Toolkit): open-source authentication & authorization framework for distributed IoT systems",
    "Auth = the local, automated point of authorization — two roles:",
    ("Authenticates/authorizes its own locally registered entities", 1),
    ("Bridges authorization between local entities and the wider network", 1),
    "Architectural concept: “locally centralized, globally distributed” authentication and authorization",
    "Published research lineage spanning a decade: IoTDI '17, FiCloud '16, IT Professional '17, ACM TIOT '20, SoftwareX '23",
    "Built and maintained at ASU KIM Labs — this thesis's own lab:",
    ("Hokeun Kim (co-author) leads the project; Dongha Kim (co-author) is a core contributor", 1),
    ("Jose Felix — this thesis's author — is a listed active contributor to the SST/IoTAuth repo itself", 1),
    "This work integrates via the sst-c-api C library, rather than reimplementing key distribution from scratch",
], top=1.65, height=5.35, font_size=15, sub_font_size=13)
set_notes(s, "This slide exists to head off a specific committee assumption: that the authentication layer is a "
             "student-written crypto stack. It isn't. SST/IoTAuth is a published research architecture with almost "
             "a decade of venues behind it — IoTDI, FiCloud, an ACM journal on DoS recovery, a SoftwareX paper for "
             "the C API this project actually links against. Say plainly that Hokeun Kim, one of your co-authors, "
             "leads the project, and that you yourself are a listed contributor to the SST/IoTAuth repository — "
             "not just a consumer of the library. That's a credibility point worth stating out loud, not just "
             "leaving in a footnote. Land on the last bullet: this thesis's contribution is applying that existing, "
             "trusted key-distribution machinery to a domain — continuous optical presence — it wasn't originally "
             "built for.")

# ==================================================================
# SLIDE 9 — Sender provisioning & key rotation
# ==================================================================
s = new_slide()
add_title(s, "Sender Provisioning & Key Rotation",
          "Corrected against source — pico_provisioner.c and lifi_session_sender.c, not the earlier diagram")
add_bullets(s, [
    "IoTAuth Server (Java, TLS-protected) → pico_provisioner (Linux host) via the SST C API",
    ("init_SST() → get_session_key() → session_key_t { key ID, cipher key, MAC key, mode, validity }", 1),
    "Session key pushed to the Pico sender over its USB serial port (CDC-ACM, /dev/ttyACM0, 1 Mbps) — the "
    "same USB connection used for power and the console, not a separate dedicated UART wire",
    "Delivery is best-effort, not confirmed: pico_provisioner writes the MSG_TYPE_KEY frame and reports "
    "success as soon as write() returns — nothing reads back an acknowledgment from the Pico",
    ("If the write itself fails (Pico not connected), the key is saved to session_key.json for manual retry "
     "— but a successful write is never verified end-to-end. The 4-state HMAC handshake once shown here "
     "(IDLE→WAITING_FOR_YES→WAITING_FOR_ACK→WAITING_FOR_HMAC_RESP) is real code, but it lives in the retired "
     "Pi4-side receiver binaries (flash_receiver.c/dash_receiver.c), explicitly marked legacy — it never "
     "confirmed sender key delivery.", 1),
    "Keys persist in dual-slot (A/B) flash, SHA-256 integrity-checked",
    ("Operator-driven rotation ('CMD: new key' at the sender console) writes the inactive slot first and "
     "refuses to overwrite an occupied slot without -f — genuine rollback safety", 1),
    ("The automated provisioning push above writes directly to whichever slot is currently active — it does "
     "not use the inactive-slot-first pattern", 1),
], top=1.75, height=5.3, font_size=14.5, sub_font_size=12.5)
set_notes(s, "This slide was corrected during defense prep after cross-checking it against pico_provisioner.c "
             "and lifi_session_sender.c directly — two claims didn't hold up. First: the host tool is "
             "pico_provisioner, not 'keys_receiver' — that name doesn't correspond to any binary in the "
             "codebase, only to a stale diagram label. Second, and more substantive: there is no delivery "
             "confirmation handshake for the sender at all. The 4-state HMAC handshake is real, implemented "
             "code — but it belongs to the retired Pi4-side receiver stack (flash_receiver.c), where the "
             "header literally comments it 'Legacy HMAC challenge.' It was never wired to the sender path. "
             "The honest story is a best-effort push: pico_provisioner writes over the Pico's USB serial port "
             "and considers it done once the write() call returns, with no read-back proving the Pico actually "
             "applied the key. If asked how you'd know a push silently failed: you wouldn't, from the host "
             "side — you'd have to check the Pico's own console output. The dual-slot/SHA-256 storage claim "
             "held up, with one nuance worth having ready: the rollback-safe inactive-slot-first behavior is "
             "real, but only for the manual 'CMD: new key' console path, not the automated push this slide's "
             "diagram is actually depicting.")

# ==================================================================
# SLIDE 10 — Auth's role at runtime (image)
# ==================================================================
s = new_slide()
add_title(s, "Auth's Role Doesn't Stop After Provisioning")
add_image(s, FIG / "sst_auth_role.png", top=1.55, max_height=5.55)
set_notes(s,
          "Two things happen with Auth in the loop that are easy to miss if you only think of provisioning as a "
          "one-time boot-up handshake. Phase one: a device entering a room broadcasts only its non-secret 8-byte "
          "key ID over light — never the session key itself. If the receiver doesn't already recognize that ID "
          "locally, it performs what the code literally calls a roaming lookup — get_session_key_by_ID over "
          "mutual TLS to the Auth server. Auth can return the session key for a valid, registered device, or "
          "reject the request outright for an unknown or revoked one. That rejection path is a second, distinct "
          "revocation mechanism from G4: G4 revokes because the physical light signal stopped; this revokes "
          "because Auth, administratively, said no — identity-layer revocation versus physical-layer revocation, "
          "and they're complementary, not redundant. Phase two: at any point after that, the receiver can issue a "
          "live HMAC challenge to the sender over the same optical or UART link — a fresh random challenge, "
          "HMAC-SHA256 under the session MAC key, AES-GCM-encrypted response, compared locally. That's a "
          "stronger, on-demand proof of live key possession, layered on top of the passive 'this frame decrypted "
          "successfully' freshness check the whole G1/G4 evaluation is built on. If asked why both mechanisms "
          "exist: the passive heartbeat check is what's continuously measured in the evaluation section; the "
          "active challenge is available whenever a stronger, interactive proof is warranted, without adding new "
          "hardware or a new channel.")

# ==================================================================
# SLIDE 11 — Adversary Model
# ==================================================================
s = new_slide()
add_title(s, "Adversary Model")
add_bullets(s, [
    "Network control — standard Dolev-Yao adversary on RF/network links (eavesdrop, replay, delay, drop, inject)",
    "Optical observation — passive sensors capture ciphertext, tags, nonces — never plaintext or key material",
    "Relay — capture the optical signal, transport it, re-emit remotely; introduces latency δᵣₑₗₐᵧ",
    "Optical injection — a rogue light source without the session key fails AES-128-GCM tag verification",
    "Denial of service — physical obstruction, ambient-light saturation, RF jamming of the network link",
    ("The system fails closed by design: denial of signal → revocation, never false authorization", 1),
])
set_notes(s, "Five capabilities, five sentences. The one to slow down on is relay, since it sets up the G3 "
             "discussion later — plant the term delta-relay here so it isn't new vocabulary when it resurfaces. "
             "On denial of service, be explicit that this is a conscious design tradeoff: availability is "
             "sacrificed for safety, and that's stated as a non-goal in the paper, not discovered as a side "
             "effect.")

# ==================================================================
# SLIDE 12 — Security Goals table
# ==================================================================
s = new_slide()
add_title(s, "Security Goals G1–G4")
rows = [
    ["G1", "Freshness / Expiration", ("Token window Δ", INK_PRIMARY), ("Achieved", GOOD, True)],
    ["G2", "Replay Resistance", ("64-entry nonce sliding window", INK_PRIMARY), ("Achieved", GOOD, True)],
    ["G3", "Relay-Bounded Authorization", ("No timing check on data path", INK_PRIMARY), ("Not achieved", CRITICAL, True)],
    ["G4", "Revocation on Interruption", ("Absence of signal = revocation", INK_PRIMARY), ("Achieved", GOOD, True)],
]
add_table(s, ["Goal", "Property", "Mechanism", "Status"], rows,
          col_widths=[1.1, 3.6, 4.6, 2.6], top=2.0, height=3.2)
add_caption(s, "This table is the scorecard for the rest of the talk — every evaluation result maps back to one row.",
            top=5.55, bold=True, italic=False)
set_notes(s, "Present this table early and deliberately so the committee has the full scorecard before any "
             "evidence. Read the four goals aloud, including the G3 status column exactly as written — do not "
             "soften 'not achieved.' Tell them directly: you will spend the evaluation section proving the three "
             "green rows with measured data, and you will spend the discussion section explaining precisely why "
             "the red row is red, on purpose, as a named limitation rather than a discovered gap.")

# ==================================================================
# SLIDE 13 — Methodology
# ==================================================================
s = new_slide()
add_title(s, "Experimental Setup & Methodology")
add_bullets(s, [
    "Testbed: Pico H sender + Pico 2 H receiver, ~15cm separation, indoor ambient light, no shielding",
    "IoTAuth provisioning server runs on a Raspberry Pi 4 under Linux, used only at session-key setup — not part of the runtime data path",
    "Frame-level protocol logic (CRC, replay, decrypt) runs on the deployed receiver firmware (receiver_pico/src/main.c)",
    ("The Δ-based revocation decision itself was computed by an external monitoring script at measurement time", 1),
    ("That decision logic has since been ported directly into the receiver firmware as well (presence_check_decay())", 1),
    "Re-confirming the reported numbers against this on-device decision is named explicitly as pending work, not silently assumed",
])
set_notes(s, "State the measurement-provenance caveat proactively and plainly, in the same tone as every other "
             "sentence on this slide — don't let your delivery shift when you get to it. The frame-level parsing "
             "and crypto were always on-device; only the revocation *decision* (the Δ timer) was external at "
             "measurement time, mirroring the same logic that's now been moved on-device. If pressed: this was "
             "a deliberate sequencing choice — get clean, directly-instrumented protocol events first, then move "
             "the decision itself on-device once the wire-level logic was validated.")

# ==================================================================
# SLIDE 13b — Link Geometry / Alignment Tolerance
# ==================================================================
s = new_slide()
add_title(s, "Link Geometry — Alignment Tolerance",
          "How much lateral misalignment the testbed's ~15cm separation actually tolerates")
add_image(s, FIG / "cone_geometry.png", top=1.55, max_height=4.4)
add_caption(s, "Free-intercept fit: half-angle ≈ 4.2° (R²=0.997) — the naive origin-forced fit "
               "overstates the cone at ≈6.1° and its residuals drift systematically.",
            top=6.1, bold=True, italic=False)
set_notes(s, "Six manually measured (distance, lateral-offset) pairs bounding the LED emission / photodiode "
             "acceptance cone, from 6 to 21 inches. Two fits are shown deliberately: a free-intercept line, "
             "which is the honest model, and a forced-through-origin line, which is the naive 'point source' "
             "assumption. The origin-forced fit looks fine in isolation (R²=0.986) but its residuals are "
             "systematically positive at short range and negative at long range — that's not noise, it's "
             "curvature the model can't capture, because the emitter/receiver aperture has real physical size "
             "even at zero distance. The free-intercept fit's ~0.55in intercept is that aperture term. Land on "
             "the practical point: at the ~15cm (~5.9in) separation used throughout the evaluation testbed, the "
             "measured tolerance is about 1 inch of lateral slop — comfortably wide for a fixed benchtop rig, "
             "but this is exactly the number that would need to shrink and be re-characterized for the mobile "
             "robotic-platform future-work item, where the LED and receiver are no longer rigidly fixtured.")

# ==================================================================
# SLIDE 14 — G4 result (image)
# ==================================================================
s = new_slide()
add_title(s, "G4 — Revocation Latency on Occlusion")
add_image(s, FIG / "revocation_latency.png", top=1.55, max_height=4.7)
add_caption(s, "Intrinsic time-to-revoke: 15.11s ± 0.06s — essentially exactly Δ (15s) plus fixed overhead.",
            top=6.35, bold=True, italic=False)
set_notes(s,
          "Two strip plots: time-to-revoke on the left, time-to-reverify on the right, two trial batches each. "
          "Blue is the as-deployed condition, 5-second external-monitor polling — 21 trials. Orange is fine-grained "
          "0.2-second polling — 20 trials, run to separate the mechanism's real latency from polling artifacts. "
          "The blue row is bimodal, clustering near 15.7 and 18.2 seconds — that's aliasing against the "
          "5-second poll interval, not the protocol misbehaving. The orange row proves it: once polling is "
          "fine-grained, time-to-revoke collapses to a tight cluster at 15.11 seconds, standard deviation six "
          "hundredths of a second — essentially exactly the 15-second freshness window plus about a tenth of "
          "a second of fixed processing overhead. That's the G4 evidence: revocation on interruption, enforced "
          "precisely. On the right, re-verification after unblocking is consistently under 1.5 seconds in both "
          "batches, bounded above by the 2.5-second heartbeat interval as expected.")

# ==================================================================
# SLIDE 15 — Delta sensitivity (image)
# ==================================================================
s = new_slide()
add_title(s, "Δ-Sensitivity to Benign Traffic")
add_image(s, FIG / "delta_sensitivity.png", top=1.55, max_height=4.7)
add_caption(s, "0.0% false-revoke rate at every candidate Δ tested — but this session can't rule out a "
               "smaller crossover under degraded conditions.", top=6.35, bold=True, italic=False)
set_notes(s,
          "This experiment asks whether Delta equals 15 seconds is actually justified, or just a number someone "
          "picked. 959 gaps between valid frames over a 40-minute benign session, checked against six candidate "
          "Delta values from 3 to 30 seconds. The honest result: every gap clustered within about 20 "
          "milliseconds of the 2.5-second heartbeat — that spike on the left — so every candidate Delta, "
          "including the most aggressive at 3 seconds, comes back at zero percent false-revoke. Say directly why "
          "this isn't as strong as it looks: one traffic pattern, a mechanically regular heartbeat, doesn't "
          "contain the irregular gaps a real degraded environment would produce, so there's no crossover to find "
          "in this data. What it does establish is a floor — Delta=15s costs nothing measured against this "
          "traffic. Full parametric justification under degraded conditions is future work, stated as such in "
          "the paper rather than let the clean number imply more than it shows.")

# ==================================================================
# SLIDE 16 — G2 result (image)
# ==================================================================
s = new_slide()
add_title(s, "G2 — Replay Rejection & the Window-Boundary Limitation")
add_image(s, FIG / "replay_boundary.png", top=1.55, max_height=4.7)
add_caption(s, "Immediate replay rejected; the same frame is re-accepted only after being deliberately evicted "
               "from a shrunk 2-slot window.", top=6.35, bold=True, italic=False)
set_notes(s,
          "Two results in one timeline. At t plus zero, a legitimate heartbeat frame arrives and gets captured "
          "and pinned. At t plus 0.21 seconds, replaying that exact frame is rejected — the green marker — "
          "because its nonce is already in the replay window. That's G2 working as designed. Then the nonce "
          "window is deliberately shrunk from its production size of 64 down to 2 slots, to make a boundary "
          "condition observable in a short test instead of needing thousands of frames. Two more legitimate "
          "frames cycle through, evicting the original nonce, and at t plus 5 seconds the same frame is replayed "
          "a third time — the amber X — and this time it's accepted and decrypted. That's not a bug found "
          "by accident, it's a documented, real boundary: replay protection is bounded by window capacity, not "
          "permanent. In production at 64 slots, an attacker needs 64 fresh legitimate frames to cycle through "
          "before a captured frame becomes replayable again, which bounds how long it stays a live threat — "
          "but the limitation is real, and it's on the same slide as the positive result, not buried in a "
          "footnote. If asked why a 2-slot window: say plainly it's artificially small only to make the boundary "
          "observable quickly — the mechanism and the limitation are identical at any window size, just slower "
          "to trigger at 64.")

# ==================================================================
# SLIDE 17 — Communication performance (image)
# ==================================================================
s = new_slide()
add_title(s, "Communication Performance")
add_image(s, FIG / "baud_sweep.png", top=1.55, max_height=4.7)
add_caption(s, "Higher gain (470kΩ) beats higher bandwidth (100kΩ) at 15cm — SNR margin, not TIA "
               "bandwidth, limits close range.", top=6.35, bold=True, italic=False)
set_notes(s,
          "The baud-rate sweep behind the bandwidth-range tradeoff in the paper. Blue is 470 kilohms — higher "
          "gain, lower bandwidth, 2-volt output swing. Orange is 100 kilohms — lower gain, higher bandwidth, "
          "400-millivolt swing. Both hold 100% frame success up to their ceiling — 300 kilobaud for 470k, 250 "
          "kilobaud for 100k — then the orange line shows measurable degradation, 96% and 86%, before both "
          "configurations hit a hard collapse where control messages themselves get corrupted. The "
          "counterintuitive result: at this close range, the higher-gain 470k configuration outperforms the "
          "higher-bandwidth 100k configuration. SNR margin from the larger voltage swing is the limiting factor "
          "at short range, not the TIA's raw bandwidth. The classic bandwidth-range tradeoff should favor the "
          "higher-bandwidth config once you're far enough that photocurrent drops and the 400-millivolt swing "
          "stops clearing the comparator's noise floor — that crossover distance hasn't been measured yet, "
          "which is why it's future work rather than a claim in this thesis.")

# ==================================================================
# SLIDE 18 — Security validation recap
# ==================================================================
s = new_slide()
add_title(s, "Security Validation — Recap")
rows = [
    ["G1", "Freshness / Expiration", ("Achieved", GOOD, True), "15.11s ± 0.06s intrinsic ≈ Δ + 0.11s"],
    ["G2", "Replay Resistance", ("Achieved", GOOD, True), "Immediate rejection confirmed; window-bounded (documented)"],
    ["G3", "Relay-Bounded Auth", ("Not achieved", CRITICAL, True), "Named limitation — no timing check on data path"],
    ["G4", "Revocation on Interruption", ("Achieved", GOOD, True), "Follows directly from G1's measured latency"],
]
add_table(s, ["Goal", "Property", "Status", "Evidence"], rows,
          col_widths=[1.0, 3.1, 2.1, 5.7], top=2.15, height=3.2)
set_notes(s, "This closes the loop back to slide 10's scorecard, now with evidence in the last column instead of "
             "just mechanism. Read it goal by goal, evidence-first: this is the moment to sound confident, "
             "because everything in the 'evidence' column is a number you already showed them on the previous "
             "four slides — you're not introducing anything new here, just tying it together.")

# ==================================================================
# SLIDE 19 — G3 discussion
# ==================================================================
s = new_slide()
add_title(s, "G3 — A Named Limitation, Not a Discovered Gap")
add_bullets(s, [
    "G1 bounds how long a captured, later-resent frame stays useful. G2 rejects a byte-identical resend outright.",
    "Neither mechanism examines WHEN a frame arrived — only whether its nonce is fresh and previously unseen",
    "A live relay forwarding genuinely fresh frames in real time passes every check the receiver performs:",
    ("Nonce never seen before → G2 passes", 1),
    ("GCM tag valid, ciphertext untouched → confidentiality/integrity passes", 1),
    ("t_last continuously refreshed by relayed-but-fresh frames → G1 passes", 1),
    "Verified directly in the receiver implementation: the LiFi data-frame path checks only CRC and byte-exact nonce identity — no timestamp, no round-trip bound",
    "What closing this gap requires: a Brands–Chaum-style distance-bounding challenge-response, binding authorization to measured round-trip time",
])
set_notes(s, "This is the slide to deliver in exactly the same tone as every positive result before it — that "
             "consistency is what reads as rigor instead of a hedge. The core distinction, if asked to restate "
             "it: G1's window bounds a captured, no-longer-live frame; it does not bound relay latency, because a "
             "continuously operating relay keeps t_last refreshed indefinitely with genuinely fresh, just-late "
             "frames. That confusion — mistaking G1's expiration window for a relay bound — is the single "
             "most likely follow-up question; have this paragraph ready verbatim.")

# ==================================================================
# SLIDE 20 — Limitations
# ==================================================================
s = new_slide()
add_title(s, "Limitations")
add_bullets(s, [
    "Line-of-sight requirement — dust, fog, or vibration can trigger unnecessary revocation if Δ is set aggressively",
    "No relay resistance (G3) — a live relay of genuinely fresh frames passes every current check",
    "Single-channel reception — four LED channels transmit, but only one photodiode receives",
    "Ambient light sensitivity — partial mitigation via programmable DAC threshold, no full adaptive control yet",
    "No hardware key protection — RP2040/RP2350 lack a TPM or secure element; keys are extractable with physical access + debugger",
    "Revocation-latency measurements are pending re-confirmation against the on-device presence decision, ported after these numbers were collected",
])
set_notes(s, "Six limitations, read at the same pace and volume as everything else in the talk — don't rush "
             "through this slide to get past it. The last bullet is the one to make sure lands clearly: it's not "
             "a generic hedge, it's a specific, checkable pending item with a known reason to be cautious. During "
             "eval prep, debugging surfaced two real, previously-invisible protocol bugs on the receiver — "
             "a framing bug that broke on binary ciphertext containing 0x0A/0x0D bytes, and a key-push tool "
             "silently writing to the wrong UART, so key rotation no-op'd for an unknown stretch of time despite "
             "reporting success. Both are fixed. The reported occlusion-test numbers were collected before the "
             "revocation decision itself was moved on-device (presence_check_decay()) — the frame-level logic "
             "was already on-device throughout, so this is a precision re-check, not a rebuild. See the backup "
             "slide on the two bugs found during eval prep if asked how you know your measurements are "
             "trustworthy.")

# ==================================================================
# SLIDE 21 — Future Work
# ==================================================================
s = new_slide()
add_title(s, "Future Work", "Ordered by priority — not a wish list")
col_top = 2.05
tb = s.shapes.add_textbox(Inches(0.75), Inches(1.65), Inches(5.6), Inches(0.4))
p = tb.text_frame.paragraphs[0]
r = p.add_run()
r.text = "CLOSES A GAP NAMED IN THIS TALK"
r.font.size = Pt(12.5)
r.font.bold = True
r.font.name = FONT
r.font.color.rgb = hexc(CRITICAL)
add_bullets(s, [
    "Distance-bounding challenge-response for G3 — measured 3–6ms processing latency suggests ample timing margin",
    "Confirm revocation-latency measurements against the on-device presence decision (receiver_pico/src/main.c), not just the external monitor",
    "Formal protocol verification (Tamarin / ProVerif) of G1–G4 — not yet attempted, explicitly scoped out",
    "Time-bounded key validity enforcement — abs_validity/rel_validity fields exist but are not yet enforced",
], top=col_top, left=0.75, width=5.6, height=4.6, font_size=15, sub_font_size=13)

tb2 = s.shapes.add_textbox(Inches(6.95), Inches(1.65), Inches(5.6), Inches(0.4))
p2 = tb2.text_frame.paragraphs[0]
r2 = p2.add_run()
r2.text = "EXTENDS THE SYSTEM"
r2.font.size = Pt(12.5)
r2.font.bold = True
r2.font.name = FONT
r2.font.color.rgb = hexc(AQUA)
add_bullets(s, [
    "Higher data rates — 47kΩ/1pF feedback network targeting 2–4 Mbps",
    "Optical concentration — collimating lenses (TX), CPC/Fresnel concentrators (RX) to extend range",
    "Hardware key protection — TPM or secure element (e.g., ATECC608)",
    "Robotic platform demonstration — 3pi+ integration already designed into the receiver PCB",
], top=col_top, left=6.95, width=5.6, height=4.6, font_size=15, sub_font_size=13)

set_notes(s, "Two columns, and the grouping itself is the point: the left column closes gaps you already admitted "
             "to earlier in the talk — G3's distance-bounding, the on-device confirmation, formal verification — while "
             "the right column extends capability the system already has. Say the grouping out loud rather than "
             "just reading bullets: it signals you know what matters most next, rather than reciting a flat wish "
             "list.")

# ==================================================================
# SLIDE 22 — Contributions / Conclusion
# ==================================================================
s = new_slide()
add_title(s, "Contributions")
add_bullets(s, [
    "First system to formalize and evaluate continuous, time-bounded optical-presence authorization with automatic revocation",
    "Working physical prototype: AES-128-GCM authenticated encryption at 1 Mbps over free-space optical, custom analog front-end",
    "~$82 total hardware cost — two orders of magnitude below commercial LiFi access-point pricing",
    "G1 (freshness), G2 (replay resistance), and G4 (revocation on interruption) empirically demonstrated with real measured data",
    "G3 (relay-bounded auth) reported as an explicit, named limitation — verified directly against the implementation, not inferred",
    "Symbolic verification explicitly scoped as future work — stated openly, not left for a reviewer to find",
])
set_notes(s, "Closing content slide before thank-you. Keep this tight and confident — every bullet here is "
             "something you already showed evidence for earlier in the talk, so this should feel like a summary, "
             "not a new argument. The last two bullets are there on purpose: naming exactly what wasn't done, "
             "right next to what was, is itself part of the contribution.")

# ==================================================================
# SLIDE 23 — Thank you / Questions
# ==================================================================
s = new_slide()
rect = s.shapes.add_shape(MSO_SHAPE.RECTANGLE, 0, 0, SLIDE_W, SLIDE_H)
rect.fill.solid()
rect.fill.fore_color.rgb = hexc(INK_PRIMARY)
rect.line.fill.background()
tb = s.shapes.add_textbox(Inches(1.0), Inches(2.8), Inches(11.3), Inches(1.2))
p = tb.text_frame.paragraphs[0]
r = p.add_run()
r.text = "Thank you — Questions"
r.font.size = Pt(40)
r.font.bold = True
r.font.name = FONT
r.font.color.rgb = hexc("FFFFFF")
tb2 = s.shapes.add_textbox(Inches(1.0), Inches(3.75), Inches(11.3), Inches(1.5))
tf2 = tb2.text_frame
tf2.word_wrap = True
p2 = tf2.paragraphs[0]
r2 = p2.add_run()
r2.text = "Jose Felix · KIM Labs, Arizona State University"
r2.font.size = Pt(15)
r2.font.name = FONT
r2.font.color.rgb = hexc("C3C2B7")
p3 = tf2.add_paragraph()
r3 = p3.add_run()
r3.text = "github.com/asu-kim/lifi-auth"
r3.font.size = Pt(13)
r3.font.name = FONT
r3.font.color.rgb = hexc("898781")
set_notes(s, "Thank the committee again. Before opening the floor, briefly restate the scorecard from memory in "
             "one breath: freshness, replay resistance, and revocation on interruption were measured and hold; "
             "relay-bounding does not, and that's named on purpose. Then stop talking and take questions — "
             "the backup slides after this cover BOM/cost, the frame format, the TIA tuning table, and a "
             "one-slide answer to the G1-vs-G3 question specifically, pull them up if asked.")

# ==================================================================
# BACKUP SECTION
# ==================================================================
add_divider("Backup", "Reference material — pull up on demand during Q&A")

# ---- B1: BOM / cost ----
s = new_slide()
add_title(s, "Backup — Bill of Materials", "Verified DigiKey invoices #120241715, #121892487 — total ≈ $82")
rows = [
    ["Sender MCU", "Pico H, RP2040", "$5.00"],
    ["Gate Driver ×4", "TC4420COA", "$1.80 ea."],
    ["MOSFET ×4", "SIR800DP-T1-GE3", "$0.88 ea."],
    ["DC-DC Module", "PTN78000WAH (12V→5V)", "$24.58"],
    ["LED Array", "SFH 4715A-CBDB", "$2.50"],
    ["Receiver MCU", "Pico 2 H, RP2350", "$6.00"],
    ["TIA", "OPA381AIDGKT", "$2.93"],
    ["Photodiode", "SFH 2400-Z", "$0.79"],
    ["Comparator", "TLV3501AIDBVR", "$2.64"],
    ["DAC", "MCP4725A1T-E/CH", "$1.27"],
    ["LiPo Charger", "BQ24074RGTR", "$2.50"],
    ["Buck-Boost Reg.", "TPS63031DSKR", "$2.14"],
    ["Passives, PCBs (×2)", "R/C/fuses/JST + fab", "≈ $25"],
]
add_table(s, ["Component", "Part Number", "Cost"], rows, col_widths=[3.5, 5.5, 2.5], top=1.6, height=5.1)
set_notes(s, "Pull up only if asked about cost or component selection. The single largest line item is the "
             "PTN78000WAH DC-DC module at $24.58, chosen for research-prototype convenience — a production "
             "design substituting a discrete buck regulator would bring total cost under $60.")

# ---- B2: Frame format ----
s = new_slide()
add_title(s, "Backup — Wire Frame Format")
rows = [
    ["Preamble", "4 B", "0xAB 0xCD 0xEF 0x12 — synchronization marker"],
    ["Type", "1 B", "Message type identifier"],
    ["Length", "2 B", "Ciphertext length N"],
    ["Nonce", "12 B", "8B random boot salt + 4B monotonic counter"],
    ["Ciphertext", "N B", "AES-128-GCM output, equal length to plaintext"],
    ["GCM Tag", "16 B", "Authentication tag — verified before any plaintext release"],
    ["CRC16", "2 B", "CRC16-CCITT, poly 0x1021, init 0xFFFF"],
]
add_table(s, ["Field", "Size", "Notes"], rows, col_widths=[2.6, 1.6, 7.3], top=1.9, height=3.6)
add_caption(s, "64-entry nonce sliding window checked against every incoming nonce BEFORE decryption is attempted.",
            top=5.85, bold=True, italic=False)
set_notes(s, "Reference for the wire format question. The key point if asked: replay checking happens before "
             "the GCM primitive is ever invoked — a duplicate nonce is rejected without spending the "
             "decryption cost, let alone releasing plaintext.")

# ---- B3: Why no symbolic verification ----
s = new_slide()
add_title(s, "Backup — Why No Symbolic Verification Yet")
add_bullets(s, [
    "G1–G4 are amenable to formal verification with Tamarin or ProVerif — identified explicitly as future work",
    "A full model covers SST HS1–HS3 mutual auth + the AES-GCM data channel + two independent replay windows",
    "That's substantial, specialized modeling work — not something to fit alongside the empirical evaluation chapter",
    "An incomplete or buggy formal model would read worse to a committee than no formal model plus an honest claim",
    "Findings in this thesis are stated as empirically demonstrated, not formally proven — stated as such throughout, not implied",
])
set_notes(s, "Only pull this up if asked directly why there's no Tamarin/ProVerif model. The core answer: this "
             "was a deliberate scoping decision, not an oversight — a rushed or incomplete formal model would "
             "be worse evidence than an honest 'empirically demonstrated, not formally proven' claim, which is "
             "exactly the language used throughout the paper.")

# ---- B4: TIA tuning table ----
s = new_slide()
add_title(s, "Backup — TIA Feedback Component Tuning")
rows = [
    ["470kΩ", "5pF", "68", "300,000", "≈2.0V"],
    ["100kΩ", "5pF", "318", "250,000", "≈400mV"],
    ["100kΩ", "1pF", "1,590", "TBD (future work)", "≈400mV"],
    ["47kΩ", "1pF", "3,386", "TBD (future work)", "≈200mV"],
]
add_table(s, ["R_f", "C_f", "BW (kHz)", "Max Baud (measured)", "Output Swing"], rows,
          col_widths=[1.9, 1.6, 2.0, 3.6, 2.6], top=2.1, height=3.0)
add_caption(s, "Only the first two rows are experimentally validated; the lower-resistance rows target >1Mbps and are future work.",
            top=5.4, bold=True, italic=False)
set_notes(s, "Reference table if pressed on the bandwidth-range tradeoff numbers or asked what's next for higher "
             "data rates. Be clear that only the first two rows have measured data — the 100k/1pF and "
             "47k/1pF rows are calculated bandwidths, not measured baud ceilings.")

# ---- B5: G1 vs G3 one-slide answer ----
s = new_slide()
add_title(s, "Backup — “Isn't Δ Basically a Relay Bound?”", "The single most likely question — answer verbatim")
add_bullets(s, [
    "No. G1's window Δ bounds how long a CAPTURED, NO-LONGER-LIVE frame remains useful.",
    "A live relay never captures-and-resends stale bytes — it forwards each frame through in real time, added latency and all.",
    "Every relayed frame is genuinely fresh from the receiver's point of view: new nonce, valid tag, arrives within Δ.",
    "A continuously operating relay keeps t_last refreshed indefinitely — Δ never expires against it.",
    "G1 and G2 answer “is this frame new and unseen?” — G3 would require answering “how long did this frame take to arrive?”, which nothing in the current implementation checks.",
])
set_notes(s, "Memorize this slide's content, don't read it verbatim in the room — but have it exact in your "
             "head. The tightest one-sentence version if you need to compress under time pressure: freshness "
             "answers 'is this new,' relay-bounding answers 'how long did this take to get here,' and this "
             "system only answers the first question.")

# ---- B6: Auth integration detail — three receiver variants ----
s = new_slide()
add_title(s, "Backup — Three Receiver Variants, Three Auth Interaction Models")
rows = [
    ["keys_receiver", "“The Manager”", "Fetches a session key from Auth at startup, pushes it to the sender"],
    ["flash_receiver", "Combined session app", "Chat + file transfer + HMAC verification + key management, combined"],
    ["ask_receiver", "“The Detective”", "No keys at start — listens for an optical key-ID broadcast, then queries Auth"],
]
add_table(s, ["Binary", "Role", "Auth interaction"], rows,
          col_widths=[2.6, 2.2, 7.7], top=1.85, height=2.2)
add_caption(s, "ask_receiver implements the roaming/discovery model shown on the Auth-runtime slide — it is the "
               "concrete code behind that diagram, not a conceptual simplification.",
            top=4.3, bold=True, italic=False)
add_bullets(s, [
    "Message types relevant to Auth interaction: MSG_TYPE_KEY_ID_ONLY (discovery), MSG_TYPE_CHALLENGE / RESPONSE (runtime HMAC), MSG_TYPE_KEY (provisioning push)",
    "All three variants share the same SST C API calls underneath: init_SST(), get_session_key(), get_session_key_by_ID()",
], top=5.05, height=2.0, font_size=13.5)
set_notes(s, "Only needed if asked to justify why three separate receiver binaries exist rather than one. The "
             "honest answer: they correspond to three different Auth interaction models that were useful to "
             "isolate during development and evaluation — a proactive key-manager, a full combined session "
             "app, and a pure discovery/roaming client — not three independent implementations of the security "
             "logic, which is shared underneath via the same SST C API and the same replay/freshness code paths.")

# ---- B7: SST/IoTAuth publication lineage ----
s = new_slide()
add_title(s, "Backup — SST/IoTAuth Publication Lineage & Team")
add_bullets(s, [
    "IoTDI '17 and FiCloud '16 — secure network architecture with Auth-mediated key distribution for IoT",
    "IT Professional '17 — the “locally centralized, globally distributed” authentication/authorization architecture",
    "ACM TIOT '20 — secure migration as a recovery mechanism from denial-of-service attacks or failures",
    "SoftwareX '23 — the C API this project links against (sst-c-api)",
    "SIGMOD '25 / Mid4CC '23 — SST applied to large-scale key-value storage and decentralized file-system access control",
    "Most in-depth technical reference: a 2017 UC Berkeley Ph.D. dissertation on the underlying architecture",
    "Active contributors (per the project's own repository): Hokeun Kim, Dongha Kim, Carlos Beltran Quinonez, Sunyoung Kim, Jose Felix — all ASU",
])
set_notes(s, "Pull this up only if a committee member asks 'what is SST, really' in more depth than the main "
             "slide covers. The point of reading a few venue names aloud is not to name-drop — it's to establish "
             "that this is externally reviewed, multi-year infrastructure, not something assembled for this "
             "thesis. The last bullet is worth reading slowly: you are named, by GitHub handle, as an active "
             "contributor to the repository this project depends on.")

# ---- B8: Two real bugs found during eval prep ----
s = new_slide()
add_title(s, "Backup — Two Real Bugs Found During Eval Prep",
          "Why the debug-receiver measurements should be trusted, not just assumed correct")
rows = [
    ["Framing bug", "Newline-terminated framing broke on any real encrypted frame — binary ciphertext routinely contains 0x0A/0x0D bytes", "Replaced with proper length-prefixed binary parsing matching the actual wire format"],
    ["Silent key-rotation no-op", "Key-push tool targeted the Pico's USB console instead of the hardware UART the firmware's key handler actually reads from", "Reported “success,” changed nothing — for an unknown, possibly long stretch of the project's history"],
]
add_table(s, ["Bug", "What was wrong", "Impact / fix"], rows, col_widths=[2.2, 5.0, 4.6], top=1.85, height=2.6)
add_caption(s, "Both found and fixed on the debug receiver during 2026-07-25 eval prep, before any of the reported measurements were taken.",
            top=4.75, bold=True, italic=False)
add_bullets(s, [
    "The receiver firmware's on-device revocation decision (presence_check_decay()) was added after these numbers were measured",
    "Re-running the occlusion test against it, to confirm the reported latency carries over unchanged, is a specific, named check — not a generic caveat",
], top=5.3, height=1.7, font_size=14)
set_notes(s, "Pull this up if asked how you know the eval numbers are trustworthy, or why the on-device "
             "confirmation gap in Limitations is called out so specifically rather than as generic future work. "
             "The honest framing: these bugs are evidence the instrumentation was rigorous enough to catch real, "
             "previously-invisible problems — including one, the silent key-rotation no-op, that had been "
             "reporting false success for an unknown stretch of time before anyone noticed. The revocation-latency "
             "numbers were collected while that same decision logic lived in an external monitoring script; it's "
             "since been ported onto the receiver itself, and re-measuring against it is flagged by name rather "
             "than left as a vague 'pending' bullet.")

prs.save(OUT)
print(f"Wrote {OUT}  ({len(prs.slides)} slides)")
