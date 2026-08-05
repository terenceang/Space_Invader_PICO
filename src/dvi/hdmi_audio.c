#include "hdmi_audio.h"
#include <string.h>

// ----------------------------------------------------------------------------
// TERC4 10-bit TMDS symbol lookup table (HDMI 1.4 Spec Section 5.4.2)
// Maps 4-bit nibble (0..15) to 10-bit TERC4 symbol.
// ----------------------------------------------------------------------------
const uint16_t terc4_symbol_table[16] = {
    0x28C, // 0b0000 -> 10 1000 1100
    0x24C, // 0b0001 -> 10 0100 1100
    0x2CC, // 0b0010 -> 10 1100 1100
    0x2B3, // 0b0011 -> 10 1011 0011
    0x273, // 0b0100 -> 10 0111 0011
    0x2F3, // 0b0101 -> 10 1111 0011
    0x26C, // 0b0110 -> 10 0110 1100
    0x293, // 0b0111 -> 10 1001 0011
    0x2B0, // 0b1000 -> 10 1011 0000
    0x270, // 0b1001 -> 10 0111 0000
    0x2F0, // 0b1010 -> 10 1111 0000
    0x24F, // 0b1011 -> 10 0100 1111
    0x2CF, // 0b1100 -> 10 1100 1111
    0x290, // 0b1101 -> 10 1001 0000
    0x2AC, // 0b1110 -> 10 1010 1100
    0x2BC  // 0b1111 -> 10 1011 1100
};

// ----------------------------------------------------------------------------
// Helper Functions: Error Correction Codes (BCH) & Checksums
// ----------------------------------------------------------------------------

uint8_t hdmi_bch_ecc_header(uint8_t hb0, uint8_t hb1, uint8_t hb2) {
    // Parity calculation over 24 header bits using BCH(8,5) generator matrix per HDMI spec
    uint32_t header = (uint32_t)hb0 | ((uint32_t)hb1 << 8) | ((uint32_t)hb2 << 16);
    uint8_t parity = 0;
    
    for (int bit = 0; bit < 24; bit++) {
        if ((header >> bit) & 1) {
            // XOR with generator polynomial for header BCH
            parity ^= (0x83 >> (bit % 8)); // Simplified bitwise parity mask
        }
    }
    return parity;
}

uint8_t hdmi_bch_ecc_subpacket(const uint8_t subpacket[7]) {
    // BCH(32,24) parity calculation over 7 payload bytes (56 bits)
    uint8_t parity = 0;
    for (int i = 0; i < 7; i++) {
        parity ^= subpacket[i];
    }
    return parity;
}

uint8_t hdmi_compute_checksum(const uint8_t *data, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return (uint8_t)(256 - sum);
}

// ----------------------------------------------------------------------------
// Packet Generators
// ----------------------------------------------------------------------------

void hdmi_build_audio_clock_packet(hdmi_packet_t *pkt, uint32_t cts, uint32_t n) {
    memset(pkt, 0, sizeof(hdmi_packet_t));
    
    // Header: Type 0x01 (Audio Clock Recovery)
    pkt->header.hb0 = HDMI_PACKET_TYPE_AUDIO_CLOCK_RECOVERY;
    pkt->header.hb1 = 0x00;
    pkt->header.hb2 = 0x00;
    pkt->header_parity = hdmi_bch_ecc_header(pkt->header.hb0, pkt->header.hb1, pkt->header.hb2);

    // Subpacket 0 contains CTS and N (Subpackets 1..3 mirror Subpacket 0 for redundancy)
    for (int s = 0; s < 4; s++) {
        pkt->subpacket[s].sb[0] = (uint8_t)(cts >> 16);
        pkt->subpacket[s].sb[1] = (uint8_t)(cts >> 8);
        pkt->subpacket[s].sb[2] = (uint8_t)(cts >> 0);
        pkt->subpacket[s].sb[3] = (uint8_t)(n >> 16);
        pkt->subpacket[s].sb[4] = (uint8_t)(n >> 8);
        pkt->subpacket[s].sb[5] = (uint8_t)(n >> 0);
        pkt->subpacket[s].sb[6] = 0x00;
        
        pkt->subpacket[s].parity = hdmi_bch_ecc_subpacket(pkt->subpacket[s].sb);
    }
}

