// Memory map of the SoC (see rtl/soc.sv).
#ifndef MMIO_H
#define MMIO_H
#include <stdint.h>

#define REG32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

#define MMIO_TEST_STATUS 0x00020000u
#define MMIO_LEDS        0x00020004u
#define MMIO_CYCLE       0x00020008u
#define MMIO_CONSOLE     0x0002000Cu
#define MMIO_FAULT       0x00020010u   /* testbench only: I2C fault injection */
#define MMIO_I2C_BASE    0x00020100u

static inline uint32_t cycles(void) { return REG32(MMIO_CYCLE); }

static inline void delay_cycles(uint32_t n) {
    uint32_t t0 = cycles();
    while (cycles() - t0 < n) {
    }
}

static inline void putc_(char c) { REG32(MMIO_CONSOLE) = (uint8_t)c; }
static inline void puts_(const char *s) {
    while (*s) putc_(*s++);
}
void put_dec(int32_t v);
void put_hex(uint32_t v);

#endif
