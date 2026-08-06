#ifndef GAME_H
#define GAME_H

#include <stdint.h>

// Initialises game state. Call once at startup.
void game_init(void);

// Renders framebuffer row `y` directly into dst (length FRAME_WIDTH).
void game_render_scanline(uint8_t *dst, unsigned y, unsigned frame_count);

// Renders a complete 320x240 8bpp frame into fb.
void game_render_frame(uint8_t *fb, unsigned frame_count);

#endif // GAME_H
