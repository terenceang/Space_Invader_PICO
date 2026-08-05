// Software audio mixer feeding HDMI embedded audio (src/dvi/hdmi_audio.c,
// src/dvi/dvi_engine.c) - mixes sound-effect playback triggered by the
// emulated machine's port 3/5 writes (see sound_effects.c) into a mono PCM
// stream, transmitted over the mini-HDMI connector's TMDS Data Islands
// rather than a physical I2S DAC. See audio_i2s.h for the driver overview.

#include <stdint.h>

#include "display_config.h"

#if DEBUG_AUDIO_TEST_TONE
#include <math.h>
#endif

#include "hardware/sync.h"

#include "audio_i2s.h"
#include "dvi_engine.h"
#include "hdmi_audio.h"

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

// Once per frame, on three well-spaced scanlines, send the AVI/Audio
// InfoFrame and an Audio Clock Recovery refresh instead of an audio sample
// packet for that scanline's HDMI Data Island - real HDMI sinks generally
// require a periodic Audio InfoFrame (and usually an AVI InfoFrame) before
// they'll enable audio decoding at all, no matter how correct the sample
// packets are. Spaced apart (rather than consecutive) so at most one of
// these rows is ever skipped before the next regular row flushes
// pending_samples below - see its overflow comment.
#define HDMI_AVI_INFOFRAME_ROW   0
#define HDMI_AUDIO_INFOFRAME_ROW 80
#define HDMI_ACR_REFRESH_ROW     160

// ----------------------------------------------------------------------------

// Steps the software audio mixer per scanline (~1.4 samples @ 44.1kHz/60Hz)
// and hands the result to the HDMI Data Island transport.
void audio_i2s_step_scanline(void) {
    static uint32_t sample_acc = 0;
    static unsigned scanline_in_frame = 0;
    static int16_t pending_samples[4];
    static unsigned pending_count = 0;

    // Pace audio sample generation to 44.1kHz (735 samples / 525 scanlines per frame)
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

        // Scale total mixed volume to 60% headroom to prevent DAC input clipping/noise
        mix = (mix * 60) / 100;

        if (mix > INT16_MAX)
            mix = INT16_MAX;
        else if (mix < INT16_MIN)
            mix = INT16_MIN;

        // pending_count can never exceed 4 here: the HDMI_*_ROW scanlines
        // above are spaced far enough apart that at most one of them is ever
        // skipped before the next regular row flushes pending_samples below,
        // and samples_to_gen is at most 2 per row (735/525 = 1.4 avg) - so
        // carried-over + new is at most 2 + 2 = 4, exactly the audio sample
        // packet's subpacket capacity (see hdmi_build_audio_sample_packet).
        if (pending_count < 4)
            pending_samples[pending_count++] = (int16_t)mix;
    }

    if (scanline_in_frame == HDMI_AVI_INFOFRAME_ROW) {
        dvi_engine_send_hdmi_avi_infoframe();
    } else if (scanline_in_frame == HDMI_AUDIO_INFOFRAME_ROW) {
        dvi_engine_send_hdmi_audio_infoframe(2, 44100);
    } else if (scanline_in_frame == HDMI_ACR_REFRESH_ROW) {
        dvi_engine_send_hdmi_acr_packet(HDMI_ACR_CTS_44100HZ, HDMI_ACR_N_44100HZ);
    } else {
        // Same sample in both L/R - this driver doesn't distinguish
        // channels (the real cabinet's sound is mono too).
        dvi_engine_send_hdmi_audio_samples(pending_samples, pending_samples, pending_count);
        pending_count = 0;
    }

    scanline_in_frame = (scanline_in_frame + 1 >= FRAME_HEIGHT) ? 0 : scanline_in_frame + 1;
}

void audio_i2s_set_mute(bool mute) {
    // No physical amp/DAC in this design (audio goes out over HDMI) - no-op.
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

    // audio_i2s_step_scanline() runs from Core 0's main loop between
    // pushing scanlines - briefly disable interrupts so it can never
    // observe a voice slot half-updated.
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
    voice_steal_next = 0;
}
