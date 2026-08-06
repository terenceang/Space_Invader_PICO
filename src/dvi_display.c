#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"

#include "dvi_display.h"
#include "display_config.h"
#include "frank_hdmi.h"

#define VREG_VSEL       VREG_VOLTAGE_1_25
#define DVI_BIT_CLK_KHZ 252000

void dvi_display_clock_init(void) {
    vreg_set_voltage(VREG_VSEL);
    sleep_ms(20);
    set_sys_clock_khz(DVI_BIT_CLK_KHZ, true);
}

void dvi_display_init(void) {
    uint32_t actual_clk = clock_get_hz(clk_sys);
    printf("[DEBUG] System Clock   : %lu Hz (Requested: %u kHz)\n",
           actual_clk, DVI_BIT_CLK_KHZ);
    printf("[DEBUG] --- DVI/HDMI Pinout Configuration ---\n");
    printf("[DEBUG] Clock Pin Pair  : GPIO %d / %d\n", FRANK_HDMI_PIN_CLK, FRANK_HDMI_PIN_CLK + 1);
    printf("[DEBUG] Data 0 Pin Pair : GPIO %d / %d (Blue/Sync)\n", FRANK_HDMI_PIN_D0, FRANK_HDMI_PIN_D0 + 1);
    printf("[DEBUG] Data 1 Pin Pair : GPIO %d / %d (Green)\n", FRANK_HDMI_PIN_D1, FRANK_HDMI_PIN_D1 + 1);
    printf("[DEBUG] Data 2 Pin Pair : GPIO %d / %d (Red)\n", FRANK_HDMI_PIN_D2, FRANK_HDMI_PIN_D2 + 1);

    frank_hdmi_init();

    // Set default palette entries (0xRRGGBB)
    frank_hdmi_set_palette(COLOR_BLACK,   0x000000);
    frank_hdmi_set_palette(COLOR_WHITE,   0xFFFFFF);
    frank_hdmi_set_palette(COLOR_YELLOW,  0xFFFF00);
    frank_hdmi_set_palette(COLOR_CYAN,    0x00FFFF);
    frank_hdmi_set_palette(COLOR_GREEN,   0x00FF00);
    frank_hdmi_set_palette(COLOR_MAGENTA, 0xFF00FF);
    frank_hdmi_set_palette(COLOR_RED,     0xFF0000);
    frank_hdmi_set_palette(COLOR_BLUE,    0x0000FF);

    printf("[DEBUG] frank-hdmi-audio initialized successfully.\n");
}

void core1_main(void) {
    frank_hdmi_run_core1();
}
