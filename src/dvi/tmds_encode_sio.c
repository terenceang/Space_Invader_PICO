#include "pico.h"
#include "hardware/structs/sio.h"

#include "tmds_encode_sio.h"

// Defined in tmds_encode_sio.S - the single ratio2/poppop SIO encode loop
// instantiation this board needs.
extern void tmds_sio_encode_channel_ratio2(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix);

static void __not_in_flash_func(configure_sio_tmds_for_channel)(unsigned channel_msb, unsigned channel_lsb) {
    sio_hw->tmds_ctrl =
        SIO_TMDS_CTRL_CLEAR_BALANCE_BITS |
        ((channel_msb - channel_lsb) << SIO_TMDS_CTRL_L0_NBITS_LSB) |
        (((channel_msb - 7u) & 0xfu) << SIO_TMDS_CTRL_L0_ROT_LSB) |
        (5u << SIO_TMDS_CTRL_PIX_SHIFT_LSB) |      // 1 + ctz(16): fixed 16bpp pixel width
        (1u << SIO_TMDS_CTRL_PIX2_NOSHIFT_LSB);    // hdouble: fixed on for this board's 2x scale
}

void __not_in_flash_func(tmds_encode_channel_16bpp)(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix,
                                                      unsigned channel_msb, unsigned channel_lsb) {
    configure_sio_tmds_for_channel(channel_msb, channel_lsb);
    tmds_sio_encode_channel_ratio2(pixbuf, symbuf, 2 * n_pix);
}
