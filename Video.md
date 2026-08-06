# Video & HDMI Audio Pipeline

This document explains how this project gets pixels and audio onto the screen:
the software pipeline, the buffering/timing budget, and how that maps onto the Waveshare RP2350-PiZero.

Video output and embedded HDMI audio are driven by `lib/frank-hdmi-audio` - a high-performance
palettized 8bpp engine configured for GPIO 32-39, 640x480p60 output, 320x240 8bpp palettized canvas,
and 32 kHz stereo PCM HDMI embedded audio.

---

## Why PIO + DMA instead of HSTX

The RP2350 has a dedicated hardware peripheral for DVI/HDMI TMDS output (HSTX), but it's hard-wired
to GPIO 12-19 only. This board's DVI traces run to GPIO 32-39, outside that range, so HSTX cannot be
used. Instead, **PIO state machines + DMA** bit-bang TMDS serially.

---

## Pipeline overview

```
Core 0 (producer & emulator, main.c)          Core 1 (HDMI engine, frank_hdmi.c)
------------------------------------          ----------------------------------
game_render_scanline()                        frank_hdmi_run_core1()
(8bpp indexed -> fb[320x240])                       |
        |                                           v
        +-----> Writes 8bpp fb -------->  fill_scanline() (8bpp -> RGB565 LUT in scratch_y)
        |                                           |
audio_i2s_step_frame()                              v
(32 kHz stereo PCM batch -> frank_hdmi_audio_write) TMDS encode (SIO interp 0)
        |                                           |
sleep_until(delayed_by_us(start, chunks * CHUNK_US)) v
(Microsecond wall-clock timekeeping anchor)       DMA IRQ_1 (H-Blanking Data Island Packetizer)
                                                    |
                                                    v
                                                  PIO TX FIFO -> GPIO 32-39
```

**Core 0** (`main.c`):
- Renders 8bpp scanlines directly into `fb` (`game_render_scanline()`).
- Generates 32 kHz PCM audio samples in 533-sample frame batches (`audio_i2s_step_frame()`).
- Paces the main loop using an absolute microsecond wall-clock anchor (`sleep_until(delayed_by_us(start, chunks_pushed * CHUNK_US))`), eliminating clock drift between CPU audio production and HDMI CTS/N clock generation.

**Core 1** (`dvi_display.c: core1_main()`):
- Runs `frank_hdmi_run_core1()`.
- Maps 8bpp palette indices to 16bpp RGB565 values using a 256-entry LUT in `scratch_y`.
- TMDS hardware encoding via RP2350 SIO interpolator.
- Injects HDMI Data Island packets (Audio samples, InfoFrames, ACR) during H-blanking under `DMA_IRQ_1`.

---

## Resolution scaling: 320x240 -> 640x480

- **Horizontal 2x**: Done during TMDS encode (`n_pix = 160` source pixels per line, hardware pixel-doubling to 320 symbols = 640 wire pixels).
- **Vertical 2x**: Handled via DMA IRQ line repeating (`v_ctr % 2` repeat check).

---

## Board-specific details (Waveshare RP2350-PiZero)

- **DVI pins are GPIO 32-39**: `pio_set_gpio_base(FRANK_HDMI_PIO, 16)` shifts PIO addressable window to 16-47. `PICO_PIO_USE_GPIO_BASE=1` is set at compile time.
- **64-bit pin-mask requirement**: TMDS data pins (32/34/36) use `pio_sm_set_pins_with_mask64()` and `pio_sm_set_pindirs_with_mask64()`.
- **Clock lane is PWM**: GPIO 38/39 differential clock pair is driven by a PWM slice at 25.2 MHz.
- **252 MHz system clock / 1.25V core voltage**: Set in `dvi_display_clock_init()`.

---

## File map

| File | Role |
|---|---|
| `src/main.c` | Core 0 entry point; main loop with microsecond wall-clock timekeeping |
| `src/game.c` / `.h` | Paces emulated CPU against frame loop, converts video RAM to 8bpp scanlines |
| `src/testcard.c` / `.h` | Colorbar / grayscale test card pattern generator (8bpp palettized) |
| `src/display_config.h` | Framebuffer resolution, 8-bit palette indices, debug flags |
| `src/dvi_display.c` / `.h` | Clock/voltage setup, palette setup, Core 1 entry point |
| `lib/frank-hdmi-audio/` | Core DVI + HDMI Data Island audio driver library |
| `src/audio/audio_i2s.c` / `.h` | Software audio mixer & 32 kHz PCM batch generator |
