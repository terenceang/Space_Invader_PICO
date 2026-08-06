# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

An emulator of the real 1978 Taito/Midway Space Invaders arcade PCB (Intel 8080 CPU,
memory map, I/O ports, shift-register sprite hardware - `src/emu/`) for the Waveshare
RP2350-PiZero board, written in C against the Raspberry Pi Pico SDK, driving DVI video
output over the board's mini-HDMI connector. It runs the *actual* arcade ROM, not a
reimplementation of the game logic - see `Emulator.md`. **Status: early stage** - the DVI
video pipeline (`src/dvi/`, `src/dvi_display.*`), the CPU core + video output
(`src/emu/`, `src/game.c`), SNES-controller input (`src/input/`, mapped to the
emulated machine's coin/start/joystick/fire inputs), and sound-effect playback
(`src/audio/`, driven by the emulated machine's own port 3/5 writes, embedded into
the mini-HDMI connector's HDMI Data Islands by `src/dvi/hdmi_audio.c` rather than
physical I2S hardware - see `DVI_ENABLE_HDMI_AUDIO` in `src/display_config.h`) are
built and working, though real audio depends on you supplying sample files (see
`sounds/README.md`) and embedded-HDMI-audio playback specifically still needs
verification against real display hardware. See the Roadmap in `README.md`.

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

### Debug test cards

`src/display_config.h` controls two independent debug screens shown before/instead of the
game, both off by default:

```c
#define DEBUG_TESTCARD 0             // 1 to show the color-bar/grayscale test card at boot
#define DEBUG_TESTCARD_SECONDS 5     // seconds to show it before handing off (0 = permanent)
#define DEBUG_CONTROLLER_TESTCARD 0  // 1 to show a live SNES button diagram instead of the game
```

`testcard.c` draws the color-bar/grayscale/moving-sync-bar pattern; `controller_testcard.c`
draws a button-diagram (D-pad, face buttons, shoulders, select/start) that lights each
button green while held, using the same `snes_controller_read()` the game itself uses - a
hardware/wiring check independent of the emulator or ROM. If both are enabled, the
color-bar card shows first, then the controller card. See `main.c` for how the two are
sequenced into the per-scanline render loop.

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
landscape - `SI_DISPLAY_ROTATION` (0/90/180/270 degrees) and `SI_DISPLAY_FLIP_H`/
`SI_DISPLAY_FLIP_V` in `src/display_config.h` tell `render_arcade_row()` how the physical
display here is actually mounted; 16 combinations cover every fixed orientation. If the
image comes out wrong (sideways, upside down, mirrored, or overlay bands on the wrong
edge), that's a `display_config.h` value to try, not a rendering-code change - see
`Emulator.md`'s "Screen orientation" section, which also explains why this is exposed as
a couple of numbers to experiment with rather than one hardcoded transform: several
earlier attempts at deriving "the one correct" transform by hand were each wrong in a
different way.

Inputs are wired via `invaders_machine_set_in1()`, called every frame from `game.c` with
the SNES controller's decoded button state (`src/input/snes_controller.c`) -
SELECT/START/LEFT/RIGHT/A|B|X|Y map to coin/start/joystick/fire respectively.

**The real arcade ROM is not vendored** - it's loaded from 4 user-supplied files in
`roms/` (gitignored) and embedded into the flash image at build time by
`cmake/generate_rom.cmake` (see `CMakeLists.txt`'s custom command generating
`generated/rom_data.c`). If you add anything that needs to know ROM contents at build
time, that generated file / `src/emu/rom_data.h` is where to look.

#### DVI & HDMI Audio pipeline (`lib/frank-hdmi-audio`, `src/dvi_display.*`)

The essentials:

**This board's DVI pins (GPIO 32-39) fall outside the RP2350's HSTX peripheral's fixed
GPIO 12-19 range, so HSTX cannot be used.** Video output and HDMI Data Island audio are driven by
`lib/frank-hdmi-audio` - a high-performance 8bpp palettized driver configured for GPIO 32-39,
640x480p60 output, 320x240 8bpp palettized framebuffer, and 32 kHz stereo PCM HDMI embedded audio.

**Dual-core split**, set up in `main.c` / `dvi_display.c`:
- **Core 0** (`main.c`): scanline / frame producer & CPU emulator. Updates the 320x240 8-bit
  palette-indexed framebuffer (`fb`) and pushes 32 kHz PCM audio samples via `frank_hdmi_audio_write()`.
- **Core 1** (`dvi_display.c: core1_main()`, never returns): runs
  `frank_hdmi_run_core1()` (`lib/frank-hdmi-audio/src/frank_hdmi.c`), which converts the 8bpp
  framebuffer to RGB565 via a 256-entry LUT in `scratch_y`, TMDS-encodes scanlines, injects audio
  Data Islands during blanking, and drives PIO TX FIFOs for the three TMDS data lanes on GPIO 32-39.

**Resolution scaling & Palette LUT**:
Logical 320x240 8bpp framebuffer is scaled to 640x480 wire timing via pixel doubling during TMDS
encode and vertical line doubling in DMA IRQs. 256 palette entries (0xRRGGBB) are mapped via
`frank_hdmi_set_palette()`.

### File map

| Path | Role |
|---|---|
| `src/main.c` | Entry point; Core 0 scanline/frame producer loop & audio dispatch |
| `src/game.c` / `.h` | Paces the emulated CPU against the frame loop, converts video RAM into 8bpp scanlines |
| `src/emu/i8080.c` / `.h` | Intel 8080 CPU interpreter - full instruction set, no machine-specific knowledge |
| `src/emu/invaders_machine.c` / `.h` | Space Invaders memory map, I/O ports, shift register, interrupt delivery |
| `src/emu/rom_data.h` | Declares the embedded ROM array defined by the CMake-generated source |
| `roms/` | User-supplied real arcade ROM files go here (gitignored, not vendored) |
| `cmake/generate_rom.cmake` | Embeds `roms/invaders.{h,g,f,e}` into a linkable C array at build time |
| `src/testcard.c` / `.h` | Debug color-bar test pattern generator (8bpp palettized) |
| `src/controller_testcard.c` / `.h` | Debug SNES-controller button diagram (lights up per button, live) |
| `src/display_config.h` | Framebuffer size, 8-bit palette color constants, refresh rate, debug flags |
| `src/dvi_display.c` / `.h` | Clock/voltage setup, palette setup, Core 1 entry point launching `frank_hdmi_run_core1()` |
| `lib/frank-hdmi-audio/` | Core DVI + HDMI Data Island audio driver library (8bpp LUT, PIO TMDS serialisers, DMA IRQs) |
| `src/audio/audio_i2s.c` / `.h` | Software audio mixer - mixes sound-effect voices into 32 kHz stereo PCM and outputs via `frank_hdmi_audio_write()` |
| `src/audio/sound_effects.c` / `.h` | Decodes port 3/5 sound-effect bits into `audio_i2s_*` calls |
| `src/audio/sound_data.h` | `sound_id_t` enum + `sound_sample_t`/`sound_table[]` declarations |
| `sounds/` | User-supplied sound-effect PCM files |
| `cmake/generate_sounds.cmake` | Embeds `sounds/*.pcm` into `sound_table[]` at build time |
| `Hardware.md` | Board pinout (DVI + I2S audio), RP2350B-specific gotchas, bring-up history |
| `Video.md` | Full DVI pipeline writeup, timing budget, why you can't block Core 0/1 |
| `Emulator.md` | 8080 core + arcade machine emulation writeup, video RAM rotation, known limitations |
| `lib/PicoDVI/` | Vendored PicoDVI library - used only by `dvi_reference_sample`, not the main app |
| `Sample Code/` | Waveshare's official demo package (gitignored, local-only) used to build `dvi_reference_sample` |

## Adding new source files

New `.c`/`.S` files must be added explicitly to the `add_executable(Space_Invader_PICO ...)`
list in `CMakeLists.txt` - there's no globbing. If a new file needs a PIO program, add it
via `pico_generate_pio_header()` following the existing `snes_controller.pio` example.

## Key Attributions

- `lib/frank-hdmi-audio`: Mikhail Matveev (`xtreme@rh1.tech`, https://github.com/rh1tech/frank-hdmi-audio).
- `PicoDVI-audio`: Shuichi Takano (https://github.com/shuichitakano/PicoDVI-audio).
- `PicoDVI`: Luke Wren (`Wren6991`, https://github.com/Wren6991/PicoDVI).
- `Space Invaders Arcade Hardware Specifications`: Computer Archeology team & Paul Robson (http://www.computerarcheology.com/Arcade/SpaceInvaders/).
