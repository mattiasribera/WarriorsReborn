"""Mesh pack: smoothed replacement meshes for skinned characters (xbrun --mesh-pack DIR).

wmesh.py  the character vertex format, the runtime's content hash and the .wmesh file format
pn.py     interpolating PN-triangle subdivision (the default): original vertices unchanged, open
          boundaries on the same edge curve, bulges capped and stopped short of other surfaces
loop.py   Loop subdivision that keeps bone slots/weights, per-corner UVs and creased edges
render.py a small software renderer (PNG) for before/after silhouettes
cli.py    `python3 -m mesh_pack build|render|info` (run with PYTHONPATH=tools)
"""
