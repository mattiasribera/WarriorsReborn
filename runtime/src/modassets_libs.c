/* Implementation unit of the vendored cgltf 1.14 (MIT, runtime/third_party/cgltf) and stb_vorbis
 * 1.22 (public domain / MIT, runtime/third_party/stb), used by src/modassets_own.c for a mod's own
 * .glb meshes and .ogg sounds. Third-party code is built without warnings so the runtime builds
 * stay at zero warnings. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#define CGLTF_IMPLEMENTATION
#include "../third_party/cgltf/cgltf.h"
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#include "../third_party/stb/stb_vorbis.c"
#pragma clang diagnostic pop
