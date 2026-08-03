#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "game.h"
#include "display_config.h"
#include "invaders_machine.h"
#include "sound_effects.h"

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

// Content dimensions as they map onto our transmission axes (x = column
// axis, ay = row axis) before any scaling - rotation 90/270 swaps which
// arcade dimension maps to which axis, same as everywhere else in this
// file (see the render_arcade_row branches below).
#if SI_DISPLAY_ROTATION == 0 || SI_DISPLAY_ROTATION == 180
#define SI_CONTENT_W SI_ARCADE_WIDTH
#define SI_CONTENT_H SI_ARCADE_HEIGHT
#else
#define SI_CONTENT_W SI_ARCADE_HEIGHT
#define SI_CONTENT_H SI_ARCADE_WIDTH
#endif

// Displayed size within the 320x240 framebuffer, per SI_SCALE_MODE (see
// display_config.h) - Emulator.md's "Image scaling" section has the full
// derivation. The FIT comparison picks whichever axis is the tighter
// constraint via cross-multiplication, avoiding floating point or a
// runtime division just to compare two ratios.
#if SI_SCALE_MODE == SI_SCALE_FIT
    #if (FRAME_WIDTH * SI_CONTENT_H) < (FRAME_HEIGHT * SI_CONTENT_W)
        #define SI_DISPLAY_W FRAME_WIDTH
        #define SI_DISPLAY_H (SI_CONTENT_H * FRAME_WIDTH / SI_CONTENT_W)
    #else
        #define SI_DISPLAY_W (SI_CONTENT_W * FRAME_HEIGHT / SI_CONTENT_H)
        #define SI_DISPLAY_H FRAME_HEIGHT
    #endif
#elif SI_SCALE_MODE == SI_SCALE_X
    #define SI_DISPLAY_W FRAME_WIDTH
    #define SI_DISPLAY_H SI_CONTENT_H
#elif SI_SCALE_MODE == SI_SCALE_Y
    #define SI_DISPLAY_W SI_CONTENT_W
    #define SI_DISPLAY_H FRAME_HEIGHT
#else // SI_SCALE_NONE
    #define SI_DISPLAY_W SI_CONTENT_W
    #define SI_DISPLAY_H SI_CONTENT_H
#endif

// Where the (possibly-scaled) displayed image sits within the 320x240
// framebuffer. The X axis is always bordered (SI_DISPLAY_W never exceeds
// FRAME_WIDTH for any mode/rotation combination this project's fixed
// 256x224 content and 320x240 canvas produce). The Y axis is bordered too
// unless SI_DISPLAY_H exceeds FRAME_HEIGHT - which only happens for
// SI_DISPLAY_ROTATION 90/270 in SI_SCALE_NONE/SI_SCALE_X (the 256-value
// arcade axis mapped straight onto our 240-row canvas) - in which case
// it's cropped symmetrically instead, exactly as this project always has
// been for those combinations (see "Screen orientation" in Emulator.md).
#define SI_ACTIVE_X_OFFSET ((FRAME_WIDTH - SI_DISPLAY_W) / 2)
#if SI_DISPLAY_H > FRAME_HEIGHT
#define SI_ACTIVE_Y_CROP   ((SI_DISPLAY_H - FRAME_HEIGHT) / 2)
#define SI_ACTIVE_Y_OFFSET 0
#define SI_ACTIVE_Y_LIMIT  FRAME_HEIGHT
#else
#define SI_ACTIVE_Y_CROP   0
#define SI_ACTIVE_Y_OFFSET ((FRAME_HEIGHT - SI_DISPLAY_H) / 2)
#define SI_ACTIVE_Y_LIMIT  (SI_ACTIVE_Y_OFFSET + SI_DISPLAY_H)
#endif

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

// Ring buffer of scanlines to avoid Core 0 / Core 1 memory contention
#define SCANLINE_BUF_COUNT 4
static uint16_t scanline_arcade_bufs[SCANLINE_BUF_COUNT][FRAME_WIDTH];
static unsigned s_scanline_buf_idx = 0;

static bool s_mid_screen_fired;

// Video RAM is organized as 224 vertical strips of 32 bytes (256 bits)
// each - byte (col*32 + row/8), bit (row%8) - because the CPU draws into
// it in the CRT's native (physically rotated) scan order. (lx, ly) here
// are coordinates in the game's own un-rotated, un-mirrored "landscape"
// space: lx 0..255 (left-right), ly 0..223 (top-to-bottom, 0 = score/UFO
// row) - this is the rotation-adjusted content sampling and is untouched
// by the overlay rotation below.
static inline int sample_bit(const uint8_t *vram, unsigned lx, unsigned ly) {
    unsigned col = (SI_ARCADE_HEIGHT - 1) - ly;
    const uint8_t *column = vram + (size_t)col * 32;
    return (column[lx >> 3] >> (lx & 7)) & 1;
}

