#!/usr/bin/env python3
"""Cutscene sweep: run the opening scene of each mission chapter headlessly, once with the
default 30 Hz world and once with --sim60, each with a --rate-probe, then compare them
(scenediff.py) keyed by game time.

Reaching a chapter: from a profile load (a copy of a save with a story profile; the menu input
below picks STORY, the profile manager, "use existing profile" and the first profile), three
--poke diagnostics (runtime/src/rateprobe.h) redirect the load:
  - the level name is written at 0x44c078 (8 free bytes before .data, so at most 7 characters);
  - MenuLoadLevel's `call lua_tostring` at 0x27fd99 becomes `mov eax,0x44c078`;
  - GetCheckPoint 0x335f40 becomes `mov ax,CHAPTER; ret` (the chapter is the checkpoint).
Missions come from field 5 of `levelNames` in config_preload3.lua (field 4: the chapter count).

usage: sweep.py run [--missions 1-23] [--chapters N] [--scenes STEMS] [-j 4] [--nice 19] [--frames 9000] [--out DIR] ...
       sweep.py run --sim-only --tag T --extra-pokes FILE ...   (a candidate fix, as tagged sim runs)
       sweep.py compare [--tag T] [--out DIR]   (scenediff over every finished pair in DIR)
Scratch only (never committed): DIR defaults to ~/.cache/cutscene-sweep/runs.
"""
import argparse
import collections
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import time

Mission = collections.namedtuple("Mission", "level chapters mission")
Job = collections.namedtuple("Job", "level chapter mission")

MENU_SCRIPT = "3200:start:10,3400:start:10,3600:a,3720:a,3840:a"
NAME_ADDR = 0x44C078
MENULOADLEVEL_CALL = (0x27FD99, "b878c04400", "e872510a00")   # mov eax,0x44c078 / call 0x324f10
GETCHECKPOINT = (0x335F40, "a1789f5b00")                       # mov eax,[0x5b9f78]

# Probe ranges beyond the .data section (which the probe always adds; it holds the character
# pool 0x4ff878, the body pool behind character +0xb0, the handle table 0x5a7570 and the position
# records 0x4fefe8): the scene camera's view matrix, the world object, the script timer list
# header, and per character slot (first OBJ_SLOTS) the heap objects behind character +0xb4
# (0x1000 bytes per character: model/animation instance) and +0x170.
CHAR_POOL, CHAR_STRIDE, OBJ_SLOTS = 0x4FF878, 0x668, 16
CHAR_OBJECTS = [(0xB4, 0x1000), (0x170, 0x200)]
EXTRA_RANGES = ["@0x4cf13c+0x1c:64", "@0x5b9f78:0x400", "@0x5a7528:0x28"]


def missions_from_lua(text):
    """Story missions (field 5 between 1 and 23) of the levelNames table, in table order."""
    i = text.index("levelNames = {", text.index("levelNames = {}") + 5)
    out = []
    for m in re.finditer(r'\{\s*"(\w+)",\s*"(\w+)",\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),', text[i:]):
        name, _, _, chapters, mission = m.groups()
        if 1 <= int(mission) <= 23 and int(chapters) > 0:
            out.append(Mission(name, int(chapters), int(mission)))
    return out


def _numbers(spec):
    out = set()
    for part in spec.split(","):
        a, _, b = part.partition("-")
        out.update(range(int(a), int(b or a) + 1))
    return out


def select(missions, spec):
    want = _numbers(spec)
    return [m for m in missions if m.mission in want]


def jobs(missions, chapters, scenes=None):
    """One job per mission chapter (chapters: None for all, or a spec such as "1" or "1-2"; scenes:
    optional set of run stems such as "m03_level87_c3")."""
    want = _numbers(chapters) if chapters else None
    out = [Job(m.level, c, m.mission) for m in missions for c in range(1, m.chapters + 1)
           if want is None or c in want]
    return [j for j in out if scenes is None or stem(j) in scenes]


def shortcut_pokes(level, chapter):
    name = level.encode()
    if len(name) > 7:
        raise ValueError("level name %r does not fit in the 8 bytes at 0x%x" % (level, NAME_ADDR))
    if not 1 <= chapter <= 0x7F:
        raise ValueError("chapter %d out of range" % chapter)
    a, new, old = MENULOADLEVEL_CALL
    g, gold = GETCHECKPOINT
    return ["0x%x:%s" % (NAME_ADDR, (name + b"\0").hex()), "0x%x:%s/%s" % (a, new, old),
            "0x%x:66b8%02x00c3/%s" % (g, chapter, gold)]


def probe_ranges():
    objs = ["@0x%x:0x%x" % (CHAR_POOL + CHAR_STRIDE * i + off, size) for off, size in CHAR_OBJECTS
            for i in range(OBJ_SLOTS)]
    return EXTRA_RANGES + objs


def command(job, sim60, xbrun, xbe, disc, hdd, frames, probe, interval, script=MENU_SCRIPT, extra_pokes=()):
    cmd = [xbrun, xbe, disc, "--hdd", hdd, "--video", "null", "--audio", "null", "--stats",
           "--frames", str(frames), "--input-script", script, "--rate-probe", "%s:%d" % (probe, interval)]
    for r in probe_ranges():
        cmd += ["--rate-probe-range", r]
    for p in list(shortcut_pokes(job.level, job.chapter)) + list(extra_pokes):
        cmd += ["--poke", p]
    if sim60:
        cmd.append("--sim60")
    return cmd


def stem(job):
    return "m%02d_%s_c%d" % (job.mission, job.level, job.chapter)


