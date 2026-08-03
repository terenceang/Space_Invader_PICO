#include <stdio.h>
#include <string.h>

#include "testcard.h"
#include "display_config.h"

// ============================================================================
// Pre-allocated Line Buffers
// ============================================================================
static uint16_t scanline_bars[FRAME_WIDTH];
static uint16_t scanline_mid[FRAME_WIDTH];
static uint16_t scanline_bottom[FRAME_WIDTH];
static uint16_t scanline_anim[FRAME_WIDTH];

// 7 Main EBU/SMPTE color bar array
static const uint16_t ebu_colors[7] = {
    COLOR_WHITE,
    COLOR_YELLOW,
    COLOR_CYAN,
    COLOR_GREEN,
    COLOR_MAGENTA,
    COLOR_RED,
    COLOR_BLUE
};

// ============================================================================
// Pattern Generators
// ============================================================================

// Generate top section: 7 main vertical color bars
static void generate_top_bars(uint16_t *buf, unsigned width) {
    unsigned bar_width = width / 7;
    for (unsigned x = 0; x < width; ++x) {
        unsigned bar_idx = x / bar_width;
        if (bar_idx > 6) bar_idx = 6;
        buf[x] = ebu_colors[bar_idx];
    }
}

// Generate middle section: Reverse color bars (-I, White, +Q, Black, etc.)
static void generate_mid_bars(uint16_t *buf, unsigned width) {
    unsigned bar_width = width / 7;
    static const uint16_t mid_colors[7] = {
        COLOR_BLUE,   COLOR_BLACK, COLOR_MAGENTA, COLOR_BLACK,
        COLOR_CYAN,   COLOR_BLACK, COLOR_WHITE
    };
    for (unsigned x = 0; x < width; ++x) {
        unsigned bar_idx = x / bar_width;
        if (bar_idx > 6) bar_idx = 6;
        buf[x] = mid_colors[bar_idx];
    }
}

// Generate bottom section: Grayscale ramp + PLUGE black level reference
static void generate_bottom_section(uint16_t *buf, unsigned width) {
    for (unsigned x = 0; x < width; ++x) {
        if (x < width * 3 / 4) {
            // 32-step grayscale ramp
            unsigned step = (x * 32) / (width * 3 / 4);
            unsigned r = step & 0x1F;
            unsigned g = (step << 1) & 0x3F;
            unsigned b = step & 0x1F;
            buf[x] = (uint16_t)((r << 11) | (g << 5) | b);
        } else {
            // PLUGE pattern blocks (Black / Dark Gray / White)
            unsigned sub_x = x - (width * 3 / 4);
            unsigned seg = sub_x / ((width / 4) / 3);
            if (seg == 0) buf[x] = COLOR_BLACK;
            else if (seg == 1) buf[x] = 0x18E3; // ~10% Gray
            else buf[x] = COLOR_WHITE;
        }
    }
}

void testcard_init(void) {
    printf("[DEBUG] Pre-rendering colorbar line buffers...\n");
    generate_top_bars(scanline_bars, FRAME_WIDTH);
    generate_mid_bars(scanline_mid, FRAME_WIDTH);
    generate_bottom_section(scanline_bottom, FRAME_WIDTH);
}

const uint16_t *testcard_get_scanline(unsigned y, unsigned frame_count) {
    // Divide frame height into testcard sections
    if (y < 160) {
        // Section 1 (0..159): 7 Main Color Bars
        return scanline_bars;
    } else if (y < 180) {
        // Section 2 (160..179): Reverse Color Bars
        return scanline_mid;
    } else if (y >= 225 && y < 235) {
        // Section 3 (225..234): Moving Sync / Animation Bar
        unsigned anim_pos = (frame_count * 2) % (FRAME_WIDTH - 20);
        memcpy(scanline_anim, scanline_bottom, sizeof(scanline_anim));
        for (unsigned x = anim_pos; x < anim_pos + 20; ++x) {
            scanline_anim[x] = COLOR_WHITE;
        }
        return scanline_anim;
    } else {
        // Section 4 (180..224, 235..239): Grayscale Ramp & PLUGE
        return scanline_bottom;
    }
}
