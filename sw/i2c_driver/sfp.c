// SFF-8472 decoding on top of sfp_i2c: identity (A0h) with checksum checks
// and live diagnostics (A2h) converted without floating point.
#include "sfp.h"

#include "sfp_i2c.h"

#define A0 0x50
#define A2 0x51

static void copy_str(char *dst, const uint8_t *src, int len) {
    int end = len;
    while (end > 0 && (src[end - 1] == ' ' || src[end - 1] == 0)) --end;  // strip padding
    for (int i = 0; i < end; ++i) dst[i] = (char)src[i];
    dst[end] = 0;
}

static uint8_t sum8(const uint8_t *p, int n) {
    uint8_t s = 0;
    for (int i = 0; i < n; ++i) s = (uint8_t)(s + p[i]);
    return s;
}

sfp_err_t sfp_identify(sfp_identity_t *id) {
    uint8_t a0[96];
    sfp_err_t err = SFP_ERR_CHECKSUM;
    // a checksum failure usually means a corrupted transfer: read again
    for (int attempt = 0; attempt < 3; ++attempt) {
        err = sfp_i2c_read(A0, 0, a0, sizeof a0);
        if (err != SFP_OK) return err;
        // A read from a slave holding SDA low comes back as all zeros, and
        // all zeros passes both checksums (0 == 0), so also require the
        // identifier byte to say SFP/SFP+ (found by the stuck-bus test).
        if (a0[0] == 0x03 && sum8(a0, 63) == a0[63] && sum8(a0 + 64, 31) == a0[95]) {
            err = SFP_OK;
            break;
        }
        ((sfp_i2c_stats_t *)sfp_i2c_stats())->checksum_fails++;
        err = SFP_ERR_CHECKSUM;
    }
    if (err != SFP_OK) return err;
    id->identifier = a0[0];
    copy_str(id->vendor, a0 + 20, 16);
    copy_str(id->part, a0 + 40, 16);
    copy_str(id->serial, a0 + 68, 16);
    id->wavelength_nm = (uint16_t)((a0[60] << 8) | a0[61]);
    id->bitrate_100mbps = a0[12];
    id->ddm = (a0[92] >> 6) & 1;
    id->ext_cal = (a0[92] >> 4) & 1;
    return SFP_OK;
}

// log2(x) in 1/1024 units for x >= 1, from the position of the top bit plus
// a 16-entry table for the fraction (linear in between). Good to about
// 0.02 dB over the 0.1 uW to 6.5 mW range a module reports.
static int32_t log2_q10(uint32_t x) {
    // frac[i] = round(1024 * log2(1 + i/16))
    static const uint16_t frac[17] = {0,   90,  174, 254, 330, 402, 470, 536, 599,
                                      659, 717, 773, 827, 879, 929, 977, 1024};
    int msb = 31;
    while (msb > 0 && !(x & (1u << msb))) --msb;
    // 8 bits below the top bit: index (4 bits) + interpolation (4 bits)
    uint32_t m = msb >= 8 ? (x >> (msb - 8)) & 0xFF : (x << (8 - msb)) & 0xFF;
    uint32_t idx = m >> 4, t = m & 0xF;
    uint32_t hi = frac[idx + 1];
    int32_t f = (int32_t)(frac[idx] + (((hi - frac[idx]) * t) >> 4));
    return msb * 1024 + f;
}

int16_t sfp_power_to_cdbm(uint16_t power_01uw) {
    if (power_01uw == 0) return SFP_NO_LIGHT;
    // dBm = 10 log10(P / 1 mW), P in 0.1 uW units: 10 log10(x) - 40
    // 10 log10(x) = 10 log2(x) * 0.30103; in hundredths of dB:
    //   cdbm = log2_q10(x) * 1000 * 0.30103 / 1024 - 4000 = log2_q10(x) * 301.03 / 1024 - 4000
    int32_t l = log2_q10(power_01uw);
    int32_t cdbm = (l * 30103 + 51200) / 102400 - 4000;
    return (int16_t)cdbm;
}

sfp_err_t sfp_read_ddm(sfp_ddm_t *d) {
    uint8_t b[10];
    // one 10-byte read: the module updates these registers itself, so two
    // separate reads could pair an old high byte with a new low byte
    sfp_err_t err = sfp_i2c_read(A2, 96, b, sizeof b);
    if (err != SFP_OK) return err;
    d->temp_c256 = (int16_t)((b[0] << 8) | b[1]);
    d->vcc_100uv = (uint16_t)((b[2] << 8) | b[3]);
    d->bias_2ua = (uint16_t)((b[4] << 8) | b[5]);
    d->tx_01uw = (uint16_t)((b[6] << 8) | b[7]);
    d->rx_01uw = (uint16_t)((b[8] << 8) | b[9]);
    d->tx_cdbm = sfp_power_to_cdbm(d->tx_01uw);
    d->rx_cdbm = sfp_power_to_cdbm(d->rx_01uw);
    return SFP_OK;
}