def niced(cmd, level):
    """Prefixes `nice -n LEVEL` (None: unchanged) so sweeps do not starve the desktop."""
    return cmd if level is None else ["nice", "-n", str(level)] + cmd


def run_name(job, sim60, tag=""):
    """Base runs are shared; a tagged --sim60 run carries a candidate fix (extra pokes)."""
    return "%s_%s" % (stem(job), "sim" + tag if sim60 else "base")


def pairs(out, tag=""):
    """(stem, base.rp, sim.rp) for every finished base run with a finished (tagged) --sim60 run."""
    res = []
    for f in sorted(os.listdir(out)):
        if f.endswith("_base.done"):
            st = f[: -len("_base.done")]
            if os.path.exists(os.path.join(out, "%s_sim%s.done" % (st, tag))):
                res.append((st, os.path.join(out, st + "_base.rp"), os.path.join(out, "%s_sim%s.rp" % (st, tag))))
    return res


def run_one(job, sim60, a):
    name = run_name(job, sim60, a.tag)
    work = os.path.join(a.out, "w_" + name)
    probe = os.path.join(a.out, name + ".rp")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    shutil.copytree(a.hdd, os.path.join(work, "hdd"))   # the save copy is never written in place
    extra = a.pokes if sim60 else ()
    cmd = command(job, sim60, a.xbrun, a.xbe, a.disc, "hdd", a.frames, probe, a.interval, extra_pokes=extra)
    t0 = time.time()
    with open(os.path.join(a.out, name + ".stdout"), "w") as log:
        rc = subprocess.call(niced(cmd, a.nice), cwd=work, stdout=log, stderr=subprocess.STDOUT)
    shutil.rmtree(work, ignore_errors=True)
    return name, rc, time.time() - t0


def cmd_run(a):
    ms = select(missions_from_lua(open(a.lua).read()), a.missions)
    js = jobs(ms, a.chapters, set(a.scenes.split(",")) if a.scenes else None)
    a.pokes = open(a.extra_pokes).read().split() if a.extra_pokes else []
    os.makedirs(a.out, exist_ok=True)
    modes = (True,) if a.sim_only else (False, True)
    todo = [(j, s) for j in js for s in modes
            if a.force or not os.path.exists(os.path.join(a.out, run_name(j, s, a.tag) + ".done"))]
    print("%d chapters, %d runs to do" % (len(js), len(todo)), flush=True)
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        futs = {ex.submit(run_one, j, s, a): (j, s) for j, s in todo}
        for f in concurrent.futures.as_completed(futs):
            name, rc, wall = f.result()
            print("%s exit %d wall %.0f s" % (name, rc, wall), flush=True)
            if rc == 0:
                open(os.path.join(a.out, name + ".done"), "w").write("%d\n" % rc)


def cmd_compare(a):
    import scenediff
    prefix = os.path.join(a.out, "report" + ("_" + a.tag if a.tag else ""))
    scenediff.sweep_report(pairs(a.out, a.tag), prefix, rates=not a.no_rates, disasm=a.disasm,
                           closure=a.closure, jobs=a.jobs)
    print("report: %s.md, %s.csv" % (prefix, prefix))


def main(argv=None):
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    data = os.environ.get("WARRIORS_DATA", os.path.join(os.path.expanduser("~"), "Personal", "warriors-port", "extracted"))
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    for name in ("run", "compare"):
        q = sub.add_parser(name)
        q.add_argument("--out", default=os.path.join(os.path.expanduser("~"), ".cache", "cutscene-sweep", "runs"))
        q.add_argument("-j", "--jobs", type=int, default=4, help="parallel runs (default 4)")
        q.add_argument("--tag", default="", help="candidate-fix tag: --sim60 runs are named *_sim<TAG>")
        if name == "run":
            q.add_argument("--missions", default="1-23")
            q.add_argument("--chapters", default=None)
            q.add_argument("--frames", type=int, default=9000)
            q.add_argument("--interval", type=int, default=50)
            q.add_argument("--xbrun", default=os.path.join(root, "runtime", "build", "xbrun"))
            q.add_argument("--xbe", default=os.path.join(data, "default.xbe"))
            q.add_argument("--disc", default=os.path.join(data, "disc"))
            q.add_argument("--lua", default=os.path.join(data, "lua_src", "Main.wad0", "config_preload3.lua"))
            q.add_argument("--hdd", default=os.path.join(os.path.expanduser("~"), ".cache", "rate-diff", "hdd_m8"))
            q.add_argument("--force", action="store_true")
            q.add_argument("--nice", type=int, default=19, help="nice level of each xbrun run (default 19)")
            q.add_argument("--scenes", default=None, help="comma-separated run stems, e.g. m03_level87_c3")
            q.add_argument("--sim-only", action="store_true", help="only the --sim60 runs (bases already exist)")
            q.add_argument("--extra-pokes", default=None,
                           help="file of --poke specs for the --sim60 runs (a candidate fix; scratch only)")
        else:
            q.add_argument("--no-rates", action="store_true", help="skip the per-field rate diff")
            q.add_argument("--disasm", default=None, help="objdump of .text for writer annotations")
            q.add_argument("--closure", default=None)
    a = p.parse_args(argv)
    for k in ("xbrun", "xbe", "disc", "hdd"):
        if hasattr(a, k):
            setattr(a, k, os.path.abspath(getattr(a, k)))
    return cmd_run(a) if a.cmd == "run" else cmd_compare(a)


if __name__ == "__main__":
    sys.exit(main())
