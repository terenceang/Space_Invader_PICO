#ifndef TESTCARD_H
#define TESTCARD_H

#include <stdint.h>

// Pre-renders the static color bar / grayscale ramp scanline buffers.
// Call once at startup, before the first call to testcard_get_scanline().
void testcard_init(void);

// Returns the scanline buffer for framebuffer row `y` of the given frame.
// `frame_count` drives the moving indicator bar in the animated section, so
// callers should pass the current frame number each time this is called.
const uint16_t *testcard_get_scanline(unsigned y, unsigned frame_count);

#endif // TESTCARD_H
