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

`src/game.c` supports both physical setups via `SI_DISPLAY_ROTATED_CCW` in
`src/display_config.h`, with **two structurally different
`render_arcade_row()` implementations** (selected at compile time) rather
than a shared one with a couple of flipped indices - two earlier attempts
at a shared, flip-only implementation were both wrong (mirrored images,
overlay bands on the wrong screen edge), because the fix isn't a matter of
picking the right flip:

**The core issue**: `col` identifies which row of the *game* a pixel
belongs to (score/UFO near col 223, shields/ship near col 0) - this is
what the overlay bands are keyed to (`overlay_color_for_col()`). A
90-degree screen-plane rotation swaps which of the transmitted image's two
axes - our row axis (`ay`, driving one call to `render_arcade_row()` each)
or our column axis (`x`, the fast loop inside it) - ends up "horizontal"
vs. "vertical" for the viewer standing in front of the rotated monitor.
For the overlay bands to appear as horizontal top/bottom stripes (matching
the real cabinet) rather than vertical left/right ones, `col` needs to be
driven by whichever of our two axes becomes the viewer's *vertical* axis
after rotation - which is a **different axis** than in the un-rotated
case. That's a transpose, not a flip - and no combination of flipping
`col` or the pixel read direction within a fixed axis assignment can fix
it, which is exactly why the two earlier attempts (see git history)
couldn't converge no matter which flip was tried.

- **`SI_DISPLAY_ROTATED_CCW == 0`** (normal landscape monitor): `col` is
  driven by `ay` (our row axis) - `col = 223 - ay`. Each row of 256 bits is
  read low-bit-first (`bitpos = x`) to become that row's 256 pixels.
  Centered in the 320x240 framebuffer with a 32px/8px black letterbox
  border (`SI_FB_X_OFFSET`/`SI_FB_Y_OFFSET`).
- **`SI_DISPLAY_ROTATED_CCW == 1`** (default; physical monitor mounted
  rotated 90 degrees from landscape, matching the real cabinet): `col` is
  instead driven by `x` (our column axis) - `col = (SI_ARCADE_HEIGHT-1) -
  (x - SI_ROT_X_OFFSET)`, centered with a 48px border each side
  (`(320-224)/2`). This means reading a *different* VRAM column for every
  pixel in the row, not a fast sequential bit-scan of one column. `ay`
  (our row axis) now drives the bit-position instead: `bitpos = (255 -
  SI_ROT_CROP) - ay`. Since the bit-position axis has 256 values and our
  framebuffer only has 240 rows, `SI_ROT_CROP = (256-240)/2 = 8` crops 8px
  off each end of that axis (a symmetric ~3%-per-side trim of the
  playfield's outer edge) so it exactly fills all 240 rows with no
  separate top/bottom letterbox needed - the border ends up entirely on
  the column axis instead, handled inline in the loop.

**How the specific `col`/`bitpos` flip directions above were determined**:
not by freehand rotation-direction algebra, which was repeatedly wrong in
this file's history (see git log for `src/game.c` - three earlier attempts
each got some combination of the orientation/mirroring/band-placement
wrong). What finally worked: writing a small simulation
(labeled corner markers pushed through the actual render code plus a
geometric rotation transform, self-tested against a known grid) and
calibrating *which* rotation transform to use against directly-observed
hardware behaviour (specifically, exactly which screen edges the red and
green overlay bands appeared on before this fix), rather than trusting a
verbal description of "clockwise" or "counter-clockwise" - front-vs-back
and other reference-frame ambiguities in describing a physical rotation
made that verbal description an unreliable input on its own. If you need
to revisit this (e.g. a different physical mounting), the fastest reliable
path is the same: reproduce that simulation, feed it a couple of
known-observed reference points from the real screen, and let it tell you
the flip directions, rather than re-deriving the rotation composition by
hand.

## Color overlay

The real machine's video hardware only ever outputs 1-bit black/white -
the color you remember from the cabinet came from cellophane/acetate
strips glued over the glass: red across the top ~32 rows (score, UFO),
green across the bottom ~40 rows (shields, player ship), and clear
elsewhere. `overlay_color_for_col()` (shared by both orientation branches)
reproduces this by tinting lit pixels based on which `col` band they fall
in (`SI_OVERLAY_RED_ROWS`, `SI_OVERLAY_GREEN_ROWS`) - purely a
video-conversion-stage cosmetic; it has no effect on and no input from the
CPU emulation. Keying it off `col` (a property of the game content itself)
rather than `ay` or `x` (properties of our transmission, which - per the
"Screen orientation" section above - swap meaning between orientation
modes) is what lets this one function serve both branches unchanged.

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