// Color overlay bands, rotated 90 degrees CLOCKWISE relative to the
// content's own top/bottom axis, independent of SI_DISPLAY_ROTATION - per
// explicit request, not to match real cabinet hardware (which has the
// bands running perpendicular to the game's own vertical axis, i.e. what
// this would be without this extra rotation). Rotating a "top" edge 90
// degrees clockwise moves it to the "right" edge, and a "bottom" edge
// moves to the "left" edge - so red (previously the top band) is now the
// right-edge band, green (previously the bottom band) is now the
// left-edge band. Keyed on `ox`, the screen-space column position within
// the active playfield (independent of `lx`/`ly`, which already have
// SI_DISPLAY_ROTATION and the mirror flags baked in) - not on the game
// content's own axes, since this rotation is deliberately decoupled from
// content orientation.
#if SI_ENABLE_COLOR_OVERLAY
static inline uint16_t overlay_color_for_screen_x(unsigned ox, unsigned active_width) {
    if (ox >= active_width - SI_OVERLAY_RED_ROWS)
        return COLOR_RED;
    if (ox < SI_OVERLAY_GREEN_ROWS)
        return COLOR_GREEN;
    return COLOR_WHITE;
}
#endif

// Color for a lit pixel: the overlay tint above, or plain white for a
// monochrome image matching the real hardware's video RAM bit-for-bit -
// see SI_ENABLE_COLOR_OVERLAY in display_config.h.
static inline uint16_t lit_pixel_color(unsigned ox, unsigned active_width) {
#if SI_ENABLE_COLOR_OVERLAY
    return overlay_color_for_screen_x(ox, active_width);
#else
    (void)ox;
    (void)active_width;
    return COLOR_WHITE;
#endif
}

// Applies the configured mirror flags in the game's own landscape space,
// before rotation - see display_config.h.
static inline void apply_mirror(unsigned *lx, unsigned *ly) {
#if SI_DISPLAY_FLIP_H
    *lx = (SI_ARCADE_WIDTH - 1) - *lx;
#endif
#if SI_DISPLAY_FLIP_V
    *ly = (SI_ARCADE_HEIGHT - 1) - *ly;
#endif
}

// CRT scanline effect: darkens alternating pixels along `lx` (post-mirror
// game-space horizontal position) rather than our transmission row index
// or `ly` - see Emulator.md's "CRT scanline effect" section.
// Converted division by 100 into a fixed-point Q8 multiply-shift for speed.
#if SI_ENABLE_SCANLINES
#define SI_SCANLINE_SCALE_Q8 (((100 - SI_SCANLINE_INTENSITY) * 256 + 50) / 100)
static inline uint16_t apply_scanline(uint16_t color, unsigned lx) {
    if (lx & 1) {
        unsigned r = (color >> 11) & 0x1F;
        unsigned g = (color >> 5) & 0x3F;
        unsigned b = color & 0x1F;
        r = (r * SI_SCANLINE_SCALE_Q8) >> 8;
        g = (g * SI_SCANLINE_SCALE_Q8) >> 8;
        b = (b * SI_SCANLINE_SCALE_Q8) >> 8;
        return (uint16_t)((r << 11) | (g << 5) | b);
    }
    return color;
}
#else
static inline uint16_t apply_scanline(uint16_t color, unsigned lx) {
    (void)lx;
    return color;
}
#endif

#if SI_DISPLAY_ROTATION == 0 || SI_DISPLAY_ROTATION == 180

