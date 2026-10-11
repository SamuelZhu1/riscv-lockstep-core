// I2C transport for SFP+ modules: byte transfers with retries, backoff and
// bus recovery. The same API sits under the ESP32-C3 build of the SFP+
// firmware; only this file's .c changes between the two (the MMIO
// controller here, the ESP-IDF I2C master there).
#ifndef SFP_I2C_H
#define SFP_I2C_H
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SFP_OK = 0,
    SFP_ERR_NACK = -1,      // module did not acknowledge after all retries
    SFP_ERR_BUS = -2,       // bus stuck and recovery failed
    SFP_ERR_CHECKSUM = -3,  // data read but checksum wrong after all retries
    SFP_ERR_ARG = -4,
} sfp_err_t;

typedef struct {
    uint32_t transfers;       // read transactions attempted
    uint32_t nacks;           // address or data bytes not acknowledged
    uint32_t retries;         // transactions repeated after an error
    uint32_t bus_recoveries;  // times SDA was found held low and clocked free
    uint32_t checksum_fails;  // identity reads that failed CC_BASE / CC_EXT
} sfp_i2c_stats_t;

typedef struct {
    uint16_t prescale;        // controller clocks per quarter SCL period
    uint32_t backoff_unit;    // cycles per backoff step (retries wait 2, 4, 8 units)
} sfp_i2c_config_t;

void sfp_i2c_init(const sfp_i2c_config_t *cfg);

// Reads n bytes starting at offset from device dev (0x50 = A0h, 0x51 = A2h)
// in one transaction, so multi-byte values are never torn.
sfp_err_t sfp_i2c_read(uint8_t dev, uint8_t offset, uint8_t *buf, size_t n);

// Clocks SCL up to 9 times until a slave stuck mid-byte releases SDA, then
// sends a STOP. Returns SFP_OK if the bus is idle afterwards.
sfp_err_t sfp_i2c_bus_recover(void);

const sfp_i2c_stats_t *sfp_i2c_stats(void);

#endif
