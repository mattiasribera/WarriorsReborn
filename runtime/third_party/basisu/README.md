# Basis Universal transcoder (vendored)

The transcoder of Basis Universal v2.50 (https://github.com/BinomialLLC/basis_universal, tag `v2_50`,
commit 9bebe16726b3a61c8c213eeee3b7cffb462ef34e), unmodified:

- `transcoder/`: the whole upstream `transcoder/` directory. License: Apache-2.0 (`LICENSE`);
  `NOTICE` is upstream's attribution notice, which redistributions must carry (Apache-2.0 section
  4(d)): ship `LICENSE` and `NOTICE` with binaries that contain it.
- `zstd/zstddeclib.c`, `zstd.h`, `zstd_errors.h`: the single-file Zstandard decoder upstream bundles
  for KTX2 supercompression. License: BSD-3-Clause (`zstd/LICENSE`, Copyright (c) 2016-present,
  Facebook, Inc.).

Only the transcoder is vendored, not the encoder: `tools/texture_basis.py` runs the `basisu` encoder
built separately. Built by `runtime/Makefile` (`BASISU_*`, `KTX2_OBJS`) into `xbrun`,
`xbrun-core.dll` and `ktx2_test`, behind `src/ktx2.cpp` (texture-pack `.ktx2` files to BC1/BC3/BC7).
Formats the runtime never asks for (PVRTC1/2, ATC, FXT1) are compiled out with upstream's own
`BASISD_SUPPORT_*` switches.
