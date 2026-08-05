// I2S audio output driver - PIO1 + ping-pong DMA, mixing sound-effect
// playback triggered by the emulated machine's port 3/5 writes (see
// sound_effects.c). See audio_i2s.h for the driver overview and
// Hardware.md for the pin choice.

#include <stdint.h>

#include "display_config.h"

#if DEBUG_AUDIO_TEST_TONE
#include <math.h>
#endif

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/sync.h"

#include "audio_i2s.pio.h"

#include "audio_i2s.h"
#include "dvi_engine.h"

// ----------------------------------------------------------------------------
// Board wiring - matches a real Raspberry Pi's I2S pin positions (GPIO18
// BCLK / GPIO19 LRCLK / GPIO21 DOUT) on this board's Pi-compatible 40-pin
// header (confirmed against the official schematic - see Hardware.md), so
// off-the-shelf I2S DAC HATs (PCM5102/UDA1334A/MAX98357A/HiFiBerry DAC, etc)
// wire up directly. None of these three are used elsewhere in this project
// (UART0 = GPIO0/1, I2C0 = GPIO6/7, DVI = GPIO32-39 - see Hardware.md's
// pinout table).
// ----------------------------------------------------------------------------
// Sound mixer: AUDIO_MAX_VOICES one-shot voices (Shot/Flash/Invader die/
// Extended play/Fleet 1-4/UFO hit all share this pool) plus a single
// looping voice (UFO hum only). All playing voices are simply summed and
// clamped - no envelope/panning/volume control, matching how simple the
// real discrete sound board's output was.
#define AUDIO_MAX_VOICES 4

typedef struct {
    const int16_t *data;
    uint32_t pos;
    uint32_t len;
    bool active;
} audio_voice_t;

static audio_voice_t voices[AUDIO_MAX_VOICES];
static unsigned voice_steal_next; // round-robin pointer, used only when every voice is busy
static audio_voice_t loop_voice;

#if DEBUG_AUDIO_TEST_TONE
// Entirely separate from loop_voice/voices above - see audio_i2s_debug_play_test_tone().
#define DEBUG_TONE_LUT_LEN 100 // 44100/100 = 441Hz exactly, so the loop wraps with no discontinuity
static int16_t debug_tone_lut[DEBUG_TONE_LUT_LEN];
static audio_voice_t debug_voice;
#endif

// Ping-pong sample buffers: 256 stereo frames (~5.8ms @ 44.1kHz) each, which
// is the refill deadline mix_samples() has to beat from the DMA IRQ -
// trivially cheap for a handful of table lookups and additions per frame.
#define AUDIO_BUF_FRAMES 256
static uint32_t audio_buf[2][AUDIO_BUF_FRAMES];

static uint audio_dma_chan[2];

// ----------------------------------------------------------------------------

static inline void mix_samples(uint32_t *buf, unsigned n_frames) {
    for (unsigned i = 0; i < n_frames; ++i) {
        int32_t mix = 0;

        if (loop_voice.active) {
            mix += loop_voice.data[loop_voice.pos];
            if (++loop_voice.pos >= loop_voice.len)
                loop_voice.pos = 0;
        }

#if DEBUG_AUDIO_TEST_TONE
        if (debug_voice.active) {
            mix += debug_voice.data[debug_voice.pos];
            if (++debug_voice.pos >= debug_voice.len)
                debug_voice.pos = 0;
        }
#endif

        for (unsigned v = 0; v < AUDIO_MAX_VOICES; ++v) {
            if (!voices[v].active)
                continue;
            mix += voices[v].data[voices[v].pos];
            if (++voices[v].pos >= voices[v].len)
                voices[v].active = false;
        }

        // Scale total mixed volume to 60% headroom to prevent DAC input clipping/noise
        mix = (mix * 60) / 100;

        if (mix > INT16_MAX)
            mix = INT16_MAX;
        else if (mix < INT16_MIN)
            mix = INT16_MIN;
        int16_t sample = (int16_t)mix;

        // Same sample in both packed halves - this driver doesn't
        // distinguish L/R (the real cabinet's sound is mono too).
        buf[i] = ((uint32_t)(uint16_t)sample << 16) | (uint16_t)sample;
    }
}

// Fires once per ~5.8ms buffer (see AUDIO_BUF_FRAMES) - refills whichever
// buffer DMA chaining just finished playing, while the other buffer plays.
static void audio_dma_irq_handler(void) {
    for (int i = 0; i < 2; ++i) {
        if (dma_hw->ints1 & (1u << audio_dma_chan[i])) {
            dma_hw->ints1 = 1u << audio_dma_chan[i];
            mix_samples(audio_buf[i], AUDIO_BUF_FRAMES);
            dma_channel_set_read_addr(audio_dma_chan[i], audio_buf[i], false);
        }
    }
}

