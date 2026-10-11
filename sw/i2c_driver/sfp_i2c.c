// sfp_i2c backend for the RISC-V core's memory-mapped I2C controller
// (rtl/i2c_master.sv).
#include "sfp_i2c.h"

#include "../common/mmio.h"

#define I2C_PRESCALE (MMIO_I2C_BASE + 0x00)
#define I2C_CMD      (MMIO_I2C_BASE + 0x04)
#define I2C_TXDATA   (MMIO_I2C_BASE + 0x08)
#define I2C_RXDATA   (MMIO_I2C_BASE + 0x0C)
#define I2C_STATUS   (MMIO_I2C_BASE + 0x10)
#define I2C_BITBANG  (MMIO_I2C_BASE + 0x14)

#define CMD_START (1u << 0)
#define CMD_STOP  (1u << 1)
#define CMD_WRITE (1u << 2)
#define CMD_READ  (1u << 3)
#define CMD_NACK  (1u << 4)

#define ST_BUSY   (1u << 0)
#define ST_RXNACK (1u << 1)
#define ST_SDA    (1u << 2)
#define ST_SCL    (1u << 3)

#define BB_EN     (1u << 0)
#define BB_SCL_LO (1u << 1)
#define BB_SDA_LO (1u << 2)

#define MAX_ATTEMPTS 4  // first try plus 3 retries

static sfp_i2c_stats_t stats;
static sfp_i2c_config_t config = {125, 50000};

static void wait_idle(void) {
    while (REG32(I2C_STATUS) & ST_BUSY) {
    }
}

static void cmd(uint32_t c) {
    REG32(I2C_CMD) = c;
    wait_idle();
}

// returns 1 if the slave acknowledged
static int write_byte(uint8_t b, uint32_t extra) {
    REG32(I2C_TXDATA) = b;
    cmd(CMD_WRITE | extra);
    return (REG32(I2C_STATUS) & ST_RXNACK) == 0;
}

static uint8_t read_byte(int last) {
    cmd(CMD_READ | (last ? (CMD_NACK | CMD_STOP) : 0));
    return (uint8_t)REG32(I2C_RXDATA);
}

void sfp_i2c_init(const sfp_i2c_config_t *cfg) {
    if (cfg) config = *cfg;
    REG32(I2C_BITBANG) = 0;
    REG32(I2C_PRESCALE) = config.prescale;
}

// half an SCL period in bit-bang mode
static void bb_wait(void) { delay_cycles(2u * config.prescale); }

sfp_err_t sfp_i2c_bus_recover(void) {
    ++stats.bus_recoveries;
    // SCL low, SDA released, then clock until the slave lets go of SDA
    REG32(I2C_BITBANG) = BB_EN | BB_SCL_LO;
    bb_wait();
    for (int i = 0; i < 9 && !(REG32(I2C_STATUS) & ST_SDA); ++i) {
        REG32(I2C_BITBANG) = BB_EN;              // SCL high
        bb_wait();
        REG32(I2C_BITBANG) = BB_EN | BB_SCL_LO;  // SCL low
        bb_wait();
    }
    // STOP: SDA low while SCL low, release SCL, then release SDA
    REG32(I2C_BITBANG) = BB_EN | BB_SCL_LO | BB_SDA_LO;
    bb_wait();
    REG32(I2C_BITBANG) = BB_EN | BB_SDA_LO;
    bb_wait();
    REG32(I2C_BITBANG) = BB_EN;
    bb_wait();
    REG32(I2C_BITBANG) = 0;
    bb_wait();
    uint32_t st = REG32(I2C_STATUS);
    return ((st & ST_SDA) && (st & ST_SCL)) ? SFP_OK : SFP_ERR_BUS;
}

// one attempt at the whole transaction: START, addr+W, offset,
// repeated START, addr+R, n bytes, STOP
static sfp_err_t read_once(uint8_t dev, uint8_t offset, uint8_t *buf, size_t n) {
    // a slave left mid-byte holds SDA low; START would fail silently
    if (!(REG32(I2C_STATUS) & ST_SDA)) {
        if (sfp_i2c_bus_recover() != SFP_OK) return SFP_ERR_BUS;
    }
    if (!write_byte((uint8_t)(dev << 1), CMD_START)) goto nack;
    if (!write_byte(offset, 0)) goto nack;
    if (!write_byte((uint8_t)((dev << 1) | 1), CMD_START)) goto nack;
    for (size_t i = 0; i < n; ++i) buf[i] = read_byte(i + 1 == n);
    return SFP_OK;
nack:
    ++stats.nacks;
    cmd(CMD_STOP);
    return SFP_ERR_NACK;
}

sfp_err_t sfp_i2c_read(uint8_t dev, uint8_t offset, uint8_t *buf, size_t n) {
    if (!buf || n == 0 || (size_t)offset + n > 256) return SFP_ERR_ARG;
    sfp_err_t err = SFP_ERR_NACK;
    for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
        if (attempt) {
            ++stats.retries;
            delay_cycles(config.backoff_unit << attempt);  // 2, 4, 8 units
        }
        ++stats.transfers;
        err = read_once(dev, offset, buf, n);
        if (err == SFP_OK) return SFP_OK;
    }
    return err;
}

const sfp_i2c_stats_t *sfp_i2c_stats(void) { return &stats; }
