#include <stdbool.h>
#include <stddef.h>

#include "game.h"
#include "display_config.h"
#include "invaders_machine.h"

// ============================================================================
// Space Invaders arcade emulator: video output.
//
// This is not a reimplementation of Space Invaders' game logic - src/emu/
// runs the *actual* Taito arcade ROM (see roms/README.md) on an emulated
// Intel 8080. This file's only job is turning that emulated machine's video
// RAM into scanlines for the existing DVI pipeline, and pacing the CPU
// against our own frame loop since there's no real 2MHz clock driving it.
// See Emulator.md for the full writeup.
// ============================================================================

// The real cabinet's CRT runs 256x224 (see Emulator.md's "Screen
// orientation" section for why it isn't 224x256 despite the tube being
// physically rotated 90 degrees in the cabinet). Our framebuffer is
// 320x240, so the arcade image is letterboxed rather than stretched, to
// keep pixels square/undistorted.
#define SI_ARCADE_WIDTH  256
#define SI_ARCADE_HEIGHT 224

// Landscape-mode (SI_DISPLAY_ROTATED_CCW == 0) letterbox: centered, black
// border on all sides.
#define SI_FB_X_OFFSET ((FRAME_WIDTH - SI_ARCADE_WIDTH) / 2)
#define SI_FB_Y_OFFSET ((FRAME_HEIGHT - SI_ARCADE_HEIGHT) / 2)

// Rotated-mode (SI_DISPLAY_ROTATED_CCW == 1) letterbox. This mode needs a
// genuinely different (transposed) layout, not just flipped landscape
// indices - see Emulator.md's "Screen orientation" section for why. The
// game's 256-wide raw bit-position axis is cropped by 8px on each end to
// exactly fit our fixed 240-row canvas (256 doesn't fit in 240), so it
// needs no further letterbox border of its own; the 224-value column axis
// gets a centered border on our column axis instead.
#define SI_ROT_CROP     ((SI_ARCADE_WIDTH - FRAME_HEIGHT) / 2)  // 8
#define SI_ROT_X_OFFSET ((FRAME_WIDTH - SI_ARCADE_HEIGHT) / 2)  // 48

// Classic overlay-strip approximation: the real machine's CRT is pure 1-bit
// monochrome, but the cabinet glued colored acetate strips over the glass -
// red across the top (score/UFO), green across the bottom (shields/ship),
// clear (white) in between. Reproduced here as a tint applied when
// unpacking lit pixels, per the user's request when this file was written.
#define SI_OVERLAY_RED_ROWS   32
#define SI_OVERLAY_GREEN_ROWS 40

// Real Space Invaders hardware runs its 8080 at ~1.9968 MHz (19.968 MHz
// crystal / 10) and delivers two interrupts per 60Hz video frame: RST 1
// when the CRT beam reaches mid-screen, RST 2 at vblank. We don't emulate
// CRT beam position directly - instead we spread the frame's cycle budget
// evenly across our own 240 scanline-producer calls (see main.c) and fire
// the interrupts at the calls nearest those two points. This is a
// deliberate simplification (see Emulator.md's Limitations section): the
// *total* cycles per frame and per-interrupt-half are right, they're just
// not distributed at literal CRT-scanline granularity.
#define SI_CPU_HZ            1996800
#define SI_CYCLES_PER_FRAME  (SI_CPU_HZ / DISPLAY_REFRESH_HZ)
#define SI_CYCLES_PER_ROW    (SI_CYCLES_PER_FRAME / FRAME_HEIGHT)
#define SI_MID_SCREEN_ROW    (FRAME_HEIGHT / 2)

static invaders_machine_t s_machine;
static uint16_t scanline_black[FRAME_WIDTH];
static uint16_t scanline_arcade[FRAME_WIDTH];
static bool s_mid_screen_fired;

// Video RAM is organized as 224 vertical strips of 32 bytes (256 bits)
// each - byte (col*32 + row/8), bit (row%8) - because the CPU draws into
// it in the CRT's native (physically rotated) scan order. `col` identifies
// which row of the *game* (score/UFO near col 223, shields/ship near col
// 0) a pixel belongs to - this is what the overlay bands are keyed to,
// regardless of orientation mode.
static uint16_t overlay_color_for_col(unsigned col) {
    if (col >= SI_ARCADE_HEIGHT - SI_OVERLAY_RED_ROWS)
        return COLOR_RED;
    if (col < SI_OVERLAY_GREEN_ROWS)
        return COLOR_GREEN;
    return COLOR_WHITE;
}

