# Hardware Documentation - Space Invader PICO (DVI Color Bar Generator)

## Board Overview

* **Board Name**: Waveshare RP2350-PiZero
* **Microcontroller**: Raspberry Pi RP2350B (80-pin QFN Dual ARM Cortex-M33 / Hazard3 RISC-V)
* **Flash Memory**: 16 MB NOR Flash
* **Video Connector**: Mini-HDMI (DVI-D TMDS differential signal output)
* **Form Factor**: Raspberry Pi Zero footprint

---

## DVI Pin Mapping & Interface Configuration

The DVI video engine utilizes the RP2350's **PIO state machines** and **DMA channels** to drive TMDS differential pairs directly over GPIO pins. This board uses the RP2350B's extended GPIO range (up to GPIO 47); its DVI/mini-HDMI traces are wired to **GPIO 32-39**, confirmed against the board schematic.

### Waveshare RP2350-PiZero DVI Pinout Table

| Signal Channel | Positive Pin (+) | Negative Pin (-) | Description |
| :--- | :--- | :--- | :--- |
| **Clock (CLK)** | **GPIO 38** | **GPIO 39** | TMDS Clock Differential Pair |
| **Data 0 (Blue / Sync)** | **GPIO 36** | **GPIO 37** | TMDS Data Lane 0 (Blue + HSYNC/VSYNC) |
| **Data 1 (Green)** | **GPIO 34** | **GPIO 35** | TMDS Data Lane 1 (Green) |
| **Data 2 (Red)** | **GPIO 32** | **GPIO 33** | TMDS Data Lane 2 (Red) |

| Auxiliary Signal | GPIO | Description |
| :--- | :--- | :--- |
| **DVI_SDA** | **GPIO 44** | DDC data (I2C, unused by this firmware) |
| **DVI_SCL** | **GPIO 45** | DDC clock (I2C, unused by this firmware) |
| **DVI_CEC** | **GPIO 46** | Consumer Electronics Control (unused by this firmware) |

> [!NOTE]
> Differential pairs occupy 2 consecutive GPIO pins ($N$ and $N+1$). For example, setting `.pins_clk = 38` configures GPIO 38 as positive and GPIO 39 as negative.

---

## I2S Audio Pin Mapping

`src/audio/audio_i2s.c` drives an external I2S DAC via PIO1 (PIO0 is fully
claimed by the DVI engine above) + a dedicated DMA IRQ line, independent of
the DVI pipeline's own PIO/DMA/timing budget - see `audio_i2s.h` for the
driver writeup.

| Signal | GPIO | 40-pin header position | Description |
| :--- | :--- | :--- | :--- |
| **BCLK** | **GPIO 18** | Physical pin 12 | I2S bit clock |
| **LRCLK / WS** | **GPIO 19** | Physical pin 35 | I2S word-select (left/right clock) |
| **DOUT** | **GPIO 21** | Physical pin 40 | I2S serial data out (to the DAC's DIN) |

> [!NOTE]
> This board's 40-pin header is wired pin-for-pin compatible with a real
> Raspberry Pi's GPIO layout (confirmed against the official schematic -
> `RP2350-PiZero.pdf`), and GPIO18/19/21 are exactly where a real Pi's I2S
> peripheral sits - so standard I2S DAC HATs (PCM5102, UDA1334A, MAX98357A,
> HiFiBerry DAC, etc.) wire up directly with no pin remapping. BCLK/LRCLK
> must stay on consecutive GPIO numbers (18/19) - `audio_i2s.pio`'s side-set
> field drives both from one 2-bit value.

### External Audio Amplifier (Removed)

> [!NOTE]
> The external MAX98357A amplifier module and GPIO 20 mute control have been removed from this firmware. Audio is handled digitally / via HDMI TMDS streams. GPIO 18, 19, 20, and 21 are freed.

