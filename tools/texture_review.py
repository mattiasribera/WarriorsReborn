#!/usr/bin/env python3
"""Generates a local static review page for one or more upscaled texture packs.

Writes OUT/index.html (default extracted/texture_review/, gitignored) showing every texture as a
row of columns: the original from the dump, then one column per pack (by default every pack
under extracted/texture_pack/, v1 realesrgan-x4plus first, then the v2 chains; or the --pack
directories in the order given). It has zoom (all columns side by side, scroll synced),
per-texture approve/reject toggles for the pack chosen under "Decisions for" and a button that
downloads those decisions as review.json ({"pack": ..., "rejected": [...], "approved": [...]}).
Apply the rejections with `tools/texture_upscale.py --model MODEL --reject review.json` (or
`--backend spandrel --chain ... --reject review.json`; rejected textures are removed from that
pack and skipped by later runs).

The images are copied into OUT/img/ by default so the folder can be copied elsewhere and opened
directly (no server needed); --no-copy references the dump and pack in place instead. The page
is derived from the user's own game data: keep it local, never publish or upload it.
"""
import argparse
import csv
import html
import json
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PAGE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Texture Review</title>
<style>
:root { --bg: #f6f6f4; --fg: #1d1d1b; --card: #ffffff; --line: #d8d8d2; --muted: #6b6b66;
        --ok: #1f7a3a; --bad: #b3261e; --accent: #2457c5; --checker: #e4e4e0; }
@media (prefers-color-scheme: dark) {
  :root { --bg: #161615; --fg: #ececea; --card: #21211f; --line: #3a3a37; --muted: #9a9a94;
          --ok: #4cc36f; --bad: #f2716a; --accent: #7aa2ff; --checker: #2c2c2a; }
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--fg); font: 14px/1.4 system-ui, sans-serif; }
header { position: sticky; top: 0; z-index: 2; background: var(--bg); border-bottom: 1px solid var(--line);
         padding: 10px 16px; display: flex; flex-wrap: wrap; gap: 10px 18px; align-items: center; }
h1 { font-size: 16px; margin: 0; }
.muted { color: var(--muted); }
button, select { font: inherit; color: var(--fg); background: var(--card); border: 1px solid var(--line);
                 border-radius: 6px; padding: 4px 10px; cursor: pointer; }
main { padding: 16px; display: grid; gap: 14px; grid-template-columns: repeat(auto-fill, minmax(min(100%, var(--cardw)), 1fr)); }
.card { background: var(--card); border: 1px solid var(--line); border-radius: 8px; padding: 10px; }
.card.rejected { border-color: var(--bad); }
.card.approved { border-color: var(--ok); }
.pair { display: grid; grid-template-columns: repeat(var(--cols), 1fr); gap: 8px; }
.missing { display: flex; align-items: center; justify-content: center; color: var(--muted); font-size: 12px;
           border: 1px dashed var(--line); }
.pair figure { margin: 0; }
.pair figcaption { font-size: 12px; color: var(--muted); }
.img { width: 100%; aspect-ratio: var(--ar); cursor: zoom-in; object-fit: contain; display: block;
       background: repeating-conic-gradient(var(--checker) 0 25%, transparent 0 50%) 0 0 / 16px 16px; }
.orig { image-rendering: pixelated; }
.meta { font: 12px/1.4 ui-monospace, monospace; margin: 6px 0; word-break: break-all; }
.actions { display: flex; gap: 8px; }
.actions .on.ok { background: var(--ok); color: #fff; border-color: var(--ok); }
.actions .on.bad { background: var(--bad); color: #fff; border-color: var(--bad); }
#modal { position: fixed; inset: 0; background: rgba(0,0,0,.85); display: none; z-index: 5; flex-direction: column; }
#modal.open { display: flex; }
#modal .bar { color: #eee; padding: 8px 16px; display: flex; gap: 12px; align-items: center; flex-wrap: wrap; }
#modal .panes { flex: 1; display: grid; grid-template-columns: repeat(var(--cols), 1fr); gap: 4px; min-height: 0; }
#modal .pane figcaption { color: #ccc; font-size: 12px; padding: 2px 4px; position: sticky; left: 0; top: 0; background: #111; }
#modal .pane { overflow: auto; background: #111; }
#modal .pane img { display: block; }
#skipped { padding: 0 16px 24px; }
table { border-collapse: collapse; font: 12px ui-monospace, monospace; }
td, th { border-bottom: 1px solid var(--line); padding: 2px 8px; text-align: left; }
</style>
</head>
<body>
<header>
  <h1>Texture Review</h1>
  <span class="muted" id="summary"></span>
  <label>Decisions for <select id="dpack"></select></label>
  <label>Show <select id="filter"><option value="all">all</option><option value="undecided">undecided</option>
    <option value="approved">approved</option><option value="rejected">rejected</option></select></label>
  <label>Size <input id="size" type="range" min="160" max="1600" step="20" value="320"></label>
  <button id="download">Download review.json</button>
  <label><button id="importbtn" type="button">Import review.json</button><input id="import" type="file" accept=".json" hidden></label>
</header>
<main id="grid"></main>
<details id="skipped"><summary>Skipped textures (not in the pack)</summary><div id="skiptable"></div></details>
<div id="modal"><div class="bar"><strong id="mtitle"></strong>
  <label>Zoom <select id="mzoom"><option>0.5</option><option selected>1</option><option>2</option><option>4</option></select>
  x upscaled size</label><span class="muted">Left: original (nearest), then each pack. Scroll is synced. Esc closes.</span>
  <button id="mclose">Close</button></div>
  <div class="panes" id="panes"></div></div>
<script id="data" type="application/json">__DATA__</script>
<script>
const D = JSON.parse(document.getElementById('data').textContent);
const N = D.packs.length;
document.documentElement.style.setProperty('--cols', N + 1);
const dpack = document.getElementById('dpack');
D.packs.forEach((p, i) => dpack.add(new Option(p, i)));
dpack.value = D.decide;
let di = D.decide, state = {};
const key = () => 'texture-review:' + D.packs[di];
const load = () => { try { state = JSON.parse(localStorage.getItem(key()) || '{}') || {}; } catch (e) { state = {}; } };
const save = () => { try { localStorage.setItem(key(), JSON.stringify(state)); } catch (e) {} };
const esc = s => String(s).replace(/[&<>"]/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;'}[c]));
load();
const grid = document.getElementById('grid');
const cards = {};
function render() {
  const f = document.getElementById('filter').value;
  let a = 0, r = 0, n = 0;
  for (const t of D.textures) {
    const has = !!t.ups[di];
    const s = has ? (state[t.hash] || 'undecided') : 'absent';
    if (has) n++;
    if (s === 'approved') a++; else if (s === 'rejected') r++;
    const c = cards[t.hash];
    c.className = 'card ' + s;
    c.style.display = f === 'all' || f === s ? '' : 'none';
    c.querySelectorAll('.actions button').forEach(b => b.disabled = !has);
    c.querySelector('.ok').classList.toggle('on', s === 'approved');
    c.querySelector('.bad').classList.toggle('on', s === 'rejected');
  }
  document.getElementById('summary').textContent =
    `${D.textures.length} textures, ${N} pack(s); ${D.packs[di]}: ${n} textures, ${a} approved, ${r} rejected, ${n - a - r} undecided`;
}
function setState(h, s) { if (state[h] === s) delete state[h]; else state[h] = s; save(); render(); }
for (const t of D.textures) {
  const c = document.createElement('div');
  c.style.setProperty('--ar', t.w + ' / ' + t.h);
  const cols = t.ups.map((u, i) => u
    ? `<figure><img class="img" loading="lazy" alt="${esc(D.packs[i])}" src="${esc(u.src)}"><figcaption>${esc(D.packs[i])} ${u.uw}x${u.uh}</figcaption></figure>`
    : `<figure><div class="img missing">not in pack</div><figcaption>${esc(D.packs[i])}</figcaption></figure>`).join('');
  c.innerHTML = `<div class="pair">
    <figure><img class="img orig" loading="lazy" alt="original" src="${esc(t.orig)}"><figcaption>original ${t.w}x${t.h}</figcaption></figure>
    ${cols}</div>
    <div class="meta">${t.hash} &middot; ${t.format}${t.alpha ? ' &middot; alpha' : ''} &middot; ${t.draws} draws &middot; frame ${t.first}</div>
    <div class="actions"><button class="ok">Approve</button><button class="bad">Reject</button></div>`;
  c.querySelectorAll('.img').forEach(i => i.addEventListener('click', () => openModal(t)));
  c.querySelector('.ok').addEventListener('click', () => setState(t.hash, 'approved'));
  c.querySelector('.bad').addEventListener('click', () => setState(t.hash, 'rejected'));
  cards[t.hash] = c;
  grid.appendChild(c);
}
document.getElementById('filter').addEventListener('change', render);
dpack.addEventListener('change', () => { di = +dpack.value; load(); render(); });
const size = document.getElementById('size');
const applySize = () => document.documentElement.style.setProperty('--cardw', size.value + 'px');
size.addEventListener('input', applySize);
if (N > 1) size.value = Math.min(1600, 180 * (N + 1));
applySize();
document.getElementById('download').addEventListener('click', () => {
  const out = { pack: D.packs[di], rejected: [], approved: [] };
  for (const t of D.textures) {
    if (!t.ups[di]) continue;
    if (state[t.hash] === 'rejected') out.rejected.push(t.hash);
    if (state[t.hash] === 'approved') out.approved.push(t.hash);
  }
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([JSON.stringify(out, null, 1)], { type: 'application/json' }));
  a.download = 'review.json';
  a.click();
});
const imp = document.getElementById('import');
document.getElementById('importbtn').addEventListener('click', () => imp.click());
imp.addEventListener('change', async () => {
  if (!imp.files.length) return;
  try {
    const j = JSON.parse(await imp.files[0].text());
    state = {};
    (j.approved || []).forEach(h => state[h] = 'approved');
    (j.rejected || []).forEach(h => state[h] = 'rejected');
    save(); render();
  } catch (e) { alert('Not a review.json: ' + e); }
});
const modal = document.getElementById('modal'), panes = document.getElementById('panes'), mz = document.getElementById('mzoom');
let cur = null, uw = 0, uh = 0;
function zoom() {
  const z = parseFloat(mz.value);
  panes.querySelectorAll('img').forEach(i => { i.style.width = (uw * z) + 'px'; i.style.height = (uh * z) + 'px'; });
}
let syncing = false;
function openModal(t) {
  cur = t;
  const up = t.ups.filter(u => u);
  uw = Math.max(...up.map(u => u.uw)); uh = Math.max(...up.map(u => u.uh));
  const srcs = [['original', t.orig, 'orig']].concat(t.ups.map((u, i) => [D.packs[i], u && u.src, '']));
  panes.innerHTML = srcs.map(([name, src, cls]) => `<div class="pane"><figcaption>${esc(name)}</figcaption>` +
    (src ? `<img class="${cls}" alt="${esc(name)}" src="${esc(src)}">` : '<p class="muted">not in pack</p>') + '</div>').join('');
  const all = [...panes.querySelectorAll('.pane')];
  all.forEach(a => a.addEventListener('scroll', () => {
    if (syncing) return;
    syncing = true;
    all.forEach(b => { if (b !== a) { b.scrollLeft = a.scrollLeft; b.scrollTop = a.scrollTop; } });
    requestAnimationFrame(() => { syncing = false; });
  }));
  document.getElementById('mtitle').textContent = t.hash + ' (' + t.w + 'x' + t.h + ')';
  zoom(); modal.classList.add('open');
}
mz.addEventListener('change', zoom);
const close = () => modal.classList.remove('open');
document.getElementById('mclose').addEventListener('click', close);
document.addEventListener('keydown', e => { if (e.key === 'Escape') close(); });
if (D.skipped.length) {
  document.getElementById('skiptable').innerHTML = '<table><tr><th>pack</th><th>hash</th><th>format</th><th>size</th><th>reason</th></tr>' +
    D.skipped.map(s => `<tr><td>${esc(s.pack)}</td><td>${esc(s.hash)}</td><td>${esc(s.format)}</td><td>${esc(s.w)}x${esc(s.h)}</td><td>${esc(s.status)}</td></tr>`).join('') + '</table>';
} else document.getElementById('skipped').style.display = 'none';
render();
</script>
</body>
</html>
"""


def png_size(path):
    with open(path, "rb") as f:
        head = f.read(24)
    return int.from_bytes(head[16:20], "big"), int.from_bytes(head[20:24], "big")


V1_PACK = "realesrgan-x4plus"


def discover_packs(root):
    """Pack directories under root that hold PNGs: the v1 pack first, then the others by name."""
    if not os.path.isdir(root):
        return []
    found = [n for n in sorted(os.listdir(root)) if not n.startswith(".") and os.path.isdir(os.path.join(root, n))
             and any(f.lower().endswith(".png") for f in os.listdir(os.path.join(root, n)))]
    found.sort(key=lambda n: n != V1_PACK)
    return [os.path.join(root, n) for n in found]


def build(dump, packs, out, copy=True, decide=0):
    """Writes out/index.html comparing the original with each pack (a path or a list of paths);
    `decide` is the index of the pack the approve/reject buttons apply to by default.
    Returns the number of textures on the page (textures present in at least one pack)."""
    if isinstance(packs, str):
        packs = [packs]
    with open(os.path.join(dump, "manifest.csv"), newline="") as f:
        manifest = {r["hash"]: r for r in csv.DictReader(f)}
    names = [os.path.basename(os.path.normpath(p)) for p in packs]
    os.makedirs(out, exist_ok=True)
    if copy:
        shutil.rmtree(os.path.join(out, "img"), ignore_errors=True)
        for d in ["orig"] + ["p%d" % i for i in range(len(packs))]:
            os.makedirs(os.path.join(out, "img", d))

    def ref(src, sub, h):
        if copy:
            shutil.copyfile(src, os.path.join(out, "img", sub, h + ".png"))
            return "img/%s/%s.png" % (sub, h)
        return os.path.relpath(src, out).replace(os.sep, "/")

    in_pack, skipped = [], []
    for i, pack in enumerate(packs):
        files = {}
        for name in os.listdir(pack):
            h = name[:-4].lower()
            if name.lower().endswith(".png") and h in manifest and os.path.exists(os.path.join(dump, h + ".png")):
                files[h] = os.path.join(pack, name)
        in_pack.append(files)
        sel_path = os.path.join(pack, "selection.csv")
        if os.path.exists(sel_path):
            with open(sel_path, newline="") as f:
                skipped += [dict(s, pack=names[i]) for s in csv.DictReader(f) if s["status"].startswith("skipped")]
    textures = []
    for h in sorted(set().union(*in_pack) if in_pack else ()):
        m = manifest[h]
        ups = []
        for i, files in enumerate(in_pack):
            if h in files:
                uw, uh = png_size(files[h])
                ups.append({"src": ref(files[h], "p%d" % i, h), "uw": uw, "uh": uh})
            else:
                ups.append(None)
        textures.append({"hash": h, "format": m["format"], "w": int(m["w"]), "h": int(m["h"]),
                         "alpha": m["has_alpha"] == "1", "draws": int(m["draws"]), "first": int(m["first_frame"]),
                         "orig": ref(os.path.join(dump, h + ".png"), "orig", h), "ups": ups})
    textures.sort(key=lambda t: -t["draws"])
    data = {"packs": names, "decide": min(max(decide, 0), max(len(names) - 1, 0)), "textures": textures,
            "skipped": skipped}
    blob = json.dumps(data).replace("</", "<\\/")
    with open(os.path.join(out, "index.html"), "w") as f:
        f.write(PAGE.replace("__DATA__", blob))
    return len(textures)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--dump", default=os.path.join(ROOT, "extracted", "texture_dump"))
    ap.add_argument("--pack", action="append",
                    help="pack directory; repeat for more columns (default: every pack under extracted/texture_pack/)")
    ap.add_argument("--decide", type=int, default=0, help="index of the pack the decisions apply to by default")
    ap.add_argument("--out", default=os.path.join(ROOT, "extracted", "texture_review"))
    ap.add_argument("--no-copy", action="store_true", help="reference the dump and pack in place instead of copying")
    args = ap.parse_args(argv)
    packs = args.pack or discover_packs(os.path.join(ROOT, "extracted", "texture_pack"))
    if not packs:
        sys.exit("no packs: pass --pack DIR or build one with tools/texture_upscale.py")
    n = build(args.dump, packs, args.out, copy=not args.no_copy, decide=args.decide)
    print("%d textures, packs: %s -> %s" % (n, ", ".join(os.path.basename(os.path.normpath(p)) for p in packs),
                                           os.path.join(args.out, "index.html")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