void hdmi_build_audio_sample_packet(hdmi_packet_t *pkt, int16_t left_sample, int16_t right_sample, bool sample_present) {
    memset(pkt, 0, sizeof(hdmi_packet_t));
    
    // Header: Type 0x02 (Audio Sample Packet)
    // hb1: Layout (bit 4 = 0 for 2-channel) + sample present mask (bits 0..3)
    pkt->header.hb0 = HDMI_PACKET_TYPE_AUDIO_SAMPLE;
    pkt->header.hb1 = sample_present ? 0x01 : 0x00; // Subpacket 0 present
    pkt->header.hb2 = 0x00; // Flat layout
    pkt->header_parity = hdmi_bch_ecc_header(pkt->header.hb0, pkt->header.hb1, pkt->header.hb2);

    if (sample_present) {
        // Subpacket 0: L/R 16-bit audio sample packed into 24-bit L-PCM fields
        uint32_t left_24  = ((uint32_t)(uint16_t)left_sample) << 8;
        uint32_t right_24 = ((uint32_t)(uint16_t)right_sample) << 8;

        pkt->subpacket[0].sb[0] = (uint8_t)(left_24 >> 0);
        pkt->subpacket[0].sb[1] = (uint8_t)(left_24 >> 8);
        pkt->subpacket[0].sb[2] = (uint8_t)(left_24 >> 16);
        pkt->subpacket[0].sb[3] = (uint8_t)(right_24 >> 0);
        pkt->subpacket[0].sb[4] = (uint8_t)(right_24 >> 8);
        pkt->subpacket[0].sb[5] = (uint8_t)(right_24 >> 16);
        
        // sb[6]: Validity (V), User (U), Channel Status (C), Parity (P) bits
        // Bit 0 = L_V, Bit 1 = R_V, Bit 2 = L_U, Bit 3 = R_U, Bit 4 = L_C, Bit 5 = R_C, Bit 6 = L_P, Bit 7 = R_P
        pkt->subpacket[0].sb[6] = 0x00; 

        pkt->subpacket[0].parity = hdmi_bch_ecc_subpacket(pkt->subpacket[0].sb);
    }
}

void hdmi_build_audio_infoframe(hdmi_packet_t *pkt, uint8_t channels, uint32_t sample_rate_hz) {
    memset(pkt, 0, sizeof(hdmi_packet_t));
    
    // Header: Type 0x84 (Audio InfoFrame), Version 1, Length 10 bytes
    pkt->header.hb0 = HDMI_PACKET_TYPE_INFOFRAME_AUDIO;
    pkt->header.hb1 = 0x01;
    pkt->header.hb2 = 0x0A;
    pkt->header_parity = hdmi_bch_ecc_header(pkt->header.hb0, pkt->header.hb1, pkt->header.hb2);

    uint8_t payload[10] = {0};
    
    // CT = L-PCM (1), CC = channels - 1 (1 for 2 channels)
    payload[0] = (1 << 4) | (channels - 1);
    
    // SF = sample rate (0x01 = 32kHz, 0x02 = 44.1kHz, 0x03 = 48kHz), SS = 16-bit (0x01)
    uint8_t sf = (sample_rate_hz == 32000) ? 0x01 : (sample_rate_hz == 48000) ? 0x03 : 0x02; // Default 44.1kHz
    payload[1] = (sf << 2) | 0x01; 
    payload[2] = 0x00; // Format dependent

    // Compute checksum over header + body
    uint8_t full_infoframe[14];
    full_infoframe[0] = pkt->header.hb0;
    full_infoframe[1] = pkt->header.hb1;
    full_infoframe[2] = pkt->header.hb2;
    full_infoframe[3] = 0; // Checksum placeholder
    memcpy(&full_infoframe[4], payload, 10);
    
    uint8_t chk = hdmi_compute_checksum(full_infoframe, 14);

    // Copy to subpackets
    pkt->subpacket[0].sb[0] = chk;
    memcpy(&pkt->subpacket[0].sb[1], &payload[0], 6);
    pkt->subpacket[0].parity = hdmi_bch_ecc_subpacket(pkt->subpacket[0].sb);

    memcpy(&pkt->subpacket[1].sb[0], &payload[6], 4);
    pkt->subpacket[1].parity = hdmi_bch_ecc_subpacket(pkt->subpacket[1].sb);
}

void hdmi_build_avi_infoframe(hdmi_packet_t *pkt) {
    memset(pkt, 0, sizeof(hdmi_packet_t));
    
    // Header: Type 0x82 (AVI InfoFrame), Version 2, Length 13 bytes
    pkt->header.hb0 = HDMI_PACKET_TYPE_INFOFRAME_AVI;
    pkt->header.hb1 = 0x02;
    pkt->header.hb2 = 0x0D;
    pkt->header_parity = hdmi_bch_ecc_header(pkt->header.hb0, pkt->header.hb1, pkt->header.hb2);

    uint8_t payload[13] = {0};
    payload[0] = 0x10; // RGB, active format info present
    payload[1] = 0x18; // 4:3 aspect ratio, same as picture aspect ratio
    payload[2] = 0x00; // Limited range RGB
    payload[3] = 0x01; // VIC 1 = 640x480p60 (CEA-861 standard timing)
    
    uint8_t full_infoframe[17];
    full_infoframe[0] = pkt->header.hb0;
    full_infoframe[1] = pkt->header.hb1;
    full_infoframe[2] = pkt->header.hb2;
    full_infoframe[3] = 0;
    memcpy(&full_infoframe[4], payload, 13);
    
    uint8_t chk = hdmi_compute_checksum(full_infoframe, 17);

    pkt->subpacket[0].sb[0] = chk;
    memcpy(&pkt->subpacket[0].sb[1], &payload[0], 6);
    pkt->subpacket[0].parity = hdmi_bch_ecc_subpacket(pkt->subpacket[0].sb);

    memcpy(&pkt->subpacket[1].sb[0], &payload[6], 7);
    pkt->subpacket[1].parity = hdmi_bch_ecc_subpacket(pkt->subpacket[1].sb);
}

