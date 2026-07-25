# Thesis Plan — Presence/Freshness Evaluation

## Where things stand

Working hardware/software, self-built:
- **Sender**: `sender/dashboard/app.py` (dashboard/control plane) + `sender/src/lifi_session_sender.c` (Pico firmware — AES-128-GCM message encryption, SST HS1–HS3 mutual-auth handshake, PIO-driven 4-channel LED TX)
- **Receiver**: `receiver/src/dash_receiver.c` (Pi4 — decodes UART/LiFi frames, verifies HS1–HS3, decrypts, reports to `app.py` over WiFi via HMAC-signed HTTP: `/pi4_frame`, `/pi4_status`, `/challenge`, `/force_key`, `/set_baud`, `/status`)

README's stated research goal (G1–G4): authorization tied to **continuous physical presence** over an optical channel — freshness, replay resistance, relay-bounded auth, revocation on interruption — verified via symbolic tools (Tamarin/ProVerif, not yet done).

**Decision: do NOT build a second sender/receiver board.** Ruled out physical-layer transmitter fingerprinting (RF-fingerprinting-style novelty angle) as the headline contribution — it fundamentally needs an impostor transmitter to prove discrimination, and building one conflicts with the "no new boards" constraint. Deprioritized in favor of the plan below, which uses only the existing single sender+receiver pair.

**Scope correction needed:** the README currently claims symbolic verification of G1–G4. That hasn't been done. Either do a lightweight formal model, or explicitly rescope the thesis claims to "empirically demonstrated," not "formally proven" — say this openly in the writeup, don't let a committee member find the gap first.

**Debugging session (2026-07-25) found and fixed two previously-invisible protocol
bugs** in the sender (`lifi_session_sender.c`) + debug receiver (`receiver_pico/src/main.c`)
pair, worth citing as validation evidence in the writeup:
1. The debug receiver's newline-terminated framing broke on any real encrypted frame
   (binary ciphertext routinely contains `0x0A`/`0x0D` bytes) — replaced with proper
   length-prefixed binary parsing matching the actual wire format.
2. `pico_provisioner`'s key push targeted the Pico's USB console instead of the
   hardware UART (`UART_ID`/GP5) the firmware's `MSG_TYPE_KEY` handler actually reads
   from — key rotation silently no-op'd (reported "success," changed nothing) for
   an unknown but possibly long stretch of the project's history. Fixed by teaching
   the USB console path to also accept a preamble-framed `MSG_TYPE_KEY` payload.

Since `dash_receiver.c` consumes the exact same wire protocol and `session_key.json`,
there's a real chance it has the same class of issue and has never been exercised
with a correctly-synced key. **Not yet verified** — flagged here so it isn't
mistaken for "the Pi4 path is known-good."

## The plan: 3 experiments, all on existing hardware, no new boards

### 1. Shadowing / occlusion test → proves G4 (revocation on interruption)

**Measures:** time from physically blocking the LED→photodiode path to the dashboard flipping `mac_key_verified` False→True in `app.py` (`pi4_health_monitor()`, `LIVENESS_WINDOW_S = 15`).

**Prerequisite (small firmware change, no new hardware):** `lifi_session_sender.c` currently only transmits when a user types a message. Add an auto-heartbeat — send a tiny encrypted frame every ~2–3s when idle, using the exact same encrypt/frame/send path already in the sender loop — so there's continuous traffic to interrupt.

**Procedure:**
1. Reach steady-state `verified: true`.
2. At marked t=0, occlude the beam.
3. Timestamp the `mac_key_status: {verified:false}` socketio event.
4. Unblock; timestamp `verified:true` reappearing.
5. Repeat 20–30 trials.

**Output:** mean/std time-to-revoke (should cluster ~15s + processing latency) and time-to-reverify. This is the empirical G4 result.

### 2. Δ (LIVENESS_WINDOW_S) tuning

**Measures:** whether 15s is justified, vs. arbitrary.

**Procedure:**
1. With the heartbeat running, log gaps between consecutive valid frames over a long benign session (30–60 min, varying alignment/ambient light) → histogram of normal gap lengths.
2. For candidate Δ values (3, 5, 10, 15, 30s), compute the fraction of benign gaps that would exceed each Δ → false-revoke rate per Δ.
3. Cross-reference against experiment 1's occlusion-detection latency → attacker-dwell-time cost per Δ.
4. Plot both curves vs. Δ; the crossover point is the data-justified choice, replacing the hardcoded constant.

Reuses the same instrumentation as #1.

### 3. Replay test → proves G2 (replay resistance) — swap-in for a relay-attack demo

