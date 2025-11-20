# High-Speed LiFi Sender PCB Design Document

## Overview
This document details the hardware design for a high-speed, 3-channel (RGB) LiFi Sender PCB. The design is optimized for the **Raspberry Pi Pico 2 W** and utilizes the **OSRAM OSTAR Stage (LE RTDUW S2WN)** LED module.

**Key Features:**
- **Controller:** Raspberry Pi Pico 2 W (RP2350).
- **Channels:** 3 Independent Channels (Red, Green, Blue) for high-speed modulation.
- **Power Input:** Single 5V High-Current Supply (e.g., 5V 10A).
- **Switching Speed:** >10 MHz potential (limited by LED/Resistor RC time constant, but driver is ultra-fast).
- **Protection:** Filtered power for MCU to prevent resets.

---

## 1. Schematic Description

### 1.1 Power Input & Filtering
- **Connector:** 2-pin Screw Terminal (5.08mm pitch) for 5V Input.
- **Main Rail (5V_HIGH):** Direct connection to LED Anodes and Gate Drivers.
- **Logic Rail (VSYS_FILT):** Filtered 5V for Pico 2 W.
    - **Filter:** LC Pi Filter.
    - **L1:** 10uH - 22uH Inductor (Rated >500mA).
    - **C1:** 100uF Electrolytic (Bulk storage).
    - **C2:** 0.1uF Ceramic (High-frequency noise decoupling).
    - **Connection:** `5V_HIGH` -> `L1` -> `VSYS_FILT` -> `Pico Pin 39 (VSYS)`.
    - **GND:** Common Ground Plane.

### 1.2 Gate Drivers (x3 Channels)
- **IC:** Texas Instruments **UCC27517** (SOT-23-5).
- **Power:** Pin 2 (VDD) connected to `5V_HIGH`.
- **Decoupling:** 0.1uF (C_bypass) + 10uF (C_bulk) placed *immediately* next to VDD pin.
- **Input:** Pin 3 (IN+) connected to Pico GPIO (PWM signal).
- **Input Ground:** Pin 4 (IN-) connected to GND.
- **Output:** Pin 1 (OUT) connected to MOSFET Gate via 4.7Ω resistor (optional, to dampen ringing).

### 1.3 MOSFET Output Stage (x3 Channels)
- **MOSFET:** Infineon **BSZ090N03LS** (TSDSON-8).
- **Configuration:** Low-Side Switch.
- **Gate:** Connected to Driver OUT.
- **Source:** Connected to GND Plane (Multiple vias for thermal/inductance).
- **Drain:** Connected to Current Limiting Resistor.

### 1.4 LED Connection & Current Limiting
- **Connector:** 8-pin Header or Wire-to-Board pads for OSRAM LED Kit wires.
- **Topology:** `5V_HIGH` -> `LED Anode` -> `LED Cathode` -> `Resistor` -> `MOSFET Drain`.
- **Resistors (High Power):**
    - **Red Channel:** 2.7Ω (5W). Target: ~1.0A.
    - **Green Channel:** 1.5Ω (3W/5W). Target: ~1.0A.
    - **Blue Channel:** 2.0Ω (3W/5W). Target: ~1.0A.

---

## 2. Bill of Materials (BOM)

| Component | Description | Value | Qty | Part Number (Example) |
| :--- | :--- | :--- | :--- | :--- |
| **MCU** | Raspberry Pi Pico 2 W | - | 1 | SC1637 |
| **Gate Driver** | Low-Side Driver, 4A, High Speed | - | 3 | **UCC27517DBVR** |
| **MOSFET** | N-Ch, 30V, 40A, Low Qg, 3x3mm | - | 3 | **BSZ090N03LS G** |
| **Inductor** | Power Inductor, Shielded, >0.5A | 22uH | 1 | SRR1260-220M |
| **Capacitor** | Electrolytic, 16V | 100uF | 1 | - |
| **Capacitor** | Ceramic X7R, 0603/0805 | 10uF | 3 | - |
| **Capacitor** | Ceramic X7R, 0402/0603 | 0.1uF | 4 | - |
| **Resistor** | Power Resistor, 5W, Wirewound/Ceramic | 2.7Ω | 1 | SQP500JB-2R7 |
| **Resistor** | Power Resistor, 5W, Wirewound/Ceramic | 1.5Ω | 1 | SQP500JB-1R5 |
| **Resistor** | Power Resistor, 5W, Wirewound/Ceramic | 2.0Ω | 1 | SQP500JB-2R0 |
| **Resistor** | Gate Resistor, 0603 (Optional) | 4.7Ω | 3 | - |
| **Connector** | Screw Terminal, 2-pos | - | 1 | - |
| **Connector** | Pin Header/Pads for LED | - | 1 | - |

---

## 3. PCB Layout Guidelines (Critical for Speed)

### 3.1 Component Placement
1.  **Gate Drivers:** Place **UCC27517** as close as physically possible to the **MOSFET** Gate. Distance should be < 5mm if possible.
2.  **Decoupling:** Place the 0.1uF capacitor for the Gate Driver **directly** across the VDD and GND pins. This is the most critical component for speed.
3.  **MOSFETs:** Place near the edge or connector to keep high-current LED loops short.

### 3.2 Trace Routing
- **Gate Drive Trace:** Short and wide (e.g., 15-20 mil). Avoid vias if possible.
- **High Current Paths (5V & LED Return):** Use **Copper Pours** or very wide traces (>50 mil or 2oz copper) for the 1A LED currents.
- **Ground Plane:** Use a solid Ground Plane on the bottom layer. Stitch top-layer ground islands to the bottom plane with multiple vias to reduce inductance.

### 3.3 Thermal Management
- **MOSFETs:** The BSZ090N03LS is efficient, but switching losses at >10MHz will generate heat. Add thermal vias under the MOSFET exposed pad to the bottom ground plane.
- **Resistors:** The current limiting resistors will get **HOT** (up to 2.7W). Space them apart and away from the Pico and sensitive caps. Do not place them directly under the LED wires.

### 3.4 Pico Connection
- Keep the Pico away from the high-current switching loops.
- Route the PWM signals from Pico to Gate Drivers as single-ended 50Ω traces (standard width is fine for short runs < 2 inches).

---

## 4. Pinout Configuration (Proposed)

| Pico Pin | GPIO | Function | Connection |
| :--- | :--- | :--- | :--- |
| 39 | VSYS | Power Input | Output of LC Filter |
| 38 | GND | Ground | Common GND |
| 1 | GP0 | **Red** PWM | UCC27517 (Red) IN+ |
| 2 | GP1 | **Green** PWM | UCC27517 (Green) IN+ |
| 4 | GP2 | **Blue** PWM | UCC27517 (Blue) IN+ |
