// Subset of the SFP+ SDK (sfp.h in the ESP32-C3 firmware): identity and
// diagnostics, enough to run the same driver on the RISC-V core.
#ifndef SFP_H
#define SFP_H
#include <stdint.h>

#include "sfp_i2c.h"

#define SFP_NO_LIGHT INT16_MIN

typedef struct {
    uint8_t identifier;          // 0x03 = SFP / SFP+
    char vendor[17], part[17], serial[17];
    uint16_t wavelength_nm;
    uint8_t bitrate_100mbps;
    uint8_t ddm, ext_cal;
} sfp_identity_t;

typedef struct {
    int16_t temp_c256;           // 1/256 degC
    uint16_t vcc_100uv;
    uint16_t bias_2ua;
    uint16_t tx_01uw, rx_01uw;   // raw power, 0.1 uW units
    int16_t tx_cdbm, rx_cdbm;    // hundredths of a dBm, or SFP_NO_LIGHT
} sfp_ddm_t;

sfp_err_t sfp_identify(sfp_identity_t *id);
sfp_err_t sfp_read_ddm(sfp_ddm_t *d);
int16_t sfp_power_to_cdbm(uint16_t power_01uw);

#endif