A live relay demo needs a capture-and-retransmit rig, which counts as new hardware — ruled out. Substitute: exploit the replay protection **already implemented** and confirmed in `dash_receiver.c:2082` — `replay_window_seen(&rwin, nonce)` — guarding every decoded `MSG_TYPE_ENCRYPTED`/`MSG_TYPE_FILE` frame (separate from the WiFi control-channel replay window used for `/force_key` etc.).

**Procedure:**
1. Capture one valid frame's raw bytes off the UART bridge (tee the bytes via existing debug logging or a serial sniff).
2. Re-inject the identical bytes into the receiver's UART input later — no LED, no second board, just replaying already-captured ciphertext+nonce+tag on the same physical UART line.
3. Confirm rejection via `replay_window_seen`; log it.
4. Boundary check: the window guarding LiFi data frames (`rwin`, `dash_receiver.c:1169`) holds `cap = NONCE_HISTORY_SIZE = 64` slots (`protocol.h:33`) — **not 16**. (16 is a *different* window, `g_req_replay_window` at `dash_receiver.c:801`, which guards the separate WiFi control-channel HTTP requests, not LiFi data.) Test what happens once legitimate traffic cycles past `cap` frames since the captured one (does the old nonce become "unseen" again and get accepted?). Document this as a real limitation, not just the happy path.

## What this gives the thesis

Three evaluation results (revocation latency, a data-justified Δ, replay resistance) from hardware already built and working, plus one small firmware addition (heartbeat) and a logging/data-collection harness on the dashboard side. That's a real empirical evaluation chapter backing G1/G2/G4.

**G3 (relay-bounded) — resolved (2026-07-25): not achieved by the current implementation, not just untested.**
Verified directly in `dash_receiver.c`: the LiFi/UART data-frame path (`MSG_TYPE_ENCRYPTED`/`MSG_TYPE_FILE`,
guarded by `rwin` at line 2082) checks *only* CRC and byte-exact nonce identity — no
timestamp, no round-trip bound, nothing timing-related. (Contrast with the *separate*
WiFi control-channel path, `verify_signed_request()` around line 815, which does
enforce a 30s clock-skew check via `X-SST-Timestamp` — but that guards `/force_key`
etc., not LiFi frames.)