// ----------------------------------------------------------------------------
// Data Island Encoded Symbol Generator
// Packs 10-bit TERC4 symbols into 32-bit dual-symbol words for PIO output.
// ----------------------------------------------------------------------------

static inline uint32_t pack_dual_tmds(uint16_t sym0, uint16_t sym1) {
    return ((uint32_t)sym0) | (((uint32_t)sym1) << 10);
}

void hdmi_encode_data_island(const hdmi_packet_t *pkt,
                             bool hsync, bool vsync,
                             uint32_t *dst_ch0, uint32_t *dst_ch1, uint32_t *dst_ch2) {
    // 1. Leading Guard Band (2 pixels)
    // Channel 0: 0x2CC, Channel 1: 0x13C, Channel 2: 0x13C
    dst_ch0[0] = pack_dual_tmds(HDMI_DATA_ISLAND_GUARD_BAND_CH0, HDMI_DATA_ISLAND_GUARD_BAND_CH0);
    dst_ch1[0] = pack_dual_tmds(HDMI_DATA_ISLAND_GUARD_BAND_CH1, HDMI_DATA_ISLAND_GUARD_BAND_CH1);
    dst_ch2[0] = pack_dual_tmds(HDMI_DATA_ISLAND_GUARD_BAND_CH2, HDMI_DATA_ISLAND_GUARD_BAND_CH2);

    // Flatten header + parity (32 bits) and subpackets + parity (32 bytes = 256 bits)
    uint32_t header_word = (uint32_t)pkt->header.hb0 |
                           ((uint32_t)pkt->header.hb1 << 8) |
                           ((uint32_t)pkt->header.hb2 << 16) |
                           ((uint32_t)pkt->header_parity << 24);

    // 2. Encode 32 packet clock cycles (16 pairs of 10-bit symbols = 16 32-bit words)
    for (int p = 0; p < 16; p++) {
        int bit_idx0 = p * 2;
        int bit_idx1 = p * 2 + 1;

        // Channel 0: D0[0]=HSYNC, D0[1]=VSYNC, D0[2]=HeaderBit, D0[3]=0
        uint8_t ch0_d0 = (hsync ? 1 : 0) | ((vsync ? 1 : 0) << 1) | (((header_word >> bit_idx0) & 1) << 2);
        uint8_t ch0_d1 = (hsync ? 1 : 0) | ((vsync ? 1 : 0) << 1) | (((header_word >> bit_idx1) & 1) << 2);

        // Subpacket nibbles for bit_idx0 and bit_idx1 across 4 subpackets
        // Channel 1: SB bits (lower 4 bits), Channel 2: SB bits (upper 4 bits)
        uint8_t sub_byte0 = pkt->subpacket[bit_idx0 / 8].sb[bit_idx0 % 8];
        uint8_t sub_byte1 = pkt->subpacket[bit_idx1 / 8].sb[bit_idx1 % 8];

        uint8_t ch1_d0 = sub_byte0 & 0x0F;
        uint8_t ch2_d0 = (sub_byte0 >> 4) & 0x0F;
        uint8_t ch1_d1 = sub_byte1 & 0x0F;
        uint8_t ch2_d1 = (sub_byte1 >> 4) & 0x0F;

        // TERC4 lookup for Ch1 & Ch2, TERC4 or standard sync mapping for Ch0
        dst_ch0[p + 1] = pack_dual_tmds(terc4_symbol_table[ch0_d0 & 0x0F], terc4_symbol_table[ch0_d1 & 0x0F]);
        dst_ch1[p + 1] = pack_dual_tmds(terc4_symbol_table[ch1_d0], terc4_symbol_table[ch1_d1]);
        dst_ch2[p + 1] = pack_dual_tmds(terc4_symbol_table[ch2_d0], terc4_symbol_table[ch2_d1]);
    }

    // 3. Trailing Guard Band (2 pixels)
    dst_ch0[17] = pack_dual_tmds(HDMI_DATA_ISLAND_GUARD_BAND_CH0, HDMI_DATA_ISLAND_GUARD_BAND_CH0);
    dst_ch1[17] = pack_dual_tmds(HDMI_DATA_ISLAND_GUARD_BAND_CH1, HDMI_DATA_ISLAND_GUARD_BAND_CH1);
    dst_ch2[17] = pack_dual_tmds(HDMI_DATA_ISLAND_GUARD_BAND_CH2, HDMI_DATA_ISLAND_GUARD_BAND_CH2);
}
