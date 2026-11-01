/* Non-volatile settings (EEPROM values) the kernel reports through ExQueryNonVolatileSetting:
 * a US NTSC console, its dashboard language (English unless --language picks another; the
 * title's XGetLanguage reads XC_LANGUAGE and loads config_strings_<ext>), and the video flags of the AV settings (16:9 with --widescreen;
 * the title reads them with XGetVideoFlags and switches its cameras and HUD to anamorphic 16:9). */
#ifndef NVSET_H
#define NVSET_H
#include <stdint.h>

/* XC_* value indexes (Xbox kernel). */
#define NV_LANGUAGE 0x007u
#define NV_VIDEO_FLAGS 0x008u           /* XC_VIDEO: flags in bits 16-23 (XGetVideoFlags: >> 16 & 0x5f) */
#define NV_FACTORY_AV_REGION 0x103u
#define NV_FACTORY_GAME_REGION 0x104u
#define NV_VIDEO_WIDESCREEN 0x00010000u /* XC_VIDEO_FLAGS_WIDESCREEN (0x01) in the stored value */

/* The REG_DWORD value of a setting (0 for those not listed above). */
uint32_t nvset_value(uint32_t index);
void nvset_widescreen(int on);

/* --language: the codes the disc has string tables for. */
#define NV_LANGUAGE_CODES "en, es, fr, de, it"
/* Selects the reported XC_LANGUAGE from a code above (1 on success; 0 leaves it unchanged). */
int nvset_language(const char *code);
/* The selected language's English name ("English" by default). */
const char *nvset_language_name(void);
#endif
