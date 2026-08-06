#include <stdint.h>
#include <stdbool.h>

#include "display_config.h"

#if DEBUG_AUDIO_TEST_TONE
#include <math.h>
#endif

#include "hardware/sync.h"
#include "audio_i2s.h"
#include "sound_data.h"
#include "frank_hdmi.h"

#define AUDIO_MAX_VOICES 4

typedef struct {
    const int16_t *data;
    uint32_t pos;
    uint32_t len;
    bool active;
} audio_voice_t;

static audio_voice_t voices[AUDIO_MAX_VOICES];
static unsigned voice_steal_next;
static audio_voice_t loop_voice;

#if DEBUG_AUDIO_TEST_TONE
#define DEBUG_TONE_LUT_LEN 32 // 32,000 Hz / 32 = 1,000 Hz (1 kHz tone)
static int16_t debug_tone_lut[DEBUG_TONE_LUT_LEN];
static audio_voice_t debug_voice;
#endif

#define FRAMES_PER_VID (FRANK_HDMI_AUDIO_RATE / 60) /* 533 */

void audio_i2s_step_scanline(void) {
    // Legacy per-scanline hook (audio is now written in per-frame batches)
}

void audio_i2s_step_frame(void) {
    static int16_t frame_buf[FRAMES_PER_VID * 2];

    for (uint32_t i = 0; i < FRAMES_PER_VID; ++i) {
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

        if (mix > INT16_MAX)
            mix = INT16_MAX;
        else if (mix < INT16_MIN)
            mix = INT16_MIN;

        int16_t sample16 = (int16_t)mix;
        frame_buf[i * 2 + 0] = sample16;
        frame_buf[i * 2 + 1] = sample16;
    }

    frank_hdmi_audio_write(frame_buf, FRAMES_PER_VID);
}

void audio_i2s_set_mute(bool mute) {
    (void)mute;
}

#if DEBUG_AUDIO_TEST_TONE
void audio_i2s_debug_play_test_tone(void) {
    for (unsigned i = 0; i < DEBUG_TONE_LUT_LEN; ++i)
        debug_tone_lut[i] = (int16_t)(20000 * sinf(2.0f * (float)M_PI * (float)i / DEBUG_TONE_LUT_LEN));

    uint32_t save = save_and_disable_interrupts();
    debug_voice.data = debug_tone_lut;
    debug_voice.len = DEBUG_TONE_LUT_LEN;
    debug_voice.pos = 0;
    debug_voice.active = true;
    restore_interrupts(save);
}

void audio_i2s_debug_stop_test_tone(void) {
    uint32_t save = save_and_disable_interrupts();
    debug_voice.active = false;
    restore_interrupts(save);
}
#endif

void audio_i2s_play_sound(sound_id_t sound_id) {
    if (sound_id >= SOUND_COUNT)
        return;
    const sound_sample_t *s = &sound_table[sound_id];
    if (s->frame_count == 0)
        return;

    uint32_t save = save_and_disable_interrupts();

    unsigned slot = AUDIO_MAX_VOICES;
    for (unsigned i = 0; i < AUDIO_MAX_VOICES; ++i) {
        if (!voices[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == AUDIO_MAX_VOICES) {
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
    }

    restore_interrupts(save);
}

void audio_i2s_init(void) {
    voice_steal_next = 0;
}
