// Slim, board-specific DVI TMDS engine for the Waveshare RP2350-PiZero.
//
// This replaces the vendored lib/PicoDVI/software/libdvi (dvi.c +
// dvi_timing.c) for the main app. It implements the same proven algorithm -
// DMA control-block chaining via CHAIN_TO, a 4-state vertical timing FSM,
// and the underrun red-fallback behaviour - hardcoded for exactly one
// configuration instead of the vendored library's general multi-board,
// multi-bpp, multi-timing-mode machinery. See Video.md for the full
// pipeline writeup and Hardware.md for the board-specific GPIO/PIO details
// referenced below.
//
// lib/PicoDVI is kept in this repo (unmodified) purely as the
// dvi_reference_sample control-test target - see Hardware.md.

#include <assert.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/address_mapped.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"
#include "hardware/regs/addressmap.h"
#include "hardware/structs/padsbank0.h"

#include "dvi_serialiser.pio.h"
#include "tmds_encode_sio.h"

#include "dvi_engine.h"
#include "display_config.h"
#include "hdmi_audio.h"

// ----------------------------------------------------------------------------
// Board-specific configuration (Waveshare RP2350-PiZero, 640x480p60 RGB565)

#define DVI_PIO_INST pio0
// DVI_PIO_GPIO_BASE is declared in dvi_engine.h - dvi_display.c needs it too
// (to call pio_set_gpio_base() before dvi_engine_init() runs).

#define DVI_DATA_PIN_BLUE  36 // D0 (also the sync lane)
#define DVI_DATA_PIN_GREEN 34 // D1
#define DVI_DATA_PIN_RED   32 // D2
#define DVI_CLK_PIN        38

#define N_TMDS_LANES   3
#define TMDS_SYNC_LANE 0 // blue lane carries H/V sync

#define DVI_H_SYNC_POLARITY false
#define DVI_H_FRONT_PORCH   16
#define DVI_H_SYNC_WIDTH    96
#define DVI_H_BACK_PORCH    48
#define DVI_H_ACTIVE_PIXELS 640

#define DVI_V_SYNC_POLARITY false
#define DVI_V_FRONT_PORCH   10
#define DVI_V_SYNC_WIDTH    2
#define DVI_V_BACK_PORCH    33
#define DVI_V_ACTIVE_LINES  480

#define DVI_SYMBOLS_PER_WORD 2 // 2x 10-bit TMDS symbols packed per 32-bit word
#define DVI_VERTICAL_REPEAT  2 // 240 source lines -> 480 output lines
#define DVI_N_TMDS_BUFFERS   3 // encoded-lines-in-flight budget (~190us slack)
#define DVI_QUEUE_DEPTH      8

#define DVI_16BPP_RED_MSB   15
#define DVI_16BPP_RED_LSB   11
#define DVI_16BPP_GREEN_MSB 10
#define DVI_16BPP_GREEN_LSB 5
#define DVI_16BPP_BLUE_MSB  4
#define DVI_16BPP_BLUE_LSB  0

// Pull time-critical functions into RAM, matching the vendored library's
// placement - required to meet the DVI timing budget (see Video.md).
#define __dvi_func(f) __not_in_flash_func(f)

// ----------------------------------------------------------------------------
// DMA control-block plumbing (mirrors the vendored dvi_timing.h layout -
// this structure is inherent to how RP2350 DMA chaining works, not
// vendor-specific)

typedef struct dma_cb {
    const void *read_addr;
    void *write_addr;
    uint32_t transfer_count;
    dma_channel_config c;
} dma_cb_t;

static_assert(sizeof(dma_cb_t) == 4 * sizeof(uint32_t), "bad dma layout");
static_assert(__builtin_offsetof(dma_cb_t, c.ctrl) == __builtin_offsetof(dma_channel_hw_t, ctrl_trig), "bad dma layout");

