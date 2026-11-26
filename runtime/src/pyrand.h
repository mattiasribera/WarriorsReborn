/* A random number generator that gives exactly the numbers of Python's random.Random(seed) (CPython
 * _randommodule.c: MT19937 seeded by init_by_array with the 32-bit words of |seed|): random(),
 * uniform(), getrandbits(k <= 32), randrange() and randint(). The mod asset engine (src/modassets.h)
 * uses it so its procedural textures match the Python reference recipes byte for byte. */
#ifndef PYRAND_H
#define PYRAND_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t mt[624];
    int i;
} PyRand;

void pyrand_seed(PyRand *r, int64_t seed);
uint32_t pyrand_u32(PyRand *r);
double pyrand_random(PyRand *r);                     /* [0, 1), 53 bits */
double pyrand_uniform(PyRand *r, double a, double b);  /* a + (b - a) * random() */
uint32_t pyrand_getrandbits(PyRand *r, int k);       /* 1 <= k <= 32 */
/* randrange(start, stop[, step]) with step > 0 and start < stop; randint(a, b) = randrange(a, b + 1). */
int64_t pyrand_randrange(PyRand *r, int64_t start, int64_t stop, int64_t step);
int64_t pyrand_randint(PyRand *r, int64_t a, int64_t b);

#ifdef __cplusplus
}
#endif
#endif
