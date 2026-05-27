# LiFi-Auth: Project Overview

## What It Is

A Li-Fi (Light Fidelity) authentication and communication system. It transmits encrypted data over visible light using LED transmitters and photodiode receivers. The core use case is secure IoT device authentication — a Raspberry Pi Pico sends AES-GCM encrypted messages over light, a second Pico (RP2350/Pico 2) receives them optically, and a Linux host decrypts and processes them.

## System Components

```
[Web Browser]
     ↓  HTTP / WebSocket
[Flask Dashboard]  ←→  sender/dashboard/app.py
     ↓  USB Serial (/dev/ttyACM0)
[Pico Sender]  ←  lifi_session_sender.uf2  (RP2040)
     ↓  Light @ 1 Mbps (4 LED channels: White/Green/Blue/Red on GP6-9)
[Photodiode + Analog Front-End]
  → TIA (470kΩ || 5pF feedback op-amp)
  → TLV3501 Comparator (threshold set by MCP4725 DAC via I2C)
  → GP27 on Pico 2
[Pico 2 Receiver]  ←  lifi_pico2_rx.uf2  (RP2350)
     ↓  USB Serial (/dev/ttyACM0)
[Linux Receiver Apps]  ←  ask_receiver / flash_receiver / keys_receiver
     ↓  Decrypted plaintext
[IoTAuth Server]  ←  deps/iotauth/  (key provisioning)
```

## Repository Layout

```
lifi-auth/
├── include/               # Shared headers (protocol, crypto, pico hardware)
├── src/                   # Shared source (pico_handler, cmd_handler, crypto wrappers)
├── sender/
│   ├── src/               # Pico sender firmware + speed test sender
│   ├── dashboard/         # Flask web UI (app.py, templates, static)
│   └── CMakeLists.txt
├── receiver/
│   ├── src/               # Linux receiver apps (ask, flash, keys, speed_test)
│   ├── include/           # Receiver-specific headers
│   └── CMakeLists.txt
├── receiver_pico/
│   ├── src/               # Pico 2 RX firmware (main.c, raw_bit_monitor.c)
│   └── src/lifi_rx.pio    # PIO UART RX state machine
├── deps/
│   ├── sst-c-api/         # IoTAuth C API (git submodule)
│   └── iotauth/           # Full IoTAuth framework (git submodule)
├── config/
│   └── mbedtls_config.h   # mbedTLS feature flags for embedded
├── CMakeLists.txt          # Master build (pico / pi4 / receiver targets)
├── make_build.sh           # Main build + artifact collection script
├── run_build.sh            # Wrapper: set_build + make_build
├── set_build.sh            # Writes .build_target file
├── dashboard.sh            # Launches Flask dashboard
├── start_server.sh         # Starts IoTAuth auth server
└── rx_monitor.py           # Simple USB serial monitor for receiver
```

## Build Targets

| Target | Output Binaries | Platform |
|--------|----------------|----------|
| `pico` | `lifi_session_sender.uf2`, `pico_speed_test_sender.uf2` | RP2040 Pico |
| `receiver` / `pi4` | `flash_receiver`, `keys_receiver`, `ask_receiver`, `speed_test_receiver` | Linux (Raspberry Pi 4 or x86) |
| `receiver_pico` | `lifi_pico2_rx.uf2`, `raw_bit_monitor.uf2` | RP2350 Pico 2 |

## Key Technologies

- **PIO (Programmable I/O):** Custom UART TX (4-pin simultaneous) and UART RX state machines — no CPU involvement in bit-banging
- **AES-128-GCM:** Authenticated encryption via mbedTLS; 16-byte key, 12-byte nonce, 16-byte tag
- **MCP4725 DAC:** I2C-controlled voltage reference for comparator threshold (I2C1 on GP2/GP3)
- **TLV3501 Comparator:** Converts analog photodiode signal to digital GPIO
- **IoTAuth / SST:** Session key provisioning and management framework
- **heatshrink:** LZ77-style compression for message payloads
- **Flask + Socket.IO:** Real-time web dashboard for sender control

## Current Hardware Status (as of debugging session)

- Sender Pico: working, transmitting light pulses
- Analog front-end: OPA (TIA) confirmed working — swings 0→2V with light
- TLV3501 comparator: wired, SHDN enabled, MCP4725 DAC at 0.8V threshold
- **Known issue:** 47Ω resistor from OPA→TLV IN- was tapped at OPA inverting input (2.8V virtual ground) instead of OPA output. Fix: move 47Ω to the OPA output node (swinging leg of 470kΩ feedback).
- Pico 2 receiver firmware: `raw_bit_monitor.uf2` used for bring-up; `lifi_pico2_rx.uf2` is the production firmware
