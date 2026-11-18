// KTX2 (Basis Universal) pack files to BC blocks: see ktx2.h.
#include "ktx2.h"
#include "../third_party/basisu/transcoder/basisu_transcoder.h"
#include <cstdlib>
#include <cstring>

extern "C" void ktx2_init(void) {
    static const bool done = (basist::basisu_transcoder_init(), true);   // a C++11 static: initialized once, thread-safe
    (void)done;
}

extern "C" const char *ktx2_transcode(const uint8_t *buf, size_t n, TexpackImage *img) {
    ktx2_init();
    if (n < 12 || n > 0x7fffffffu) return "not a KTX2 file";
    basist::ktx2_transcoder tr;
    if (!tr.init(buf, (uint32_t)n)) return "not a KTX2 file (or a damaged one)";
    if (tr.get_faces() != 1 || tr.get_layers() > 1 || tr.get_header().m_pixel_depth > 1) return "KTX2 is not a plain 2D texture";
    if (tr.is_video()) return "KTX2 video";
    uint32_t format;
    basist::transcoder_texture_format target;
    uint32_t flags = 0;
    if (tr.is_etc1s()) {
        format = tr.get_has_alpha() ? TEXPACK_BC3 : TEXPACK_BC1;
        target = tr.get_has_alpha() ? basist::transcoder_texture_format::cTFBC3_RGBA : basist::transcoder_texture_format::cTFBC1_RGB;
        flags = basist::cDecodeFlagsBC1ForbidThreeColorBlocks;
    } else if (tr.is_uastc() || tr.is_xubc7()) {
        format = TEXPACK_BC7;
        target = basist::transcoder_texture_format::cTFBC7_RGBA;
        flags = basist::cDecodeFlagsHighQuality;
    } else {
        return "KTX2 format not supported (only ETC1S, UASTC LDR 4x4 or XUBC7)";
    }
    const uint32_t w = tr.get_width(), h = tr.get_height(), levels = tr.get_levels();
    if (!w || !h || w > 16384 || h > 16384) return "KTX2 size out of range";
    if (levels != texpack_mip_count(w, h)) return "KTX2 without the full mip chain";
    size_t total = 0;
    for (uint32_t l = 0; l < levels; l++) total += texpack_level_bytes(format, w >> l ? w >> l : 1, h >> l ? h >> l : 1);
    if (!tr.start_transcoding()) return "KTX2 data damaged (codebooks)";
    uint8_t *out = static_cast<uint8_t *>(std::malloc(total));
    if (!out) return "out of memory";
    basist::ktx2_transcoder_state state;
    state.clear();   // no constructor: clear() sets "no level decompressed yet" (else stack garbage can claim level 0 is)
    size_t at = 0;
    for (uint32_t l = 0; l < levels; l++) {
        const uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
        basist::ktx2_image_level_info info;
        if (!tr.get_image_level_info(info, l, 0, 0) || info.m_orig_width != lw || info.m_orig_height != lh) {
            std::free(out);
            return "KTX2 level sizes do not halve";
        }
        const size_t bytes = texpack_level_bytes(format, lw, lh);
        const uint32_t blocks = (uint32_t)(bytes / (format == TEXPACK_BC1 ? 8 : 16));
        if (info.m_num_blocks_x * info.m_num_blocks_y != blocks ||
            !tr.transcode_image_level(l, 0, 0, out + at, blocks, target, flags, 0, 0, -1, -1, &state)) {
            std::free(out);
            return "KTX2 data damaged (a level failed to transcode)";
        }
        at += bytes;
    }
    *img = TexpackImage{format, w, h, levels, out};
    return nullptr;
}
