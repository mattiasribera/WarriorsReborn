/* Kernel crypto exports: XcSHAInit (335), XcSHAUpdate (336), XcSHAFinal (337), XcHMAC (340).
 * Signatures: nxdk lib/xboxkrnl/xboxkrnl.h; behavior: Cxbx-Reloaded EmuKrnlXc.cpp. */
#include "kxc.h"
#include <string.h>

#ifndef KXC_TEST
#include "kernel.h"
#endif

/* SHA-1 per RFC 3174 / FIPS 180-1. */
#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha_block(uint32_t st[5], const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e;
}

void xc_sha_init(XcShaCtx *c) {
    memset(c, 0, sizeof *c);
    c->state[0] = 0x67452301; c->state[1] = 0xEFCDAB89; c->state[2] = 0x98BADCFE;
    c->state[3] = 0x10325476; c->state[4] = 0xC3D2E1F0;
}

void xc_sha_update(XcShaCtx *c, const uint8_t *data, uint32_t len) {
    uint32_t used = (c->count[0] >> 3) & 63;
    uint32_t lo = c->count[0] + (len << 3);
    c->count[1] += (len >> 29) + (lo < c->count[0]);
    c->count[0] = lo;
    while (len) {
        uint32_t n = 64 - used < len ? 64 - used : len;
        memcpy(c->buffer + used, data, n);
        used += n; data += n; len -= n;
        if (used == 64) { sha_block(c->state, c->buffer); used = 0; }
    }
}

void xc_sha_final(XcShaCtx *c, uint8_t digest[20]) {
    uint8_t len[8];
    for (int i = 0; i < 4; i++) { len[i] = (uint8_t)(c->count[1] >> (24 - 8 * i)); len[4 + i] = (uint8_t)(c->count[0] >> (24 - 8 * i)); }
    static const uint8_t pad[64] = {0x80};
    uint32_t used = (c->count[0] >> 3) & 63;
    xc_sha_update(c, pad, used < 56 ? 56 - used : 120 - used);
    xc_sha_update(c, len, 8);
    for (int i = 0; i < 20; i++) digest[i] = (uint8_t)(c->state[i / 4] >> (24 - 8 * (i % 4)));
}

void xc_hmac(const uint8_t *key, uint32_t keylen, const uint8_t *d1, uint32_t len1,
             const uint8_t *d2, uint32_t len2, uint8_t digest[20]) {
    uint8_t ipad[64] = {0}, opad[64], inner[20];
    if (keylen > 64) keylen = 64;
    memcpy(ipad, key, keylen);
    for (int i = 0; i < 64; i++) { opad[i] = ipad[i] ^ 0x5c; ipad[i] ^= 0x36; }
    XcShaCtx c;
    xc_sha_init(&c);
    xc_sha_update(&c, ipad, 64);
    if (len1) xc_sha_update(&c, d1, len1);
    if (len2) xc_sha_update(&c, d2, len2);
    xc_sha_final(&c, inner);
    xc_sha_init(&c);
    xc_sha_update(&c, opad, 64);
    xc_sha_update(&c, inner, 20);
    xc_sha_final(&c, digest);
}

#ifndef KXC_TEST
/* VOID XcSHAInit(PUCHAR pbSHAContext) */
static uint64_t k_XcSHAInit(KFrame *f, const uint32_t *a) { xc_sha_init(G(a[0])); return 0; }
/* VOID XcSHAUpdate(PUCHAR pbSHAContext, PUCHAR pbInput, ULONG dwInputLength) */
static uint64_t k_XcSHAUpdate(KFrame *f, const uint32_t *a) { xc_sha_update(G(a[0]), G(a[1]), a[2]); return 0; }
/* VOID XcSHAFinal(PUCHAR pbSHAContext, PUCHAR pbDigest) */
static uint64_t k_XcSHAFinal(KFrame *f, const uint32_t *a) { xc_sha_final(G(a[0]), G(a[1])); return 0; }
/* VOID XcHMAC(PBYTE pbKeyMaterial, ULONG cbKeyMaterial, PBYTE pbData, ULONG cbData,
 *             PBYTE pbData2, ULONG cbData2, PBYTE HmacData) */
