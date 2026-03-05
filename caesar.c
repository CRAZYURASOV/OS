#include "caesar.h"
#include <stdint.h>

static uint8_t g_key = 0;

void set_key(char key) {
    g_key = (uint8_t)key;
}

void caesar(void* src, void* dst, int len) {
    if (!src || !dst || len <= 0) return;

    uint8_t* s = (uint8_t*)src;
    uint8_t* d = (uint8_t*)dst;

    for (int i = 0; i < len; ++i) {
        d[i] = (uint8_t)(s[i] ^ g_key);
    }
}