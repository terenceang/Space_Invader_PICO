// I2S audio output driver - PIO1 + ping-pong DMA, driving a continuous test
// tone. See audio_i2s.h for the bring-up rationale and Hardware.md for the
// pin choice.

#include <math.h>
#include <stdint.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"

#include "audio_i2s.pio.h"

#include "audio_i2s.h"

// ----------------------------------------------------------------------------
// Board wiring - matches a real Raspberry Pi's I2S pin positions (GPIO18
// BCLK / GPIO19 LRCLK / GPIO21 DOUT) on this board's Pi-compatible 40-pin
// header (confirmed against the official schematic - see Hardware.md), so
// off-the-shelf I2S DAC HATs (PCM5102/UDA1334A/MAX98357A/HiFiBerry DAC, etc)
// wire up directly. None of these three are used elsewhere in this project
// (UART0 = GPIO0/1, the WS2812 status LED = GPIO2, I2C0 = GPIO6/7, DVI =
// GPIO32-39 - see Hardware.md's pinout table).
#define AUDIO_PIN_DATA       21 // DOUT
#define AUDIO_PIN_CLOCK_BASE 18 // BCLK (GPIO18) / LRCLK (GPIO18+1 = GPIO19)

#define AUDIO_PIO_INST pio1 // PIO0 is fully claimed by the DVI engine (src/dvi/)

// ----------------------------------------------------------------------------
// Test tone: a conservative-amplitude sine, both channels identical (this
// driver doesn't yet distinguish L/R - see audio_i2s.h). Not meant to be the
// final game sound - just enough to confirm the I2S signal path works.
#define AUDIO_SAMPLE_RATE_HZ 44100
#define AUDIO_TONE_HZ        440   // concert A - easy to identify by ear
#define AUDIO_AMPLITUDE      8000  // ~-12dBFS of a full-scale int16 (32767)

#define AUDIO_SINE_LUT_LEN 256
static int16_t sine_lut[AUDIO_SINE_LUT_LEN];

// DDS phase accumulator: top 8 bits of a free-wrapping uint32_t index the
// 256-entry LUT, so no branch/modulo is needed to wrap the phase - see
// generate_samples() below.
static uint32_t phase_acc;
static uint32_t phase_step;

// Ping-pong sample buffers: 256 stereo frames (~5.8ms @ 44.1kHz) each, which
// is the refill deadline generate_samples() has to beat from the DMA IRQ -
// trivially cheap for a LUT-only sine, plenty of slack for real game audio
// mixing later too.
#define AUDIO_BUF_FRAMES 256
static uint32_t audio_buf[2][AUDIO_BUF_FRAMES];

static uint audio_dma_chan[2];

// ----------------------------------------------------------------------------

static void generate_samples(uint32_t *buf, unsigned n_frames) {
    for (unsigned i = 0; i < n_frames; ++i) {
        int16_t sample = sine_lut[phase_acc >> 24];
        phase_acc += phase_step;
        // Same sample in both packed halves - see the "Test tone" comment
        // above for why L/R aren't distinguished yet.
        buf[i] = ((uint32_t)(uint16_t)sample << 16) | (uint16_t)sample;
    }
}

// Fires once per ~5.8ms buffer (see AUDIO_BUF_FRAMES) - refills whichever
// buffer DMA chaining just finished playing, while the other buffer plays.
static void audio_dma_irq_handler(void) {
    for (int i = 0; i < 2; ++i) {
        if (dma_hw->ints1 & (1u << audio_dma_chan[i])) {
            dma_hw->ints1 = 1u << audio_dma_chan[i];
            generate_samples(audio_buf[i], AUDIO_BUF_FRAMES);
        }
    }
}

void audio_i2s_init(void) {
    for (int i = 0; i < AUDIO_SINE_LUT_LEN; ++i)
        sine_lut[i] = (int16_t)(AUDIO_AMPLITUDE * sinf(2.0f * (float)M_PI * (float)i / AUDIO_SINE_LUT_LEN));
    phase_acc = 0;
    phase_step = (uint32_t)(((uint64_t)AUDIO_TONE_HZ << 32) / AUDIO_SAMPLE_RATE_HZ);

    PIO pio = AUDIO_PIO_INST;
    uint sm = pio_claim_unused_sm(pio, true);
    uint offset = pio_add_program(pio, &audio_i2s_program);

    // 32 BCLK cycles/frame (16 bits x 2 channels), 2 PIO clock cycles/BCLK
    // cycle (this program's bitloop has one `out` + one `jmp` per bit) - see
    // audio_i2s.pio.
    float clkdiv = (float)clock_get_hz(clk_sys) / (AUDIO_SAMPLE_RATE_HZ * 32 * 2);
    audio_i2s_program_init(pio, sm, offset, AUDIO_PIN_DATA, AUDIO_PIN_CLOCK_BASE, clkdiv);

    for (int i = 0; i < 2; ++i)
        audio_dma_chan[i] = dma_claim_unused_channel(true);

    uint dreq = pio_get_dreq(pio, sm, true);
    for (int i = 0; i < 2; ++i) {
        generate_samples(audio_buf[i], AUDIO_BUF_FRAMES);

        dma_channel_config cfg = dma_channel_get_default_config(audio_dma_chan[i]);
        channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
        channel_config_set_read_increment(&cfg, true);
        channel_config_set_write_increment(&cfg, false);
        channel_config_set_dreq(&cfg, dreq);
        // Chain to the other buffer's channel on completion - each retrigger
        // reloads read_addr/transfer_count from this configure call, so the
        // two channels free-run a ping-pong loop with no further software
        // intervention beyond refilling the just-finished buffer (see
        // audio_dma_irq_handler()).
        channel_config_set_chain_to(&cfg, audio_dma_chan[i ^ 1]);
        dma_channel_configure(audio_dma_chan[i], &cfg, &pio->txf[sm], audio_buf[i], AUDIO_BUF_FRAMES, false);
        dma_channel_set_irq1_enabled(audio_dma_chan[i], true);
    }

    irq_set_exclusive_handler(DMA_IRQ_1, audio_dma_irq_handler);
    irq_set_enabled(DMA_IRQ_1, true);

    pio_sm_set_enabled(pio, sm, true);
    dma_channel_start(audio_dma_chan[0]);
}