enum dvi_line_state { DVI_STATE_FRONT_PORCH = 0, DVI_STATE_SYNC, DVI_STATE_BACK_PORCH, DVI_STATE_ACTIVE, DVI_STATE_COUNT };

#define DVI_SYNC_LANE_CHUNKS   DVI_STATE_COUNT // 4: front porch, sync, back porch, active
#define DVI_NOSYNC_LANE_CHUNKS 2               // 2: blanking (fp+sync+bp combined), active

struct dvi_scanline_dma_list {
    dma_cb_t l0[DVI_SYNC_LANE_CHUNKS];
    dma_cb_t l1[DVI_NOSYNC_LANE_CHUNKS];
    dma_cb_t l2[DVI_NOSYNC_LANE_CHUNKS];
};

static inline dma_cb_t *dvi_lane_from_list(struct dvi_scanline_dma_list *l, int i) {
    return i == 0 ? l->l0 : i == 1 ? l->l1 : l->l2;
}

struct dvi_lane_dma_cfg {
    uint chan_ctrl;
    uint chan_data;
    void *tx_fifo;
    uint dreq;
};

// ----------------------------------------------------------------------------
// Module state (this board only ever has one DVI instance, so plain statics
// replace the vendored library's struct-dvi_inst-passed-by-pointer design)

static const PIO dvi_pio = DVI_PIO_INST;
static const uint dvi_sm_tmds[N_TMDS_LANES] = {0, 1, 2};
static const uint dvi_pins_tmds[N_TMDS_LANES] = {DVI_DATA_PIN_BLUE, DVI_DATA_PIN_GREEN, DVI_DATA_PIN_RED};

static struct dvi_lane_dma_cfg dma_cfg[N_TMDS_LANES];

static struct dvi_scanline_dma_list dma_list_vblank_sync;
static struct dvi_scanline_dma_list dma_list_vblank_nosync;
static struct dvi_scanline_dma_list dma_list_active;
static struct dvi_scanline_dma_list dma_list_error;

static uint v_ctr;
static enum dvi_line_state v_state;

// A TMDS buffer must survive two IRQs after its last DMA use before being
// freed: one to confirm the control block loaded it, one after the actual
// transfer completed.
static uint32_t *tmds_buf_release_next;
static uint32_t *tmds_buf_release;
// How far behind the encoder is on TMDS scanlines - drives the red-fallback
// "solid error scanline" behaviour rather than corrupting frame timing.
static uint late_scanline_ctr;

queue_t dvi_q_colour_valid;
queue_t dvi_q_colour_free;
static queue_t q_tmds_valid;
static queue_t q_tmds_free;

typedef struct {
    int16_t left;
    int16_t right;
} hdmi_audio_sample_t;

#define HDMI_AUDIO_QUEUE_DEPTH 64
static queue_t q_hdmi_audio_samples;

static uint32_t __not_in_flash("dvi_engine_data") hdmi_island_ch0[18];
static uint32_t __not_in_flash("dvi_engine_data") hdmi_island_ch1[18];
static uint32_t __not_in_flash("dvi_engine_data") hdmi_island_ch2[18];

// Each symbol appears twice, concatenated in one word (DVI_SYMBOLS_PER_WORD
// == 2 on this board). Spec-defined DVI control symbols, pseudo-
// differential encoded - not board-specific. Needs RAM placement: DMA reads
// this constantly during blanking.
static const uint32_t __not_in_flash("dvi_engine") dvi_ctrl_syms[4] = {
    0xd5354, // vsync=0 hsync=0
    0x2acab, // vsync=0 hsync=1
    0x55154, // vsync=1 hsync=0
    0xaaeab, // vsync=1 hsync=1
};

// DC-balanced filler symbol pair, used to fill the active region with solid
// red when no encoded scanline is ready in time (buffer underrun fallback).
static const uint32_t __not_in_flash("dvi_engine") empty_scanline_tmds[3] = {
    0x7fd00u, // blue/sync lane: 0x00, 0x00
    0x7fd00u, // green lane: 0x00, 0x00
    0xbfa01u, // red lane: 0xfc, 0xfc
};

