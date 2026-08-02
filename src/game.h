#ifndef GAME_H
#define GAME_H

#include <stdint.h>

// Initialises game state. Call once at startup, before the first call to
// game_get_scanline().
void game_init(void);

// Returns the scanline buffer for framebuffer row `y`. `frame_count` counts
// frames since the game started (not since boot - see main.c's
// DEBUG_TESTCARD handoff), so game logic can drive animation/timing from it.
const uint16_t *game_get_scanline(unsigned y, unsigned frame_count);

#endif // GAME_H
