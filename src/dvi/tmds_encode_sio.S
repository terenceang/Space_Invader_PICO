// Single hand-extracted TMDS SIO encode routine for this board's exact
// configuration: 16bpp RGB565 source, horizontal pixel-doubling (hdouble),
// DVI_SYMBOLS_PER_WORD == 2.
//
// This is the "ratio2 poppop" instantiation of the vendored macro
// tmds_encode_sio_loop (from lib/PicoDVI/software/libdvi/tmds_encode.S),
// written out directly instead of through the macro/template system, since
// this project only ever needs this one shape. Arm-Thumb only: this project
// targets rp2350-arm-s (Cortex-M33), not the RISC-V Hazard3 core, so the
// RISC-V half of the vendored file is dropped entirely.

#include "pico.h"
#include "hardware/regs/addressmap.h"
#include "hardware/regs/sio.h"

.syntax unified
.cpu cortex-m0plus
.thumb

.section .scratch_x.tmds_sio_encode_channel_ratio2, "ax"
.global tmds_sio_encode_channel_ratio2
.type tmds_sio_encode_channel_ratio2, %function
.thumb_func

// r0: input pixel buffer (word-aligned, 2 pixels/word)
// r1: output symbol buffer (word-aligned)
// r2: pixel count (must be even; caller passes 2x true pixel count, matching
//     the vendored tmds_encode_data_channel_16bpp SIO call convention)
tmds_sio_encode_channel_ratio2:
	push {r4, lr}
	lsls r2, r2, #1
	adds r2, r1
	ldr r3, =SIO_BASE + SIO_TMDS_CTRL_OFFSET
	b 2f
.align 2
// Larger ldr/str immediate offsets below need Thumb-2 encoding.
.cpu cortex-m33
1:
	ldr r4, [r0, #0]
	str r4, [r3, #SIO_TMDS_WDATA_OFFSET - SIO_TMDS_CTRL_OFFSET]
	ldr r4, [r3, #SIO_TMDS_POP_DOUBLE_L0_OFFSET - SIO_TMDS_CTRL_OFFSET]
	str r4, [r1, #0]
	ldr r4, [r3, #SIO_TMDS_POP_DOUBLE_L0_OFFSET - SIO_TMDS_CTRL_OFFSET]
	str r4, [r1, #4]
	ldr r4, [r0, #4]
	str r4, [r3, #SIO_TMDS_WDATA_OFFSET - SIO_TMDS_CTRL_OFFSET]
	ldr r4, [r3, #SIO_TMDS_POP_DOUBLE_L0_OFFSET - SIO_TMDS_CTRL_OFFSET]
	str r4, [r1, #8]
	ldr r4, [r3, #SIO_TMDS_POP_DOUBLE_L0_OFFSET - SIO_TMDS_CTRL_OFFSET]
	str r4, [r1, #12]
	adds r0, #8
	adds r1, #16
.cpu cortex-m0plus
2:
	cmp r1, r2
	blo 1b
	pop {r4, pc}
