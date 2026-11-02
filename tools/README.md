# Tools

Helper scripts for working on WarriorsReborn. None of them is needed to play: the launcher does
everything a player needs. They are for people who want to build the port, make a texture pack
or dig into how the game works.

Results with the improvements so far: https://youtu.be/VKQwYA3RYdc

## Before you start

- Every tool works on **your own copy** of the Xbox game. Nothing here downloads or ships game
  files.
- Anything a tool extracts or generates from the game (files, textures, packs, frames, reports)
  belongs under `extracted/` or `~/.cache/`, which git ignores. Never commit or publish it.
  `check_public_tree.py` (below) catches most mistakes.
- Python 3.10+ and the standard library, unless a tool lists more. Run them from the repository
  root.

Paths in the examples follow the layout the runtime README uses: `extracted/default.xbe` and
`extracted/disc/` for the game, `runtime/build/` for the built runtime.

## Getting the game files out of your disc image

### `xdvdfs_extract.py`
Reads an original Xbox disc image (XDVDFS) without mounting it.

```sh
python3 tools/xdvdfs_extract.py game.iso list
python3 tools/xdvdfs_extract.py game.iso extract default.xbe extracted/default.xbe
python3 tools/xdvdfs_extract.py game.iso extract-all extracted/disc
```

Use it to get `default.xbe` and the `disc/` tree that the runtime expects.

### `wad_extract.py`
Unpacks every file from the game's WAD archives (`XBoxWad.idx` + `*.wad`). The index stores
only name hashes, so names are recovered from strings in the XBE and the files themselves;
files whose name is still unknown are written as `_unnamed/<hash>.bin`.

```sh
python3 tools/wad_extract.py game.iso extracted/default.xbe extracted/wad
```

Useful to read the game's Lua scripts, configs and models. The runtime does not need it.

### `scan_rw_chunks.py`
Quick census of RenderWare chunk headers (models, textures, animations) inside the disc image,
grouped by chunk type and RenderWare version. An optional name filter limits the scan.

```sh
python3 tools/scan_rw_chunks.py game.iso            # every file
python3 tools/scan_rw_chunks.py game.iso .txd .dff  # only names containing these
```

## Texture packs

A texture pack replaces the game's textures at run time (`--texture-pack DIR`). The pipeline is:
dump → upscale → review → load.

1. **Dump** the textures the game uses: `runtime/build/xbrun ... --dump-textures extracted/texture_dump`
   (play the areas you want covered).
2. **Upscale** with `texture_upscale.py`.
3. **Review** with `texture_review.py` and drop the textures that got worse.
4. **Load**: `--texture-pack extracted/texture_pack/<name>`, or pick the folder in the launcher.

Packs are made from your copy of the game. Keep them for yourself; do not share or upload them.

### `texture_upscale.py`
Builds a pack from a dump (`manifest.csv` + `<hash>.png`). Two backends:

