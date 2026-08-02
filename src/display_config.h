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

// Screen orientation, to match however the physical display is actually
// mounted - see Emulator.md's "Screen orientation" section. Neither of
// these has any effect on the DVI engine's own fixed 640x480p60 output
// timing (src/dvi/) - only on how src/game.c samples video RAM into the
// framebuffer.
//
// SI_DISPLAY_ROTATION: rotation applied to the image, in degrees
// clockwise. Must be 0, 90, 180, or 270 - anything else is a compile
// error. The real Space Invaders cabinet's monitor is mounted vertically;
// 270 (rotate 90 degrees counter-clockwise) is this project's default
// starting guess for that setup, not a verified-correct value for any
// particular physical mount - if the image comes out sideways, upside
// down, or mirrored, that's what SI_DISPLAY_ROTATION and the two flip
// flags below are for: try the other rotation values and/or flips (16
// combinations total) rather than editing the rendering code itself.
#ifndef SI_DISPLAY_ROTATION
#define SI_DISPLAY_ROTATION 270
#endif

// Mirror the image horizontally/vertically, applied independently of
// SI_DISPLAY_ROTATION (before it, in the game's own un-rotated coordinate
// space) - covers cabinets/mounts that also flip the image, e.g. a mirror
// used to fold the optical path, or a display driven through a connector
// that inverts a scan direction.
#ifndef SI_DISPLAY_FLIP_H
#define SI_DISPLAY_FLIP_H 0
#endif
#ifndef SI_DISPLAY_FLIP_V
#define SI_DISPLAY_FLIP_V 0
#endif

#if SI_DISPLAY_ROTATION != 0 && SI_DISPLAY_ROTATION != 90 && \
    SI_DISPLAY_ROTATION != 180 && SI_DISPLAY_ROTATION != 270
#error "SI_DISPLAY_ROTATION must be 0, 90, 180, or 270"
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
