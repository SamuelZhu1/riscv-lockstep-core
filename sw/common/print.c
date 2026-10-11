#include "mmio.h"

void put_dec(int32_t v) {
    char buf[12];
    int n = 0;
    uint32_t u = v < 0 ? (uint32_t)(-v) : (uint32_t)v;
    if (v < 0) putc_('-');
    do {
        buf[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    while (n) putc_(buf[--n]);
}

void put_hex(uint32_t v) {
    puts_("0x");
    for (int s = 28; s >= 0; s -= 4) putc_("0123456789abcdef"[(v >> s) & 0xF]);
}
