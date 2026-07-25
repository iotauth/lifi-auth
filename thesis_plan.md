# Thesis Plan — Presence/Freshness Evaluation

## Where things stand

Working hardware/software, self-built:
- **Sender**: `sender/dashboard/app.py` (dashboard/control plane) + `sender/src/lifi_session_sender.c` (Pico firmware — AES-128-GCM message encryption, SST HS1–HS3 mutual-auth handshake, PIO-driven 4-channel LED TX)
- **Receiver**: `receiver/src/dash_receiver.c` (Pi4 — decodes UART/LiFi frames, verifies HS1–HS3, decrypts, reports to `app.py` over WiFi via HMAC-signed HTTP: `/pi4_frame`, `/pi4_status`, `/challenge`, `/force_key`, `/set_baud`, `/status`)

README's stated research goal (G1–G4): authorization tied to **continuous physical presence** over an optical channel — freshness, replay resistance, relay-bounded auth, revocation on interruption — verified via symbolic tools (Tamarin/ProVerif, not yet done).

**Decision: do NOT build a second sender/receiver board.** Ruled out physical-layer transmitter fingerprinting (RF-fingerprinting-style novelty angle) as the headline contribution — it fundamentally needs an impostor transmitter to prove discrimination, and building one conflicts with the "no new boards" constraint. Deprioritized in favor of the plan below, which uses only the existing single sender+receiver pair.

**Scope correction needed:** the README currently claims symbolic verification of G1–G4. That hasn't been done. Either do a lightweight formal model, or explicitly rescope the thesis claims to "empirically demonstrated," not "formally proven" — say this openly in the writeup, don't let a committee member find the gap first.

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
4. Boundary check: the window only holds `cap = 16` slots — test what happens once legitimate traffic cycles past that many frames since the captured one (does the old nonce become "unseen" again and get accepted?). Document this as a real limitation, not just the happy path.

## What this gives the thesis

Three evaluation results (revocation latency, a data-justified Δ, replay resistance) from hardware already built and working, plus one small firmware addition (heartbeat) and a logging/data-collection harness on the dashboard side. That's a real empirical evaluation chapter backing G1/G2/G4.

**G3 (relay-bounded)** — no clean no-new-hardware test exists. Options: argue it analytically (latency-budget lower bound from the system's own known processing times), or explicitly scope it out of the thesis as future work requiring a relay rig.

## Next steps (pick up here next session)

- [ ] Write the heartbeat auto-send into `lifi_session_sender.c`
- [ ] Build the dashboard-side logging harness (timestamp `mac_key_status`, frame-gap histogram, revoke/reverify events)
- [ ] Run shadowing test (20–30 trials), get time-to-revoke/reverify stats
- [ ] Run Δ-tuning analysis using benign-session gap histogram
- [ ] Build the UART frame-replay test script, confirm `replay_window_seen` rejection + probe the 16-slot boundary
- [ ] Decide + write the scoping language on symbolic verification (drop or caveat) and on G3 (analytical argument vs. explicit non-goal)
