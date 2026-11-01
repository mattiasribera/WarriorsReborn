/* Kernel crypto exports (XcSHAInit/XcSHAUpdate/XcSHAFinal/XcHMAC) used to sign saves. */
#ifndef KXC_H
#define KXC_H
#include <stdint.h>

/* SHA-1 context as Cxbx-Reloaded's XcSHA* keep it (SHA1_CTX: state, bit count, block buffer).
 * The title owns the storage: XAPI allocates 0x7c bytes and passes ctx+8 to XcSHAInit
 * (0x346c57 / 0x346c2b), so the context must fit 116 bytes (Microsoft A_SHA_CTX size). */
typedef struct {
    uint32_t state[5];
    uint32_t count[2];      /* message length in bits: [0] low, [1] high */
    uint8_t buffer[64];
} XcShaCtx;
_Static_assert(sizeof(XcShaCtx) <= 116, "SHA context exceeds the 116 bytes XAPI reserves");

void xc_sha_init(XcShaCtx *c);
void xc_sha_update(XcShaCtx *c, const uint8_t *data, uint32_t len);
void xc_sha_final(XcShaCtx *c, uint8_t digest[20]);
/* Xbox XcHMAC: HMAC-SHA1 over data1 || data2. Keys longer than 64 bytes are truncated to 64
 * (not hashed first, unlike RFC 2104), matching the kernel as Cxbx-Reloaded implements it. */
void xc_hmac(const uint8_t *key, uint32_t keylen, const uint8_t *d1, uint32_t len1,
             const uint8_t *d2, uint32_t len2, uint8_t digest[20]);

void kxc_register(void);
#endif
