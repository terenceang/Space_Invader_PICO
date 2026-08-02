#ifndef DVI_DISPLAY_H
#define DVI_DISPLAY_H

#include "dvi_engine.h"

// Raises core voltage and sets the system clock for the DVI bit clock.
// Must be called before stdio_init_all(), since it must run before the
// clock is stable for USB/UART.
void dvi_display_clock_init(void);

// Configures the board's DVI pinout/timing, brings up the PIO/DMA TMDS
// engine, and prints pinout diagnostics over stdio. Must be called after
// stdio_init_all() (so diagnostics are visible) and before
// multicore_launch_core1(core1_main).
void dvi_display_init(void);

// Core 1 entry point: waits for the first scanline, then runs the DVI TMDS
// serialiser loop. Never returns.
void core1_main(void);

#endif // DVI_DISPLAY_H