- `ncnn` (default): [realesrgan-ncnn-vulkan](https://github.com/xinntao/Real-ESRGAN), the
  official release unpacked into `~/tools/realesrgan`; any Vulkan GPU.
- `spandrel`: a chain of PyTorch models run by `spandrel_infer.py` in a separate venv (CUDA GPU
  recommended).

It skips tiny, linear and UI/font-like textures by default, wrap-pads so tiling textures stay
seamless, upscales alpha separately, and can apply review rejections. Needs numpy and
`runtime/build/pngraw` (built by `make -C runtime`).

```sh
python3 tools/texture_upscale.py --model realesrgan-x4plus
python3 tools/texture_upscale.py --backend spandrel --chain 1x-DEDXT,4x-GameAI-2-0
python3 tools/texture_upscale.py --model realesrgan-x4plus --reject review.json
python3 tools/texture_upscale.py --help   # selection, sizes, padding, GPU
```

Output: `extracted/texture_pack/<name>/<hash>.png`.

### `spandrel_infer.py`
The inference step for the `spandrel` backend: runs one or more super-resolution models over a
folder of PNGs, in tiles so 6 GB GPUs do not run out of memory. `texture_upscale.py` calls it;
you only run it directly to try a model chain on a few images. Model files are loaded as
tensors only (`safetensors`, or `torch.load(weights_only=True)`), never as arbitrary code.

```sh
~/tools/upscale-venv/bin/python -I tools/spandrel_infer.py -i IN -o OUT \
    --model ~/tools/upscale-models/1x_DEDXT.pth --model ~/tools/upscale-models/4x-GameAI-2-0.pth
```

Needs a venv with `torch`, `spandrel` and `safetensors`.

### `texture_review.py`
Generates a local HTML page that shows every texture next to its upscaled versions, one column
per pack, with synced zoom and approve/reject toggles. A button exports your decisions as
`review.json` for `texture_upscale.py --reject`.

```sh
python3 tools/texture_review.py                                   # every pack under extracted/texture_pack/
python3 tools/texture_review.py --pack extracted/texture_pack/A --pack extracted/texture_pack/B
```

Open `extracted/texture_review/index.html` in a browser. The page holds game textures: keep it
local.

## Research and debugging

These help find out how the original game works and check the port against it. They need the
runtime built (`make -C runtime`) and, for some, `objdump` (binutils).

### `d3d_census.py`
Static census of how the game's code uses Xbox Direct3D: every call site per D3D function,
recovered constant arguments, direct reads/writes of D3D globals, and vertex-shader microcode,
shader declarations and texture headers found in the data. It is how the renderer's work list
was planned.

```sh
make -C runtime symbols   # builds runtime/build/default.syms
python3 tools/d3d_census.py extracted/default.xbe runtime/build/default.syms
```

Writes its report to `extracted/`. `--wad-dir` points it at extracted WAD files
(`wad_extract.py`) to scan them too; `--module-map` is optional.

### `cutscene_sweep/sweep.py` and `cutscene_sweep/scenediff.py`
Checks that the 60 FPS mode (`--sim60`) does not make cutscenes run too fast or too slow. For
each mission chapter, `sweep.py` runs the opening scene headless twice, at 30 Hz and with
`--sim60`, recording `--rate-probe` snapshots, then `scenediff.py` compares the two runs by
game time: characters or the camera moving at about 2x or 0.5x speed, and spawns or timers
firing at a different time.

```sh
python3 tools/cutscene_sweep/sweep.py run --missions 1-23 -j 4 --hdd PATH_TO_SAVE_COPY
python3 tools/cutscene_sweep/sweep.py compare
```

Needs a copy of a save with a story profile (`--hdd`). Each run works on its own copy, so
the save itself is never modified. Results go to `~/.cache/cutscene-sweep/` (`report.md` and
`report.csv`). Each pair runs at nice 19 by default (`--nice`), and `-j` sets how many run at
once; a full sweep takes a while.

### `ratediff/ratediff.py`
Finds game memory fields that change about twice as fast (or half as fast) per game second
with `--sim60` as at 30 Hz: the usual cause of something running at the wrong speed in 60 FPS
mode. It takes two `--rate-probe` snapshot files from the same input, groups the flagged
fields and annotates them with the instructions that touch them.

```sh
runtime/build/xbrun ... --rate-probe base.probe          # same input script both times
runtime/build/xbrun ... --sim60 --rate-probe sim.probe
python3 tools/ratediff/ratediff.py base.probe sim.probe --xbe extracted/default.xbe
```

Report in `~/.cache/rate-diff/report`. `--from-frame/--to-frame` limit the window and `--top`
the number of findings.

## Repository upkeep

### `check_public_tree.py`
Fails when the repository contains something that must never be published: game-data
directories, disc images, executables, game archives, media, saves, dumps, files over 5 MB,
personal paths, private IP addresses or e-mail addresses. CI runs it on every push. Run it
before you open a pull request.

```sh
python3 tools/check_public_tree.py      # exit 0 clean, 1 findings
```

### `package_release.py`
Builds the Windows release zip exactly as the release workflow (`.github/workflows/release.yml`)
does: `WarriorsReborn-vX.Y.Z/` with the three programs, the upstream `SDL2.dll`, `ARGUMENTS.md`,
a `README.txt` and `licenses/`, written with a fixed order and date so the same inputs give the
same zip, plus `SHA256SUMS.txt`. Build the Windows programs first and pass the SDL2 mingw
development package, Vulkan-Headers and (unless `zig` is on `PATH`) zig's `lib` folder in the
versions the workflow pins. `notes` prints a version's CHANGELOG section as release notes.

```sh
make -C runtime -j4 windows && make -C launcher -j4 windows
python3 tools/package_release.py package --sdl2-devel ~/.cache/launcher/sdl/SDL2-2.32.10 \
    --vulkan-headers path/to/Vulkan-Headers-1.4.363 --out dist
python3 tools/package_release.py notes --version 0.1.2
```

### `icon/make_icon.py`
Draws the launcher's icon, an original geometric Ferris wheel, from code alone (no game
artwork, standard library only). Writes `assets/icon/wonder-wheel.ico` and `.svg`;
`--png-dir DIR` also writes each size as a PNG to look at.

```sh
python3 tools/icon/make_icon.py --png-dir /tmp/icon-preview
```

## Contributing

Ideas, bug reports and pull requests are welcome. Open an issue with what you tried and what
happened; for rendering or controller bugs, include your GPU, driver and controller. If a tool
fails on your setup, its `--help` and the full error output help a lot.
