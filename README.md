# Space Invader PICO

**Version: 0.5.0**

A real emulator of the 1978 Taito/Midway Space Invaders arcade PCB (Intel 8080 CPU, memory map, I/O ports and shift-register sprite hardware) for the [Waveshare RP2350-PiZero](https://www.waveshare.com/wiki/RP2350-PiZero), written in C against the Raspberry Pi Pico SDK, driving DVI video output over the board's mini-HDMI connector. This runs the *actual* arcade ROM (user-supplied - see [`roms/README.md`](roms/README.md)), not a from-scratch reimplementation of the game logic.

**Status: early stage.** The DVI video pipeline, the 8080 emulator core + video output, SNES-controller input, and sound-effect playback (embedded into the HDMI signal itself - no physical audio hardware on this board) are built and working, though real audio depends on you supplying sample files and embedded-HDMI-audio playback specifically still needs verification against real display hardware - see [Roadmap](#roadmap) and [`Emulator.md`](Emulator.md).

## What's here right now

- An Intel 8080 CPU interpreter and Space Invaders arcade machine emulation (`src/emu/`) - full instruction set, real port/shift-register hardware, running the unmodified original ROM. See [`Emulator.md`](Emulator.md).
- A custom, board-specific DVI/TMDS output engine (`src/dvi/`) - PIO + DMA driven (this board's DVI pins fall outside the RP2350's HSTX peripheral's fixed GPIO range, so HSTX isn't an option here; see [`Video.md`](Video.md)).
- A 320x240 RGB565 framebuffer, scaled 2x to the board's fixed 640x480p60 DVI output. The emulated machine's 256x224 video RAM is un-rotated and letterboxed into it, with the classic red/white/green cabinet overlay tint reproduced at the video-conversion stage.
- A debug test card (color bars, grayscale ramp, moving sync bar) for verifying the display pipeline independent of any game code.
- A `dvi_reference_sample` build target that builds Waveshare's own official demo, kept as a hardware/toolchain control test.

## Hardware

- Waveshare RP2350-PiZero board
- A mini-HDMI cable and a DVI/HDMI-capable display
- USB cable for flashing/power

## Building

This project targets the [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) (developed against SDK 2.3.0) and the `waveshare_rp2350_pizero` board definition.

**You need the real arcade ROM.** This project doesn't include Taito's copyrighted ROM - drop your own legally-obtained dump into `roms/` before building (see [`roms/README.md`](roms/README.md) for the exact files/names needed). Without it, the firmware still builds (against a zero-filled placeholder) but won't run the actual game.

**Easiest path**: open the folder in VS Code with the [Raspberry Pi Pico extension](https://marketplace.visualstudio.com/items?itemName=raspberry-pi.raspberry-pi-pico) installed - the `.vscode/` config in this repo is already set up for it. Build, then flash via the extension or by copying the generated `.uf2` to the Pico while it's in BOOTSEL mode.

**Manual CLI build**, once you have the Pico SDK, ARM toolchain, CMake, and Ninja on your `PATH`:

```sh
cmake -S . -B build -G Ninja -DPICO_BOARD=waveshare_rp2350_pizero
cmake --build build
```

This produces `build/Space_Invader_PICO.uf2`. Hold BOOTSEL while plugging in the board (or double-tap reset if already running), then copy the `.uf2` onto the drive that appears.

The same build also produces `build/dvi_reference_sample.uf2` - Waveshare's own official DVI demo, useful as a known-good fallback if you ever suspect a hardware or toolchain problem rather than an application bug (see [`Hardware.md`](Hardware.md)). Note: this target needs `Sample Code/` on disk locally (it's gitignored - see that folder's absence after a fresh clone), since it's Waveshare's demo package, not part of this project.

## Debug test card

By default, the app shows the debug test card for 5 seconds at boot, then switches to the game. This is controlled by `DEBUG_TESTCARD` in `src/display_config.h`:

```c
#define DEBUG_TESTCARD 1          // 0 to skip the test card and boot straight into the game
#define DEBUG_TESTCARD_SECONDS 5
```

## Project layout

| Path | What it is |
|---|---|
| `src/main.c` | Entry point; scanline dispatch loop (test card → game handoff) |
| `src/game.c` / `.h` | Paces the emulated CPU against the frame loop and converts its video RAM into scanlines - see [`Emulator.md`](Emulator.md) |
| `src/emu/` | The Intel 8080 CPU core + Space Invaders arcade machine emulation (memory map, ports, shift register) - see [`Emulator.md`](Emulator.md) |
| `roms/` | Where you put the real arcade ROM (gitignored, not vendored - see `roms/README.md`) |
| `src/testcard.c` / `.h` | Debug test pattern generator |
| `src/display_config.h` | Framebuffer size, colors, refresh rate, debug flags |
| `src/dvi_display.c` / `.h`, `src/dvi/` | The DVI output engine - see [`Video.md`](Video.md) for the full pipeline writeup |
| `Hardware.md` | Board pinout, RP2350B-specific gotchas, bring-up history |
| `Video.md` | How the DVI pipeline actually works, timing budget, why you can't block Core 0/1 |
| `Emulator.md` | How the 8080 CPU core + arcade machine emulation works, video RAM rotation, known limitations |
| `lib/PicoDVI/` | Vendored [PicoDVI](https://github.com/Wren6991/PicoDVI) library - used only by the `dvi_reference_sample` control-test target, not by the main app |
| `Sample Code/` | Waveshare's official demo package (gitignored - local reference only, used to build `dvi_reference_sample`) |

## Roadmap

- [x] DVI bring-up on this board's GPIO32-39 pinout
- [x] Custom slim DVI engine (own code, not the general-purpose vendored library)
- [x] Intel 8080 CPU core + Space Invaders arcade machine emulation, running the real ROM
- [x] Video RAM → framebuffer conversion (rotation, letterboxing, color overlay)
- [ ] Joystick/fire input wired to `invaders_machine_set_in1()`
- [x] I2S audio output driver (`src/audio/`) - PIO1 + DMA, currently a bring-up test tone
- [ ] Sound (ports 3/5 discrete sound-effect bits, currently dropped, not yet decoded into real playback)

## License

[MIT](LICENSE) for this project's own code (the DVI engine, the 8080 CPU core and arcade machine emulation in `src/emu/`). This project vendors [PicoDVI](https://github.com/Wren6991/PicoDVI) (BSD-3-Clause, see `lib/PicoDVI/LICENSE`) for the `dvi_reference_sample` control-test target, and references Waveshare's official RP2350-PiZero demo package for the same purpose (see that package's own license, in `Sample Code/01-DVI/LICENSE` once fetched locally). **Not covered**: the Space Invaders arcade ROM itself is Taito/Midway's copyrighted work, is not included in this repository, and is not covered by this project's MIT license - see `roms/README.md`.