#if SI_DISPLAY_ROTATED_CCW

// Physical display mounted rotated 90 degrees CCW (matching the real
// cabinet). A screen-plane rotation swaps which of the transmitted image's
// two axes ends up horizontal vs. vertical for the viewer - so the game's
// own top-bottom axis (`col`, and the overlay bands tied to it) needs to
// end up on OUR transmission's column axis (x) here, not our row axis
// (ay), unlike the landscape case below. That means reading a DIFFERENT
// VRAM column for every output pixel in the row, not a fast sequential
// bit-scan of one column - see Emulator.md's "Screen orientation" section
// for the full reasoning (an earlier version of this function kept `col`
// tied to `ay` in both modes, which put the overlay bands on the wrong
// screen axis entirely - left/right instead of top/bottom - no matter
// which pixels were flipped, since that axis assignment was the actual bug).
static void render_arcade_row(uint16_t *buf, const uint8_t *vram, unsigned ay) {
    unsigned bitpos = ay + SI_ROT_CROP;

    for (unsigned x = 0; x < FRAME_WIDTH; ++x) {
        if (x < SI_ROT_X_OFFSET || x >= SI_ROT_X_OFFSET + SI_ARCADE_HEIGHT) {
            buf[x] = COLOR_BLACK;
            continue;
        }
        unsigned col = x - SI_ROT_X_OFFSET;
        const uint8_t *column = vram + (size_t)col * 32;
        uint8_t byte = column[bitpos >> 3];
        int on = (byte >> (bitpos & 7)) & 1;
        buf[x] = on ? overlay_color_for_col(col) : COLOR_BLACK;
    }
}

#else

// Normal, un-rotated landscape monitor: un-rotate video RAM's native
// (physically-vertical-cabinet) addressing into a normal upright wide
// image - displayed row `ay` (0 = top) comes from raw column
// `(SI_ARCADE_HEIGHT - 1 - ay)`, and that column's 256 bits become the
// row's 256 pixels left to right.
static void render_arcade_row(uint16_t *buf, const uint8_t *vram, unsigned ay) {
    unsigned col = (SI_ARCADE_HEIGHT - 1) - ay;
    const uint8_t *column = vram + (size_t)col * 32;
    uint16_t lit_color = overlay_color_for_col(col);

    for (unsigned x = 0; x < SI_ARCADE_WIDTH; ++x) {
        uint8_t byte = column[x >> 3];
        int on = (byte >> (x & 7)) & 1;
        buf[SI_FB_X_OFFSET + x] = on ? lit_color : COLOR_BLACK;
    }
}

#endif

void game_init(void) {
    for (unsigned x = 0; x < FRAME_WIDTH; ++x) {
        scanline_black[x] = COLOR_BLACK;
        scanline_arcade[x] = COLOR_BLACK; // side borders; never touched again
    }
    invaders_machine_init(&s_machine);
    s_mid_screen_fired = false;
}

const uint16_t *game_get_scanline(unsigned y, unsigned frame_count) {
    (void)frame_count;

    if (y == 0) {
        // Real hardware fires this as the CRT beam finishes the visible
        // frame and enters vertical blank - functionally "the end of the
        // previous frame", which is equivalent to "just before this one".
        invaders_machine_interrupt_vblank(&s_machine);
        s_mid_screen_fired = false;
    }

    invaders_machine_run_cycles(&s_machine, SI_CYCLES_PER_ROW);

    if (!s_mid_screen_fired && y >= SI_MID_SCREEN_ROW) {
        invaders_machine_interrupt_mid_screen(&s_machine);
        s_mid_screen_fired = true;
    }

#if SI_DISPLAY_ROTATED_CCW
    // Rotated mode uses the full 240-row canvas - see the SI_ROT_CROP
    // comment above for why no top/bottom letterbox is needed here (the
    // border ends up on the column axis instead, handled inside
    // render_arcade_row itself).
    render_arcade_row(scanline_arcade, invaders_machine_vram(&s_machine), y);
    return scanline_arcade;
#else
    if (y < SI_FB_Y_OFFSET || y >= SI_FB_Y_OFFSET + SI_ARCADE_HEIGHT)
        return scanline_black;

    render_arcade_row(scanline_arcade, invaders_machine_vram(&s_machine), y - SI_FB_Y_OFFSET);
    return scanline_arcade;
#endif
}
