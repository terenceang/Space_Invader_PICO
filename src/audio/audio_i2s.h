#ifndef AUDIO_I2S_H
#define AUDIO_I2S_H

// I2S audio output driver for the Waveshare RP2350-PiZero - see Hardware.md's
// "I2S audio pinout" section for the pin choice and DAC wiring.
//
// This is a hardware bring-up driver, not yet fed by the emulator: the real
// arcade ROM's discrete sound-trigger ports (3/5) are still stubbed out (see
// Emulator.md's Limitations section) - there's no sound sample playback
// wired up yet, only a continuous test tone so the I2S signal path (pins,
// PIO timing, DMA hand-off) can be verified against a DAC board/scope before
// real game audio is built on top of it.
//
// Uses PIO1 (PIO0 is fully claimed by the DVI engine, src/dvi/) and
// DMA_IRQ_1 (DVI already owns DMA_IRQ_0 - see src/dvi/dvi_engine.h), so this
// is entirely independent of the DVI pipeline's PIO/DMA/timing budget.
// Registers its IRQ on whichever core calls audio_i2s_init(), matching
// dvi_engine_register_irqs_this_core()'s pattern - call it from Core 0
// (main.c, before multicore_launch_core1()) so it doesn't compete with the
// DVI engine's own DMA_IRQ_0 handler on Core 1.

// Brings up the I2S PIO/DMA engine and immediately starts outputting a
// continuous test tone (see audio_i2s.c for the frequency/amplitude - both
// deliberately conservative for a first hardware bring-up). Call once,
// after dvi_display_clock_init()'s set_sys_clock_khz() (the PIO clock
// divider is computed from the system clock).
void audio_i2s_init(void);

#endif // AUDIO_I2S_H
