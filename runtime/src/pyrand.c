/* MT19937 as CPython's random module uses it (reference: Matsumoto and Nishimura's mt19937ar.c). */
#include "pyrand.h"

static void init_genrand(PyRand *r, uint32_t s) {
    r->mt[0] = s;
    for (int i = 1; i < 624; i++) r->mt[i] = 1812433253u * (r->mt[i - 1] ^ (r->mt[i - 1] >> 30)) + (uint32_t)i;
    r->i = 624;
}

void pyrand_seed(PyRand *r, int64_t seed) {
    uint64_t v = seed < 0 ? (uint64_t)0 - (uint64_t)seed : (uint64_t)seed;
    uint32_t key[2] = {(uint32_t)v, (uint32_t)(v >> 32)};
    int len = key[1] ? 2 : 1;
    init_genrand(r, 19650218u);
    int i = 1, j = 0;
    for (int k = 624 > len ? 624 : len; k; k--) {
        r->mt[i] = (r->mt[i] ^ ((r->mt[i - 1] ^ (r->mt[i - 1] >> 30)) * 1664525u)) + key[j] + (uint32_t)j;
        i++, j++;
        if (i >= 624) r->mt[0] = r->mt[623], i = 1;
        if (j >= len) j = 0;
    }
    for (int k = 623; k; k--) {
        r->mt[i] = (r->mt[i] ^ ((r->mt[i - 1] ^ (r->mt[i - 1] >> 30)) * 1566083941u)) - (uint32_t)i;
        i++;
        if (i >= 624) r->mt[0] = r->mt[623], i = 1;
    }
    r->mt[0] = 0x80000000u;
    r->i = 624;
}

uint32_t pyrand_u32(PyRand *r) {
    static const uint32_t mag01[2] = {0, 0x9908b0dfu};
    if (r->i >= 624) {
        int k;
        uint32_t y;
        for (k = 0; k < 624 - 397; k++) {
            y = (r->mt[k] & 0x80000000u) | (r->mt[k + 1] & 0x7fffffffu);
            r->mt[k] = r->mt[k + 397] ^ (y >> 1) ^ mag01[y & 1];
        }
        for (; k < 623; k++) {
            y = (r->mt[k] & 0x80000000u) | (r->mt[k + 1] & 0x7fffffffu);
            r->mt[k] = r->mt[k + (397 - 624)] ^ (y >> 1) ^ mag01[y & 1];
        }
        y = (r->mt[623] & 0x80000000u) | (r->mt[0] & 0x7fffffffu);
        r->mt[623] = r->mt[396] ^ (y >> 1) ^ mag01[y & 1];
        r->i = 0;
    }
    uint32_t y = r->mt[r->i++];
    y ^= y >> 11;
    y ^= (y << 7) & 0x9d2c5680u;
    y ^= (y << 15) & 0xefc60000u;
    y ^= y >> 18;
    return y;
}

double pyrand_random(PyRand *r) {
    uint32_t a = pyrand_u32(r) >> 5, b = pyrand_u32(r) >> 6;
    return (a * 67108864.0 + b) * (1.0 / 9007199254740992.0);
}

double pyrand_uniform(PyRand *r, double a, double b) { return a + (b - a) * pyrand_random(r); }

uint32_t pyrand_getrandbits(PyRand *r, int k) { return pyrand_u32(r) >> (32 - k); }

/* _randbelow_with_getrandbits(n), n >= 1 (n < 2^32 here). */
static uint64_t below(PyRand *r, uint64_t n) {
    int k = 0;
    while (k < 32 && (n >> k)) k++;
    uint64_t v = pyrand_getrandbits(r, k);
    while (v >= n) v = pyrand_getrandbits(r, k);
    return v;
}

int64_t pyrand_randrange(PyRand *r, int64_t start, int64_t stop, int64_t step) {
    if (step <= 0 || stop <= start) return start;
    uint64_t n = (uint64_t)(stop - start + step - 1) / (uint64_t)step;
    return start + step * (int64_t)below(r, n);
}

int64_t pyrand_randint(PyRand *r, int64_t a, int64_t b) { return pyrand_randrange(r, a, b + 1, 1); }
