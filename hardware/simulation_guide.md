# High-Speed LiFi Simulation Guide

This guide explains how to simulate the critical "Gate Driver -> MOSFET -> LED" stage of your design to verify 10MHz+ switching performance.

## Option 1: LTspice (Recommended for Power Electronics)
LTspice is the industry standard for simulating switching power circuits. It is free and very robust.

### 1. Setup
1.  Download and install **LTspice**.
2.  Download the SPICE models:
    -   **UCC27517**: Search "UCC27517 PSpice Model" on TI.com.
    -   **CSD17309Q3**: Search "CSD17309Q3 PSpice Model" on TI.com.

### 2. Create the Schematic
1.  **Pulse Source (Pico Signal)**:
    -   Add a `voltage` source.
    -   Right-click -> `Advanced` -> `PULSE`.
    -   Vinitial: 0, Von: 3.3, Tdelay: 0, Trise: 2n, Tfall: 2n, Ton: 20n, Tperiod: 40n (This simulates a ~25MHz signal).
2.  **Gate Driver**:
    -   Import the UCC27517 `.lib` file (Drag and drop into LTspice -> Right click symbol name -> Create Symbol).
    -   Connect VDD to a 5V DC source.
    -   Connect IN+ to your Pulse Source.
3.  **MOSFET**:
    -   Import the CSD17309Q3 `.lib` file.
    -   Connect Gate to Driver Output.
    -   Connect Source to Ground.
4.  **Load (LED + Resistor)**:
    -   Resistor: 2.0Ω (for Blue channel).
    -   LED: Use a diode model or a series voltage source (approx 3.0V) + small resistance to mimic the LED.
    -   Connect: 5V Source -> LED -> Resistor -> MOSFET Drain.

### 3. Run Simulation
-   **Command**: `.tran 200n` (Run transient analysis for 200 nanoseconds).
-   **Probe**: Click on the MOSFET Drain and the LED current.
-   **Verify**: Check if the current pulses look square.

---

## Option 2: KiCad (NGSPICE)
You can simulate directly in KiCad if you set it up correctly.

### 1. Schematic Setup
1.  In your KiCad Schematic, place your components.
2.  **Assign SPICE Models**:
    -   Double-click a symbol (e.g., the MOSFET).
    -   Click `Simulation Model`.
    -   Select `SPICE model from file` and browse to your downloaded `.lib` file.
3.  **Sources**:
    -   Use `Simulation_SPICE:VSOURCE` for voltage sources.
    -   Configure the input source as a Pulse (similar settings to LTspice above).

### 2. Run
1.  Go to **Inspect > Simulator**.
2.  Set **Transient Analysis**: Step `0.1n`, Final Time `200n`.
3.  Run and Probe the output.

## What to Look For
-   **Rise/Fall Times**: With the CSD17309Q3, the current should rise from 0A to ~1A in **< 5ns**.
-   **Ringing**: If there is excessive oscillation at the edges, you may need to increase the Gate Resistor (currently 4.7Ω) or improve layout.
