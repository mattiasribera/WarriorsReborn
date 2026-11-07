# Building your own texture pack

A texture pack is a folder of replacement images the game loads instead of its own textures.
You make it from **your own copy** of the game: the runtime dumps the textures it draws, you
edit or upscale them, and the runtime loads the result.

> **Personal use only.** A pack is derived from the game's own textures. Keep it on your own
> PC and never share it publicly or upload it anywhere (forums, mod sites, cloud links, this
> repository or its releases).

Contents:

1. [Dump the textures](#1-dump-the-textures)
2. [Edit by hand](#2-edit-by-hand)
3. [AI upscaling](#3-ai-upscaling)
4. [Compress to BC1/BC7 .dds](#4-compress-to-bc1bc7-dds)
5. [Load the pack](#5-load-the-pack)
6. [Troubleshooting](#6-troubleshooting)
7. [Emission masks (neon glow)](#7-emission-masks-neon-glow)

## 1. Dump the textures

In the launcher, type in **Extra arguments** (put a path with spaces in quotes):

```text
--dump-textures C:\Warriors\texture_dump
```

and play. Every unique texture the game draws is written once as `<hash>.png`, and
`manifest.csv` lists them (hash, format, size, mip levels, alpha, draw count, first frame).
Later runs into the same folder only add new textures, so dump over several sessions. Remove
the argument when you are done.

- Only textures that are actually **drawn** are dumped. Play through the game, or load later
  saves and visit every area, to collect them all. The whole game has about 2,900 unique
  textures.
- The file name is a hash of the Xbox texture content (format, size and texels). Packs made
  for other versions of the game or for emulators use different names and never match.

## 2. Edit by hand

Any image editor works:

- Copy the PNGs you want to change into a new folder, e.g. `C:\Warriors\packs\my-edits`, and
  edit them there. **Keep the file name** (`0123456789abcdef.png`); that is what links the image
  to the game's texture.
- Any size works (bigger is sharper; power-of-two sides are not required). The runtime builds
  the mip chain when it loads the file.
- **Keep the alpha channel.** Transparent parts of the original (fences, foliage, decals, fx)
  must stay transparent, or they turn into solid blocks.
- Textures you do not include stay as in the game.

## 3. AI upscaling

The tools are Python scripts in this repository's `tools/` folder. They decode PNGs with the
small `pngraw` helper: `pngraw.exe` on Windows, `pngraw` on Linux. The tools look for it next to
the scripts (`tools\pngraw.exe`) and then in `runtime\build\`; `--pngraw PATH` points them
anywhere else. They run natively on Windows; WSL 2 is an optional alternative (below).

### Set up on Windows (once)

You need a checkout of this repository that matches your game build, Python 3.12 and an NVIDIA
GPU with its normal Windows driver. In PowerShell, from the repository folder:

```powershell
winget install zig.zig                     # zig builds pngraw.exe
winget install astral-sh.uv                # uv installs Python and its packages
# open a new PowerShell so both are on PATH, then build the PNG helper:
mkdir -Force runtime\build
zig cc -target x86-windows-gnu -O1 -o runtime\build\pngraw.exe runtime\tools\pngraw.c runtime\src\texpack.c runtime\src\texpack_stb.c runtime\src\xtex.c
uv venv --python 3.12 $HOME\tools\upscale-venv
$V = "$HOME\tools\upscale-venv\Scripts\python.exe"
uv pip install --python $V torch torchvision --index-url https://download.pytorch.org/whl/cu128
uv pip install --python $V spandrel numpy
& $V -c "import torch; print(torch.__version__, torch.cuda.is_available())"   # must print True
```

If you build on Linux instead, `make -C runtime build/pngraw.exe` cross-compiles the same file; copy it into
`runtime\build\` or `tools\` of the Windows checkout. `pngraw.exe` needs nothing beyond
Windows 10 or later.

Run the tools with the venv's Python: `& $V tools\texture_upscale.py ...`. The commands below are
written for a Linux shell; on Windows use `& $V` for `$V`, `\` in paths, a backtick instead of
`\` at the end of a continued line, and Windows folders such as `C:\Warriors\texture_dump`
instead of `/mnt/c/Warriors/texture_dump`. `--python` defaults to the venv above
(`Scripts\python.exe` on Windows, `bin/python` on Linux).

### Optional: set up in WSL 2 or on Linux

The tools also run in [WSL 2](https://learn.microsoft.com/windows/wsl/install) (Ubuntu). Your
Windows folders are under `/mnt/c/` there, and an NVIDIA GPU works in WSL with the normal Windows
driver (do not install a Linux GPU driver inside WSL). In the Ubuntu terminal, from a checkout
of this repository that matches your game build:

```sh
sudo apt install build-essential cmake git
make -C runtime build/pngraw CC=cc         # the PNG helper the tools use
curl -LsSf https://astral.sh/uv/install.sh | sh   # uv, a Python installer
uv venv --python 3.12 ~/tools/upscale-venv
V=~/tools/upscale-venv/bin/python
uv pip install --python $V torch torchvision --index-url https://download.pytorch.org/whl/cu128
uv pip install --python $V spandrel numpy
$V -c "import torch; print(torch.__version__, torch.cuda.is_available())"   # must print True
```

Choose the CUDA index (`cu128` above) that [pytorch.org](https://pytorch.org/get-started/locally/)
lists for your GPU. **Check `torch.cuda.is_available()` after installing spandrel:** installing
spandrel from PyPI can replace the CUDA build of torch with the CPU-only wheel (it does on
native Windows). If it prints `False` or the version has no `+cu`, reinstall torch from the CUDA
index:

```sh
uv pip install --python $V --reinstall torch torchvision --index-url https://download.pytorch.org/whl/cu128
```

A 6 GB GPU is enough; lower `--tile` if it runs out of memory.

### Choose models

Download the model files yourself from [OpenModelDB](https://openmodeldb.info/) (search by name)
into `~/tools/upscale-models/` (`%USERPROFILE%\tools\upscale-models\` on Windows). Nothing is bundled with this project; each model has its own
licence. The file name must match the name you use in `--chain` (case, `-` and `_` are
ignored); `.safetensors` or `.pth`. The files are only loaded as weights.

| Model | Use | Licence |
|---|---|---|
| 4x-PBRify_UpscalerV4 | surfaces, props, characters, fx (trained on compressed 2000s game textures) | CC0-1.0 |
| 2xAoMR_mosr | optional second step for 8x on small surfaces | CC-BY-4.0 |
| 1x-DXTDecompressor-Source-V3 | removes DXT blocks before a text model | WTFPL |
| 4xTextureDAT2_otf | text-preserving: UI, signs, lettering | CC-BY-4.0 |

The game's textures are DXT-compressed. Prefer models trained on compressed game textures:
clean-photo models (often named `hq`) treat the DXT 4x4 blocks as detail and harden them into
flat patches. Lost detail cannot come back; a good model invents plausible texture instead of
blocks.

### Simple pack: one model for everything

```sh
$V tools/texture_upscale.py --backend spandrel --chain 4x-PBRify_UpscalerV4 \
    --dump /mnt/c/Warriors/texture_dump --out /mnt/c/Warriors/packs
#   -> /mnt/c/Warriors/packs/4x-PBRify_UpscalerV4/<hash>.png
```

By default it skips textures under 16 px (`--min-size`), linear (`LIN_*`) textures and
UI/font-like textures (`--include-ui` to upscale them too), wrap-pads tiling textures so they
stay seamless, upscales alpha separately and limits results to 2048 px (`--max-size`). It only
does textures not yet in the pack (`--force` redoes them), so you can add new dumps later.
`--name` sets the pack folder name, `--gpu N` the CUDA device.

Long runs: add `--keep-work ~/texwork` to keep the intermediate files; if the run stops,
the same command resumes and does not redo finished images. A texture that failed is listed as
`failed:no-output` in the pack's `selection.csv` and the command exits with 2.

### Keep some textures original

Tiny textures, flat single-colour fills and masks gain nothing from AI and can get worse.
Leave them to the game with either file (hashes from your dump):

```sh
--skip-list skip.txt                # one hash per line, # comments
--keep-original keep-original.json  # {"hashes": ["0123456789abcdef", ...]}
```

Textures not in the pack simply use the game's own texture.

### Compare and reject

`$V tools/texture_review.py --dump /mnt/c/Warriors/texture_dump --pack /mnt/c/Warriors/packs/my-pack`
writes a local page that shows original and upscaled side by side. Mark bad results as
rejected, download `review.json` and rerun the build command with `--reject review.json`: those
textures leave the pack and stay skipped.

## 4. Compress to BC1/BC7 .dds

A PNG pack works, but every PNG is decoded and its mips built when it loads, and it sits in
video memory as uncompressed RGBA (4 bytes per texel). A `.dds` pack is uploaded as is: it loads
faster and takes 1/8 (BC1) or 1/4 (BC7) of the memory.

`tools/texture_compress.py` needs `bc7enc` from
[bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo) (MIT or public domain), built yourself:

```sh
git clone https://github.com/richgel999/bc7enc_rdo ~/bc7enc_rdo
cmake -S ~/bc7enc_rdo -B ~/bc7enc_rdo/build -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_CXX_FLAGS="-include cstdint"
cmake --build ~/bc7enc_rdo/build

$V tools/texture_compress.py --pack /mnt/c/Warriors/packs/my-pack \
    --out /mnt/c/Warriors/packs/my-pack-bc --encoder ~/bc7enc_rdo/build/bc7enc -j 4
```

Opaque textures become BC1 (`--opaque bc7` for higher quality), textures with alpha BC7.
UI/font textures are copied as PNG by default (`--no-ui-heuristic` compresses them too).
`--with-png` also copies every PNG as a fallback for GPUs without BC support. Up-to-date
outputs are skipped, so rerunning after changes only redoes changed textures.

## 5. Load the pack

- **Launcher:** Add-ons, **Texture pack**, choose the pack folder (the one with the
  `<hash>.png` / `<hash>.dds` files).
- **Command line or Extra arguments:** `--texture-pack C:\Warriors\packs\my-pack-bc`.

Layering: `--texture-pack` can be given up to 8 times; for each texture the first pack that
has it wins. The launcher's packs come first, then the ones in Extra arguments. To put hand
edits over an AI pack, choose `my-edits` in the launcher and add
`--texture-pack C:\Warriors\packs\my-pack-bc` in Extra arguments. The launcher's button-icon
pack, when chosen, also takes one slot.

When a pack has both `<hash>.dds` and `<hash>.png`, the `.dds` wins (if the GPU samples BC
formats); the `.png` is the fallback when the `.dds` cannot be used.

Memory: with a pack the renderer's texture budget is 1 GiB. What counts is the set of
textures the game has loaded at one time (the current area), not the size of the pack on disk:
a 3 GB pack can fit easily. PNG textures cost 4 to 8 times more than `.dds`, so large PNG packs
are the usual reason to run out.

When the driver reports how much video memory the game may use (the `VK_EXT_memory_budget`
extension: current NVIDIA, AMD and Intel drivers), the budget adapts: every 2 seconds it is set to
what is left after everything else the game holds (render targets, effects, buffers), keeping a
quarter of that (at least 128 MiB) free, never above 1 GiB and never below 128 MiB. On an
integrated GPU sharing system memory, or a 4 GB card running other programs, the cache then stays
smaller instead of pushing the driver into swapping. It shrinks at once under pressure (the cache is
emptied and refilled, a short burst of reloads), grows back only after 6 seconds of room, and does
not shrink again within 10 seconds of emptying the cache unless the driver is over its budget, so
it never thrashes. `--texpack-budget MB` lowers (or raises, up to 4096) the maximum. Without the
extension the budget is the fixed 1 GiB (or `--texpack-budget`). `--stats` prints the cap chosen
(`[texpack] cache cap ...`) and the driver's usage and budget (`[vram] driver ...`).

Loading: pack files are read and decoded on two low-priority loader threads, never on the thread
that draws, so a texture seen for the first time does not stall the frame. Until its replacement is
ready the game's own texture is drawn, then the replacement takes its place (usually within a few
frames; longer while the disk is busy). At most about 16 MB (or 3 ms) of replacements are sent to
the GPU per frame, so a burst of new textures is spread over a few frames instead of one long
one. Neon emission masks (`<hash>.emit.dds`) load the same way and only glow once their texture's
replacement is shown. When a level loads, the textures it registers are read ahead (prefetched,
up to 160 MB waiting in memory), so most replacements are ready before they are first drawn.
`--texpack-sync` restores loading on the drawing thread (debugging), `--no-texpack-prefetch`
turns the read-ahead off. The exit log line `[texpack] loader queue: ...` counts prefetches,
deferred uploads and dropped images.

## 6. Troubleshooting

Open the log with **View log** on the launcher's Home page and look for `[texpack]` lines.

| Symptom | Check |
|---|---|
| Nothing changes | `[texpack] N replacement textures in DIR` should show N > 0. If 0, the folder holds subfolders instead of the files, or the names are not `<16 hex digits>.png`/`.dds`. Packs from other versions or emulators never match. |
| `cannot read the texture pack` | Wrong path; in Extra arguments put a path with spaces in quotes. |
| Some textures stay old | They are in no pack (not dumped, skipped, kept original or rejected), or an earlier layered pack has its own version. |
| A hand edit does not show | An older `.dds` with the same hash wins over your `.png`: rerun `texture_compress.py` or delete that `.dds`. |
| `skipping <file>: <reason>` | That `.dds` is malformed (partial mip chain, sRGB, wrong size); the `.png` is used if present. Recreate it with `texture_compress.py`. |
| Black or solid blocks where there was transparency | The edit lost its alpha channel; re-export with alpha. |
| Seams on repeating walls or floors | Hand edits must tile; the upscaler pads tiling textures for you. |
| Stutter or `texture cache drops` above 0 on exit | The loaded textures exceed the budget: use a `.dds` pack, or rebuild with a lower `--max-size` (e.g. 1024). |
| `torch.cuda.is_available()` is False, or upscaling is very slow | torch is the CPU build: reinstall it from the PyTorch CUDA index (see Set up). |
| CUDA out of memory | Lower `--tile` (e.g. 256), close other GPU programs. |
| `missing ...\pngraw.exe` (Windows) | Build it with the `zig cc` line in Set up on Windows, put it next to the tools, or pass `--pngraw PATH`. |
| `missing .../pngraw` (Linux, WSL) | Run `make -C runtime build/pngraw CC=cc` in the repository. |
| DXT blocks turned into flat patches | The model is a clean-photo model; use one trained on compressed game textures. |

## 7. Emission masks (neon glow)

`--neon-glow` (launcher: Display, **Neon glow** / "Brillo de neones") makes lit and neon signs
glow at night: the lit tubes, bulbs and panels keep their own colour through fog and darkness and
feed the bloom. Which texels are lit comes from an emission mask per texture, in any pack folder:

| File | Format |
|---|---|
| `<hash>.emit.png` | 8-bit grey PNG (the red channel is read): 0 = not lit, 255 = fully lit, soft edges welcome |
| `<hash>.emit.dds` | BC4 UNORM (DXGI 80, or the legacy FourCC `ATI1` / `BC4U`); level 0 is read, mips are rebuilt |

`<hash>` is the colour texture's hash (the same name as its `<hash>.png` / `<hash>.dds`). The
mask may have any size: it is sampled with the texture's own coordinates, so a 256x256 mask works
over the original 128x128 texture and over a 4x upscaled one. Masks can sit next to a pack's
colour textures or in a mask-only folder over the original textures; for each texture the first
pack that has a mask wins, and within one pack the `.emit.dds` wins. The colour packs ignore these
files. The launcher passes `neon-masks` (next to the game) as a mask-only pack while neon glow is on.

Make the masks yourself in any image editor: open the colour texture, paint the lit tubes,
bulbs and panels white (soft edges welcome) and everything else black, and save it as
`<hash>.emit.png` in 8-bit grey. A dumped or upscaled texture works as the starting point. Masks,
like the textures, are derived from your copy of the game: keep them to yourself.

## Legal

Dumps and packs come from your own copy of the game and are for your personal use only. Never
share them publicly or upload them, in whole or in part. This project contains and distributes
no game textures, and model files are downloaded by you under their own licences.
