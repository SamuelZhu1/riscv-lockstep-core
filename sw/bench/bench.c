// Small CPI benchmark: insertion sort, CRC-32 and a linked-list walk.
// Mixes dependent loads, taken and not-taken branches and calls, so the
// cycle count shows what load-use stalls and branch flushes cost.
#include "../common/mmio.h"

#define N 200

static uint32_t data[N];
static struct node { struct node *next; uint32_t v; } nodes[64];

static uint32_t crc32(const uint8_t *p, int n) {
    uint32_t c = 0xFFFFFFFFu;
    for (int i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

int main(void) {
    uint32_t x = 12345;
    for (int i = 0; i < N; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        data[i] = x;
    }
    uint32_t t0 = cycles();
    for (int i = 1; i < N; ++i) {
        uint32_t v = data[i];
        int j = i - 1;
        while (j >= 0 && data[j] > v) { data[j + 1] = data[j]; --j; }
        data[j + 1] = v;
    }
    for (int i = 1; i < N; ++i)
        if (data[i - 1] > data[i]) return 1;
    uint32_t crc = crc32((const uint8_t *)data, sizeof data);
    for (int i = 0; i < 64; ++i) { nodes[i].next = &nodes[(i * 37 + 11) & 63]; nodes[i].v = i; }
    uint32_t sum = 0;
    struct node *n = &nodes[0];
    for (int i = 0; i < 2000; ++i) { sum += n->v; n = n->next; }
    uint32_t t = cycles() - t0;
    puts_("bench: cycles ");
    put_dec((int32_t)t);
    puts_(" crc ");
    put_hex(crc);
    puts_(" sum ");
    put_dec((int32_t)sum);
    putc_('\n');
    return 0;
}
