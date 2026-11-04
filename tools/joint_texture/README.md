# Joint texture

`make_joint.py` draws the hand-rolled joint that the runtime shows instead of the cigarette prop
(guest texture `8f3dc7227bda2c45`, 16x16 DXT1). It is original procedural art with a fixed seed,
so every run gives the same image. The only numbers taken from the game are the 16 shading
factors around the prop's circumference; no game image is read or stored.

The runtime embeds the 64x64 result as `runtime/src/builtin_joint.h` (generated, committed) and
applies it by default, ahead of any texture pack (`runtime/src/builtin_tex.c`).

Regenerate or check the header (needs numpy and Pillow):

    uv run --no-project --with numpy --with pillow python tools/joint_texture/make_joint.py --header runtime/src/builtin_joint.h
    uv run --no-project --with numpy --with pillow python tools/joint_texture/make_joint.py --check runtime/src/builtin_joint.h

`make_joint.py OUT_DIR` writes the same image as a texture-pack PNG for inspection.
`tests/test_builtin_tex.py` runs the check (or, without numpy/Pillow, compares the header's SHA-256).
