// Runs the SFP+ I2C driver bare-metal on the core against the testbench's
// SFF-8472 module model, then injects bus faults through the FAULT register
// and checks every one is recovered without a reset.
#include "../common/mmio.h"
#include "sfp.h"

#ifndef PRESCALE
#define PRESCALE 8          // fast SCL for simulation; the board build uses 125 (100 kHz at 50 MHz)
#endif
#ifndef N_FAULTS
#define N_FAULTS 200
#endif

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) ++a, ++b;
    return *a == *b;
}

static int failures;
static void check(int ok, const char *what) {
    if (!ok) {
        ++failures;
        puts_("FAIL ");
        puts_(what);
        putc_('\n');
    }
}

static int within(int32_t v, int32_t want, int32_t tol) { return v >= want - tol && v <= want + tol; }

static uint32_t lfsr = 0xACE1u;
static uint32_t rand16(void) {
    lfsr ^= lfsr << 13;
    lfsr ^= lfsr >> 17;
    lfsr ^= lfsr << 5;
    return lfsr & 0xFFFF;
}

int main(void) {
    sfp_i2c_config_t cfg = {PRESCALE, 400};
    sfp_i2c_init(&cfg);

    // 1. identity
    sfp_identity_t id;
    uint32_t t0 = cycles();
    sfp_err_t e = sfp_identify(&id);
    uint32_t t_id = cycles() - t0;
    check(e == SFP_OK, "identify");
    check(id.identifier == 0x03, "identifier");
    check(streq(id.vendor, "LAB OPTICS"), "vendor");
    check(streq(id.part, "SFP-10G-LR"), "part number");
    check(streq(id.serial, "LO2611A0042"), "serial");
    check(id.wavelength_nm == 1310, "wavelength");
    check(id.bitrate_100mbps == 0x67, "bitrate");
    check(id.ddm == 1 && id.ext_cal == 0, "ddm flags");
    puts_("module: ");
    puts_(id.vendor);
    putc_(' ');
    puts_(id.part);
    puts_(" sn ");
    puts_(id.serial);
    puts_(", ");
    put_dec(id.wavelength_nm);
    puts_(" nm, identify took ");
    put_dec((int32_t)t_id);
    puts_(" cycles\n");

    // 2. diagnostics
    sfp_ddm_t d;
    e = sfp_read_ddm(&d);
    check(e == SFP_OK, "read ddm");
    check(d.temp_c256 == 0x2380, "temperature");
    check(d.vcc_100uv == 33000, "vcc");
    check(d.bias_2ua == 3000, "bias");
    check(within(d.tx_cdbm, -301, 2), "tx power dBm");
    check(within(d.rx_cdbm, -699, 2), "rx power dBm");
    puts_("ddm: tx ");
    put_dec(d.tx_cdbm);
    puts_(" cdBm, rx ");
    put_dec(d.rx_cdbm);
    puts_(" cdBm\n");

    // 3. fixed-point dBm against reference values (hundredths of dB)
    static const struct { uint16_t p; int16_t cdbm; } ref[] = {
        {1, -4000}, {10, -3000}, {100, -2000}, {1000, -1000}, {10000, 0},
        {5000, -301}, {2000, -699}, {3162, -500}, {65535, 816}, {123, -1910},
    };
    int32_t worst = 0;
    for (unsigned i = 0; i < sizeof ref / sizeof ref[0]; ++i) {
        int32_t err = sfp_power_to_cdbm(ref[i].p) - ref[i].cdbm;
        if (err < 0) err = -err;
        if (err > worst) worst = err;
    }
    check(worst <= 3, "dBm conversion accuracy");
    puts_("dBm conversion worst error: ");
    put_dec(worst);
    puts_(" hundredths of a dB\n");
    check(sfp_power_to_cdbm(0) == SFP_NO_LIGHT, "no light");

    // 4. fault injection: NACK bursts and a slave holding SDA low mid-byte
    const sfp_i2c_stats_t *st = sfp_i2c_stats();
    uint32_t nack_before = st->nacks, rec_before = st->bus_recoveries;
    int recovered = 0, nack_faults = 0, stuck_faults = 0;
    for (int i = 0; i < N_FAULTS; ++i) {
        if (i & 1) {
            REG32(MMIO_FAULT) = 0x100;               // stuck SDA on the next byte read
            ++stuck_faults;
        } else {
            REG32(MMIO_FAULT) = 1 + rand16() % 3;    // NACK the next 1 to 3 address bytes
            ++nack_faults;
        }
        sfp_identity_t again;
        if (sfp_identify(&again) == SFP_OK && streq(again.serial, id.serial)) ++recovered;
    }
    check(recovered == N_FAULTS, "every injected fault recovered");
    puts_("faults recovered: ");
    put_dec(recovered);
    putc_('/');
    put_dec(N_FAULTS);
    puts_(" (");
    put_dec(nack_faults);
    puts_(" NACK bursts, ");
    put_dec(stuck_faults);
    puts_(" stuck-bus), nacks seen ");
    put_dec((int32_t)(st->nacks - nack_before));
    puts_(", bus recoveries ");
    put_dec((int32_t)(st->bus_recoveries - rec_before));
    puts_(", checksum retries ");
    put_dec((int32_t)st->checksum_fails);
    putc_('\n');

    return failures ? 10 + failures : 0;
}