void audio_i2s_step_scanline(void) {
    // Pace audio sample generation to 44.1kHz (735 samples / 525 scanlines per frame)
    static uint32_t sample_acc = 0;
    sample_acc += 735;
    uint32_t samples_to_gen = sample_acc / 525;
    sample_acc %= 525;

    for (uint32_t i = 0; i < samples_to_gen; ++i) {
        int32_t mix = 0;

        if (loop_voice.active) {
            mix += loop_voice.data[loop_voice.pos];
            if (++loop_voice.pos >= loop_voice.len)
                loop_voice.pos = 0;
        }

#if DEBUG_AUDIO_TEST_TONE
        if (debug_voice.active) {
            mix += debug_voice.data[debug_voice.pos];
            if (++debug_voice.pos >= debug_voice.len)
                debug_voice.pos = 0;
        }
#endif

        for (unsigned v = 0; v < AUDIO_MAX_VOICES; ++v) {
            if (!voices[v].active)
                continue;
            mix += voices[v].data[voices[v].pos];
            if (++voices[v].pos >= voices[v].len)
                voices[v].active = false;
        }

        mix = (mix * 60) / 100;

        if (mix > INT16_MAX)
            mix = INT16_MAX;
        else if (mix < INT16_MIN)
            mix = INT16_MIN;
        int16_t sample = (int16_t)mix;

        dvi_engine_send_hdmi_audio_sample(sample, sample);
    }
}

void audio_i2s_set_mute(bool mute) {
    // MAX98357A hardware mute disabled - no-op.
    (void)mute;
}

#if DEBUG_AUDIO_TEST_TONE
void audio_i2s_debug_play_test_tone(void) {
    for (unsigned i = 0; i < DEBUG_TONE_LUT_LEN; ++i)
        debug_tone_lut[i] = (int16_t)(4000 * sinf(2.0f * (float)M_PI * (float)i / DEBUG_TONE_LUT_LEN));

    uint32_t save = save_and_disable_interrupts();
    debug_voice.data = debug_tone_lut;
    debug_voice.len = DEBUG_TONE_LUT_LEN;
    debug_voice.pos = 0;
    debug_voice.active = true;
    restore_interrupts(save);
}
#endif

void audio_i2s_play_sound(sound_id_t sound_id) {
    if (sound_id >= SOUND_COUNT)
        return;
    const sound_sample_t *s = &sound_table[sound_id];
    if (s->frame_count == 0)
        return; // not supplied at build time - silent, see sound_data.h

    // mix_samples() runs from the DMA IRQ on this same core - briefly
    // disable interrupts so it can never observe a voice slot half-updated.
    uint32_t save = save_and_disable_interrupts();

    unsigned slot = AUDIO_MAX_VOICES;
    for (unsigned i = 0; i < AUDIO_MAX_VOICES; ++i) {
        if (!voices[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == AUDIO_MAX_VOICES) {
        // All voices busy - steal the oldest rather than drop the new
        // trigger. Arcade effects are tens of milliseconds long, so a steal
        // is inaudible in practice and simpler than a priority scheme.
        slot = voice_steal_next;
        voice_steal_next = (voice_steal_next + 1) % AUDIO_MAX_VOICES;
    }
    voices[slot].data = s->samples;
    voices[slot].len = s->frame_count;
    voices[slot].pos = 0;
    voices[slot].active = true;

    restore_interrupts(save);
}

void audio_i2s_set_sound_loop(sound_id_t sound_id, bool active) {
    if (sound_id >= SOUND_COUNT)
        return;

    uint32_t save = save_and_disable_interrupts();

    if (!active) {
        loop_voice.active = false;
    } else {
        const sound_sample_t *s = &sound_table[sound_id];
        if (s->frame_count == 0) {
            loop_voice.active = false;
        } else if (!loop_voice.active || loop_voice.data != s->samples) {
            loop_voice.data = s->samples;
            loop_voice.len = s->frame_count;
            loop_voice.pos = 0;
            loop_voice.active = true;
        }
        // else: already looping this exact sample - leave position alone
        // so a repeated port write doesn't restart the waveform mid-play.
    }

    restore_interrupts(save);
}

void audio_i2s_init(void) {
    // MAX98357A & PIO1 hardware initialization removed - audio is handled in software / HDMI TMDS.
    voice_steal_next = 0;
}
