# Video Pipeline - How DVI Output Works

This document explains how this project actually gets pixels onto the screen:
the software pipeline, the buffering/timing budget it has to live inside, and
the specifics of how that maps onto the Waveshare RP2350-PiZero. For pinout
tables and board bring-up history, see `Hardware.md` - this doc is about the
runtime pipeline.

The DVI engine (`src/dvi/`) is a slim, project-owned driver written
specifically for this board's fixed configuration (GPIO 32-39, 640x480p60,
RGB565), not the general-purpose vendored
[PicoDVI](https://github.com/Wren6991/PicoDVI) library. `lib/PicoDVI` is still
present in this repo, but only as the `dvi_reference_sample` build target - a
control-test reference kept for hardware/toolchain bring-up regression checks
and for diffing against if the custom engine ever misbehaves (see
Hardware.md's Reference Materials section). It implements the same
proven algorithm (DMA control-block chaining, the vertical timing state
machine, the underrun red-fallback behaviour) with the vendored library's
multi-board/multi-bpp/multi-timing-mode generality stripped out.

---

## Why PIO + DMA instead of HSTX

The RP2350 has a dedicated hardware peripheral for DVI/HDMI-style TMDS output
(HSTX), but it's hard-wired to GPIO 12-19 only (confirmed directly from the
pico-sdk's GPIO function-select table in `hardware_gpio/include/hardware/gpio.h`
- no other pin lists HSTX as an available function). This board's DVI traces
run to GPIO 32-39, outside that range, so HSTX cannot be used at all. Instead
this project uses the same approach the original RP2040 boards use: **PIO
state machines + DMA** to bit-bang TMDS serially. The tradeoff is that
"hardware" video output here is really a very tightly-timed software pipeline
sharing the chip with everything else you write - see
[Timing budget](#timing-budget--why-you-cant-block) below.

---

## Pipeline overview

```
Core 0 (producer, main.c)              Core 1 (encoder + ISR, dvi_display.c)
------------------------------         --------------------------------------
testcard_get_scanline(y, frame)
        |
        v
  dvi_q_colour_valid              -->  dvi_engine_encode_loop()
  (depth 8)                                  |
                                              v
                                       TMDS-encode 16bpp RGB565
                                       -> 10-bit TMDS symbols
                                       (RP2350 SIO hardware encoder,
                                       tmds_encode_sio.c/.S)
                                              |
                                              v
  dvi_q_colour_free  <----------------  queue_add(q_tmds_valid)
  (buffer recycled)                          |
                                              v
                                        q_tmds_valid (depth 8,
                                        but only 3 physical
                                        buffers exist - see below)
                                              |
                                              v
                                   DMA IRQ (dvi_dma_irq_handler,
                                   fires every scanline period)
                                              |
                                              v
                                   Loads next scanline into DMA
                                   control blocks -> PIO TX FIFO
                                   -> GPIO 32-39 (TMDS pairs)
```

**Core 0** (`main.c`) is the *scanline producer*. It's a plain busy loop with
no interrupt involved - it generates/selects a 320-pixel scanline and pushes a
*pointer* to it into `dvi_q_colour_valid`, 240 times per frame (once per
framebuffer row), then repeats. `queue_add_blocking_u32()` naturally throttles
it to match consumption: if the queue is full, Core 0 just waits.

**Core 1** (`dvi_display.c: core1_main()`) never returns. It does two jobs on
one core, both implemented in `src/dvi/dvi_engine.c`:
1. `dvi_engine_encode_loop()` pulls scanline pointers from `dvi_q_colour_valid`,
   TMDS-encodes them (RGB565 -> TMDS symbols, using the RP2350's SIO hardware
   TMDS encoder via `src/dvi/tmds_encode_sio.c` + `.S`), and pushes the
   encoded buffer to an internal `q_tmds_valid` queue.
2. The DMA IRQ handler (`dvi_dma_irq_handler()` in `src/dvi/dvi_engine.c`),
   registered on this core, fires once per scanline period (~31.78 µs at
   640x480p60) and reloads the DMA control blocks that feed the PIO TX FIFOs
   for all three TMDS lanes.

## Resolution scaling: 320x240 -> 640x480

The framebuffer is 320x240 (`display_config.h`), but the wire timing is
640x480p60. Both axes are scaled 2x, by two different mechanisms:

- **Horizontal 2x**: done during TMDS encode. `tmds_encode_channel_16bpp()`
  (`src/dvi/tmds_encode_sio.c`) is called with 320 source pixels per channel,
  but the SIO hardware encoder is configured for horizontal pixel-doubling
  (`hdouble`), so it produces 640 active-line TMDS symbols from those 320
  source pixels.
- **Vertical 2x**: `DVI_VERTICAL_REPEAT = 2` (`src/dvi/dvi_engine.c`). Each
  TMDS-encoded scanline is physically retransmitted twice by the DMA IRQ
  handler before it's recycled - Core 0/Core 1 only ever produce/encode 240
  rows per frame, not 480.

## Timing budget - why you can't block

This is the one hard rule for anything added to the Core 0 loop or the Core 1
encode path: **no call may block for more than a few tens of microseconds.**

The pipeline's buffering margin is small by design:

| Buffer | Depth | Time budget |
|---|---|---|
| `dvi_q_colour_valid` / `dvi_q_colour_free` | 8 entries | ~500 µs (raw scanline pointers, cheap) |
| TMDS-encoded buffers (`DVI_N_TMDS_BUFFERS`, `src/dvi/dvi_engine.c`) | **3** | ~190 µs at 640x480p60 (2 physical lines per buffer x 31.78 µs) |

Once the DMA IRQ handler finds `q_tmds_valid` empty, it doesn't wait - it
immediately falls back to a hard-coded **solid red scanline**, for every line
until the producer catches up (`late_scanline_ctr` in `dvi_engine.c` tracks
and discards the backlog so the image doesn't shift vertically once it
recovers). That fallback is a visible glitch, not a graceful degrade.

We hit this directly during bring-up: a once-per-second heartbeat
`printf()` on Core 0 blocked on UART TX (~8 ms at 115200 baud to send ~90
bytes) - about 40x longer than the entire pipeline's ~190 µs slack. The
result was ~245 scanlines (close to half the 480-line frame) of solid red,
landing at the top of the frame once a second, because the stall happened
right as Core 0 wrapped back to row 0 of the next frame. Fixed by removing
the blocking print from the hot loop (`src/main.c`) rather than trying to
buy more slack - the lesson generalizes:

- **Fine**: bounded CPU-bound work of any amount that fits the ~16.67 ms/frame
  budget - game logic, collision checks, sprite/pixel updates, GPIO polling.
- **Not fine, ever, on Core 0 or Core 1**: `sleep_ms()`, blocking UART/USB
  stdio, blocking I2C/SPI transfers, flash writes, or anything else with an
  unbounded/slow wait baked in. Push slow I/O through DMA/IRQ-driven
  non-blocking paths, or spread it across many frames, instead.

---

## Board-specific details (Waveshare RP2350-PiZero)

Full pin tables live in `Hardware.md`; the parts that are specific to *how
the pipeline is wired up in code* are:

- **DVI pins are GPIO 32-39**, above the RP2350's default 0-31 PIO addressing
  window. `dvi_display_init()` calls `pio_set_gpio_base(pio0, 16)` before
  `dvi_engine_init()` claims any PIO program/state machine, shifting the PIO's
  addressable window to 16-47. `PICO_PIO_USE_GPIO_BASE=1` is also set at
  compile time (`CMakeLists.txt`).
- **64-bit pin-mask requirement**: because the TMDS data pins (32/34/36) are
  >= 32, `src/dvi/dvi_serialiser.pio` uses
  `pio_sm_set_pins_with_mask64()` / `pio_sm_set_pindirs_with_mask64()` instead
  of the 32-bit-only default helpers. See `Hardware.md` for the full story on
  this (it was the root cause of an earlier "no picture" bug).
- **Clock lane is PWM, not PIO**: the differential TMDS clock pair
  (GPIO 38/39) is driven by a PWM slice outputting a 50% duty cycle
  complementary pair, separate from the PIO-driven data lanes.
- **252 MHz system clock / 1.25V core voltage**: set in
  `dvi_display_clock_init()`, required to hit the 640x480p60 TMDS bit clock.
  This must run *before* `stdio_init_all()` so the clock is stable before
  UART/USB come up.
- **Core 1 gets elevated DMA bus priority** (`BUSCTRL_BUS_PRIORITY_PROC1_BITS`,
  set in `dvi_display_init()`) so its time-critical DMA transfers aren't
  starved by Core 0's flash/SRAM traffic.
- **HSTX is physically unusable for this connector** - it's fixed to GPIO
  12-19 on RP2350, nowhere near this board's GPIO 32-39 DVI traces. Don't
  attempt to "optimize" this later by switching to HSTX; it's not a config
  option, it's a wiring constraint. (Commercial boards that do route HSTX to
  a DVI/HDMI connector - e.g. Adafruit's Feather RP2350 with HSTX Port - are a
  different board, not a firmware change on this one.)

## File map

| File | Role |
|---|---|
| `src/main.c` | Core 0 entry point; scanline producer loop |
| `src/testcard.c` / `.h` | Generates the current test-pattern scanlines (color bars, grayscale ramp, moving animation bar) |
| `src/display_config.h` | Framebuffer resolution + RGB565 color constants |
| `src/dvi_display.c` / `.h` | Clock/voltage setup, PIO GPIO-base fix, bus priority, Core 1 entry point - calls into `src/dvi/` |
| `src/dvi/dvi_engine.c` / `.h` | The engine itself: hardcoded 640x480p60 timing, DMA control-block lists, vertical timing FSM, DMA IRQ handler, scanline queues |
| `src/dvi/dvi_serialiser.pio` | PIO program that shifts TMDS symbols out to GPIO (trimmed from the vendored version - debug UART variant dropped) |
| `src/dvi/tmds_encode_sio.c` / `.h` / `.S` | SIO hardware TMDS encoder wrapper - one hand-extracted Arm-Thumb encode routine for this board's exact 16bpp/hdouble configuration |
| `src/dvi/util_queue_u32_inline.h` | Generic pico_util queue-of-pointers helper (copied so `src/dvi/` has no dependency on the vendored library) |
| `lib/PicoDVI/software/libdvi/` | Vendored library - kept only for the `dvi_reference_sample` control-test target, not used by the main app |
