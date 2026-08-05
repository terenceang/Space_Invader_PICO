#ifndef HDMI_AUDIO_H
#define HDMI_AUDIO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ----------------------------------------------------------------------------
// HDMI 1.4 Specification Data Island & Audio Packet Constants
// ----------------------------------------------------------------------------

// Packet Types
#define HDMI_PACKET_TYPE_NULL                 0x00
#define HDMI_PACKET_TYPE_AUDIO_CLOCK_RECOVERY 0x01
#define HDMI_PACKET_TYPE_AUDIO_SAMPLE         0x02
#define HDMI_PACKET_TYPE_GENERAL_CONTROL      0x03
#define HDMI_PACKET_TYPE_ACP_PACKET           0x04
#define HDMI_PACKET_TYPE_ISRC1_PACKET         0x05
#define HDMI_PACKET_TYPE_ISRC2_PACKET         0x06
#define HDMI_PACKET_TYPE_ONE_BIT_AUDIO_SAMPLE 0x07
#define HDMI_PACKET_TYPE_DST_AUDIO_PACKET     0x08
#define HDMI_PACKET_TYPE_HBR_AUDIO_STREAM     0x09
#define HDMI_PACKET_TYPE_GAMUT_METADATA       0x0A

#define HDMI_PACKET_TYPE_INFOFRAME_AVI        0x82
#define HDMI_PACKET_TYPE_INFOFRAME_AUDIO      0x84

// TMDS TERC4 Symbols (10-bit symbols for 4-bit data encoding during Data Islands)
extern const uint16_t terc4_symbol_table[16];

// HDMI Guard Band 10-bit TMDS symbols for Data Island entry and exit
#define HDMI_DATA_ISLAND_GUARD_BAND_CH0 0x2CC // Channel 0 (Sync + Header LSB)
#define HDMI_DATA_ISLAND_GUARD_BAND_CH1 0x13C // Channel 1 (Subpacket lower nibbles)
#define HDMI_DATA_ISLAND_GUARD_BAND_CH2 0x13C // Channel 2 (Subpacket upper nibbles)

// Data Island dimensions
#define HDMI_DATA_ISLAND_PACKET_PIXELS 32 // 32 clock cycles per packet
#define HDMI_DATA_ISLAND_GUARD_PIXELS  2  // 2 leading + 2 trailing guard band pixels

// ----------------------------------------------------------------------------
// Data Structures
// ----------------------------------------------------------------------------

// HDMI Packet Header (3 bytes + Parity = 4 bytes transmitted on Channel 0)
typedef struct {
    uint8_t hb0; // Packet Type
    uint8_t hb1; // Type-dependent header byte 1
    uint8_t hb2; // Type-dependent header byte 2
} hdmi_header_t;

// HDMI Subpacket (7 payload bytes + 1 parity byte)
typedef struct {
    uint8_t sb[7];
    uint8_t parity;
} hdmi_subpacket_t;

// Complete HDMI Data Island Packet (Header + 4 Subpackets = 36 bytes raw)
typedef struct {
    hdmi_header_t header;
    uint8_t       header_parity;
    hdmi_subpacket_t subpacket[4];
} hdmi_packet_t;

// ----------------------------------------------------------------------------
// Function Declarations
// ----------------------------------------------------------------------------

/**
 * Computes BCH(8,5) parity for HDMI Packet Header byte.
 */
uint8_t hdmi_bch_ecc_header(uint8_t hb0, uint8_t hb1, uint8_t hb2);

/**
 * Computes BCH(32,24) parity for a 7-byte HDMI subpacket.
 */
uint8_t hdmi_bch_ecc_subpacket(const uint8_t subpacket[7]);

/**
 * Computes simple 8-bit checksum for InfoFrames (sum of all header + body bytes % 256 == 0).
 */
uint8_t hdmi_compute_checksum(const uint8_t *data, size_t len);

/**
 * Builds an Audio Clock Recovery (N/CTS) packet.
 * Recommended N values: 6272 for 44.1 kHz, 4096 for 32 kHz, 6144 for 48 kHz (at 25.2 MHz pixel clock).
 */
void hdmi_build_audio_clock_packet(hdmi_packet_t *pkt, uint32_t cts, uint32_t n);

/**
 * Builds a 2-channel 16-bit L-PCM Audio Sample Packet (contains 1 stereo sample pair in subpacket 0).
 */
void hdmi_build_audio_sample_packet(hdmi_packet_t *pkt, int16_t left_sample, int16_t right_sample, bool sample_present);

/**
 * Builds an Audio InfoFrame (Type 0x84) describing 2-channel L-PCM audio capabilities.
 */
void hdmi_build_audio_infoframe(hdmi_packet_t *pkt, uint8_t channels, uint32_t sample_rate_hz);

/**
 * Builds an AVI InfoFrame (Type 0x82) describing 640x480 RGB444/RGB565 video timing.
 */
void hdmi_build_avi_infoframe(hdmi_packet_t *pkt);

/**
 * Encodes an HDMI packet into 32-bit packed TMDS words (2 symbols per 32-bit word)
 * suitable for DMA transmission during horizontal blanking Data Island periods.
 *
 * @param pkt           Input HDMI packet with computed ECC/parity.
 * @param hsync         Active HSYNC state during the packet transmission.
 * @param vsync         Active VSYNC state during the packet transmission.
 * @param dst_ch0       Output buffer for Channel 0 (Sync + Header, size >= 18 words: 1 lead guard + 16 packet + 1 trail guard).
 * @param dst_ch1       Output buffer for Channel 1 (Data LSBs, size >= 18 words).
 * @param dst_ch2       Output buffer for Channel 2 (Data MSBs, size >= 18 words).
 */
void hdmi_encode_data_island(const hdmi_packet_t *pkt,
                             bool hsync, bool vsync,
                             uint32_t *dst_ch0, uint32_t *dst_ch1, uint32_t *dst_ch2);

#endif // HDMI_AUDIO_H