| MAX98357A Pin | Connects To | Description |
| :--- | :--- | :--- |
| **LRC** | **GPIO 19** (LRCLK/WS) | Word-select clock |
| **BCLK** | **GPIO 18** (BCLK) | Bit clock |
| **DIN** | **GPIO 21** (DOUT) | I2S serial audio data (from the RP2350's perspective this is an output, "DOUT") |
| **GAIN** | Left floating (or resistor to GND/VIN per datasheet) | Sets fixed gain (9dB default when floating; 3/6/12/15dB via a resistor divider - see the MAX98357A datasheet's gain table) |
| **SD** | **GPIO 20** | Shutdown/enable, active-low; driven by `src/audio/audio_i2s.c` as a mute control (`audio_i2s_set_mute()`) - high = amplifier enabled, low = muted/shutdown. Physical pin 38 on the 40-pin header, adjacent to BCLK/LRCLK/DOUT (pins 12/35/40) |
| **VIN** | 5V rail (board 40-pin header) | Amplifier supply - needs 5V for full 3W output, not 3V3 |
| **GND** | GND (board 40-pin header) | Common ground with the RP2350-PiZero |

> [!NOTE]
> GAIN is the only pin left as a pure analog/logic strap - `src/audio/audio_i2s.c`
> drives BCLK/LRCLK/DOUT for the I2S signal path and GPIO20/SD as a plain GPIO
> output for mute/enable. `audio_i2s_init()` enables the amp by default;
> `src/audio/sound_effects.c` then drives it from the real cabinet's own port 3
> bit 5 AMP-enable line for the rest of the time the game runs.

> [!NOTE]
> **Hardware bring-up finding**: if output sounds like broadband static/noise
> rather than a clean tone, check the physical BCLK/LRCLK/DIN hookup wiring
> before suspecting firmware. During this project's own bring-up, a real DMA
> bug (`CHAIN_TO` not resetting `READ_ADDR` on retrigger - now fixed in
> `audio_i2s.c`) was found and fixed, but noise persisted afterward; it was
> isolated down to hookup-wire EMI pickup, confirmed by the fact that even
> the simplest possible single-buffer, no-chaining, no-mixer DMA transfer
> still came out noisy. Short, twisted (with a GND wire), and away from the
> DVI GPIO32-39 lines is what actually fixed it - not a code change. Use
> `DEBUG_AUDIO_TEST_TONE` and `DEBUG_AUDIO_ONLY` (`src/display_config.h`) to
> reproduce a clean isolated test if this needs debugging again.

### GPIO already claimed by this project

Consulted when picking the I2S pins above, and worth checking again before
wiring up anything else (e.g. joystick/fire input - still unwired, see
`invaders_machine_set_in1()`):

| GPIO | Used by | Source |
| :--- | :--- | :--- |
| 0 / 1 | UART0 TX/RX (stdio) | `PICO_DEFAULT_UART_TX/RX_PIN`, `Sample Code/boards/waveshare_rp2350_pizero.h` |
| 2 | Free - not actually a WS2812 on this board (see note below) | header pin 3 (standard Pi I2C1 SDA position), per the official schematic |
| 6 / 7 | I2C0 SDA/SCL (board default, not currently driven by this firmware) | `PICO_DEFAULT_I2C_SDA/SCL_PIN`, same board header |
| 14 / 15 / 16 | PIO SNES Controller (Latch/Clock/Data) | `src/input/snes_controller.c` |
| 18 / 19 / 21 | I2S audio (BCLK/LRCLK/DOUT) | `src/audio/audio_i2s.c` |
| 20 | MAX98357A SD (mute/shutdown, active-low) | `src/audio/audio_i2s.c` |
| 32-39 | DVI TMDS (3 data lanes + differential clock) | `src/dvi/dvi_engine.c` |
| 44 / 45 / 46 | DVI DDC/CEC (wired to the mini-HDMI connector's SDA/SCL/CEC pins, unused by this firmware) | board schematic |

### SNES Controller Pin Mapping

`src/input/snes_controller.c` reads a standard SNES controller via a PIO2 state machine (`src/input/snes_controller.pio`):

| Signal | GPIO | 40-pin header position | Description |
| :--- | :--- | :--- | :--- |
| **LATCH** | **GPIO 14** | Physical pin 8 | Latch pulse output (12 µs pulse) |
| **CLOCK** | **GPIO 15** | Physical pin 10 | Shift clock output (500 kHz) |
| **DATA** | **GPIO 16** | Physical pin 36 | Serial data input (internal pull-up enabled) |
| **VCC** | **3.3V / 5V** | Physical pin 1 or 2 | Controller power supply |
| **GND** | **GND** | Physical pin 6, 9, 14, 20, or 34 | Common ground |

> [!NOTE]
> `Sample Code/boards/waveshare_rp2350_pizero.h` defines `PICO_DEFAULT_WS2812_PIN 2`
> with the comment "The PRO Micro doesn't have a plain LED, but a WS2812" - that
> comment and define are boilerplate carried over from a different board's SDK
> file (the RP2040 Pro Micro), not a description of this board's actual PCB.
> Checked against the official schematic (`RP2350-PiZero.pdf`): the only LED on
> this board (`LED1`) is a plain 0603 indicator wired straight to VBUS/3V3
> through a resistor - not addressable, not GPIO-driven, and not on GPIO2. GPIO2
> is simply the header's standard Pi-compatible I2C1 SDA pin (physical pin 3)
> and is free for this project to use.

---

## Operating Parameters & Clock Setup

| Parameter | Value | Details |
| :--- | :--- | :--- |
| **Target Architecture** | `rp2350-arm-s` | ARM Cortex-M33 |
| **Core Voltage ($V_{REG}$)** | `1.25V` | `VREG_VOLTAGE_1_25` (Overclock stability) |
| **System Clock ($f_{SYS}$)** | `252.000 MHz` | Required TMDS bit clock for 640x480p60 (`DVI_BIT_CLK_KHZ` in `src/dvi_display.c`) |
| **Video Timing** | 640x480 @ 60Hz | CEA-861 DVI standard timing (25.2 MHz pixel clock) |
| **Framebuffer Resolution** | 320x240 @ 8bpp | 8-bit palette-indexed (75 KB SRAM), scaled 2x to 640x480 |
| **Color Format** | 8bpp Indexed | 256-entry 24-bit RGB palette LUT mapped in `scratch_y` |

---

## System Resource Allocation

### Dual-Core Processing Architecture
* **Core 0**:
  * Initializes system clock (252 MHz), voltage regulator (1.25V), and stdio (USB CDC + UART).
  * Executes Intel 8080 CPU emulation & SNES controller input decoding.
  * Renders 8bpp scanlines directly into `fb` (`game_render_scanline()`).
  * Generates 32 kHz stereo PCM audio samples in per-frame batches (`audio_i2s_step_frame()`).
  * Microsecond wall-clock anchored loop (`sleep_until()`).
* **Core 1**:
  * Runs `frank_hdmi_run_core1()` from `lib/frank-hdmi-audio`.
  * Converts 8bpp palette indices to RGB565 via `fill_scanline()` in `scratch_y`.
  * TMDS hardware encoding via RP2350 SIO interpolator.
  * Serviced by `DMA_IRQ_1` to inject HDMI Data Island packets (Audio samples, InfoFrames, ACR) during H-blanking.

I2S audio (`src/audio/audio_i2s.c`) doesn't fit neatly into this split: it's
initialized on Core 0 (`main.c`, before `multicore_launch_core1()`) but then
runs entirely off its own PIO1 state machine + ping-pong DMA once started -
no further attention from either core's hot loop, so it doesn't compete with
either core's own timing budget.

### Hardware Peripherals Used
* **PIO0**: Runs the DVI TMDS serializer state machines (State Machines 0, 1, 2) for the three data lanes.
* **PIO1**: Runs the I2S audio transmitter state machine (`src/audio/audio_i2s.pio`) - entirely separate from PIO0/DVI.
* **PWM**: Drives the differential clock pair (GPIO 38/39) - a PWM slice generates a 50% duty-cycle complementary output, rather than the clock being bit-banged through PIO.
* **DMA**: Transfers encoded TMDS buffers to the PIO TX FIFOs (`DMA_IRQ_0`), and separately feeds the I2S PIO via a 2-channel ping-pong chain (`DMA_IRQ_1`) - the two are on different IRQ lines so audio buffer refills can never be delayed by (or delay) the DVI engine's own IRQ handler.
* **Bus Priority**: Core 1 DMA given elevated bus priority via `BUSCTRL_BUS_PRIORITY_PROC1_BITS`.
* **USB CDC + UART**: Stdio telemetry is mirrored to both USB serial and UART0 (GPIO0 TX / GPIO1 RX, `115200` baud). UART requires no host handshake, so debug output and DVI/Core 1 bring-up are never gated behind USB enumeration.

---

## RP2350B PIO Requirements for GPIO >= 32

This board's DVI pins (32-39) sit in the upper half of the RP2350B's extended GPIO range, which several parts of the PIO ecosystem handle differently from GPIO 0-31:

1. **HSTX is not an option.** The RP2350's native HSTX peripheral is physically fixed to GPIO 12-19 only - it cannot be routed to GPIO 32-39 in any configuration. TMDS output goes through PIO + DMA instead, same as on RP2040.

2. **PIO GPIO base must be shifted.** Each PIO block can only address 32 consecutive GPIOs at a time (window 0-31 or 16-47, selected at runtime via `pio_set_gpio_base()`). Since the TMDS pins (32-39) sit above the default 0-31 window, `pio_set_gpio_base(pio0, 16)` must be called before `dvi_engine_init()` (which claims the PIO program/state machines) - and while the PIO instance has no programs loaded yet. The project also sets `PICO_PIO_USE_GPIO_BASE=1` at compile time.

3. **The PIO pin-mask helper needs the 64-bit variant.** `src/dvi/dvi_serialiser.pio` configures the TMDS data pins using `pio_sm_set_pins_with_mask64()` / `pio_sm_set_pindirs_with_mask64()`, since the 32-bit variants only support pins < 32. (This file is our own trimmed copy of the vendored PIO program - see `Video.md` - but this fix carries over unchanged, since it's inherent to GPIO32-39 on RP2350B, not vendor-specific.)

---

## Reference Materials

* **`Sample Code/`** - Waveshare's official RP2350-PiZero demo package (DVI, USB, MicroSD examples), from the [official wiki](https://www.waveshare.com/wiki/RP2350-PiZero), including a prebuilt `uf2/01-DVI.uf2` and its own `libdvi`.
* **`dvi_reference_sample`** build target (see `CMakeLists.txt`) - builds `Sample Code/01-DVI/apps/hello_dvi/main.c` (Waveshare's own hello_dvi demo) against the vendored `libdvi`, as a hardware/toolchain control-test reference. The main `Space_Invader_PICO` app does **not** use `libdvi` - it has its own slim, board-specific engine in `src/dvi/` (see `Video.md`). `lib/PicoDVI` is kept in the repo solely to build this reference target.

---

## Serial Terminal Diagnostics Output

Connecting a serial terminal (such as VS Code Serial Monitor or TeraTerm) over either the USB CDC port or UART0 (GPIO0/GPIO1, `115200` baud) displays hardware status:

```text
==================================================
  Space Invader PICO  v0.8.0
==================================================
[DEBUG] Microcontroller: RP2350B (Cortex-M33)
[DEBUG] Core Voltage   : 1.25V
[DEBUG] System Clock   : 252000000 Hz (Requested: 252000 kHz)
[DEBUG] RP2350 detected - using PIO/DMA TMDS serialiser
[DEBUG] (HSTX not used: DVI pins are outside its fixed GPIO12-19 range)
[DEBUG] --- DVI Pinout Configuration ---
[DEBUG] Clock Pin Pair  : GPIO 38 / 39
[DEBUG] Data 0 Pin Pair : GPIO 36 / 37 (Blue/Sync)
[DEBUG] Data 1 Pin Pair : GPIO 34 / 35 (Green)
[DEBUG] Data 2 Pin Pair : GPIO 32 / 33 (Red)
[DEBUG] Setting PIO GPIO base to 16 (required for GPIO32-39)...
[DEBUG] Initializing DVI engine...
[DEBUG] DVI engine initialized successfully.
[DEBUG] Pre-rendering colorbar line buffers...
[DEBUG] DEBUG_TESTCARD enabled: test card for 5 seconds, then the game.
[DEBUG] Launching Core 1 for DVI TMDS serialiser...
[DEBUG] Core 1 launched.

[STATUS] Rendering DVI 640x480 @ 60Hz...
```

The version string (`v0.2.0`) comes from `PROJECT_VERSION` in `CMakeLists.txt`'s `project()` call - `SPACE_INVADER_PICO_VERSION` is passed through as a compile definition so `main.c` doesn't need a second, separately-maintained copy of it.

> [!NOTE]
> No output is printed once rendering starts. An earlier per-second
> heartbeat print was removed after it was found to block Core 0 on
> UART TX long enough to starve the DVI pipeline's buffers and cause a
> visible flicker - see `Video.md` for the full explanation.
