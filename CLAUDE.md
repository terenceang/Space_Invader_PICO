# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

An emulator of the real 1978 Taito/Midway Space Invaders arcade PCB (Intel 8080 CPU,
memory map, I/O ports, shift-register sprite hardware - `src/emu/`) for the Waveshare
RP2350-PiZero board, written in C against the Raspberry Pi Pico SDK, driving DVI video
output over the board's mini-HDMI connector. It runs the *actual* arcade ROM, not a
reimplementation of the game logic - see `Emulator.md`. **Status: early stage** - the DVI
video pipeline (`src/dvi/`, `src/dvi_display.*`) and the CPU core + video output
(`src/emu/`, `src/game.c`) are built and working; input (joystick/fire) and sound are not
wired up yet, so the ROM currently just runs its own attract-mode loop. See the Roadmap
in `README.md`.

**The real arcade ROM is required and is not in this repo** (Taito's copyrighted work -
see `roms/README.md`). Without it in `roms/`, the build substitutes a zero-filled
placeholder so compilation still succeeds, but the firmware won't run the actual game.

## Build

Targets Pico SDK 2.3.0 and the `waveshare_rp2350_pizero` board definition (defined in
`Sample Code/boards/waveshare_rp2350_pizero.h`, referenced via `PICO_BOARD` in
`CMakeLists.txt`).

```sh
cmake -S . -B build -G Ninja -DPICO_BOARD=waveshare_rp2350_pizero
cmake --build build
```