// ----------------------------------------------------------------------------
// Serialiser bring-up (PIO state machines + differential PWM clock)

static void dvi_configure_pad(uint gpio) {
    // 2mA drive, slew rate limiting, digital receiver disabled - matches the
    // vendored library's pad settings (fine up to 720p30; keeps the 3V3 LDO
    // cool compared to max drive strength).
    hw_write_masked(
        &padsbank0_hw->io[gpio],
        (0 << PADS_BANK0_GPIO0_DRIVE_LSB),
        PADS_BANK0_GPIO0_DRIVE_BITS | PADS_BANK0_GPIO0_SLEWFAST_BITS | PADS_BANK0_GPIO0_IE_BITS);
    gpio_set_outover(gpio, GPIO_OVERRIDE_NORMAL);
}

static void dvi_serialiser_start(bool enable) {
    uint mask = 0;
    for (int i = 0; i < N_TMDS_LANES; ++i)
        mask |= 1u << (dvi_sm_tmds[i] + PIO_CTRL_SM_ENABLE_LSB);
    if (enable) {
        // DVI allows phase offset between clock and data links, so PWM and
        // PIO don't need to be synchronised.
        hw_set_bits(&dvi_pio->ctrl, mask);
        pwm_set_enabled(pwm_gpio_to_slice_num(DVI_CLK_PIN), true);
    } else {
        hw_clear_bits(&dvi_pio->ctrl, mask);
        pwm_set_enabled(pwm_gpio_to_slice_num(DVI_CLK_PIN), false);
    }
}

// ----------------------------------------------------------------------------
// Per-scanline DMA blocklist construction (init-time only, not hot path)

static const uint32_t *get_ctrl_sym(bool vsync, bool hsync) {
    return &dvi_ctrl_syms[(!!vsync << 1) | !!hsync];
}

static void set_data_cb(dma_cb_t *cb, const struct dvi_lane_dma_cfg *cfg,
                         const void *read_addr, uint transfer_count, uint read_ring, bool irq_on_finish) {
    cb->read_addr = read_addr;
    cb->write_addr = cfg->tx_fifo;
    cb->transfer_count = transfer_count;
    cb->c = dma_channel_get_default_config(cfg->chan_data);
    channel_config_set_ring(&cb->c, false, read_ring);
    channel_config_set_dreq(&cb->c, cfg->dreq);
    channel_config_set_chain_to(&cb->c, cfg->chan_ctrl); // reprogrammed by IRQ, or CHAIN_TO on completion
    channel_config_set_irq_quiet(&cb->c, !irq_on_finish);
}

static void dvi_setup_scanline_for_vblank(bool vsync_asserted, struct dvi_scanline_dma_list *l) {
    bool vsync = DVI_V_SYNC_POLARITY == vsync_asserted;
    const uint32_t *sym_hsync_off = get_ctrl_sym(vsync, !DVI_H_SYNC_POLARITY);
    const uint32_t *sym_hsync_on = get_ctrl_sym(vsync, DVI_H_SYNC_POLARITY);
    const uint32_t *sym_no_sync = get_ctrl_sym(false, false);

    dma_cb_t *synclist = dvi_lane_from_list(l, TMDS_SYNC_LANE);
    set_data_cb(&synclist[0], &dma_cfg[TMDS_SYNC_LANE], sym_hsync_off, DVI_H_FRONT_PORCH / DVI_SYMBOLS_PER_WORD, 2, false);
    set_data_cb(&synclist[1], &dma_cfg[TMDS_SYNC_LANE], sym_hsync_on, DVI_H_SYNC_WIDTH / DVI_SYMBOLS_PER_WORD, 2, false);
    set_data_cb(&synclist[2], &dma_cfg[TMDS_SYNC_LANE], sym_hsync_off, DVI_H_BACK_PORCH / DVI_SYMBOLS_PER_WORD, 2, true);
    set_data_cb(&synclist[3], &dma_cfg[TMDS_SYNC_LANE], sym_hsync_off, DVI_H_ACTIVE_PIXELS / DVI_SYMBOLS_PER_WORD, 2, false);

    for (int i = 0; i < N_TMDS_LANES; ++i) {
        if (i == TMDS_SYNC_LANE) continue;
        dma_cb_t *cblist = dvi_lane_from_list(l, i);
        set_data_cb(&cblist[0], &dma_cfg[i], sym_no_sync,
                    (DVI_H_FRONT_PORCH + DVI_H_SYNC_WIDTH + DVI_H_BACK_PORCH) / DVI_SYMBOLS_PER_WORD, 2, false);
        set_data_cb(&cblist[1], &dma_cfg[i], sym_no_sync, DVI_H_ACTIVE_PIXELS / DVI_SYMBOLS_PER_WORD, 2, false);
    }
}

