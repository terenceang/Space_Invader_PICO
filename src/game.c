#include "game.h"
#include "display_config.h"

// ============================================================================
// PLACEHOLDER - not gameplay yet.
//
// This is scaffolding: a static placeholder scene (a row of invader-shaped
// blocks and a player-ship bar) that proves the DEBUG_TESTCARD -> game
// handoff in main.c actually works. There is no movement, no input, no
// collision, no score - none of that exists yet. Replace this file's
// contents with real Space Invaders logic (invader grid + formation
// movement, player movement/shooting, collision detection, score/lives)
// when that work starts.
// ============================================================================

static uint16_t scanline_black[FRAME_WIDTH];
static uint16_t scanline_invader_row[FRAME_WIDTH];
static uint16_t scanline_ship_row[FRAME_WIDTH];

#define INVADER_ROW_Y_START 40
#define INVADER_ROW_Y_END   56
#define SHIP_ROW_Y_START    200
#define SHIP_ROW_Y_END      216

static void generate_invader_row(uint16_t *buf, unsigned width) {
    const unsigned n_invaders = 5;
    const unsigned block_w = 24;
    const unsigned gap = (width - n_invaders * block_w) / (n_invaders + 1);
    for (unsigned x = 0; x < width; ++x)
        buf[x] = COLOR_BLACK;
    for (unsigned i = 0; i < n_invaders; ++i) {
        unsigned start = gap + i * (block_w + gap);
        for (unsigned x = start; x < start + block_w && x < width; ++x)
            buf[x] = COLOR_WHITE;
    }
}

static void generate_ship_row(uint16_t *buf, unsigned width) {
    const unsigned ship_w = 40;
    unsigned start = (width - ship_w) / 2;
    for (unsigned x = 0; x < width; ++x)
        buf[x] = COLOR_BLACK;
    for (unsigned x = start; x < start + ship_w; ++x)
        buf[x] = COLOR_GREEN;
}

void game_init(void) {
    for (unsigned x = 0; x < FRAME_WIDTH; ++x)
        scanline_black[x] = COLOR_BLACK;
    generate_invader_row(scanline_invader_row, FRAME_WIDTH);
    generate_ship_row(scanline_ship_row, FRAME_WIDTH);
}

const uint16_t *game_get_scanline(unsigned y, unsigned frame_count) {
    (void)frame_count; // placeholder scene is static - no animation yet
    if (y >= INVADER_ROW_Y_START && y < INVADER_ROW_Y_END)
        return scanline_invader_row;
    if (y >= SHIP_ROW_Y_START && y < SHIP_ROW_Y_END)
        return scanline_ship_row;
    return scanline_black;
}
