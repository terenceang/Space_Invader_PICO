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

## Operating Parameters & Clock Setup

| Parameter | Value | Details |
| :--- | :--- | :--- |
| **Target Architecture** | `rp2350-arm-s` | ARM Cortex-M33 |
| **Core Voltage ($V_{REG}$)** | `1.25V` | `VREG_VOLTAGE_1_25` (Overclock stability) |
| **System Clock ($f_{SYS}$)** | `252.000 MHz` | Required TMDS bit clock for 640x480p60 (`DVI_BIT_CLK_KHZ` in `src/dvi_display.c`) |
| **Video Timing** | 640x480 @ 60Hz | CEA-861 DVI standard timing (25.2 MHz pixel clock) |
| **Framebuffer Resolution** | 320x240 @ 16bpp | Scaled 2x horizontally and vertically to 640x480 |
| **Color Format** | RGB565 (16-bit) | 5 bits Red, 6 bits Green, 5 bits Blue |

---

## System Resource Allocation

### Dual-Core Processing Architecture
* **Core 0**:
  * Initializes system clock, voltage regulator, and stdio (USB CDC + UART).
  * Pre-renders scanline pattern buffers.
  * Pushes active scanline pointers into `dvi_q_colour_valid`.
  * Prints one-time boot/diagnostic banner over serial before the render loop starts (no periodic output once rendering begins - see `Video.md` for why).
* **Core 1**:
  * Dedicated high-speed thread executing `dvi_engine_encode_loop()` (`src/dvi/dvi_engine.c`).
  * Encodes 16-bit RGB pixels into 10-bit TMDS symbols on the fly.
  * Handles DMA IRQ interrupts.

### Hardware Peripherals Used
* **PIO0**: Runs the DVI TMDS serializer state machines (State Machines 0, 1, 2) for the three data lanes.
* **PWM**: Drives the differential clock pair (GPIO 38/39) - a PWM slice generates a 50% duty-cycle complementary output, rather than the clock being bit-banged through PIO.
* **DMA**: Transfers encoded TMDS buffers to the PIO TX FIFOs.
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
  Space Invader PICO  v0.1.0
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

The version string (`v0.1.0`) comes from `PROJECT_VERSION` in `CMakeLists.txt`'s `project()` call - `SPACE_INVADER_PICO_VERSION` is passed through as a compile definition so `main.c` doesn't need a second, separately-maintained copy of it.

> [!NOTE]
> No output is printed once rendering starts. An earlier per-second
> heartbeat print was removed after it was found to block Core 0 on
> UART TX long enough to starve the DVI pipeline's buffers and cause a
> visible flicker - see `Video.md` for the full explanation.