// tmdsbuf == NULL builds the permanent red-fallback list (dma_list_error).
// tmdsbuf != NULL is only used once at init to establish dma_list_active's
// non-repeating ring config; the real per-scanline buffer address is patched
// in later by dvi_update_scanline_data_dma().
static void dvi_setup_scanline_for_active(uint32_t *tmdsbuf, struct dvi_scanline_dma_list *l) {
    const uint32_t *sym_hsync_off = get_ctrl_sym(!DVI_V_SYNC_POLARITY, !DVI_H_SYNC_POLARITY);
    const uint32_t *sym_hsync_on = get_ctrl_sym(!DVI_V_SYNC_POLARITY, DVI_H_SYNC_POLARITY);
    const uint32_t *sym_no_sync = get_ctrl_sym(false, false);

    dma_cb_t *synclist = dvi_lane_from_list(l, TMDS_SYNC_LANE);
    set_data_cb(&synclist[0], &dma_cfg[TMDS_SYNC_LANE], sym_hsync_off, DVI_H_FRONT_PORCH / DVI_SYMBOLS_PER_WORD, 2, false);
    set_data_cb(&synclist[1], &dma_cfg[TMDS_SYNC_LANE], sym_hsync_on, DVI_H_SYNC_WIDTH / DVI_SYMBOLS_PER_WORD, 2, false);
    set_data_cb(&synclist[2], &dma_cfg[TMDS_SYNC_LANE], sym_hsync_off, DVI_H_BACK_PORCH / DVI_SYMBOLS_PER_WORD, 2, true);

    for (int i = 0; i < N_TMDS_LANES; ++i) {
        dma_cb_t *cblist = dvi_lane_from_list(l, i);
        if (i != TMDS_SYNC_LANE) {
            set_data_cb(&cblist[0], &dma_cfg[i], sym_no_sync,
                        (DVI_H_FRONT_PORCH + DVI_H_SYNC_WIDTH + DVI_H_BACK_PORCH) / DVI_SYMBOLS_PER_WORD, 2, false);
        }
        int target_block = i == TMDS_SYNC_LANE ? DVI_SYNC_LANE_CHUNKS - 1 : DVI_NOSYNC_LANE_CHUNKS - 1;
        if (tmdsbuf) {
            set_data_cb(&cblist[target_block], &dma_cfg[i], tmdsbuf + i * (DVI_H_ACTIVE_PIXELS / DVI_SYMBOLS_PER_WORD),
                        DVI_H_ACTIVE_PIXELS / DVI_SYMBOLS_PER_WORD, 0, false);
        } else {
            set_data_cb(&cblist[target_block], &dma_cfg[i], &empty_scanline_tmds[i],
                        DVI_H_ACTIVE_PIXELS / DVI_SYMBOLS_PER_WORD, 2, false);
        }
    }
}

