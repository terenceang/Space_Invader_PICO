#ifndef DVI_DISPLAY_H
#define DVI_DISPLAY_H

// Raises core voltage and sets the system clock for the DVI bit clock.
// Must be called before stdio_init_all(), since it must run before the
// clock is stable for USB/UART.
void dvi_display_clock_init(void);

// Configures the board's DVI pinout/timing and brings up the frank-hdmi-audio driver.
void dvi_display_init(void);

// Core 1 entry point: runs the frank-hdmi-audio TMDS serialiser loop. Never returns.
void core1_main(void);

#endif // DVI_DISPLAY_H
