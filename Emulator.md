# Emulator Core - How the "game" actually runs

This project doesn't reimplement Space Invaders' game logic in C. `src/emu/`
emulates the real 1978 Taito/Midway arcade PCB - an Intel 8080 CPU, its
memory map, its I/O ports and shift-register sprite hardware - and runs the
*actual arcade ROM* on it (supplied locally by the user, see
[`roms/README.md`](roms/README.md); never vendored in this repo). `src/game.c`
is just the glue that paces that emulated CPU against this project's own
frame loop and turns its video RAM into scanlines for the DVI pipeline in
`src/dvi/`. For how those scanlines actually get to the screen, see
[`Video.md`](Video.md) - this document stops at "here's a 320x240 RGB565
scanline," which is where `Video.md`'s pipeline picks up.

---

## File map

| File | Role |
|---|---|
| `src/emu/i8080.h` / `.c` | Self-contained Intel 8080 interpreter - registers, flags, all 256 opcodes, interrupt delivery. No knowledge of Space Invaders or the Pico SDK; wired up via function pointers. |
| `src/emu/invaders_machine.h` / `.c` | The arcade PCB wiring around that CPU: ROM/RAM memory map, input ports, the shift-register sprite hardware, and the two per-frame interrupts. |
| `src/emu/rom_data.h` | Declares the embedded ROM array defined by the CMake-generated source (see below). |
| `roms/README.md` | What ROM files to supply and where. |
| `cmake/generate_rom.cmake` | Turns `roms/invaders.{h,g,f,e}` into a linkable C byte array at build time. |
| `src/game.c` | Runs the CPU in small slices interleaved with scanline output; un-rotates video RAM into the framebuffer; applies the classic color-overlay tint. |

## The CPU core (`src/emu/i8080.c`)

A from-scratch interpreter for the full documented Intel 8080 instruction
set, including the well-known undocumented opcode *duplicates* real 8080
silicon exhibits (these are hardware facts, not a shortcut):

- `0x08/0x10/0x18/0x20/0x28/0x30/0x38` all behave as `NOP`
- `0xCB` behaves as `JMP`
- `0xD9` behaves as `RET`
- `0xDD/0xED/0xFD` all behave as `CALL`

Register-field decoding for the four densely-packed instruction blocks
(`MOV r,r'`, the ALU-immediate-source group, `MVI`, `INR`/`DCR`) is done
generically from the opcode bits rather than as ~150 near-duplicate switch
cases - the 8080 encodes an 8-bit operand register the same way in all four
(`0`=B `1`=C `2`=D `3`=E `4`=H `5`=L `6`=memory-via-HL `7`=A).

Flag behaviour follows real 8080 quirks that matter for this specific ROM,
not just "a plausible ALU":

- `DAA` is fully implemented (double-dabble BCD correction) because Space
  Invaders' scoring code keeps the on-screen score in packed BCD and uses
  `DAA` to maintain it - getting this wrong shows up immediately as garbled
  score digits.
- `ANA` sets the auxiliary-carry flag to the **logical OR of bit 3 of both
  operands** - a documented real-hardware quirk, not always-0 like `XRA`/`ORA`.

The core has zero dependency on the Pico SDK or this project's memory map -
it's wired to a specific machine purely through the `mem_read`/`mem_write`/
`io_in`/`io_out`/`ctx` function pointers in `i8080_t`, set up by
`invaders_machine_init()`.

## The machine (`src/emu/invaders_machine.c`)

Memory map:

| Range | Contents |
|---|---|
| `$0000-$1FFF` | ROM (`space_invaders_rom`, embedded from `roms/`) |
| `$2000-$23FF` | Work RAM |
| `$2400-$3FFF` | Video RAM (7168 bytes, 256x224 1bpp) |
| `$4000-$FFFF` | Mirrors `$2000-$3FFF` (real PCB doesn't fully decode the top address bits; the ROM never actually reads/writes up here) |

I/O ports (matching the real cabinet's wiring):

| Port | Direction | Purpose |
|---|---|---|
| 0, 1, 2 | Read | Input bits (coin/start/joystick/fire, DIP switches) |
| 3 | Read | Shift-register result (see below) |
| 2 | Write | Shift-register read offset (0-7 bits) |
| 4 | Write | Shifts a new byte into the shift register |
| 3, 5 | Write | Discrete sound-effect trigger bits - **not emulated yet**, writes are accepted and dropped |
| 6 | Write | Watchdog reset strobe - **not emulated**, no-op |

**The shift register** is the real hardware's trick for drawing
arbitrarily bit-shifted sprites (bullets, aliens, the player ship) without
the CPU doing the shifting itself: `OUT 4` pushes a new byte in from the
top (`shift_register = (new_byte << 8) | (shift_register >> 8)`), `OUT 2`
sets a 0-7 bit offset, and `IN 3` returns
`(shift_register >> (8 - offset)) & 0xFF` - an 8-bit window into the
16-bit register at an arbitrary bit position. The game uses this
constantly; without it, sprites would render torn or not move smoothly
between byte boundaries.

**Inputs are currently unwired.** `invaders_machine_set_in1()` exists and
the `SI_IN1_*` bit masks match the real cabinet's port layout, but nothing
calls it yet - this pass is CPU core + video only (see the project
[Roadmap](README.md#roadmap)). With no coin inserted and no start pressed,
the ROM runs its own real attract-mode/demo loop untouched, exactly as
real hardware does sitting idle - which is itself a nice side effect of
emulating the actual ROM instead of writing new game logic.

## Interrupt timing vs. our frame loop

Real hardware interrupts the CPU twice per 60Hz frame, synced to the CRT
beam: `RST 1` at mid-screen, `RST 2` at vblank. This project has no literal
CRT beam position to sync against - `main.c`'s scanline loop just calls
`game_get_scanline(y, ...)` for `y` in `0..239` once per (roughly) 60Hz
iteration, throttled by the DVI queue rather than by real video timing.

`game.c` approximates the real timing by spreading the frame's total cycle
budget evenly across those 240 calls (`SI_CYCLES_PER_ROW = SI_CYCLES_PER_FRAME
/ FRAME_HEIGHT`), running that slice before producing each scanline, and
firing `RST 1` at `y == FRAME_HEIGHT/2` and `RST 2` at `y == 0` (i.e. "start
of frame", functionally equivalent to "end of the previous one"). The
*total* cycles per frame and per interrupt-half are correct; they're just
not distributed at true CRT-scanline granularity. This is a deliberate,
documented simplification (see Limitations below) - not something the
actual game logic is sensitive to.

Running the CPU in small slices this way, rather than as one big per-frame
burst, is also what keeps this compatible with the DVI pipeline's hard
timing budget - see `Video.md`'s "Timing budget" section. A slice of
~139 cycles (`33280 cycles/frame / 240 rows`) takes on the order of a few
microseconds to interpret, comfortably inside Core 0's per-scanline budget;
running the whole frame's ~33280 cycles in one burst before touching any
scanlines would stall the producer for the better part of a millisecond -
several times the DVI queue's entire slack - and show up as the same
solid-red flicker that a blocking `printf()` caused during DVI bring-up.

## Screen orientation / video RAM rotation (`render_arcade_row()` in `src/game.c`)

The real cabinet's monitor is mounted **vertically** (portrait) - this is
the actual arcade hardware's native orientation (confirmed by, among other
things, MAME's driver for this game using the `ROT270` orientation flag,
which exists specifically to mark games whose cabinet monitor is physically
rotated from normal landscape). The game draws into video RAM in that
native vertical scan order, not in the "normal, landscape" left-to-right/
top-to-bottom order a hobbyist emulator running on an ordinary PC monitor
would want. Video RAM is 7168 bytes = 224 columns x 32 bytes each (256 bits
per column): byte `col*32 + row/8`, bit `row%8`.

`render_arcade_row()` in `src/game.c` supports both physical setups via
`SI_DISPLAY_ROTATED_CCW` in `src/display_config.h`:

- **`SI_DISPLAY_ROTATED_CCW == 0`** - a normal, un-rotated landscape
  monitor (the common case for a hobbyist display, and this project's
  original default before the cabinet-accurate portrait mount was wired
  up). Un-rotates video RAM's native vertical addressing into a normal
  upright wide image: displayed row `ay` (0 = top) reads from raw column
  `223 - ay`, and that column's 256 bits become the row's 256 pixels read
  low-bit-first, left to right.
- **`SI_DISPLAY_ROTATED_CCW == 1`** (default) - the physical display is
  itself mounted rotated 90 degrees **counter-clockwise**, matching the
  real cabinet's vertical monitor. In this case the framebuffer we
  transmit should show the landscape-case image rotated a *further* 180
  degrees, so that combined with the physical CCW mount downstream, the
  viewer sees an upright image: raw column is read directly as `ay` (not
  flipped), and the 256 bits are read high-bit-first (flipped) to become
  pixels left to right. The overlay bands (below) are computed from
  `223 - ay` in this mode specifically so red/green don't end up swapped
  top-for-bottom.

  *Derivation, for anyone changing this*: our DVI engine's transmission
  timing is fixed at 640x480p60 landscape (see `Video.md`) regardless of
  how the physical screen is mounted - a physical rotation is applied
  downstream of whatever we send, by the monitor. If `T` is the frame we
  transmit and the monitor is rotated CCW, the viewer perceives
  `CCW90(T)`. We want that to equal the correct upright image, which -
  since raw video RAM read directly (no software rotation at all) is
  itself the correct image for an *un-rotated* vertical monitor - works
  out to `CW90(landscape)`, where `landscape` is this file's
  `SI_DISPLAY_ROTATED_CCW == 0` output. Solving `CCW90(T) = CW90(landscape)`
  for `T` gives `T = CW90(CW90(landscape))`, i.e. `landscape` rotated a
  further 180 degrees - which is exactly "flip the column that was
  flipped, and flip the bit-read that wasn't."

Either way, the 256x224 active image is centered in the 320x240
framebuffer (`SI_FB_X_OFFSET`/`SI_FB_Y_OFFSET`, 16px/8px black letterbox
border) rather than stretched, to keep pixels square. The letterbox
dimensions are identical in both modes - only the pixel *sourcing* inside
that box changes - since a rotation doesn't change the image's bounding
box, only its content's arrangement within it.

## Color overlay

The real machine's video hardware only ever outputs 1-bit black/white -
the color you remember from the cabinet came from cellophane/acetate
strips glued over the glass: red across the top ~32 rows (score, UFO),
green across the bottom ~40 rows (shields, player ship), and clear
elsewhere. `render_arcade_row()` reproduces this by tinting lit pixels
based on which row band they fall in (`SI_OVERLAY_RED_ROWS`,
`SI_OVERLAY_GREEN_ROWS`) - purely a video-conversion-stage cosmetic; it has
no effect on and no input from the CPU emulation. The band check always
uses the row's position in upright-landscape terms (see the "Screen
orientation" section above) regardless of `SI_DISPLAY_ROTATED_CCW`, so the
red/green bands land in the right place either way.

## Limitations (this pass: CPU core + video only)

- **No sound.** Ports 3/5 (the discrete sound-effect trigger bits) are
  read/accepted but dropped - see the Roadmap.
- **No input.** Coin/start/joystick/fire aren't wired to anything - the ROM
  runs its attract-mode loop indefinitely. `invaders_machine_set_in1()` is
  ready for whatever GPIO/controller wiring comes next.
- **Interrupt timing is frame-cycle-budget-accurate, not CRT-scanline-accurate**
  (see above) - correct in total cycles delivered per interrupt period,
  approximate in exactly which of our 240 scanline-producer calls they land on.
- **No watchdog.** Real cabinets reset if the ROM stops periodically
  strobing port 6 (a hung game resets itself); this emulator just keeps
  running a hung CPU state forever. Not expected to matter since we're
  running the unmodified real ROM, which strobes it correctly.