Consequence: a live relay that forwards genuinely fresh frames from the real sender
in real time — not a captured-and-resent replay, an actual pass-through — would clear
every check the system has. CRC passes (bytes weren't corrupted). The replay window
passes (the nonce was never seen before; it's a live forward, not a resend). The GCM
tag passes (correct key, valid auth). **G2 (replay resistance) and G3 (relay-bounded)
are genuinely different threat models and must not be conflated in the writeup** —
G2 defends against resending stale captured bytes; G3 would require bounding the
*time* a frame took to arrive, which nothing in this system does.

Retracting the earlier "argue it analytically from the system's own processing
latency" idea — that doesn't hold up. The debug receiver's measured `elapsed_ms`
(3–6ms, preamble-lock to full CRC-validated frame) is real data, but it isn't a
*security* argument for G3 unless something in the protocol actually rejects frames
for arriving too slowly, and nothing does. Its only honest use is a smaller,
future-work-supporting footnote: *"end-to-end processing latency is on the order of
single-digit milliseconds, suggesting a tight round-trip timing bound (distance-bounding,
à la Brands–Chaum) is plausible to add without a hardware redesign — but doing so is
future work, not part of the implemented system."*

**Decision: scope G3 as an explicit, named limitation**, not an analytical proof
substituting for a test. This is a stronger, more defensible position for a defense
than a hand-wavy latency-budget argument would have been — being precise about what
the system does and doesn't defend against reads as rigor, not as a gap.

## Symbolic verification (Tamarin/ProVerif) — scoping decision

Per the "Scope correction needed" note above: the README currently claims symbolic
verification of G1–G4 that was never done. **Decision (2026-07-25): rescope rather
than attempt a rushed model.** A real Tamarin/ProVerif model of this protocol
(SST HS1–HS3 mutual auth + the AES-GCM data channel + the two independent replay
windows) is substantial, specialized modeling work — not something to fit in
alongside the empirical chapter, and an incomplete/buggy formal model would likely
read worse to a committee than no formal model plus an honest claim. Writeup
language: findings in this thesis are **empirically demonstrated** (G4 revocation
timing, G2 replay resistance, both with real measured data), not formally proven;
symbolic verification of the full protocol is named explicitly as future work,
alongside G3's distance-bounding gap above — don't let a committee member surface
either omission first.

## Next steps (pick up here next session)

**Scope note (2026-07-25):** rather than verify/fix the Pi4 path first, the pieces
below were prototyped against the simpler, already-instrumented debug receiver
(`receiver_pico/src/main.c`) instead of `dash_receiver.c` — same wire protocol, no
WiFi/HMAC layer involved, faster to iterate on. Whether this becomes the actual
reported eval platform or a validation step before porting to the Pi4 is still open.

- [x] Write the heartbeat auto-send into `lifi_session_sender.c` (2.5s idle interval, reuses the existing encrypt/frame/send path)
- [x] Build the replay-test mechanism: nonce replay window on `receiver_pico` (mirrors `rwin`, runtime-adjustable cap via `replaycap <n>`) + a `replay` console command that re-injects the last captured frame — no new hardware
- [x] Build the logging harness: `thesis_eval/liveness_monitor.py` mirrors `pi4_health_monitor()`/`_set_mac_key_verified()`'s exact state machine and thresholds, parses the receiver's new `[EVT] ...` lines, logs to timestamped CSV, supports manual markers via stdin
- [x] Run shadowing test against the debug receiver — 21 trials at `--poll-interval 5.0` (production default) + 20 trials at `--poll-interval 0.2` (isolates poll-granularity jitter from the intrinsic window latency). Results: **as-deployed time-to-revoke = 17.27s ± 1.25s** (bimodal ~15.7s/~18.2s, an artifact of the 5s poll cadence), **intrinsic time-to-revoke = 15.11s ± 0.06s** (tight cluster once poll jitter is minimized — essentially exactly `LIVENESS_WINDOW_S` + ~0.1s fixed overhead). Time-to-reverify ≈ 1.3–1.4s in both batches, bounded by the 2.5s heartbeat interval. Logs + analysis in `thesis_eval/logs/liveness_*_poll*.csv` (+`_analysis.txt`), computed via `thesis_eval/analyze_occlusion.py`.
- [x] **Δ-tuning: scoped down, not completed as originally planned.** Ran the 40-min benign session (`thesis_eval/logs/liveness_20260725_151932.csv`, 960 frames / 959 gaps) — result was degenerate: gaps clustered at 2.409–2.591s (98% within ±20ms of the 2.5s heartbeat), so false-revoke rate = 0.0% at every candidate Δ (3/5/10/15/20/30s). A single mechanically-regular heartbeat isn't a stand-in for the irregular real-world traffic the false-revoke analysis needs — there's no crossover to find in this data. **Decision (user, 2026-07-25): don't chase a second deliberately-perturbed session** — diminishing returns / risk of looking contrived (hand-disturbing a photodiode isn't obviously representative of real degradation either). Writeup claim is narrowed accordingly: *"Δ=15s carries zero measured false-revoke cost against heartbeat-only traffic; a full parametric justification of Δ would require characterizing gap variability under real degraded-signal conditions — left as future work."* Full report + histogram + dwell-cost table: `thesis_eval/delta_tuning.py`, artifact at https://claude.ai/code/artifact/3668a907-f91e-4f2d-a296-d5e8431a8c55.
- [x] Build + run the replay boundary test: `thesis_eval/replay_test.py` automates the whole procedure (capture+pin a real frame, immediate-replay rejection, `replaycap 2` + cycle 2 legitimate frames, replay the pinned frame again). **Confirmed both**: immediate replay rejected, and the boundary limitation reproduced — the pinned frame was accepted (and decrypted) again once evicted from a 2-slot window. Log: `thesis_eval/logs/replay_test_20260725_151700.csv`. (Needed one fix along the way: the first version replayed whichever frame was *most recently* auto-captured, not the originally-pinned one, since `last_frame` auto-updates on every real receive — added a separate `pin`/`replaypin` command pair so the frame under test stays frozen regardless of intervening traffic.)
- [ ] Decide whether to port the proven-working logic to `dash_receiver.c`/Pi4 for final reported numbers, or keep the debug receiver as the eval platform
- [x] **G3 scoping — resolved.** Verified the LiFi data-frame path has no timing/freshness check at all (only CRC + byte-exact nonce identity) — a live relay of genuinely fresh frames would pass every check the system has. G3 is scoped as an explicit, named limitation (not achieved, not just untested), clearly distinguished from G2 (replay resistance, which *is* achieved and is a different threat model). See "What this gives the thesis" above.
- [x] **Symbolic verification scoping — resolved.** Rescoped to "empirically demonstrated," not "formally proven" — a real Tamarin/ProVerif model is out of scope for this pass. See "Symbolic verification" section above.