static void __dvi_func(dvi_update_scanline_data_dma)(const uint32_t *tmdsbuf, struct dvi_scanline_dma_list *l) {
    for (int i = 0; i < N_TMDS_LANES; ++i) {
        const uint32_t *lane_tmdsbuf = tmdsbuf + i * (DVI_H_ACTIVE_PIXELS / DVI_SYMBOLS_PER_WORD);
        if (i == TMDS_SYNC_LANE)
            dvi_lane_from_list(l, i)[3].read_addr = lane_tmdsbuf;
        else
            dvi_lane_from_list(l, i)[1].read_addr = lane_tmdsbuf;
    }
}

// ----------------------------------------------------------------------------
// Hot path: DMA IRQ handler + vertical timing FSM

static inline void __attribute__((always_inline)) load_dma_op(struct dvi_scanline_dma_list *l) {
    for (int i = 0; i < N_TMDS_LANES; ++i) {
        dma_channel_config cfg = dma_channel_get_default_config(dma_cfg[i].chan_ctrl);
        channel_config_set_ring(&cfg, true, 4); // 16-byte write wrap
        channel_config_set_read_increment(&cfg, true);
        channel_config_set_write_increment(&cfg, true);
        dma_channel_configure(
            dma_cfg[i].chan_ctrl, &cfg,
            &dma_hw->ch[dma_cfg[i].chan_data],
            dvi_lane_from_list(l, i),
            4, // all 4 registers, then halt until next CHAIN_TO
            false);
    }
}

static void __dvi_func(dvi_timing_state_advance)(void) {
    v_ctr++;
    if ((v_state == DVI_STATE_FRONT_PORCH && v_ctr == DVI_V_FRONT_PORCH) ||
        (v_state == DVI_STATE_SYNC && v_ctr == DVI_V_SYNC_WIDTH) ||
        (v_state == DVI_STATE_BACK_PORCH && v_ctr == DVI_V_BACK_PORCH) ||
        (v_state == DVI_STATE_ACTIVE && v_ctr == DVI_V_ACTIVE_LINES)) {
        v_state = (v_state + 1) % DVI_STATE_COUNT;
        v_ctr = 0;
    }
}

// Fires once per scanline period (~31.78us at 640x480p60). See Video.md's
// "Timing budget" section for why nothing in this path may block.
static void __dvi_func(dvi_dma_irq_handler)(void) {
    dma_hw->ints0 = 1u << dma_cfg[TMDS_SYNC_LANE].chan_data;

    dvi_timing_state_advance();
    if (tmds_buf_release && !queue_try_add_u32(&q_tmds_free, &tmds_buf_release))
        panic("TMDS free queue full in IRQ!");
    tmds_buf_release = tmds_buf_release_next;
    tmds_buf_release_next = NULL;

    // Make sure all three channels have definitely loaded their last block.
    for (int i = 0; i < N_TMDS_LANES; ++i) {
        while (dma_debug_hw->ch[dma_cfg[i].chan_data].dbg_tcr != DVI_H_ACTIVE_PIXELS / DVI_SYMBOLS_PER_WORD)
            tight_loop_contents();
    }

    uint32_t *tmdsbuf;
    while (late_scanline_ctr > 0 && queue_try_remove_u32(&q_tmds_valid, &tmdsbuf)) {
        // This buffer would land in the wrong vertical position now - drop it.
        queue_add_blocking_u32(&q_tmds_free, &tmdsbuf);
        --late_scanline_ctr;
    }

    if (v_state != DVI_STATE_ACTIVE) {
        tmdsbuf = NULL;
    } else if (queue_try_peek_u32(&q_tmds_valid, &tmdsbuf)) {
        if (v_ctr % DVI_VERTICAL_REPEAT == DVI_VERTICAL_REPEAT - 1) {
            queue_remove_blocking_u32(&q_tmds_valid, &tmdsbuf);
            tmds_buf_release_next = tmdsbuf;
        }
    } else {
        // No valid scanline ready - solid red fallback (see Video.md).
        tmdsbuf = NULL;
        if (v_ctr % DVI_VERTICAL_REPEAT == DVI_VERTICAL_REPEAT - 1)
            ++late_scanline_ctr;
    }

    switch (v_state) {
        case DVI_STATE_ACTIVE:
            if (tmdsbuf) {
                dvi_update_scanline_data_dma(tmdsbuf, &dma_list_active);
                load_dma_op(&dma_list_active);
            } else {
                load_dma_op(&dma_list_error);
            }
            break;
        case DVI_STATE_SYNC:
            load_dma_op(&dma_list_vblank_sync);
            break;
        default:
            load_dma_op(&dma_list_vblank_nosync);
            break;
    }
}

