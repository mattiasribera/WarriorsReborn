/* Implementation unit of the vendored stb_image (PNG only) and stb_image_write (public domain /
 * MIT, runtime/third_party/stb, see README there), used by src/texpack.c. Third-party code is
 * built without warnings so the runtime builds stay at zero warnings. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "../third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb/stb_image_write.h"
#pragma clang diagnostic pop
