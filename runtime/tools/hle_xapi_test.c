/* Game-independent test of the XAPI timing HLE (src/hle_xapi.c): maps the XAPI range of the
 * XBE, plants the entry bytes of QueryPerformanceCounter, installs the patch and calls it with
 * the guest calling convention (stdcall, one LARGE_INTEGER pointer). The counter must tick at
 * the Xbox CPU clock that XAPI's QueryPerformanceFrequency reports (733333333 Hz), not at the
 * host's time-stamp counter rate. */
#include "../src/hle_xapi.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

__asm__(".text\n.globl tcall\ntcall:\n"
        "push %ebp\npush %ebx\npush %esi\npush %edi\nmov %esp,%ebp\n"
        "mov 20(%ebp),%eax\nmov 24(%ebp),%ecx\nmov 28(%ebp),%esi\n"
        "1: test %ecx,%ecx\njz 2f\ndec %ecx\npushl (%esi,%ecx,4)\njmp 1b\n"
        "2: call *%eax\nmov %esp,%ecx\nsub %ebp,%ecx\nmov 32(%ebp),%edx\nmov %ecx,(%edx)\n"
        "mov %ebp,%esp\npop %edi\npop %esi\npop %ebx\npop %ebp\nret\n");
uint32_t tcall(uint32_t fn, int nargs, const uint32_t *args, int32_t *esp_delta);

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)
#define P(x) ((uint32_t)(uintptr_t)(x))

/* XAPILIB QueryPerformanceCounter as linked into the title: mov ecx,[esp+4]; rdtsc; ... */
static const uint8_t qpc_bytes[] = {0x8b, 0x4c, 0x24, 0x04, 0x0f, 0x31, 0x89, 0x01, 0x89, 0x51, 0x04,
                                    0x33, 0xc0, 0x40, 0xc2, 0x04, 0x00};

static uint64_t qpc(int32_t *delta, uint32_t *ret) {
    uint64_t v = 0;
    uint32_t a = P(&v);
    *ret = tcall(XAPI_QPC_ADDR, 1, &a, delta);
    return v;
}

static void sleep_ms(int ms) {
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

int main(void) {
    /* Pure conversion: host monotonic time -> Xbox TSC ticks. */
    CHECK(xapi_tsc_from_time(1, 0) == XBOX_TSC_FREQUENCY, "1 s = %llu ticks", (unsigned long long)xapi_tsc_from_time(1, 0));
    CHECK(xapi_tsc_from_time(0, 1000000) == 733333, "1 ms = %llu ticks", (unsigned long long)xapi_tsc_from_time(0, 1000000));
    CHECK(xapi_tsc_from_time(3600, 500000000) == 3600ull * 733333333 + 366666666,
          "1 h 0.5 s = %llu ticks (no 64-bit overflow)", (unsigned long long)xapi_tsc_from_time(3600, 500000000));

    const uint32_t lo = XAPI_QPC_ADDR & ~0xfffu, len = 0x2000;
    uint8_t *m = mmap((void *)(uintptr_t)lo, len, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (m != (void *)(uintptr_t)lo) { perror("mmap"); return 1; }
    memset(m, 0xCC, len);
    uint8_t *entry = (uint8_t *)(uintptr_t)XAPI_QPC_ADDR;

    /* A mismatching entry is left alone. */
    memcpy(entry, qpc_bytes, sizeof qpc_bytes);
    entry[4] = 0x90;
    CHECK(hle_xapi_install(NULL) == 0 && entry[0] == 0x8b, "unexpected entry bytes: not patched");

    memcpy(entry, qpc_bytes, sizeof qpc_bytes);
    CHECK(hle_xapi_install(NULL) == 1 && entry[0] == 0xE9, "QueryPerformanceCounter patched");
    int32_t delta = 1;
    uint32_t ret = 0;
    uint64_t t0 = qpc(&delta, &ret);
    CHECK(ret == 1 && delta == 0, "returns TRUE and pops its argument (ret %u, esp delta %d)", ret, delta);
    struct timespec h0, h1;
    clock_gettime(CLOCK_MONOTONIC, &h0);
    t0 = qpc(&delta, &ret);
    sleep_ms(200);
    uint64_t t1 = qpc(&delta, &ret);
    clock_gettime(CLOCK_MONOTONIC, &h1);
    double host_s = (h1.tv_sec - h0.tv_sec) + (h1.tv_nsec - h0.tv_nsec) / 1e9;
    double rate = (double)(t1 - t0) / host_s;
    CHECK(t1 > t0 && rate > 0.97 * XBOX_TSC_FREQUENCY && rate <= 1.001 * XBOX_TSC_FREQUENCY,
          "counter ticks at the Xbox CPU clock (%.0f Hz, expected %u)", rate, XBOX_TSC_FREQUENCY);
    /* The title's millisecond timer divides by 733333 (0x6ca90): 200 ms of sleep reads ~200 ms. */
    uint64_t ms = (t1 - t0) / 733333;
    CHECK(ms >= 200 && ms < 260, "title millisecond timer over a 200 ms sleep reads %llu ms", (unsigned long long)ms);

    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("ALL OK\n");
    return 0;
}