// SI_DISPLAY_ROTATION == 0: normal, un-rotated landscape monitor - output
// keeps the source's own 256x224 shape.
// SI_DISPLAY_ROTATION == 180: output is the same shape, upside down.
static void render_arcade_row(uint16_t *buf, const uint8_t *vram, unsigned ay) {
    int screen_y = (int)ay - SI_SCREEN_OFFSET_Y;
    if (screen_y < (int)SI_ACTIVE_Y_OFFSET || screen_y >= (int)(SI_ACTIVE_Y_OFFSET + SI_DISPLAY_H)) {
        memset(buf, 0, FRAME_WIDTH * sizeof(uint16_t));
        return;
    }
    unsigned display_y = (unsigned)screen_y - SI_ACTIVE_Y_OFFSET;
    unsigned oy = (display_y * SI_CONTENT_H) / SI_DISPLAY_H;

    int start_x = SI_SCREEN_OFFSET_X + SI_ACTIVE_X_OFFSET;
    int end_x = start_x + (int)SI_DISPLAY_W;
    int clip_start = start_x < 0 ? 0 : (start_x > FRAME_WIDTH ? FRAME_WIDTH : start_x);
    int clip_end = end_x < 0 ? 0 : (end_x > FRAME_WIDTH ? FRAME_WIDTH : end_x);

    if (clip_start > 0)
        memset(buf, 0, (size_t)clip_start * sizeof(uint16_t));
    if (clip_end < FRAME_WIDTH)
        memset(buf + clip_end, 0, (size_t)(FRAME_WIDTH - clip_end) * sizeof(uint16_t));

    if (clip_start >= clip_end)
        return;

    uint32_t step_x = ((uint32_t)SI_CONTENT_W << 16) / SI_DISPLAY_W;
    uint32_t ox_fp = (uint32_t)(clip_start - start_x) * step_x;

#if SI_DISPLAY_ROTATION == 180
    unsigned ly = (SI_ARCADE_HEIGHT - 1) - oy;
#else
    unsigned ly = oy;
#endif

    for (int x = clip_start; x < clip_end; ++x) {
        unsigned ox = ox_fp >> 16;
        ox_fp += step_x;

#if SI_DISPLAY_ROTATION == 180
        unsigned lx = (SI_ARCADE_WIDTH - 1) - ox;
#else
        unsigned lx = ox;
#endif
        unsigned cur_lx = lx, cur_ly = ly;
        apply_mirror(&cur_lx, &cur_ly);

        unsigned col = (SI_ARCADE_HEIGHT - 1) - cur_ly;
        const uint8_t *column = vram + (size_t)col * 32;
        int bit = (column[cur_lx >> 3] >> (cur_lx & 7)) & 1;

        uint16_t color = bit ? lit_pixel_color(ox, SI_ARCADE_WIDTH) : COLOR_BLACK;
        buf[x] = apply_scanline(color, cur_lx);
    }
}

#else // SI_DISPLAY_ROTATION == 90 or 270

// SI_DISPLAY_ROTATION == 90: output rotated 90 degrees clockwise.
// SI_DISPLAY_ROTATION == 270: output rotated 90 degrees counter-clockwise.
static void render_arcade_row(uint16_t *buf, const uint8_t *vram, unsigned ay) {
    int screen_ay = (int)ay - SI_SCREEN_OFFSET_Y;
    if (screen_ay < (int)SI_ACTIVE_Y_OFFSET || screen_ay >= (int)SI_ACTIVE_Y_LIMIT) {
        memset(buf, 0, FRAME_WIDTH * sizeof(uint16_t));
        return;
    }
    unsigned display_ay = (unsigned)screen_ay - SI_ACTIVE_Y_OFFSET + SI_ACTIVE_Y_CROP;
    unsigned oy = (display_ay * SI_CONTENT_H) / SI_DISPLAY_H;

    int start_x = SI_SCREEN_OFFSET_X + SI_ACTIVE_X_OFFSET;
    int end_x = start_x + (int)SI_DISPLAY_W;
    int clip_start = start_x < 0 ? 0 : (start_x > FRAME_WIDTH ? FRAME_WIDTH : start_x);
    int clip_end = end_x < 0 ? 0 : (end_x > FRAME_WIDTH ? FRAME_WIDTH : end_x);

    if (clip_start > 0)
        memset(buf, 0, (size_t)clip_start * sizeof(uint16_t));
    if (clip_end < FRAME_WIDTH)
        memset(buf + clip_end, 0, (size_t)(FRAME_WIDTH - clip_end) * sizeof(uint16_t));

    if (clip_start >= clip_end)
        return;

    uint32_t step_x = ((uint32_t)SI_CONTENT_W << 16) / SI_DISPLAY_W;
    uint32_t ox_fp = (uint32_t)(clip_start - start_x) * step_x;

    for (int x = clip_start; x < clip_end; ++x) {
        unsigned ox = ox_fp >> 16;
        ox_fp += step_x;

#if SI_DISPLAY_ROTATION == 90
        unsigned lx = oy;
        unsigned ly = (SI_ARCADE_HEIGHT - 1) - ox;
#else // 270
        unsigned lx = (SI_ARCADE_WIDTH - 1) - oy;
        unsigned ly = ox;
#endif
        unsigned cur_lx = lx, cur_ly = ly;
        apply_mirror(&cur_lx, &cur_ly);

        uint16_t color = sample_bit(vram, cur_lx, cur_ly) ? lit_pixel_color(ox, SI_ARCADE_HEIGHT) : COLOR_BLACK;
        buf[x] = apply_scanline(color, cur_lx);
    }
}

#endif

void game_init(void) {
    invaders_machine_init(&s_machine);
    s_machine.sound_write = sound_effects_on_port_write;
    s_mid_screen_fired = false;
    s_scanline_buf_idx = 0;
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

    uint16_t *buf = scanline_arcade_bufs[s_scanline_buf_idx];
    s_scanline_buf_idx = (s_scanline_buf_idx + 1) % SCANLINE_BUF_COUNT;

    render_arcade_row(buf, invaders_machine_vram(&s_machine), y);
    return buf;
}

