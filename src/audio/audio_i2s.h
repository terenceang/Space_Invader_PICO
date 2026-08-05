#ifndef AUDIO_I2S_H
#define AUDIO_I2S_H

#include <stdbool.h>

#include "display_config.h"
#include "sound_data.h"

// I2S audio output driver for the Waveshare RP2350-PiZero - see Hardware.md's
// "I2S audio pinout" section for the pin choice and DAC wiring.
//
// Mixes up to AUDIO_MAX_VOICES simultaneous one-shot sound effects plus one
// looping voice (real hardware only ever loops the UFO hum) into a single
// mono stream, duplicated onto both I2S channels - see audio_i2s.c. Sample
// data itself comes from sound_table (sound_data.h), embedded at build time
// from user-supplied files in sounds/ (see sounds/README.md) - there's no
// audio ROM to draw from, since the real cabinet's sounds came from
// discrete analog circuitry, not samples.
//
// Uses PIO1 (PIO0 is fully claimed by the DVI engine, src/dvi/) and
// DMA_IRQ_1 (DVI already owns DMA_IRQ_0 - see src/dvi/dvi_engine.h), so this
// is entirely independent of the DVI pipeline's PIO/DMA/timing budget.
// Registers its IRQ on whichever core calls audio_i2s_init(), matching
// dvi_engine_register_irqs_this_core()'s pattern - call it from Core 0
// (main.c, before multicore_launch_core1()) so it doesn't compete with the
// DVI engine's own DMA_IRQ_0 handler on Core 1.

// Brings up the I2S PIO/DMA engine and the MAX98357A's mute/enable GPIO
// (unmuted by default). Call once, after dvi_display_clock_init()'s
// set_sys_clock_khz() (the PIO clock divider is computed from the system
// clock).
void audio_i2s_init(void);

// Steps the software audio mixer per scanline (~1.4 samples) and feeds the sample stream.
void audio_i2s_step_scanline(void);

// Drives the MAX98357A's SD (shutdown, active-low) pin - see Hardware.md's
// "MAX98357A Amplifier Module" section. mute=true shuts the amp down
// (silent, low power); mute=false enables it. audio_i2s_init() enables the
// amp by default; sound_effects.c also drives this from the real port 3 bit
// 5 AMP-enable line.
void audio_i2s_set_mute(bool mute);

// Starts sound_id playing from the beginning on the next free voice
// (stealing the least-recently-started of AUDIO_MAX_VOICES if all are
// busy - see audio_i2s.c). A sound_id whose file wasn't supplied at build
// time (sound_table[id].frame_count == 0 - see sounds/README.md) is
// silently a no-op. Only ever touches a few words under a brief interrupt-
// disable, so it's safe to call from Core 0's hot loop - see Hardware.md's
// timing budget note.
void audio_i2s_play_sound(sound_id_t sound_id);

// Starts/stops looping playback of sound_id. Only one sound can loop at a
// time (the real cabinet only ever loops the UFO hum, hence a single loop
// voice rather than one per AUDIO_MAX_VOICES slot). active=true (re)starts
// the loop from the beginning, unless sound_id is already the one looping -
// then it's a no-op, so repeated writes while the trigger bit stays set
// don't restart the waveform mid-play. active=false stops immediately
// (goes silent, not "let it finish").
void audio_i2s_set_sound_loop(sound_id_t sound_id, bool active);

#if DEBUG_AUDIO_TEST_TONE
// Hardware bring-up helper only (see display_config.h's
// DEBUG_AUDIO_TEST_TONE) - starts a continuous ~441Hz test tone on a
// dedicated debug voice, entirely separate from the real one-shot/loop
// voices, so nothing the emulated machine's port 3/5 writes do can
// interrupt it. Call once, after audio_i2s_init(). Meant for confirming the
// I2S signal path (pins, PIO timing, DMA hand-off, MAX98357A wiring/mute
// pin) works before trusting real sound-effect playback.
void audio_i2s_debug_play_test_tone(void);
#endif

#endif // AUDIO_I2S_H