static uint64_t k_XcHMAC(KFrame *f, const uint32_t *a) {
    xc_hmac(G(a[0]), a[1], G(a[2]), a[3], G(a[4]), a[5], G(a[6]));
    return 0;
}

void kxc_register(void) {
    kernel_register(335, k_XcSHAInit);
    kernel_register(336, k_XcSHAUpdate);
    kernel_register(337, k_XcSHAFinal);
    kernel_register(340, k_XcHMAC);
}
#endif

#ifdef KXC_TEST
#include <stdio.h>

static int fails;
static void hex(const uint8_t *d, char *out) { for (int i = 0; i < 20; i++) sprintf(out + 2 * i, "%02x", d[i]); }
static void expect(const char *name, const uint8_t *d, const char *want) {
    char got[41];
    hex(d, got);
    if (strcmp(got, want)) { fails++; printf("FAIL %s: got %s want %s\n", name, got, want); }
    else printf("ok %s\n", name);
}

/* RFC 3174 section 7.3 test vectors (TEST4 is "01234567" x 80). */
static void sha_vector(const char *name, const char *msg, int repeat, const char *want) {
    XcShaCtx c;
    uint8_t d[20];
    xc_sha_init(&c);
    for (int i = 0; i < repeat; i++) xc_sha_update(&c, (const uint8_t *)msg, (uint32_t)strlen(msg));
    xc_sha_final(&c, d);
    expect(name, d, want);
}

static void hmac_vector(const char *name, const uint8_t *key, uint32_t klen, const uint8_t *data, uint32_t dlen,
                        const char *want) {
    uint8_t d[20];
    xc_hmac(key, klen, data, dlen, NULL, 0, d);
    expect(name, d, want);
    /* The same message split across data1 and data2 gives the same MAC. */
    uint32_t half = dlen / 2;
    xc_hmac(key, klen, data, half, data + half, dlen - half, d);
    expect(name, d, want);
}

int main(void) {
    sha_vector("sha1 abc", "abc", 1, "a9993e364706816aba3e25717850c26c9cd0d89d");
    sha_vector("sha1 448-bit", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 1,
               "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    sha_vector("sha1 million a", "a", 1000000, "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    sha_vector("sha1 0123456 x80", "0123456701234567012345670123456701234567012345670123456701234567", 10,
               "dea356a2cddd90c7a7ecedc5ebb563934f460452");
    sha_vector("sha1 empty", "", 1, "da39a3ee5e6b4b0d3255bfef95601890afd80709");

    /* RFC 2202 section 3, test cases 1-5 (cases 6-7 use 80-byte keys, which the Xbox kernel
     * truncates instead of hashing; covered separately below). */
    uint8_t k[80], dd[50];
    memset(k, 0x0b, 20);
    hmac_vector("hmac rfc2202 1", k, 20, (const uint8_t *)"Hi There", 8, "b617318655057264e28bc0b6fb378c8ef146be00");
    hmac_vector("hmac rfc2202 2", (const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28,
                "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
    memset(k, 0xaa, 20);
    memset(dd, 0xdd, 50);
    hmac_vector("hmac rfc2202 3", k, 20, dd, 50, "125d7342b9ac11cd91a39af48aa17b4f63f175d3");
    for (int i = 0; i < 25; i++) k[i] = (uint8_t)(i + 1);
    memset(dd, 0xcd, 50);
    hmac_vector("hmac rfc2202 4", k, 25, dd, 50, "4c9007f4026250c6bc8414f9bf50c86c2d7235da");
    memset(k, 0x0c, 20);
    hmac_vector("hmac rfc2202 5", k, 20, (const uint8_t *)"Test With Truncation", 20,
                "4c1a03424b55e07fe7f27be1d58bb9324a9a5a04");

    /* Xbox key truncation: an 80-byte key MACs like its first 64 bytes. */
    uint8_t a[20], b[20];
    memset(k, 0xaa, 80);
    xc_hmac(k, 80, (const uint8_t *)"msg", 3, NULL, 0, a);
    xc_hmac(k, 64, (const uint8_t *)"msg", 3, NULL, 0, b);
    if (memcmp(a, b, 20)) { fails++; printf("FAIL hmac key truncation\n"); } else printf("ok hmac key truncation\n");

    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("ALL OK\n");
    return 0;
}
#endif
