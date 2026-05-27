/*
 * raw_bit_monitor.c
 *
 * Hardware bring-up test:
 *  1. Sets MCP4725 DAC to ~0.8V via I2C → TLV comparator threshold
 *  2. Samples GP27 (TLV OUT) continuously and prints raw 1s/0s over USB
 *
 * Connections:
 *  GP2  (pin 4)  = I2C1 SDA → MCP4725 SDA
 *  GP3  (pin 5)  = I2C1 SCL → MCP4725 SCL
 *  GP27 (pin 32) = TLV OUT  → TLV pin 5 via 33Ω
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"

#define RX_PIN        27
#define I2C_PORT      i2c1
#define SDA_PIN       2   /* GP2 = physical pin 4 */
#define SCL_PIN       3   /* GP3 = physical pin 5 */
#define BUF_BITS      200

/* ~0.8V threshold: DAC = 0x3E1 = 993 → (993/4095)*3.3V ≈ 0.80V
 * Midpoint of the ~1.6V OPA swing so comparator toggles cleanly */
static const uint8_t mcp_cmd[2] = { 0x03, 0xE1 };

static char bits[BUF_BITS + 2];

int main(void) {
    stdio_init_all();
    sleep_ms(3000);  /* Allow USB to enumerate */

    /* I2C init */
    i2c_init(I2C_PORT, 10 * 1000);
    gpio_set_function(SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(SDA_PIN);
    gpio_pull_up(SCL_PIN);

    printf("\n=== Raw Bit Monitor ===\n");
    printf("RX pin: GP%d | I2C: GP%d SDA, GP%d SCL\n", RX_PIN, SDA_PIN, SCL_PIN);
    fflush(stdout);

    /* Full I2C bus scan */
    printf("Scanning I2C1...\n");
    fflush(stdout);
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        uint8_t dummy;
        int r = i2c_read_timeout_us(I2C_PORT, addr, &dummy, 1, false, 10000);
        if (r >= 0) {
            printf("  Found device at 0x%02X\n", addr);
            fflush(stdout);
            found++;
        }
    }
    if (!found) printf("  No I2C devices found — check SDA/SCL wiring\n");
    fflush(stdout);

    /* Program MCP4725 threshold */
    uint8_t mcp_addr = 0;
    for (uint8_t addr = 0x60; addr <= 0x63; addr++) {
        int r = i2c_write_timeout_us(I2C_PORT, addr, mcp_cmd, 2, false, 50000);
        if (r == 2 && mcp_addr == 0) mcp_addr = addr;
    }

    const char *dac_status;
    if (mcp_addr)
        dac_status = "MCP4725 OK — threshold ~0.80V";
    else
        dac_status = "MCP4725 NOT FOUND — threshold at EEPROM default (1.64V)";
    printf("%s\n\n", dac_status);
    fflush(stdout);

    /* RX pin — test with pull-up, pull-down, then no pull to detect floating */
    gpio_init(RX_PIN);
    gpio_set_dir(RX_PIN, GPIO_IN);

    gpio_pull_up(RX_PIN);
    sleep_ms(1);
    int up_ones = 0;
    for (int i = 0; i < 1000; i++) up_ones += gpio_get(RX_PIN);

    gpio_pull_down(RX_PIN);
    sleep_ms(1);
    int down_ones = 0;
    for (int i = 0; i < 1000; i++) down_ones += gpio_get(RX_PIN);

    gpio_disable_pulls(RX_PIN);

    printf("GP%d pull-up:   %d/1000 HIGH\n", RX_PIN, up_ones);
    printf("GP%d pull-down: %d/1000 HIGH\n", RX_PIN, down_ones);
    if (up_ones > 990 && down_ones < 10)
        printf(">>> PIN IS FLOATING — comparator output not connected to GP%d!\n", RX_PIN);
    else if (up_ones > 990 && down_ones > 990)
        printf(">>> Pin actively driven HIGH — comparator stuck or OPA always above threshold\n");
    else if (up_ones < 10 && down_ones < 10)
        printf(">>> Pin actively driven LOW\n");
    else
        printf(">>> Pin toggling — signal present\n");
    printf("DAC: %s\n", dac_status);
    /* DAC sweep: ramp threshold from 0 to 3.3V to find OPA floor */
    printf("Sweeping DAC threshold to find OPA floor (cover photodiode now)...\n");
    fflush(stdout);
    sleep_ms(2000);

    uint16_t flip_dac = 0;
    for (uint16_t dac = 0; dac <= 4095; dac += 4) {
        uint8_t hi = (dac >> 8) & 0x0F;
        uint8_t lo = dac & 0xFF;
        uint8_t sweep_cmd[2] = { hi, lo };
        i2c_write_timeout_us(I2C_PORT, mcp_addr ? mcp_addr : 0x62,
                             sweep_cmd, 2, false, 50000);
        sleep_ms(2);
        /* sample 20 reads */
        int s = 0;
        for (int i = 0; i < 20; i++) s += gpio_get(RX_PIN);
        if (s == 0 && flip_dac == 0) {
            flip_dac = dac;
            float v = (dac / 4095.0f) * 3.3f;
            printf(">>> Comparator flipped LOW at DAC=0x%03X (%.3fV) — OPA floor ~%.3fV\n",
                   dac, v, v);
            fflush(stdout);
            break;
        }
    }
    if (!flip_dac) {
        printf(">>> Comparator never flipped — OPA above 3.3V or inputs swapped?\n");
    }

    /* Set threshold 200mV above floor for margin */
    uint16_t threshold_dac = (flip_dac > 0) ? (flip_dac + 248) : 0x7FF;
    if (threshold_dac > 4095) threshold_dac = 4095;
    {
        uint8_t hi = (threshold_dac >> 8) & 0x0F;
        uint8_t lo = threshold_dac & 0xFF;
        uint8_t t_cmd[2] = { hi, lo };
        i2c_write_timeout_us(I2C_PORT, mcp_addr ? mcp_addr : 0x62,
                             t_cmd, 2, false, 50000);
    }
    float final_v = (threshold_dac / 4095.0f) * 3.3f;
    printf("Threshold set to DAC=0x%03X (%.3fV).\n", threshold_dac, final_v);
    fflush(stdout);

    /* DAC toggle test: alternate 0V / 3.3V — if GP27 never changes, DAC not wired to comparator */
    printf("DAC TOGGLE TEST (6 steps, ~2s each)...\n");
    fflush(stdout);
    static const uint8_t dac_lo[2] = { 0x00, 0x00 };
    static const uint8_t dac_hi[2] = { 0x0F, 0xFF };
    uint8_t i2c_addr = mcp_addr ? mcp_addr : 0x62;
    for (int t = 0; t < 6; t++) {
        const uint8_t *cmd = (t % 2 == 0) ? dac_lo : dac_hi;
        float v = (t % 2 == 0) ? 0.0f : 3.3f;
        i2c_write_timeout_us(I2C_PORT, i2c_addr, cmd, 2, false, 50000);
        sleep_ms(200);
        int ones = 0;
        for (int i = 0; i < 500; i++) ones += gpio_get(RX_PIN);
        printf("  DAC=%.1fV -> GP27 %s (%d/500)\n",
               v, ones > 490 ? "HIGH" : ones < 10 ? "LOW" : "MIXED", ones);
        fflush(stdout);
        sleep_ms(1800);
    }
    printf("If all lines show HIGH: DAC output not wired to comparator IN pin.\n\n");
    fflush(stdout);

    /* Restore working threshold */
    i2c_write_timeout_us(I2C_PORT, i2c_addr, mcp_cmd, 2, false, 50000);
    printf("Flood starting...\n\n");
    fflush(stdout);

    uint32_t loop = 0;
    while (true) {
        int ones_in_buf = 0;
        for (int i = 0; i < BUF_BITS; i++) {
            int b = gpio_get(RX_PIN);
            bits[i] = b ? '1' : '0';
            ones_in_buf += b;
        }
        bits[BUF_BITS]     = '\0';

        /* Every 10 loops: print summary + pull-up vs pull-down test */
        if (loop % 10 == 0) {
            gpio_pull_up(RX_PIN);   sleep_ms(1);
            int up = 0;
            for (int i = 0; i < 200; i++) up += gpio_get(RX_PIN);
            gpio_pull_down(RX_PIN); sleep_ms(1);
            int dn = 0;
            for (int i = 0; i < 200; i++) dn += gpio_get(RX_PIN);
            gpio_disable_pulls(RX_PIN);

            const char *verdict;
            if (up > 190 && dn < 10)        verdict = "FLOATING (wire GP27 to TLV OUT!)";
            else if (up > 190 && dn > 190)  verdict = "actively driven HIGH";
            else if (up < 10  && dn < 10)   verdict = "actively driven LOW";
            else                            verdict = "toggling";

            printf("[loop %lu | %d%% HIGH | pull-up=%d pull-dn=%d | %s | %s]\n",
                   loop, (ones_in_buf * 100) / BUF_BITS, up, dn, verdict, dac_status);
            fflush(stdout);
        }
        printf("%s\n", bits);
        fflush(stdout);
        sleep_ms(500);
        loop++;
    }
    return 0;
}
