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

// Letterbox for SI_DISPLAY_ROTATION == 0 or 180 (output is 256x224-shaped,
// same as the source): centered, black border on all sides.
#define SI_FB_X_OFFSET ((FRAME_WIDTH - SI_ARCADE_WIDTH) / 2)
#define SI_FB_Y_OFFSET ((FRAME_HEIGHT - SI_ARCADE_HEIGHT) / 2)

// Letterbox for SI_DISPLAY_ROTATION == 90 or 270 (output is 224x256-shaped
// - width/height swap under a 90-degree rotation). The 224-value axis gets
// a centered border on our column axis; the 256-value axis is cropped by
// 8px on each end instead of bordered, since it doesn't fit our fixed
// 240-row canvas (256 > 240) - a symmetric ~3%-per-side trim of the
// playfield's outer edge that exactly fills all 240 rows.
#define SI_ROT_X_OFFSET ((FRAME_WIDTH - SI_ARCADE_HEIGHT) / 2)
#define SI_ROT_CROP     ((SI_ARCADE_WIDTH - FRAME_HEIGHT) / 2)

// Classic overlay-strip approximation: the real machine's CRT is pure 1-bit
// monochrome, but the cabinet glued colored acetate strips over the glass -
// red across the top (score/UFO), green across the bottom (shields/ship),
// clear (white) in between. Reproduced here as a tint applied when
// unpacking lit pixels.
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
static uint16_t scanline_arcade[FRAME_WIDTH];
static bool s_mid_screen_fired;

// Video RAM is organized as 224 vertical strips of 32 bytes (256 bits)
// each - byte (col*32 + row/8), bit (row%8) - because the CPU draws into
// it in the CRT's native (physically rotated) scan order. `col` identifies
// which row of the *game* (score/UFO near col 223, shields/ship near col
// 0) a pixel belongs to - this is what the overlay bands are keyed to.
// (lx, ly) here are coordinates in the game's own un-rotated,
// un-mirrored "landscape" space: lx 0..255 (left-right), ly 0..223
// (top-to-bottom, 0 = score/UFO row).
static uint16_t sample_pixel(const uint8_t *vram, unsigned lx, unsigned ly) {
    unsigned col = (SI_ARCADE_HEIGHT - 1) - ly;
    const uint8_t *column = vram + (size_t)col * 32;
    int on = (column[lx >> 3] >> (lx & 7)) & 1;
    if (!on)
        return COLOR_BLACK;
    if (col >= SI_ARCADE_HEIGHT - SI_OVERLAY_RED_ROWS)
        return COLOR_RED;
    if (col < SI_OVERLAY_GREEN_ROWS)
        return COLOR_GREEN;
    return COLOR_WHITE;
}

// Applies the configured mirror flags in the game's own landscape space,
// before rotation - see display_config.h.
static void apply_mirror(unsigned *lx, unsigned *ly) {
#if SI_DISPLAY_FLIP_H
    *lx = (SI_ARCADE_WIDTH - 1) - *lx;
#endif
#if SI_DISPLAY_FLIP_V
    *ly = (SI_ARCADE_HEIGHT - 1) - *ly;
#endif
}

#if SI_DISPLAY_ROTATION == 0 || SI_DISPLAY_ROTATION == 180

// SI_DISPLAY_ROTATION == 0: normal, un-rotated landscape monitor - output
// keeps the source's own 256x224 shape.
// SI_DISPLAY_ROTATION == 180: output is the same shape, upside down.
static void render_arcade_row(uint16_t *buf, const uint8_t *vram, unsigned ay) {
    if (ay < SI_FB_Y_OFFSET || ay >= SI_FB_Y_OFFSET + SI_ARCADE_HEIGHT) {
        for (unsigned x = 0; x < FRAME_WIDTH; ++x)
            buf[x] = COLOR_BLACK;
        return;
    }
    unsigned oy = ay - SI_FB_Y_OFFSET;

    for (unsigned x = 0; x < FRAME_WIDTH; ++x) {
        if (x < SI_FB_X_OFFSET || x >= SI_FB_X_OFFSET + SI_ARCADE_WIDTH) {
            buf[x] = COLOR_BLACK;
            continue;
        }
        unsigned ox = x - SI_FB_X_OFFSET;
#if SI_DISPLAY_ROTATION == 180
        unsigned lx = (SI_ARCADE_WIDTH - 1) - ox;
        unsigned ly = (SI_ARCADE_HEIGHT - 1) - oy;
#else
        unsigned lx = ox;
        unsigned ly = oy;
#endif
        apply_mirror(&lx, &ly);
        buf[x] = sample_pixel(vram, lx, ly);
    }
}

#else // SI_DISPLAY_ROTATION == 90 or 270

// SI_DISPLAY_ROTATION == 90: output rotated 90 degrees clockwise from the
// source - output is 224x256-shaped (width/height swap under rotation).
// SI_DISPLAY_ROTATION == 270: output rotated 90 degrees counter-clockwise
// (equivalently, 270 clockwise) - also 224x256-shaped.
// Both need a DIFFERENT VRAM column for every output pixel in the row, not
// a fast sequential bit-scan of one column, since `col` now depends on our
// column axis (x) instead of our row axis (ay) - see Emulator.md.
static void render_arcade_row(uint16_t *buf, const uint8_t *vram, unsigned ay) {
    unsigned oy = ay + SI_ROT_CROP; // 0..255; no separate letterbox needed on this axis

    for (unsigned x = 0; x < FRAME_WIDTH; ++x) {
        if (x < SI_ROT_X_OFFSET || x >= SI_ROT_X_OFFSET + SI_ARCADE_HEIGHT) {
            buf[x] = COLOR_BLACK;
            continue;
        }
        unsigned ox = x - SI_ROT_X_OFFSET;
#if SI_DISPLAY_ROTATION == 90
        unsigned lx = oy;
        unsigned ly = (SI_ARCADE_HEIGHT - 1) - ox;
#else // 270
        unsigned lx = (SI_ARCADE_WIDTH - 1) - oy;
        unsigned ly = ox;
#endif
        apply_mirror(&lx, &ly);
        buf[x] = sample_pixel(vram, lx, ly);
    }
}

#endif

void game_init(void) {
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

    render_arcade_row(scanline_arcade, invaders_machine_vram(&s_machine), y);
    return scanline_arcade;
}
