# LiFi System Evaluation & Research Methodology

This guide outlines the formal evaluation points, metrics, and experimental methodologies for characterizing your LiFi system for a research paper.

## 1. Core Evaluation Metrics (The "Real Points")

These are the standard metrics used in Optical Wireless Communication (OWC) research.

### 1.1. Bit Error Rate (BER)
**Definition**: The ratio of bit errors to the total number of transferred bits during a studied time interval.
**Target**:
- **Raw BER**: $< 10^{-3}$ (limit for Forward Error Correction - FEC).
- **Error-Free**: $< 10^{-9}$ (standard for robust links).
**Measurement**:
- Send a known Pseudo-Random Bit Sequence (PRBS).
- Compare received bits with the known sequence.
- $BER = \frac{N_{error}}{N_{total}}$

### 1.2. Data Rate (Throughput)
**Definition**: The maximum speed (Mbps) at which the system maintains a BER below the target threshold.
**Measurement**:
- Increase the modulation frequency (clock speed) until the BER exceeds the threshold.
- Record the highest stable frequency.

### 1.3. Signal-to-Noise Ratio (SNR)
**Definition**: The ratio of signal power to noise power.
**Measurement**:
- Measure the peak-to-peak voltage of the received signal ($V_{pp}$).
- Measure the RMS noise voltage when the transmitter is OFF ($V_{noise}$).
- $SNR_{dB} = 20 \log_{10}(\frac{V_{pp}}{V_{noise}})$

### 1.4. Bandwidth (-3dB Frequency)
**Definition**: The frequency range where the signal power drops by half (-3dB) compared to the DC/low-frequency power.
**Measurement**:
- Input a sine wave sweep (e.g., 100kHz to 50MHz).
- Measure the amplitude at the receiver.
- The -3dB point is where amplitude is $0.707 \times A_{max}$.

### 1.5. Range & Field of View (FoV)
**Definition**: The maximum distance and angle at which the link remains stable.
**Measurement**:
- **Distance**: Measure BER at 0.5m, 1m, 1.5m, etc.
- **Angle**: Rotate the receiver/transmitter and measure power drop-off (Lambertian distribution).

---

## 2. Experimental Methodology (The "Formal Way")

To publish, your experiments must be **reproducible** and **controlled**.

### 2.1. Experimental Setup Diagram
Include a block diagram in your paper:
`[PC/Data Source] -> [Modulator/Driver] -> [LED]  ~~~(Channel)~~~  [Photodiode] -> [TIA/Amp] -> [Oscilloscope/Decoder] -> [PC/Analyzer]`

### 2.2. Controlled Environment
- **Dark Room**: Baseline performance without interference.
- **Ambient Light**: Test with lights ON to demonstrate robustness (real-world scenario).
- **Alignment**: Use a fixed optical rail or marked positions to ensure consistent LOS (Line of Sight).

### 2.3. Measurement Procedure
1.  **System Characterization**:
    - Measure the **Rise Time** and **Fall Time** of the LED current (using a sense resistor) and the Receiver output.
    - *Why*: This explains *why* your bandwidth is limited (e.g., slow LED phosphor vs. slow photodiode capacitance).
2.  **SNR vs. Distance**:
    - Place Rx at varying distances. Measure SNR. Plot $SNR (dB)$ vs $Distance (m)$.
3.  **BER vs. SNR (or Data Rate)**:
    - The most important plot.
    - Vary the noise (by adding ambient light) or signal strength (distance) and plot BER.
    - Or, fix the distance and plot BER vs. Data Rate to find the cutoff.

---

## 3. Breadboard vs. PCB Evaluation
Since you are moving from breadboard to PCB, this is a valuable research comparison.

| Metric | Breadboard (Baseline) | PCB (Improved) | Analysis |
| :--- | :--- | :--- | :--- |
| **Rise Time** | Likely slower (>50ns) due to parasitic inductance. | Faster (<20ns) due to tight loops. | Show oscilloscope screenshots of both. |
| **Max Frequency** | Limited by noise/crosstalk. | Higher stability. | Quantify the % improvement. |
| **Noise Floor** | Higher (long wires act as antennas). | Lower (ground planes). | Measure $V_{noise, RMS}$ for both. |

---

## 4. Tools for Evaluation

### 4.1. Hardware
- **Oscilloscope**: Essential for measuring Rise/Fall times and capturing Eye Diagrams.
- **Function Generator**: For bandwidth sweeping.
- **Logic Analyzer** (or Pico): For capturing bits to calculate BER.

### 4.2. Software (Python)
You can write a script to:
1.  Generate a PRBS pattern.
2.  Send it via the Pico.
3.  Read the received bits.
4.  Align the streams (sync).
5.  Calculate BER automatically.

## 5. Paper Structure (Standard)
1.  **Introduction**: Why LiFi? Why this specific driver/LED?
2.  **System Design**: Schematic, PCB layout choices (minimizing inductance).
3.  **Experimental Setup**: Description of equipment and environment.
4.  **Results & Discussion**:
    - Bandwidth plots.
    - Eye diagrams (Open eye = good, Closed eye = bad).
    - BER graphs.
    - Comparison with State-of-the-Art (or your own Breadboard baseline).
5.  **Conclusion**: Summary of achievements.
