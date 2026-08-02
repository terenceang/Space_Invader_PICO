#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/pio.h"
#include "hardware/structs/bus_ctrl.h"

#include "dvi_engine.h"
#include "dvi_display.h"

// ============================================================================
// DVI & Display Configuration - Waveshare RP2350-PiZero (fixed)
// ============================================================================
//
// This board's DVI/mini-HDMI traces run to GPIO32-39, NOT the GPIO22-29
// used by the original RP2040-PiZero (confirmed against the board
// schematic - see Hardware.md). GPIO32-39 also fall outside the RP2350
// HSTX peripheral's fixed GPIO12-19 range, so HSTX cannot drive this
// connector - PIO/DMA TMDS serialisation is used instead (see Video.md).
//
// This project only ever targets this one board/timing/pixel format, so
// dvi_engine.c hardcodes the pin/timing configuration directly rather than
// accepting it as a runtime-swappable struct.

// Power voltage setup (1.25V for high overclock stability at 252 MHz)
#define VREG_VSEL       VREG_VOLTAGE_1_25
#define DVI_BIT_CLK_KHZ 252000

void dvi_display_clock_init(void) {
    // Raise core voltage to 1.25V FIRST before overclocking system clock
    vreg_set_voltage(VREG_VSEL);
    sleep_ms(20);

    // Set system clock to 252 MHz for 640x480 DVI timing
    set_sys_clock_khz(DVI_BIT_CLK_KHZ, true);
}

void dvi_display_init(void) {
    uint32_t actual_clk = clock_get_hz(clk_sys);
    printf("[DEBUG] System Clock   : %lu Hz (Requested: %u kHz)\n",
           actual_clk, DVI_BIT_CLK_KHZ);
    printf("[DEBUG] RP2350 detected - using PIO/DMA TMDS serialiser\n");
    printf("[DEBUG] (HSTX not used: DVI pins are outside its fixed GPIO12-19 range)\n");

    printf("[DEBUG] --- DVI Pinout Configuration ---\n");
    printf("[DEBUG] Clock Pin Pair  : GPIO 38 / 39\n");
    printf("[DEBUG] Data 0 Pin Pair : GPIO 36 / 37 (Blue/Sync)\n");
    printf("[DEBUG] Data 1 Pin Pair : GPIO 34 / 35 (Green)\n");
    printf("[DEBUG] Data 2 Pin Pair : GPIO 32 / 33 (Red)\n");

    // RP2350B's PIO can only address 32 consecutive GPIOs at a time (pins
    // 0-31 or 16-47, selected by gpiobase). Our TMDS pins (32-39) sit above
    // the default 0-31 window, so the PIO's GPIO base must be shifted
    // before any program/state machine is claimed on it - otherwise PIO
    // pin numbers alias modulo 32 and silently drive the wrong GPIOs.
    printf("[DEBUG] Setting PIO GPIO base to %d (required for GPIO32-39)...\n", DVI_PIO_GPIO_BASE);
    pio_set_gpio_base(pio0, DVI_PIO_GPIO_BASE);

    printf("[DEBUG] Initializing DVI engine...\n");
    dvi_engine_init();
    printf("[DEBUG] DVI engine initialized successfully.\n");

    // Grant Core 1 high bus priority for DMA transfers
    hw_set_bits(&bus_ctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC1_BITS);
}

// ============================================================================
// Core 1 Execution: Dedicated DVI TMDS Serialiser Loop
// ============================================================================
void __scratch_x("core1_main") core1_main() {
    dvi_engine_register_irqs_this_core();

    // Wait until Core 0 puts the first scanline into the valid queue
    while (queue_is_empty(&dvi_q_colour_valid))
        __wfe();

    dvi_engine_start();
    dvi_engine_encode_loop();
}