// ----------------------------------------------------------------------------
// Public API

void dvi_engine_init(void) {
    // PIO state machines (one per TMDS lane) - see dvi_serialiser.pio.
    uint offset = pio_add_program(dvi_pio, &dvi_serialiser_program);
    for (int i = 0; i < N_TMDS_LANES; ++i) {
        pio_sm_claim(dvi_pio, dvi_sm_tmds[i]);
        dvi_serialiser_program_init(dvi_pio, dvi_sm_tmds[i], offset, dvi_pins_tmds[i]);
        dvi_configure_pad(dvi_pins_tmds[i]);
        dvi_configure_pad(dvi_pins_tmds[i] + 1);
    }

    // Differential TMDS clock via PWM (both GPIOs must share a slice, so the
    // lower-numbered pin must be even - GPIO38 is).
    assert(DVI_CLK_PIN % 2 == 0);
    uint slice = pwm_gpio_to_slice_num(DVI_CLK_PIN);
    pwm_config pwm_cfg = pwm_get_default_config();
    pwm_config_set_output_polarity(&pwm_cfg, true, false); // complementary outputs
    pwm_config_set_wrap(&pwm_cfg, 9);
    pwm_init(slice, &pwm_cfg, false);
    pwm_set_both_levels(slice, 5, 5); // 5 cycles high, 5 low
    for (uint i = DVI_CLK_PIN; i <= DVI_CLK_PIN + 1; ++i) {
        gpio_set_function(i, GPIO_FUNC_PWM);
        dvi_configure_pad(i);
    }

    // One DMA control channel + one data channel per TMDS lane.
    for (int i = 0; i < N_TMDS_LANES; ++i) {
        dma_cfg[i].chan_ctrl = dma_claim_unused_channel(true);
        dma_cfg[i].chan_data = dma_claim_unused_channel(true);
        dma_cfg[i].tx_fifo = (void *)&dvi_pio->txf[dvi_sm_tmds[i]];
        dma_cfg[i].dreq = pio_get_dreq(dvi_pio, dvi_sm_tmds[i], true);
    }

    late_scanline_ctr = 0;
    tmds_buf_release_next = NULL;
    tmds_buf_release = NULL;
    v_ctr = 0;
    v_state = DVI_STATE_FRONT_PORCH;

    queue_init(&q_tmds_valid, sizeof(void *), DVI_QUEUE_DEPTH);
    queue_init(&q_tmds_free, sizeof(void *), DVI_QUEUE_DEPTH);
    queue_init(&dvi_q_colour_valid, sizeof(void *), DVI_QUEUE_DEPTH);
    queue_init(&dvi_q_colour_free, sizeof(void *), DVI_QUEUE_DEPTH);
    queue_init(&q_hdmi_audio_samples, sizeof(hdmi_audio_sample_t), HDMI_AUDIO_QUEUE_DEPTH);

    // Pre-encode initial Audio Clock Recovery packet (N=6272, CTS=25200 for 44.1kHz @ 25.2MHz pixel clock)
    hdmi_packet_t acr_pkt;
    hdmi_build_audio_clock_packet(&acr_pkt, 25200, 6272);
    hdmi_encode_data_island(&acr_pkt, !DVI_H_SYNC_POLARITY, !DVI_V_SYNC_POLARITY,
                            hdmi_island_ch0, hdmi_island_ch1, hdmi_island_ch2);

    dvi_setup_scanline_for_vblank(true, &dma_list_vblank_sync);
    dvi_setup_scanline_for_vblank(false, &dma_list_vblank_nosync);
    dvi_setup_scanline_for_active((uint32_t *)SRAM_BASE, &dma_list_active); // placeholder addr, patched per-scanline
    dvi_setup_scanline_for_active(NULL, &dma_list_error);

    for (int i = 0; i < DVI_N_TMDS_BUFFERS; ++i) {
        void *tmdsbuf = malloc(N_TMDS_LANES * (DVI_H_ACTIVE_PIXELS / DVI_SYMBOLS_PER_WORD) * sizeof(uint32_t));
        if (!tmdsbuf)
            panic("TMDS buffer allocation failed");
        queue_add_blocking_u32(&q_tmds_free, &tmdsbuf);
    }
}

