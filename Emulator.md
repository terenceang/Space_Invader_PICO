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

`src/game.c` handles this with two independent, orthogonal settings in
`src/display_config.h`:

- **`SI_DISPLAY_ROTATION`**: `0`, `90`, `180`, or `270` degrees clockwise.
  `0` outputs the source's own 256x224 shape unchanged (normal landscape
  monitor). `180` is the same shape, upside down. `90`/`270` swap the
  output to a 224x256 shape (matching a physically-rotated monitor).
- **`SI_DISPLAY_FLIP_H`** / **`SI_DISPLAY_FLIP_V`**: independently mirror
  the image horizontally/vertically, applied in the game's own
  un-rotated coordinate space *before* rotation - `apply_mirror()` in
  `src/game.c`.

Together these cover all 16 fixed ways a rectangular display can be
mounted. Every previous attempt at hand-deriving a single "correct"
transform for a physically-rotated monitor got some detail wrong (see git
log for `src/game.c` - several different failure modes: mirrored image,
overlay bands on the wrong screen edge, bands on the right edge but wrong
side) - freehand rotation-composition algebra proved unreliable for this
specific transform across repeated attempts, and a verbal description of
"which way the screen is rotated" turned out to be an unreliable input on
its own (front-vs-back and other reference-frame ambiguities). Exposing
all 16 combinations as two numbers to try, rather than shipping one
hardcoded guess, means finding the right one is a `SI_DISPLAY_ROTATION` /
`SI_DISPLAY_FLIP_H` / `SI_DISPLAY_FLIP_V` edit and a rebuild, not another
round of code changes.

**Implementation**: `render_arcade_row()` computes, for each output pixel,
its position `(lx, ly)` in the game's own un-rotated "landscape" space
(`lx` 0-255 left-to-right, `ly` 0-223 top-to-bottom, `ly=0` = score/UFO
row) - via one of four formulas selected by `SI_DISPLAY_ROTATION` at
compile time - applies the mirror flags, then reads the pixel with
`sample_pixel(vram, lx, ly)`, which derives `col = 223 - ly` and reads
that VRAM column directly. All four rotation formulas were verified by
simulation (rendering a labeled test pattern, rotating it with an
independently-checked pure array-rotation function, and confirming the
per-pixel formula produces an identical result) before being written into
this file - not just "looks right" from inspection.

For `SI_DISPLAY_ROTATION == 0` or `180`, the output keeps the source's
256x224 shape, centered in the 320x240 framebuffer with a 32px/8px black
border (`SI_FB_X_OFFSET`/`SI_FB_Y_OFFSET`). For `90`/`270`, the output is
224x256-shaped instead (width/height swap under a 90-degree rotation) -
this means reading a *different* VRAM column for every pixel in a row,
not a fast sequential bit-scan of one column, since `col` now depends on
the transmission's column axis (`x`) instead of its row axis (`ay`). The
224-value axis gets a 48px border on our column axis
(`SI_ROT_X_OFFSET`); the 256-value axis doesn't fit our fixed 240-row
canvas, so instead of a border it's cropped 8px on each end
(`SI_ROT_CROP` - a symmetric ~3%-per-side trim of the playfield's outer
edge) to exactly fill all 240 rows.

## Color overlay

`SI_ENABLE_COLOR_OVERLAY` in `src/display_config.h` (default 1) turns this
whole feature on or off - set to 0 for a plain white-on-black monochrome
image matching the real hardware's video RAM bit-for-bit, with no tint
applied, useful for checking the raw video output independent of the
cosmetic overlay. `lit_pixel_color()` is the single call site that checks
this flag; `overlay_color_for_screen_x()` itself is compiled out entirely
when disabled.

The real machine's video hardware only ever outputs 1-bit black/white -
the color you remember from the cabinet came from cellophane/acetate
strips glued over the glass: red near one edge, green near the opposite
edge, clear in between. `sample_bit()` decides which pixels are lit
(purely from VRAM, via `lx`/`ly` - the rotation/mirror-adjusted content
coordinates); `overlay_color_for_screen_x()` decides what color a lit
pixel gets, and it's **deliberately decoupled from content rotation**: it
keys off `ox`, the raw screen-space column position within the active
playfield, not off anything derived from `lx`/`ly`. On real hardware the
overlay bands run perpendicular to the game's own vertical axis (red at
the score/UFO end, green at the shields/ship end) - this project's bands
are instead rotated 90 degrees clockwise from that **by explicit request,
independent of whatever `SI_DISPLAY_ROTATION` is set to**: red is the
right-edge band, green is the left-edge band, in screen terms, regardless
of how the game content itself is rotated/mirrored. If a future change
wants the overlay to track the game's score/ship axis again instead of a
fixed screen edge, that means going back to keying it off `ly`/`col`
(what an earlier version of this file did) rather than `ox` - the two
are genuinely different design choices, not a bug either way.

## CRT scanline effect

`SI_ENABLE_SCANLINES` / `SI_SCANLINE_INTENSITY` (0-100) in
`src/display_config.h` add an optional darkened-alternate-pixels effect
approximating a real CRT's visible scan lines. `apply_scanline()` darkens
alternating pixels along `lx` (the post-mirror game-space horizontal
coordinate), applied per-pixel inside `render_arcade_row()` at the point
each pixel's final color is decided.

**This axis was picked empirically, not derived from theory.** Two earlier
versions of this effect existed first: one keyed on `y`/`ay` (our
transmission row index, darkening whole rows as a post-process), then one
keyed on `ly` (the game-space vertical coordinate, reasoned to be
row-invariant and therefore equivalent to the first version for
`SI_DISPLAY_ROTATION == 0`). Both were expected to produce horizontal
bands for the confirmed `SI_DISPLAY_ROTATION 0` / `SI_DISPLAY_FLIP_H 1`
setup - that reasoning about `ly` being row-invariant was correct as far
as it went, but on real hardware both versions actually produced
*vertical* bands, not horizontal. `lx` was tried next, on direct request,
and confirmed correct. This project's rotation-related bugs have
repeatedly turned out to be spots where a plausible-sounding derivation
about "which axis maps to what after rotation" didn't hold up against
what the actual hardware showed (see the "Screen orientation" section's
own history of this) - this is another instance of that, not a case where
the earlier reasoning was internally inconsistent.

Each row is physically doubled to 2 scanlines by the DVI engine's
`DVI_VERTICAL_REPEAT` (see `Video.md`), so the overall darkening still
lands as a repeating pattern at the final 640x480 output, with the pitch
and orientation depending on `SI_DISPLAY_ROTATION` as observed rather than
as separately derived per rotation mode. `SI_SCANLINE_INTENSITY` is a
compile-time constant, so the per-channel darkening divisions fold into
cheap multiply-shift sequences at compile time, not runtime division -
negligible added cost per pixel.

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