This produces `build/Space_Invader_PICO.uf2` (flash by holding BOOTSEL while plugging in
the board, then copying the `.uf2` to the drive that appears) and also
`build/dvi_reference_sample.uf2` (Waveshare's own official DVI demo - see below).

Opening the folder in VS Code with the Raspberry Pi Pico extension also works; `.vscode/`
is already configured for it (build/run/flash/reset tasks in `.vscode/tasks.json`, SDK
paths in `.vscode/settings.json`). There is no separate lint step or test suite - this is
firmware; correctness is verified by building and by flashing/observing on real hardware.

There is no CI here. When you change anything under `src/`, build (`cmake --build build`)
before considering the change done - a broken build is the primary failure mode this
project can catch automatically.

### The `dvi_reference_sample` target

`CMakeLists.txt` also builds a second executable, `dvi_reference_sample`, from
`Sample Code/01-DVI/apps/hello_dvi/main.c` (Waveshare's own official demo) linked against
the vendored `lib/PicoDVI` library. This is a hardware/toolchain control test only - a
known-good fallback to flash if you ever suspect a hardware or environment problem rather
than a bug in this project's own code. It needs `Sample Code/` on disk locally; that
directory is gitignored (Waveshare's demo package, not part of this project) so it won't
exist after a fresh clone. Do not add feature work to it or to `lib/PicoDVI`.

### Debug test card

`src/display_config.h` controls whether boot shows the debug test card (color bars,
grayscale ramp, moving sync bar) before handing off to the game:

```c
#define DEBUG_TESTCARD 1          // 0 to skip the test card and boot straight into the game
#define DEBUG_TESTCARD_SECONDS 5
```

## Architecture

Read `Emulator.md` before touching anything under `src/emu/` or `src/game.c`, and
`Video.md`/`Hardware.md` before touching anything under `src/dvi/` or `src/dvi_display.*`
- they cover the pipeline, timing budget, and board-specific pinout gotchas in depth.

### Emulator core (`src/emu/`, `src/game.c`)

`src/emu/i8080.c` is a from-scratch Intel 8080 interpreter: full documented instruction
set plus the well-known undocumented opcode duplicates real 8080 silicon has (e.g.
`0xCB`=`JMP`, `0xD9`=`RET`, `0xDD/0xED/0xFD`=`CALL`) - these are hardware facts to
preserve, not bugs to fix. It has zero dependency on the Pico SDK or this project's
memory map; it's wired to a specific machine purely through the `mem_read`/`mem_write`/
`io_in`/`io_out`/`ctx` function pointers in `i8080_t`.

`src/emu/invaders_machine.c` wires that CPU to the real Space Invaders arcade memory map
(`$0000-$1FFF` ROM, `$2000-$3FFF` RAM including video RAM at `$2400`) and I/O ports
(inputs, DIP switches, and the 16-bit shift register the real hardware uses to draw
bit-shifted sprites - `OUT 4` shifts a byte in, `OUT 2` sets a 0-7 bit read offset, `IN 3`
reads the shifted result).

`src/game.c` doesn't contain game logic - it runs the emulated CPU in small slices
interleaved with each scanline (`SI_CYCLES_PER_ROW`), firing the two real per-frame
interrupts (`RST 1` mid-screen, `RST 2` vblank) at the scanline calls nearest their real
timing, then samples the emulated machine's 256x224 1bpp video RAM into the 320x240
RGB565 framebuffer (letterboxed, with the classic red/green cabinet overlay tint applied
at this conversion step). Running the CPU in small per-scanline slices rather than one
big per-frame burst is required by the DVI pipeline's hard timing budget below - see
`Emulator.md`'s "Interrupt timing" section for why.

**Screen orientation**: the real cabinet's monitor is mounted vertically (portrait), not
landscape - `SI_DISPLAY_ROTATED_CCW` in `src/display_config.h` (default 1) tells
`render_arcade_row()` whether the physical display here is mounted the same way (rotated
90 degrees CCW) or is a normal landscape monitor, and it samples video RAM differently for
each case. See `Emulator.md`'s "Screen orientation" section before changing either the
rotation math or the overlay band logic - they're coupled (get the flip direction wrong
and the red/green overlay bands land upside down).

Inputs aren't wired to anything yet (`invaders_machine_set_in1()` exists for whatever
GPIO/controller work comes next) - with no coin/start, the ROM just runs its own real
attract-mode loop, which is itself a correct emulation of idle hardware.

**The real arcade ROM is not vendored** - it's loaded from 4 user-supplied files in
`roms/` (gitignored) and embedded into the flash image at build time by
`cmake/generate_rom.cmake` (see `CMakeLists.txt`'s custom command generating
`generated/rom_data.c`). If you add anything that needs to know ROM contents at build
time, that generated file / `src/emu/rom_data.h` is where to look.

### DVI pipeline (`src/dvi/`, `src/dvi_display.*`)

The essentials:

**This board's DVI pins (GPIO 32-39) fall outside the RP2350's HSTX peripheral's fixed
GPIO 12-19 range, so HSTX cannot be used.** Video output is instead a PIO + DMA
bit-banged TMDS pipeline (`src/dvi/`) - a slim, project-owned engine hardcoded for this
board's exact configuration (GPIO 32-39, 640x480p60, RGB565), *not* the general-purpose
vendored PicoDVI library (`lib/PicoDVI/`, kept only to build `dvi_reference_sample`).

**Dual-core split**, set up in `main.c` / `dvi_display.c`:
- **Core 0** (`main.c`): scanline *producer*. A plain busy loop with no interrupts - each
  iteration picks/generates one 320-pixel scanline (test card or game) and pushes a
  *pointer* to it into `dvi_q_colour_valid`, 240 times per frame, then repeats.
  `queue_add_blocking_u32()` throttles it to match consumption.
- **Core 1** (`dvi_display.c: core1_main()`, never returns): runs
  `dvi_engine_encode_loop()` (`src/dvi/dvi_engine.c`), which pulls scanline pointers,
  TMDS-encodes them via the RP2350's SIO hardware encoder
  (`src/dvi/tmds_encode_sio.c/.S`), and feeds an internal queue that the DMA IRQ handler
  (also on Core 1) drains into the PIO TX FIFOs for the three TMDS data lanes on
  GPIO 32-39. The differential clock pair (GPIO 38/39) is driven separately by a PWM
  slice, not PIO.

**Resolution scaling (320x240 framebuffer -> 640x480p60 wire timing)** happens via two
different mechanisms on two axes: horizontal 2x is done during TMDS encode
(`hdouble` config in `tmds_encode_sio.c`), vertical 2x is done by physically
retransmitting each encoded scanline twice in the DMA IRQ handler
(`DVI_VERTICAL_REPEAT` in `dvi_engine.c`).

**Hard timing rule - read before adding anything to the Core 0 loop or the Core 1 encode
path**: no call may block for more than a few tens of microseconds. The TMDS buffer depth
is only 3 encoded scanlines (~190us of slack at 640x480p60); once it empties, the DMA IRQ
handler immediately falls back to a solid red scanline until the producer catches up. A
`printf()` over UART from the hot loop is enough to visibly break this (this happened
during bring-up - see `Video.md`'s "Timing budget" section for the full incident writeup).
Bounded CPU-bound work (game logic, collision checks, sprite updates) is fine; `sleep_ms()`,
blocking UART/USB/I2C/SPI, and flash writes are never fine on either core once rendering
has started.

**RP2350B GPIO >= 32 quirks** (see `Hardware.md` for full detail) that matter if you touch
`dvi_display.c` or `src/dvi/dvi_serialiser.pio`:
- `pio_set_gpio_base(pio0, 16)` must run before `dvi_engine_init()` claims any PIO
  program/state machine, and `PICO_PIO_USE_GPIO_BASE=1` must stay set at compile time
  (`CMakeLists.txt`) - PIO can only address 32 consecutive GPIOs at a time and the default
  window (0-31) doesn't cover this board's pins.
- The PIO program uses the 64-bit pin-mask helpers (`pio_sm_set_pins_with_mask64()` /
  `pio_sm_set_pindirs_with_mask64()`) since the 32-bit variants don't support pins >= 32.
  This was the root cause of an earlier "no picture" bug during bring-up.
- The 252 MHz system clock / 1.25V core voltage setup (`dvi_display_clock_init()`) must
  run before `stdio_init_all()`, so the clock is stable before UART/USB come up.

### File map

| Path | Role |
|---|---|
| `src/main.c` | Entry point; Core 0 scanline producer / dispatch loop (test card -> game handoff) |
| `src/game.c` / `.h` | Paces the emulated CPU against the frame loop, converts video RAM into scanlines |
| `src/emu/i8080.c` / `.h` | Intel 8080 CPU interpreter - full instruction set, no machine-specific knowledge |
| `src/emu/invaders_machine.c` / `.h` | Space Invaders memory map, I/O ports, shift register, interrupt delivery |
| `src/emu/rom_data.h` | Declares the embedded ROM array defined by the CMake-generated source |
| `roms/` | User-supplied real arcade ROM files go here (gitignored, not vendored) |
| `cmake/generate_rom.cmake` | Embeds `roms/invaders.{h,g,f,e}` into a linkable C array at build time |
| `src/testcard.c` / `.h` | Debug test pattern generator |
| `src/display_config.h` | Framebuffer size, RGB565 color constants, refresh rate, debug flags |
| `src/dvi_display.c` / `.h` | Clock/voltage setup, PIO GPIO-base fix, bus priority, Core 1 entry point |
| `src/dvi/dvi_engine.c` / `.h` | Hardcoded 640x480p60 timing, DMA control-block lists, vertical timing FSM, DMA IRQ handler, scanline queues |
| `src/dvi/dvi_serialiser.pio` | PIO program shifting TMDS symbols out to GPIO (trimmed from the vendored version) |
| `src/dvi/tmds_encode_sio.c` / `.h` / `.S` | SIO hardware TMDS encoder wrapper for this board's exact 16bpp/hdouble config |
| `src/dvi/util_queue_u32_inline.h` | Generic pico_util queue-of-pointers helper, copied so `src/dvi/` has no dependency on the vendored library |
| `Hardware.md` | Board pinout, RP2350B-specific gotchas, bring-up history |
| `Video.md` | Full DVI pipeline writeup, timing budget, why you can't block Core 0/1 |
| `Emulator.md` | 8080 core + arcade machine emulation writeup, video RAM rotation, known limitations |
| `lib/PicoDVI/` | Vendored PicoDVI library - used only by `dvi_reference_sample`, not the main app |
| `Sample Code/` | Waveshare's official demo package (gitignored, local-only) used to build `dvi_reference_sample` |

## Adding new source files

New `.c`/`.S` files must be added explicitly to the `add_executable(Space_Invader_PICO ...)`
list in `CMakeLists.txt` - there's no globbing. If a new file needs a PIO program, add it
via `pico_generate_pio_header()` following the existing `dvi_serialiser.pio` example.
