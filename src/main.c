#include <stdbool.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"

#include "display_config.h"
#include "dvi_display.h"
#include "game.h"
#include "audio_i2s.h"
#if DEBUG_TESTCARD
#include "testcard.h"
#endif

// ============================================================================
// Core 0 Execution: Main Program & Scanline Dispatcher
// ============================================================================
int main() {
    dvi_display_clock_init();

    // Initialize stdio (UART + USB CDC). UART works immediately with no
    // host handshake, so DVI/Core 1 bring-up below is never gated behind a
    // USB enumeration wait - it starts as soon as the clock is stable,
    // matching the timing of Waveshare's verified-working hello_dvi
    // reference. USB output before a host connects is simply dropped.
    stdio_init_all();

    printf("\n==================================================\n");
    printf("  Space Invader PICO  v%s\n", SPACE_INVADER_PICO_VERSION);
    printf("==================================================\n");
    printf("[DEBUG] Microcontroller: RP2350B (Cortex-M33)\n");
    printf("[DEBUG] Core Voltage   : 1.25V\n");

    dvi_display_init();

#if DEBUG_TESTCARD
    testcard_init();
    printf("[DEBUG] DEBUG_TESTCARD enabled: test card for %d seconds, then the game.\n",
           DEBUG_TESTCARD_SECONDS);
#endif
    game_init();

    // Audio mixer bring-up (see audio_i2s.h) - game_init() above already
    // wired the emulated machine's port 3/5 sound-effect writes to it. Feeds
    // the DVI engine's HDMI Data Island transport, not physical I2S hardware.
    printf("[DEBUG] Initializing audio mixer...\n");
    audio_i2s_init();
    printf("[DEBUG] Audio mixer initialized.\n");
#if DEBUG_AUDIO_TEST_TONE
    audio_i2s_debug_play_test_tone();
    printf("[DEBUG] DEBUG_AUDIO_TEST_TONE enabled: playing continuous ~441Hz test tone.\n");
#endif

    // Launch Core 1 for TMDS output stream
    printf("[DEBUG] Launching Core 1 for DVI TMDS serialiser...\n");
    multicore_launch_core1(core1_main);
    printf("[DEBUG] Core 1 launched.\n");

    printf("\n[STATUS] Rendering DVI 640x480 @ 60Hz...\n");

    uint32_t frame_count = 0;
#if DEBUG_TESTCARD
    const uint32_t testcard_frames = (uint32_t)DEBUG_TESTCARD_SECONDS * DISPLAY_REFRESH_HZ;
#endif

    // Main scanline output loop
    //
    // NOTE: this loop must never block for more than a few microseconds.
    // PicoDVI's buffering margin is tiny (DVI_N_TMDS_BUFFERS = 3 encoded
    // lines, ~190us at 640x480p60), so any blocking call here - e.g. a
    // printf() over UART, which blocks on the TX FIFO for milliseconds -
    // starves the DMA feed and shows up as a solid-red flicker on-screen.
    // Keep debug/status output out of this loop entirely.
    while (true) {
#if DEBUG_TESTCARD
        bool show_testcard = frame_count < testcard_frames;
#endif
        for (unsigned y = 0; y < FRAME_HEIGHT; ++y) {
            audio_i2s_step_scanline();

            const uint16_t *scanline;
#if DEBUG_TESTCARD
            scanline = show_testcard ? testcard_get_scanline(y, frame_count) : game_get_scanline(y, frame_count);
#else
            scanline = game_get_scanline(y, frame_count);
#endif

            // Push scanline pointer to DVI transmission queue
            queue_add_blocking_u32(&dvi_q_colour_valid, &scanline);

            // Drain free scanline queue
            const uint16_t *freed_buf;
            while (queue_try_remove_u32(&dvi_q_colour_free, &freed_buf))
                ;
        }

        ++frame_count;
    }
}