void dvi_engine_register_irqs_this_core(void) {
    uint32_t mask_sync_channel = 1u << dma_cfg[TMDS_SYNC_LANE].chan_data;
    uint32_t mask_all_channels = 0;
    for (int i = 0; i < N_TMDS_LANES; ++i)
        mask_all_channels |= 1u << dma_cfg[i].chan_ctrl | 1u << dma_cfg[i].chan_data;

    dma_hw->ints0 = mask_sync_channel;
    hw_write_masked(&dma_hw->inte0, mask_sync_channel, mask_all_channels);
    irq_set_exclusive_handler(DMA_IRQ_0, dvi_dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);
}

void dvi_engine_start(void) {
    load_dma_op(&dma_list_vblank_nosync);
    dma_start_channel_mask(
        (1u << dma_cfg[0].chan_ctrl) | (1u << dma_cfg[1].chan_ctrl) | (1u << dma_cfg[2].chan_ctrl));

    // Don't want the FIFOs to bottom out - wait for full before shifting out.
    for (int i = 0; i < N_TMDS_LANES; ++i)
        while (!pio_sm_is_tx_fifo_full(dvi_pio, dvi_sm_tmds[i]))
            tight_loop_contents();
    dvi_serialiser_start(true);
}

static void __dvi_func(dvi_prepare_scanline)(uint32_t *scanbuf) {
    uint32_t *tmdsbuf;
    queue_remove_blocking_u32(&q_tmds_free, &tmdsbuf);
    uint words_per_channel = DVI_H_ACTIVE_PIXELS / DVI_SYMBOLS_PER_WORD;
    tmds_encode_channel_16bpp(scanbuf, tmdsbuf + 0 * words_per_channel, DVI_H_ACTIVE_PIXELS / 2, DVI_16BPP_BLUE_MSB, DVI_16BPP_BLUE_LSB);
    tmds_encode_channel_16bpp(scanbuf, tmdsbuf + 1 * words_per_channel, DVI_H_ACTIVE_PIXELS / 2, DVI_16BPP_GREEN_MSB, DVI_16BPP_GREEN_LSB);
    tmds_encode_channel_16bpp(scanbuf, tmdsbuf + 2 * words_per_channel, DVI_H_ACTIVE_PIXELS / 2, DVI_16BPP_RED_MSB, DVI_16BPP_RED_LSB);
    queue_add_blocking_u32(&q_tmds_valid, &tmdsbuf);
}

void __dvi_func(dvi_engine_encode_loop)(void) {
    while (1) {
        uint32_t *scanbuf;
        queue_remove_blocking_u32(&dvi_q_colour_valid, &scanbuf);
        dvi_prepare_scanline(scanbuf);
        queue_add_blocking_u32(&dvi_q_colour_free, &scanbuf);
    }
}

void dvi_engine_send_hdmi_audio_sample(int16_t left, int16_t right) {
    hdmi_audio_sample_t sample = { left, right };
    queue_try_add(&q_hdmi_audio_samples, &sample);
}
