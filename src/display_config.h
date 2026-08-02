#ifndef DISPLAY_CONFIG_H
#define DISPLAY_CONFIG_H

// Framebuffer resolution (320x240 16bpp, scaled 2x horizontally and
// vertically to the 640x480 DVI output timing)
#define FRAME_WIDTH  320
#define FRAME_HEIGHT 240

// Fixed by the DVI timing this project always runs (640x480p60 - see
// Video.md). Used to convert seconds into frame counts.
#define DISPLAY_REFRESH_HZ 60

// Debug test card: at boot, main.c shows the colour-bar/grayscale test
// pattern (testcard.c) for DEBUG_TESTCARD_SECONDS before handing off to the
// game. Set to 0 to skip the test card and boot straight into the game.
#ifndef DEBUG_TESTCARD
#define DEBUG_TESTCARD 0
#endif
#define DEBUG_TESTCARD_SECONDS 5

// Set to 1 if the physical display is mounted rotated 90 degrees
// counter-clockwise from normal landscape - matching the real Space
// Invaders cabinet's vertical monitor orientation (see Emulator.md's
// "Screen orientation" section). Set to 0 for a normal, un-rotated
// landscape monitor instead. This only affects how src/game.c samples
// video RAM into the framebuffer - it has no effect on the DVI engine's
// own fixed 640x480p60 output timing (src/dvi/), which is unaffected
// either way.
#ifndef SI_DISPLAY_ROTATED_CCW
#define SI_DISPLAY_ROTATED_CCW 1
#endif

// ============================================================================
// RGB565 Color Definitions (16-bit)
// ============================================================================
#define COLOR_WHITE   0xFFFF // 100% White
#define COLOR_YELLOW  0xFFE0 // 100% Yellow
#define COLOR_CYAN    0x07FF // 100% Cyan
#define COLOR_GREEN   0x07E0 // 100% Green
#define COLOR_MAGENTA 0xF81F // 100% Magenta
#define COLOR_RED     0xF800 // 100% Red
#define COLOR_BLUE    0x001F // 100% Blue
#define COLOR_BLACK   0x0000 // 100% Black

#endif // DISPLAY_CONFIG_H
