#ifndef DVI_TMDS_ENCODE_SIO_H
#define DVI_TMDS_ENCODE_SIO_H

#include <stddef.h>
#include <stdint.h>

// Encodes one RGB565 colour channel of a 320-pixel source scanline into
// TMDS symbols, using the RP2350's SIO hardware TMDS encoder with
// horizontal pixel-doubling (320 source pixels -> 640 active TMDS symbols
// per lane). This is the only encode shape this project needs: fixed
// 16bpp RGB565 input, fixed hdouble output, fixed DVI_SYMBOLS_PER_WORD == 2.
//
// pixbuf: word-aligned source scanline (RGB565, 2 pixels/word)
// symbuf: word-aligned output buffer, n_pix TMDS symbol-pair words
// n_pix: source pixel count for this channel (320 for this board's framebuffer)
// channel_msb/channel_lsb: bit range within each 16-bit pixel for this channel
void tmds_encode_channel_16bpp(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix,
                                unsigned channel_msb, unsigned channel_lsb);

#endif
