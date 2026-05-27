# LiFi Bandwidth Benchmark Results
**Setup:** White LED (single channel), ~15 cm distance, 50 packets per baud rate
**Date:** 2026-03-27
**Receiver:** Pico 2 + TLV3501 comparator + photodiode

---

## Rf = 470 kΩ  (higher gain, lower bandwidth)
*Log: rx_20260327_175020.log — test ran at 17:54:59*

| Baud Rate | Sent | Recv | Loss | Success |
|-----------|------|------|------|---------|
| 9,600     | 50   | 50   | 0    | 100%  ✓ |
| 50,000    | 50   | 50   | 0    | 100%  ✓ |
| 100,000   | 50   | 50   | 0    | 100%  ✓ |
| 150,000   | 50   | 50   | 0    | 100%  ✓ |
| 200,000   | 50   | 50   | 0    | 100%  ✓ |
| 250,000   | 50   | 50   | 0    | 100%  ✓ |
| 300,000   | 50   | 50   | 0    | 100%  ✓ |
| 350,000   | —    | —    | —    | COLLAPSE (control msgs corrupted) |

**Measured ceiling: 300 kbaud clean**
Higher gain → better SNR → clean signal all the way to 300k, then hard cutoff.

---

## Rf = 100 kΩ  (lower gain, higher bandwidth)
*Log: rx_20260327_182703.log — test ran at 18:27:23*

| Baud Rate | Sent | Recv | Loss | Success |
|-----------|------|------|------|---------|
| 9,600     | 50   | 50   | 0    | 100%  ✓ |
| 50,000    | 50   | 50   | 0    | 100%  ✓ |
| 100,000   | 50   | 50   | 0    | 100%  ✓ |
| 150,000   | 50   | 50   | 0    | 100%  ✓ |
| 200,000   | 50   | 50   | 0    | 100%  ✓ |
| 250,000   | 50   | 50   | 0    | 100%  ✓ |
| 300,000   | 50   | 48   | 2    | 96%   ⚠ |
| 350,000   | 50   | 43   | 7    | 86%   ⚠ |
| 400,000   | —    | —    | —    | COLLAPSE |
| 500,000   | —    | —    | —    | COLLAPSE |

**Measured ceiling: 250 kbaud clean, degrades 300–350k**
Lower gain → weaker signal → SNR drops at 300k. Higher bandwidth extends range slightly but not cleanly.

---

## Comparison

| Rf      | Clean ceiling | Degraded range | Hard failure |
|---------|---------------|----------------|--------------|
| 470 kΩ  | 300 kbaud     | none           | 350k+        |
| 100 kΩ  | 250 kbaud     | 300–350k       | 400k+        |

**Key finding:** 470 kΩ outperforms 100 kΩ at this distance.
Reducing Rf cuts transimpedance gain (weaker signal, lower SNR) faster than it extends
usable bandwidth. The TIA bandwidth was not the limiting factor — SNR was.

**Theoretical TIA bandwidth (470 kΩ):** ~318 kHz → measured ceiling 300 kbaud ✓ (matches)

---

## Paper Table (Table V — TIA Tuning)

| Rf       | Transimpedance Gain | Theoretical BW | Measured Max Baud | Notes              |
|----------|--------------------:|---------------:|------------------:|--------------------|
| 470 kΩ   | 470 kV/A            | ~318 kHz       | 300,000 baud      | Best clean ceiling |
| 100 kΩ   | 100 kV/A            | ~1.5 MHz*      | 250,000 baud      | SNR limited        |

*Higher theoretical BW but SNR degradation dominates at this distance/power level.
