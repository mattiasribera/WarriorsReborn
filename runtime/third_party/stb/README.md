# stb (vendored)

`stb_image.h` v2.30, `stb_image_write.h` v1.16 and `stb_vorbis.c` v1.22 from
https://github.com/nothings/stb at commit a7d4c2175630e1ea5b6f8dd00ab96fed0b97b96d, unmodified.
License: public domain or MIT (see `LICENSE`; `stb_vorbis.c` carries the same text at its end).
`stb_image.h` / `stb_image_write.h` are built only through `src/texpack_stb.c` (PNG only), used
for texture dumps and replacement packs (`src/texpack.c`) and for a mod's own PNG textures
(`src/modassets_own.c`); `stb_vorbis.c` only through `src/modassets_libs.c`, which decodes a
mod's own `.ogg` sounds (`src/modassets_own.c`).
