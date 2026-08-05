#ifndef DVI_ENGINE_H
#define DVI_ENGINE_H

#include <stdbool.h>

#include "pico/util/queue.h"
#include "util_queue_u32_inline.h"

// PIO can only address 32 consecutive GPIOs at a time (window 0-31 or
// 16-47, selected at runtime via pio_set_gpio_base()). This board's TMDS
// pins (32-39) sit above the default 0-31 window, so callers must shift the
// PIO instance to this base - via pio_set_gpio_base(pio0, DVI_PIO_GPIO_BASE)
// - before dvi_engine_init() claims any PIO program/state machine.
#define DVI_PIO_GPIO_BASE 16

// Scanline queues: main.c pushes framebuffer scanline pointers (uint16_t*,
// RGB565, 320 pixels) into dvi_q_colour_valid, and drains freed pointers
// back out of dvi_q_colour_free once this engine is done with them. Same
// push/drain pattern main.c already used against the vendored library's
// dvi0.q_colour_valid/q_colour_free.
extern queue_t dvi_q_colour_valid;
extern queue_t dvi_q_colour_free;

// Brings up the PIO/DMA/PWM TMDS engine for this board's fixed
// configuration (GPIO 32-39, 640x480p60, RGB565). Must run after
// dvi_display_clock_init()'s set_sys_clock_khz(), and after
// pio_set_gpio_base(pio0, DVI_PIO_GPIO_BASE) - see Hardware.md, "RP2350B
// PIO Requirements for GPIO >= 32".
void dvi_engine_init(void);

// Registers the DMA IRQ handler on whichever core calls this. Always uses
// DMA_IRQ_0 - this project only ever needs one DVI output, so the second
// IRQ line the vendored library supported is dropped.
void dvi_engine_register_irqs_this_core(void);

// Starts DMA/PIO/PWM output. Call after registering IRQs and once at least
// one scanline has been pushed to dvi_q_colour_valid.
void dvi_engine_start(void);

// Core 1 entry point: repeatedly pulls a scanline pointer from
// dvi_q_colour_valid, TMDS-encodes it, and pushes the pointer back to
// dvi_q_colour_free. Never returns, but still services the DMA IRQ.
void dvi_engine_encode_loop(void);

// Encodes an Audio Sample Packet carrying 0-4 16-bit stereo PCM sample pairs
// (one per Data Island subpacket - the packet's full capacity) and makes it
// the next HDMI Data Island transmitted during active-video horizontal
// blanking. left[]/right[] must have at least count elements; count > 4 is
// clamped. A no-op if DVI_ENABLE_HDMI_AUDIO is 0.
void dvi_engine_send_hdmi_audio_samples(const int16_t *left, const int16_t *right, unsigned count);

// Same transmission mechanism as dvi_engine_send_hdmi_audio_samples(), but
// for the InfoFrame/Audio Clock Recovery packets a real HDMI sink expects
// periodically - see audio_i2s_step_scanline()'s scheduling of these across
// each frame. A no-op if DVI_ENABLE_HDMI_AUDIO is 0.
void dvi_engine_send_hdmi_avi_infoframe(void);
void dvi_engine_send_hdmi_audio_infoframe(uint8_t channels, uint32_t sample_rate_hz);
void dvi_engine_send_hdmi_acr_packet(uint32_t cts, uint32_t n);
void dvi_engine_send_hdmi_gcp(bool avmute);

#endif
