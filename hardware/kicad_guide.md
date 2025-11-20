# KiCad PCB Layout Guide: High-Speed LiFi Sender

This guide will walk you through turning the [Sender PCB Design](sender_pcb_design.md) into a manufacturing-ready PCB using **KiCad 8.0** (or 7.0).

## 1. Project Setup
1.  Open KiCad and click **File > New Project**.
2.  Name it `lifi_sender_rgb`.
3.  This will create `lifi_sender_rgb.kicad_sch` (Schematic) and `lifi_sender_rgb.kicad_pcb` (Layout).

## 2. Schematic Capture (`.kicad_sch`)

### 2.1 Symbol & Footprint Libraries (Recommended)
For a "finished product" design, it is best to use the specific symbols and footprints for your parts. You can download them for free from these trusted sources:

1.  **UCC27517 (Gate Driver)**:
    -   **Source**: [SnapMagic (formerly SnapEDA)](https://www.snapeda.com/parts/UCC27517DBVR/Texas%20Instruments/view-part/) or [Ultra Librarian](https://www.ultralibrarian.com/).
    -   **Search**: "UCC27517DBVR".
    -   **Download**: Select "KiCad" format.
2.  **BSZ090N03LS (MOSFET)**:
    -   **Source**: [Infineon Website](https://www.infineon.com/) or [SnapMagic](https://www.snapeda.com/).
    -   **Search**: "BSZ090N03LS".
    -   **Download**: Select "KiCad" format.

**How to Import (Step-by-Step):**

1.  **Organize Files**:
    -   Navigate to your project folder (`lifi_sender_rgb`).
    -   Create a new folder named `libs` inside this directory.
    -   **Symbols (`.kicad_sym`)**: Place these directly in the `libs` folder. **Do NOT** put them in the `.pretty` folder.
    -   **Footprints (`.kicad_mod`)**: These MUST go inside a `.pretty` folder (see step 3).

2.  **Add Symbols (Manage Symbol Libraries)**:
    -   **Menu**: Go to **Preferences** -> **Manage Symbol Libraries**.
    -   **Tab**: Click **Project Specific Libraries** (top left).
    -   **Action**: Click the **Folder Icon** (Add existing library).
    -   **Select**: Navigate to your `libs` folder and select the **`.kicad_sym`** file (e.g., `UCC27517DBVR.kicad_sym`).
    -   *Note: Do NOT select the .pretty folder here.*

3.  **Add Footprints (Manage Footprint Libraries)**:
    -   **Menu**: Go to **Preferences** -> **Manage Footprint Libraries**.
    -   *Note: If you see "Design Block Libraries", you are in the wrong place or looking at a different panel. Look specifically for "Manage Footprint Libraries".*
    -   **Tab**: Click **Project Specific Libraries**.
    -   **Action**: Click the **Folder Icon**.
    -   **Select**: Navigate to your `libs` folder and select the **`.pretty`** folder (e.g., `UCC27517.pretty`).
    -   *Note: The .pretty folder contains the .kicad_mod file, but you select the folder itself.*

### 2.2 Symbol Placement
Add your components (Press `A`):

| Component | KiCad Symbol Name | Qty | Notes |
| :--- | :--- | :--- | :--- |
| **Pico 2 W** | `MCU_Module:RaspberryPi_Pico` | 1 | Standard library is fine. |
| **Gate Driver** | `UCC27517` (Your Imported Lib) | 3 | Use the specific symbol you downloaded. |
| **MOSFET** | `BSZ090N03LS` (Your Imported Lib) | 3 | Use the specific symbol you downloaded. |
| **Inductor** | `Device:L` | 1 | Generic is fine, assign footprint later. |
| **Capacitor (Bulk)** | `Device:C_Polarized` | 1 | 100uF Electrolytic. |
| **Capacitor (Ceramic)** | `Device:C` | 7 | 0.1uF (x4), 10uF (x3). |
| **Resistor** | `Device:R` | 3 | High Power Resistors. |
| **Terminal Block** | `Connector:Screw_Terminal_01x02` | 1 | Power Input. |
| **LED Header** | `Connector:Conn_01x08_Pin` | 1 | For OSRAM Kit wires. |

### 2.2 Wiring Connections
Wire the components exactly as described in the [Design Document](sender_pcb_design.md).
- **Critical**: Connect the `UCC27517` VDD pin to the 5V rail, and place a 0.1uF and 10uF capacitor pair on the schematic right next to it.
- **PWM**: Connect Pico GPIO 0, 1, 2 to the inputs of the three Gate Drivers.

### 2.3 Footprint Assignment
This is the most important step. Open the **Footprint Assignment Tool** and assign these footprints:

| Component | Recommended Footprint |
| :--- | :--- |
| **Pico 2 W** | `Module:RaspberryPi_Pico` (Ensure it has the castellated holes if you plan to solder it flat, or pin headers) |
| **Gate Driver** | `Package_TO_SOT_SMD:SOT-23-5` (Standard for UCC27517) |
| **MOSFET** | `Package_SO:Infineon_PG-TSDSON-8` (Or `Package_SO:SOIC-8` if you change parts, but BSZ is TSDSON-8) |
| **Inductor** | `Inductor_SMD:L_Bourns_SRR1260` (Or match your specific part size) |
| **Cap (100uF)** | `Capacitor_THT:CP_Radial_D8.0mm_P3.50mm` (Standard Electrolytic) |
| **Cap (Ceramic)** | `Capacitor_SMD:C_0603_1608Metric` (Easy to hand solder) |
| **Resistor (Power)** | `Resistor_THT:R_Axial_DIN0414_L11.9mm_D4.5mm_P15.24mm_Horizontal` (For 5W Cement/Wirewound) |
| **Terminal** | `TerminalBlock:TerminalBlock_bornier-2_P5.08mm` |

## 3. PCB Layout (`.kicad_pcb`)

### 3.1 Setup
1.  **Update PCB**: Press `F8` in Schematic to push changes to the PCB.
2.  **Board Setup**: Go to **File > Board Setup**.
    - **Constraints**: Set Min Track Width to `0.25mm` (signal) and `0.5mm` (power).

### 3.2 Placement Strategy
1.  **Drivers & MOSFETs**: Group each Driver + MOSFET + Decoupling Cap together.
    - Place the **0.1uF cap** for the driver on the *same side* as the driver, as close as possible.
2.  **Pico**: Place the Pico on one side of the board, with the USB connector facing the edge.
3.  **Power**: Place the Screw Terminal and LC Filter near the power entry point.

### 3.3 Routing (The "High Speed" Part)
1.  **Ground Plane**: Add a **Filled Zone** on the **Bottom Layer (B.Cu)** connected to `GND`. This is crucial.
2.  **Power Traces**: Use **1.0mm - 2.0mm** wide traces for the 5V rail and the path from MOSFET to LED. These carry 1A+.
3.  **Gate Signals**: Route the trace from Driver Output to MOSFET Gate using a direct, short path (0.3mm width is fine). **Avoid vias** on this specific line if possible.
4.  **Pico PWM**: Route these on the top layer. They are lower current, so standard 0.25mm width is fine.

### 3.4 Thermal Vias
- Under the MOSFETs (if they have a thermal pad), add multiple small vias connecting to the Bottom GND plane to help dissipate heat.

## 4. Manufacturing
1.  **DRC**: Run **Inspect > Design Rules Checker**. Fix any errors.
2.  **Plot**: Go to **File > Fabrication Outputs > Gerbers** to generate files for the PCB manufacturer (e.g., JLCPCB, PCBWay).
