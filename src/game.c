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

// The real cabinet's CRT runs 256x224 (see Emulator.md's "Video RAM
// rotation" section for why it isn't 224x256 despite the tube being
// physically rotated 90 degrees in the cabinet). Our framebuffer is
// 320x240, so the arcade image is letterboxed - centered with a black
// border - rather than stretched, to keep pixels square/undistorted.
#define SI_ARCADE_WIDTH  256
#define SI_ARCADE_HEIGHT 224
#define SI_FB_X_OFFSET ((FRAME_WIDTH - SI_ARCADE_WIDTH) / 2)
#define SI_FB_Y_OFFSET ((FRAME_HEIGHT - SI_ARCADE_HEIGHT) / 2)

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

// Samples one row of the arcade's 256x224 1bpp video RAM into 16bpp RGB565
// pixels, applying the overlay tint. Video RAM is organized as 224 vertical
// strips of 32 bytes (256 bits) each - byte (col*32 + row/8), bit (row%8) -
// because the CPU draws into it in the CRT's native (physically rotated)
// scan order. See Emulator.md's "Screen orientation" section for the full
// derivation; summary: which raw VRAM column feeds which transmitted row
// (`col` below), and therefore where the overlay bands fall, is the SAME
// in both orientation modes - a screen-plane rotation only changes which
// direction each row's bits should be read in (`bitpos` below). Getting
// this backwards (flipping `col` instead of `bitpos`, or vice versa) is
// what produces a left-right mirrored image with the overlay bands in the
// wrong place, which is exactly the bug an earlier version of this
// function had.
static void render_arcade_row(uint16_t *buf, const uint8_t *vram, unsigned ay) {
    unsigned col = (SI_ARCADE_HEIGHT - 1) - ay;
    const uint8_t *column = vram + (size_t)col * 32;

    uint16_t lit_color = COLOR_WHITE;
    if (ay < SI_OVERLAY_RED_ROWS)
        lit_color = COLOR_RED;
    else if (ay >= SI_ARCADE_HEIGHT - SI_OVERLAY_GREEN_ROWS)
        lit_color = COLOR_GREEN;

    for (unsigned x = 0; x < SI_ARCADE_WIDTH; ++x) {
#if SI_DISPLAY_ROTATED_CCW
        unsigned bitpos = (SI_ARCADE_WIDTH - 1) - x;
#else
        unsigned bitpos = x;
#endif
        uint8_t byte = column[bitpos >> 3];
        int on = (byte >> (bitpos & 7)) & 1;
        buf[SI_FB_X_OFFSET + x] = on ? lit_color : COLOR_BLACK;
    }
}

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

    if (y < SI_FB_Y_OFFSET || y >= SI_FB_Y_OFFSET + SI_ARCADE_HEIGHT)
        return scanline_black;

    render_arcade_row(scanline_arcade, invaders_machine_vram(&s_machine), y - SI_FB_Y_OFFSET);
    return scanline_arcade;
}
