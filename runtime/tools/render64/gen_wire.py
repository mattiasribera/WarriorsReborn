#!/usr/bin/env python3
"""Generates runtime/src/render_wire_gen.h: the renderer.h call stream's wire format (the
--record-renderer recording and the renderer-process proxy's messages).

Parses the prototypes of runtime/src/renderer.h and gamma_ramp.h and RendererConfig's fields, and
emits, from the tables below:
  * RW_ID_* (one id per recorded function) and RW_SCHEMA (a hash of everything the format depends
    on: a recording only replays with the same schema);
  * under RW_GEN_WRAP: rr_<function> wrappers for the runtime (src/render_rec.c) that call the
    real function and, while recording, serialize the call (arguments, payloads, the result); with
    --render-process loopback (rr_loop) they serialize the call instead and the loopback decoder
    makes it (a call with a result waits for it: rr_loop_result); queries and LOCAL calls first let
    the decoder catch up (rr_loop_sync); the #defines that route hle_d3d.c's calls to them are under
    RW_GEN_ROUTE;
  * under RW_GEN_REPLAY: rw_replay_call, the decoder that makes the call again (the replay tool,
    tools/render64/render64_replay.c);
  * rw_config_put / rw_config_get: RendererConfig field by field (pointers and the window
    callbacks are not ABI-neutral).

Run it after changing renderer.h: `python3 runtime/tools/render64/gen_wire.py`; `--check` fails when the
committed header is stale. Every pointer parameter of a recorded function needs a PAYLOAD entry
(the generator stops otherwise), so a new renderer.h call cannot be recorded half-way.
"""
import hashlib
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
SRC = ROOT / "runtime" / "src"
HEADERS = [SRC / "renderer.h", SRC / "gamma_ramp.h"]
OUT = SRC / "render_wire_gen.h"

# Pure queries: passed through, never recorded (counted per frame as the round trips a renderer
# process would have to answer or cache).
QUERY = {
    "renderer_output_size", "renderer_samples", "renderer_frames_in_flight", "renderer_supersample",
    "renderer_fps_overlay_on", "renderer_fps_overlay_gpu", "renderer_sharpen", "renderer_fsr", "renderer_anisotropy",
    "renderer_error", "renderer_device_lost", "renderer_device_name", "renderer_has_bc", "renderer_movie_upload_count",
    "renderer_texture_drop_count", "renderer_memory_budget", "renderer_texture_bytes", "renderer_vram",
    "renderer_swapchain_bytes", "renderer_default_pixel_shader", "renderer_pixel_shader_count", "renderer_texture_count",
    "renderer_state_default", "renderer_pipeline_count", "renderer_surface_size", "renderer_surface_count",
    "renderer_glass_count", "renderer_emission_count", "renderer_relief_count", "renderer_hair_count",
    "renderer_skin_count", "renderer_mat_count", "renderer_bloom_on", "renderer_bloom_count", "renderer_taa_on",
    "renderer_taa_count", "renderer_taa_object_draws", "renderer_taa_jitter", "renderer_taa_upscale",
    "renderer_filmic_on", "renderer_filmic_count", "renderer_auto_exposure_on", "renderer_auto_exposure_state",
    "renderer_dof_on", "renderer_dof_count", "renderer_dof_state", "renderer_rain_on", "renderer_rain_count",
    "renderer_sky_on", "renderer_sky_count", "renderer_skyline_count", "renderer_rain_splash_on",
    "renderer_rain_splash_count", "renderer_rain_wet_on", "renderer_rain_wet_count", "renderer_wet_sssr_on",
    "renderer_wet_hiz_read", "renderer_material_map_on", "renderer_rain_cover_on", "renderer_rain_cover_count",
    "renderer_rain_chars_on", "renderer_rain_chars_count", "renderer_ssao_on", "renderer_ssao_count",
    "renderer_ssao_gtao_on", "renderer_ssgi_on", "renderer_contact_on", "renderer_contact_count", "renderer_lamp_lights_on",
    "renderer_lamp_lights_count", "renderer_light_shafts_on", "renderer_light_shafts_count", "renderer_volfog_on",
    "renderer_volfog_count", "renderer_char_lights_on", "renderer_char_lights_count", "renderer_world_lights_count",
    "renderer_lamp_shadows_on", "renderer_lamp_wall_map_on", "renderer_lamp_shadows_count",
    "renderer_world_shadow_on", "renderer_world_shadow_count", "renderer_world_shadow_gate", "renderer_soft_on",
    "renderer_soft_count", "renderer_can_present", "renderer_present_info", "renderer_last_timing",
    "renderer_pipeline_report", "renderer_pipeline_counts", "renderer_skin_on", "renderer_skin_stats",
    "renderer_debug_vertices", "renderer_debug_normals", "renderer_gamma_ramp_on", "renderer_gamma_ramp_count",
    "renderer_texture_lod_bias", "renderer_taa_fsr3", "renderer_fsr3_masks", "renderer_frame_gen_on", "renderer_frame_gen_count",
    "renderer_frame_gen_present_wait", "renderer_frame_gen_presented", "renderer_wet_glass_on", "renderer_wet_glass_count",
    "renderer_hdr_on",
    # one-frame-late results and output buffers (a renderer process answers them from its stats block or
    # a synchronous request; master plan 3.2): not part of the stream
    "renderer_frame_gen_pacing", "renderer_frame_gen_readback", "renderer_gpu_passes", "renderer_hdr_readback",
}
# Debug hooks of this process: passed through, not recorded and not counted.
LOCAL = {"renderer_set_vertex_check", "renderer_set_vertex_check_frame", "renderer_set_draw_tag",
         "renderer_pipeline_cache_save",
         # the recorder's RW_REC_PIPE_PLAN record and the replay (render_rec.c, render64_replay.c)
         "renderer_pipeline_misses", "renderer_pipeline_plan",
         # the frame time travels inside renderer_begin's record (FRAME_TIME below), not as calls
         "renderer_frame_time", "renderer_set_frame_time"}

# Payload of each pointer parameter of a recorded function: (byte-count expression in C over the
# parameters, category). Struct pointers without an entry are sent as sizeof(struct) (all of
# them are plain 4-byte fields, the same layout in i386 and x86_64; RW_SCHEMA also covers sizeof).
PAYLOAD = {
    ("renderer_draw", "v"): ("(size_t)nv * sizeof(RVertex)", "RW_CAT_VERTS"),
    ("renderer_draw", "idx"): ("(size_t)ni * 4", "RW_CAT_INDICES"),
    ("renderer_draw_lit", "v"): ("(size_t)nv * sizeof(RVertex)", "RW_CAT_VERTS"),
    ("renderer_draw_lit", "idx"): ("(size_t)ni * 4", "RW_CAT_INDICES"),
    ("renderer_draw_lit", "normals"): ("(size_t)nv * 12", "RW_CAT_NORMALS"),
    ("renderer_draw_world_lit", "v"): ("(size_t)nv * sizeof(RVertex)", "RW_CAT_VERTS"),
    ("renderer_draw_world_lit", "idx"): ("(size_t)ni * 4", "RW_CAT_INDICES"),
    ("renderer_draw_world_lit", "normals"): ("(size_t)nv * 12", "RW_CAT_NORMALS"),
    ("renderer_skyline", "v"): ("(size_t)nv * sizeof(RVertex)", "RW_CAT_VERTS"),
    ("renderer_skyline", "idx"): ("(size_t)ni * 4", "RW_CAT_INDICES"),
    ("renderer_texture_create", "data"): ("rw_texture_bytes(format, w, h, levels)", "RW_CAT_TEXTURES"),
    ("renderer_movie_plane", "data"): ("(size_t)w * h", "RW_CAT_MOVIE"),
    ("renderer_gamma_ramp", "lut"): ("256 * 4", "RW_CAT_ARGS"),
    ("renderer_fps_overlay_text", "text"): ("rw_str_bytes(text)", "RW_CAT_ARGS"),
    ("renderer_draw_skinned", "data"): ("data_bytes", "RW_CAT_SKIN_DATA"),
    ("renderer_draw_skinned", "idx"): ("(size_t)ni * 4", "RW_CAT_INDICES"),
    ("renderer_draw_skinned_world_lit", "data"): ("data_bytes", "RW_CAT_SKIN_DATA"),
    ("renderer_draw_skinned_world_lit", "idx"): ("(size_t)ni * 4", "RW_CAT_INDICES"),
    ("renderer_draw_skinned_world_lit", "normals"): ("(size_t)job->nv * 12", "RW_CAT_NORMALS"),
}
# Output parameters of recorded calls: not sent (renderer_end's frame: the recorder adds its hash).
OUTPUT = {("renderer_end", "out")}
# Pointer parameters with their own codec in src/render_wire.c (rw_put_<codec> / rw_get_<codec>), and
# what the writer passes after the pointer. base, skin_prog, skin_consts keep stream state (render_wire.h:
# the base image when changed, programs by reference, constants as deltas).
CODEC = {
    ("renderer_configure", "c"): ("config", ""),
    ("renderer_material_map", "m"): ("map", ""),
    ("renderer_lamp_wall_map", "m"): ("map", ""),
    ("renderer_lamp_wall_map_region", "m"): ("map", ""),
    ("renderer_draw_skinned", "nrm"): ("skin_normals", "job->nv"),
    ("renderer_begin", "base"): ("base", "base ? rw_base_bytes(pitch_bytes) : 0"),
    ("renderer_draw_skinned", "ins"): ("skin_prog", "ins ? (size_t)job->nins * 16 : 0"),
    ("renderer_draw_skinned", "consts"): ("skin_consts", ""),
    ("renderer_draw_skinned_world_lit", "ins"): ("skin_prog", "ins ? (size_t)job->nins * 16 : 0"),
    ("renderer_draw_skinned_world_lit", "consts"): ("skin_consts", ""),
}
# Codecs whose reader returns the parameter's pointer (const void *rw_get_<codec>(RwReader *)).
CODEC_PTR = {"base", "skin_prog", "skin_consts"}
# Parameters that carry a renderer handle (texture, surface, movie plane, previous frame): the
# replay maps the recorded value to the one its own renderer returned.
HANDLE_PARAMS = {
    ("renderer_set_texture", "handle"), ("renderer_set_stage_texture", "handle"), ("renderer_set_target", "color"),
    ("renderer_set_target", "depth"), ("renderer_set_emission", "mask"), ("renderer_set_relief", "normal"),
    ("renderer_set_relief", "specular"), ("renderer_set_hair", "mask"), ("renderer_set_skin", "mask"),
}
HANDLE_RESULT = {"renderer_texture_find", "renderer_texture_create", "renderer_movie_plane", "renderer_surface",
                 "renderer_prev_frame"}
# Calls whose last value is state the renderer keeps across frames: the recorder keeps their last
# record and sends it again at the start of a recording (with the configuration, renderer_init
# and the surfaces made so far), so a recording that starts mid-game replays into a fresh
# renderer. The texture cache is dropped at that frame instead (see render_rec.c).
STICKY = {"renderer_configure", "renderer_init", "renderer_set_texture_budget", "renderer_set_relief_dry",
          "renderer_set_parallax", "renderer_set_relief_height", "renderer_set_relief", "renderer_set_emission",
          "renderer_set_hair", "renderer_set_skin", "renderer_set_eye_open", "renderer_gamma_ramp",
          "renderer_fps_overlay_text", "renderer_material_map", "renderer_lamp_wall_map", "renderer_set_relief_glossy",
          "renderer_taa_render_size", "renderer_taa_sharpness", "renderer_saturation", "renderer_hdr_levels"}
# Hand-written hooks in render_rec.c around these calls (rr_pre_<name> / rr_post_<name>).
PRE_HOOK = {"renderer_begin"}
# Post hooks run after the call's record was written.
POST_HOOK = {"renderer_configure", "renderer_begin", "renderer_end", "renderer_present", "renderer_lamp_wall_map_region", "renderer_shutdown",
             "renderer_init", "renderer_set_texture_budget"}
# Calls whose record ends with the renderer's frame time (renderer_frame_time after the call, ms of
# the host clock): the replay hands it back (renderer_set_frame_time before the call), so time-driven
# state (auto exposure's adaptation) is the recording's, not the replaying machine's.
FRAME_TIME = {"renderer_begin"}
# Calls whose every record is kept for the start of a recording (the surfaces made so far).
SAVED_LIST = {"renderer_surface"}
# LOCAL calls with a hand-written hook in render_rec.c (rr_pre_<name>, before the call).
LOCAL_HOOK = {"renderer_set_frame_time"}

# Calls whose loopback branch first asks render_rec.c how to get the result (rr_loop_ask_<name>(args, &ret),
# render_rec.c): RR_LOOP_WAIT the decoder's result (a round trip to a renderer
# process), RR_LOOP_KNOWN the game side knows it (the texture-handle mirror: the record goes, the decoder checks
# the value), RR_LOOP_LATER the caller does not need it now (fire-and-forget: the recording gets the decoder's
# value), RR_LOOP_SKIP no record at all (a call without effect: a texture looked up again in the same frame).
# rr_loop_told_<name>(args, ret) then sees every result.
LOOP_ASK = {"renderer_texture_find", "renderer_texture_create", "renderer_draw_skinned", "renderer_draw_skinned_world_lit"}

# Draw-state setters (render_wire.c ds_ids, RW_REC_DRAW_STATE): their loopback branch packs the arguments as
# their record's payload would carry them and hands them to rr_ds_call (render_rec.c), with no record writer
# (~11 k calls per frame in the busy street). Plain struct pointers and scalars only.
DRAW_STATE = {"renderer_set_stage_texture", "renderer_set_pixel_shader", "renderer_set_alpha_test", "renderer_set_state",
              "renderer_set_target", "renderer_set_emission", "renderer_set_relief", "renderer_set_hair", "renderer_set_skin",
              "renderer_set_eye_open"}

SCALARS = {"uint32_t": 4, "int": 4, "float": 4, "int32_t": 4, "uint64_t": 8}
# Queries a renderer process answers from its per-frame snapshot (master plan 3.1 stats block): every
# query without parameters that returns a scalar, except these, whose value changes inside a frame
# (measured with --renderer-traffic: the recorder answers from the snapshot and counts stale answers).
# renderer_gamma_ramp_on: true from the renderer_gamma_ramp call on (a loopback run from boot answered it
# stale once, in the frame that set the first ramp).
# renderer_texture_bytes: grows with every renderer_texture_create inside a frame and drives hle_d3d's
# texture-pack budget (28 stale answers in a loopback run with the HD packs).
SNAPSHOT_LIVE = {"renderer_gamma_ramp_on", "renderer_texture_bytes"}
SNAP_FIELD = {"int": "i", "uint32_t": "u", "float": "f", "int32_t": "i32", "uint64_t": "u64"}

# --render-process helper (W6): the renderer is in the x86_64 helper, so every query
# the snapshot does not answer, and every LOCAL call, is a remote request (RW_REC_QUERY in the stream, the reply
# on the BACK ring: rr_rq_<name> on the game side, rw_serve_query in the helper). Exceptions:
# LATE: output-only queries the helper answers after every frame in its per-frame ack (the value of the last
# frame the game waited for, one or two frames late: master plan 3.4), so the per-frame ones cost no round trip.
# Before the first ack of a renderer_init (--dynres, the [taa] and [fsr] start lines) a LATE query is one round trip
# (render_rec.c rr_rq_begin), not a 0 from an empty answer.
LATE = {"renderer_last_timing", "renderer_frame_gen_pacing", "renderer_fps_overlay_gpu", "renderer_skin_stats",
        "renderer_taa_upscale", "renderer_fsr", "renderer_memory_budget"}
# CONST: the same answer for the whole run: asked once, then kept on the game side.
CONST = {"renderer_default_pixel_shader", "renderer_state_default"}
# Pointers into the renderer's own memory (debug checks): NULL out of process.
REMOTE_NULL = {"renderer_debug_vertices", "renderer_debug_normals"}
# Answered on the game side (render_rec.c rr_ga_<name>): renderer_texture_bytes drives hle_d3d's texture-pack
# budget several times a frame; the helper's last ack plus the bytes created since.
GAME_ANSWER = {"renderer_texture_bytes"}
# Output buffers of remote queries: their byte count over the parameters (both sides evaluate it; the game copies
# at most that many bytes of the reply).
REMOTE_BUF = {
    ("renderer_pipeline_report", "line"): "(size_t)(size > 0 ? size : 0)",
    ("renderer_gpu_passes", "buf"): "(size_t)len",
    ("renderer_frame_gen_readback", "out"): "(size_t)(pitch_bytes > 0 ? pitch_bytes : 0) * rr_rq_rows()",
    ("renderer_hdr_readback", "out"): "(size_t)(pitch_bytes > 0 ? pitch_bytes : 0) * rr_rq_rows()",
    ("renderer_fsr3_masks", "rgba"): "(size_t)(w > 0 && h > 0 ? w : 0) * (size_t)(h > 0 ? h : 0) * 4",
    ("renderer_wet_hiz_read", "out"): "(size_t)(max_texels > 0 ? max_texels : 0) * 4",
}
REMOTE_SCALARS = {"int": ("rw_i32", "rw_ri32", 4), "uint32_t": ("rw_u32", "rw_ru32", 4), "float": ("rw_f32", "rw_rf32", 4),
                  "int32_t": ("rw_i32", "rw_ri32", 4), "uint64_t": ("rw_u64", "rw_ru64", 8), "size_t": ("rw_u32", "rw_ru32", 4),
                  "double": ("rw_f64", "rw_rf64", 8), "uint8_t": (None, None, 1)}


def remote_outputs(f, structs):
    """The output parameters of a remote query: (name, C element type, byte-count expression)."""
    outs = []
    for p in f.params:
        key = (f.name, p.name)
        if key in REMOTE_BUF:
            outs.append((p.name, p.base(), REMOTE_BUF[key]))
        elif p.kind == "array":
            n = re.search(r"\[(.*)\]", p.decl).group(1)
            outs.append((p.name, p.type, f"sizeof({p.type}) * ({n})"))
        elif p.kind == "ptr":
            b = p.base()
            if b not in structs and b not in REMOTE_SCALARS:
                sys.exit(f"gen_wire: {f.name}: remote output {p.name} of type {b}: give it a REMOTE_BUF entry")
            outs.append((p.name, b, f"sizeof({b})"))
    return outs


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r"^\s*#[^\n]*", " ", text, flags=re.M)
    text = text.replace('extern "C" {', " ")   # the __cplusplus guard's brace (its '}' is dropped below)
    return text


def statements(text):
    """Top-level statements (split at ';' outside braces), struct bodies included verbatim."""
    out, depth, cur = [], 0, []
    for ch in text:
        cur.append(ch)
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth = max(depth - 1, 0)
        elif ch == ";" and depth == 0:
            out.append(" ".join("".join(cur).split()))
            cur = []
    return out


def split_params(s):
    parts, depth, cur = [], 0, ""
    for ch in s:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        parts.append(cur.strip())
    return [] if parts == ["void"] else parts


class Param:
    def __init__(self, decl):
        self.decl = decl
        m = re.match(r"^(.*?)\(\s*\*\s*(\w+)\s*\)\s*(\[.*\])$", decl)   # const float (*consts)[4]
        if m:
            self.name, self.kind, self.type = m.group(2), "ptr", decl
            return
        m = re.match(r"^(.*?)(\w+)\s*(\[[^\]]*\])$", decl)   # float out[2]
        if m:
            self.name, self.kind, self.type = m.group(2), "array", m.group(1).strip()
            return
        m = re.match(r"^(.*?)(\w+)$", decl)
        self.type, self.name = m.group(1).strip(), m.group(2)
        self.kind = "ptr" if "*" in self.type else "scalar"

    def base(self):
        return self.type.replace("const", "").replace("*", "").strip()


class Func:
    def __init__(self, ret, name, params):
        self.ret, self.name, self.params = ret.strip(), name, [Param(p) for p in split_params(params)]

    def proto(self, prefix=""):
        ps = ", ".join(p.decl for p in self.params) or "void"
        sep = "" if self.ret.endswith("*") else " "
        return f"{self.ret}{sep}{prefix}{self.name}({ps})"


def parse():
    funcs, structs = [], {}
    for h in HEADERS:
        for st in statements(strip_comments(h.read_text())):
            m = re.match(r"^typedef struct \{(.*)\} (\w+);$", st)
            if m:
                structs[m.group(2)] = m.group(1)
                continue
            if st.startswith(("typedef", "enum", "extern")) or "{" in st:
                continue
            m = re.match(r"^(.*?)\b(renderer_\w+)\s*\((.*)\);$", st)
            if m:
                funcs.append(Func(m.group(1), m.group(2), m.group(3)))
    return funcs, structs


def config_fields(body):
    """RendererConfig's fields as (kind, name) in declaration order."""
    fields = []
    for decl in body.split(";"):
        decl = " ".join(decl.split())
        if not decl:
            continue
        m = re.match(r"^\w[\w\s\*]*\(\s*\*\s*(\w+)\s*\)\s*\(.*\)$", decl)
        if m:
            fields.append(("fnptr", m.group(1)))
            continue
        m = re.match(r"^((?:const\s+)?\w+)\s+(.*)$", decl)
        base, rest = m.group(1), m.group(2)
        for item in rest.split(","):
            item = item.strip()
            stars = item.count("*")
            name = re.sub(r"[\*\s]|const", "", item)
            if stars:
                fields.append(("ptr:" + base.replace("const", "").strip() + "*" * stars, name))
            else:
                fields.append(("scalar:" + base, name))
    return fields


CONFIG_PTRS = {   # RendererConfig pointer fields: how they travel
    "instance_ext": "drop",          # the window system's extensions: the receiving process has its own window
    "filmic": "filmic",              # RFilmic + its LUT
    "auto_exposure": "pod",
    "dof": "pod",
    "pipeline_cache_dir": "string",
}
CONFIG_FNPTRS = {"create_surface": "flag", "drawable_size": "drop"}


def gen():
    funcs, structs = parse()
    names = [f.name for f in funcs]
    for n in QUERY | LOCAL | STICKY | PRE_HOOK | POST_HOOK | SAVED_LIST | HANDLE_RESULT | FRAME_TIME | SNAPSHOT_LIVE | LOCAL_HOOK | LOOP_ASK | DRAW_STATE | LATE | CONST | REMOTE_NULL | GAME_ANSWER:
        if n not in names:
            sys.exit(f"gen_wire: {n} is in the tables but not in renderer.h")
    for n in LATE | CONST | REMOTE_NULL | GAME_ANSWER:
        if n not in QUERY:
            sys.exit(f"gen_wire: {n} is a helper rule but not a QUERY")
    recorded = [f for f in funcs if f.name not in QUERY and f.name not in LOCAL]
    for f in recorded:
        for p in f.params:
            key = (f.name, p.name)
            if p.kind == "array":
                sys.exit(f"gen_wire: {f.name}: array parameter {p.name} of a recorded call (a query?)")
            if key in OUTPUT:
                continue
            if p.kind == "ptr" and key not in PAYLOAD and key not in CODEC and not p.base().startswith("R"):
                sys.exit(f"gen_wire: {f.name}: pointer parameter {p.name} needs a PAYLOAD or CODEC entry")
            if p.kind == "ptr" and p.base().startswith("R") and key not in PAYLOAD and key not in CODEC:
                if p.base() not in structs:
                    sys.exit(f"gen_wire: {f.name}: {p.base()} is not a struct of renderer.h")
                if "*" in structs[p.base()]:
                    sys.exit(f"gen_wire: {f.name}: {p.base()} holds pointers: give {p.name} a CODEC")
            if p.kind == "scalar" and p.type not in SCALARS:
                sys.exit(f"gen_wire: {f.name}: scalar type {p.type} of {p.name}")
        if f.ret not in ("void",) and f.ret not in SCALARS:
            sys.exit(f"gen_wire: {f.name}: return type {f.ret}")
        if f.ret in ("uint64_t", "float"):   # the loopback passes results as 32-bit integers
            sys.exit(f"gen_wire: {f.name}: a {f.ret} result of a recorded call")
    cfg = config_fields(structs["RendererConfig"])
    for kind, name in cfg:
        if kind.startswith("ptr:") and name not in CONFIG_PTRS:
            sys.exit(f"gen_wire: RendererConfig.{name}: pointer field without a CONFIG_PTRS rule")
        if kind == "fnptr" and name not in CONFIG_FNPTRS:
            sys.exit(f"gen_wire: RendererConfig.{name}: callback without a CONFIG_FNPTRS rule")
        if kind.startswith("scalar:") and kind[7:] not in SCALARS:
            sys.exit(f"gen_wire: RendererConfig.{name}: type {kind}")

    queries = [f for f in funcs if f.name in QUERY]
    pod_structs = sorted(s for s in structs if "*" not in structs[s] and s != "RVertexCheckDraw")
    schema_src = "\n".join([f.proto() for f in recorded] + [f"{k} {n}" for k, n in cfg] + sorted(map(str, PAYLOAD.items()))
                           + sorted(map(str, CODEC.items())) + sorted(map(str, OUTPUT)) + sorted(STICKY) + sorted(SAVED_LIST) + sorted(map(str, HANDLE_PARAMS)) + sorted(HANDLE_RESULT) + sorted(FRAME_TIME) + pod_structs)
    schema = int(hashlib.sha256(schema_src.encode()).hexdigest()[:8], 16)
    # the helper's protocol also depends on the queries (their indices and signatures) and the helper rules
    helper_src = "\n".join([schema_src] + [f.proto() for f in funcs if f.name in QUERY or f.name in LOCAL]
                           + sorted(LATE) + sorted(CONST) + sorted(REMOTE_NULL) + sorted(GAME_ANSWER) + sorted(map(str, REMOTE_BUF.items())))
    helper_schema = int(hashlib.sha256(helper_src.encode()).hexdigest()[:8], 16)

    o = []
    w = o.append
    w("/* Generated by tools/render64/gen_wire.py from renderer.h and gamma_ramp.h: do not edit. */")
    w("#ifndef RENDER_WIRE_GEN_H")
    w("#define RENDER_WIRE_GEN_H")
    w(f"#define RW_SCHEMA 0x{schema:08x}u")
    w(f"#define RW_HELPER_SCHEMA 0x{helper_schema:08x}u   /* the render64 helper's handshake: the stream and the remote queries */")
    w(f"#define RW_FUNC_COUNT {len(funcs)}   /* renderer.h + gamma_ramp.h functions: recorded, queries, local */")
    w("enum {")
    w("    RW_ID_NONE,")
    for f in recorded:
        w(f"    RW_ID_{f.name[9:].upper()},")
    w("    RW_ID_COUNT")
    w("};")
    w(f"#define RW_QUERY_COUNT {len(queries)}")
    w("#endif")
    w("#if defined(RW_GEN_NAMES) && !defined(RW_GEN_NAMES_DONE)")
    w("#define RW_GEN_NAMES_DONE")
    w("static const char *const rw_id_name[RW_ID_COUNT] = {")
    w('    "none",')
    for f in recorded:
        w(f'    "{f.name}",')
    w("};")
    w("/* Queries (not recorded): their names, for the per-frame round-trip count. */")
    w("static const char *const rw_query_name[RW_QUERY_COUNT] = {")
    for f in queries:
        w(f'    "{f.name}",')
    w("};")
    w("#endif")
    w("#ifndef RW_GEN_SIZES_DONE")
    w("#define RW_GEN_SIZES_DONE")
    w("/* sizeof of every plain struct of renderer.h: the recording's header carries them. */")
    w(f"#define RW_STRUCT_COUNT {len(pod_structs) + 1}")
    w("#define RW_STRUCT_SIZES { sizeof(RVertex), " + ", ".join(f"sizeof({s})" for s in pod_structs) + " }")
    w("#endif")
    w("")

    # ---- RendererConfig codec ----
    w("#ifdef RW_GEN_CONFIG")
    w("void rw_put_config(RwWriter *w, const RendererConfig *c) {")
    w("    rw_u32(w, c != NULL);")
    w("    if (!c) return;")
    for kind, name in cfg:
        if kind.startswith("scalar:"):
            t = kind[7:]
            fn = {"int": "rw_i32", "uint32_t": "rw_u32", "float": "rw_f32", "int32_t": "rw_i32", "uint64_t": "rw_u64"}[t]
            w(f"    {fn}(w, c->{name});")
        elif kind == "fnptr":
            if CONFIG_FNPTRS[name] == "flag":
                w(f"    rw_u32(w, c->{name} != NULL);")
        else:
            rule = CONFIG_PTRS[name]
            if rule == "filmic":
                w(f"    rw_put_filmic(w, c->{name});")
            elif rule == "pod":
                t = kind[4:].rstrip("*")
                w(f"    rw_blob(w, c->{name}, c->{name} ? sizeof({t}) : 0, c->{name} != NULL, RW_CAT_ARGS);")
            elif rule == "string":
                w(f"    rw_blob(w, c->{name}, rw_str_bytes(c->{name}), c->{name} != NULL, RW_CAT_ARGS);")
    w("}")
    w("/* Fills *c from the stream; pointers point into the record (valid while it is). The window")
    w(" * callbacks come back NULL: *had_surface says whether the recording process presented. */")
    w("int rw_get_config(RwReader *r, RendererConfig *c, int *had_surface) {")
    w("    memset(c, 0, sizeof *c);")
    w("    *had_surface = 0;")
    w("    if (!rw_ru32(r)) return 0;")
    for kind, name in cfg:
        if kind.startswith("scalar:"):
            t = kind[7:]
            fn = {"int": "rw_ri32", "uint32_t": "rw_ru32", "float": "rw_rf32", "int32_t": "rw_ri32", "uint64_t": "rw_ru64"}[t]
            if name == "instance_ext_count":
                w(f"    (void){fn}(r);")
            else:
                w(f"    c->{name} = {fn}(r);")
        elif kind == "fnptr":
            if CONFIG_FNPTRS[name] == "flag":
                w("    *had_surface = (int)rw_ru32(r);")
        else:
            rule = CONFIG_PTRS[name]
            t = kind[4:].rstrip("*")
            if rule == "filmic":
                w(f"    c->{name} = rw_get_filmic(r);")
            elif rule in ("pod", "string"):
                w(f"    c->{name} = (const {t} *)rw_rblob(r, NULL);")
    w("    return 1;")
    w("}")
    w("#endif")
    w("")

    # ---- runtime wrappers ----
    w("#ifdef RW_GEN_ROUTE")
    for f in funcs:
        w(f"#define {f.name} rr_{f.name}")
    w("#endif")
    w("#ifdef RW_GEN_PROTOS")
    for f in funcs:
        w(f.proto("rr_") + ";")
    w("#endif")
    w("")
    w("#ifdef RW_GEN_WRAP")
    snap = [(qi, f) for qi, f in enumerate(queries) if not f.params and f.ret in SCALARS and f.name not in SNAPSHOT_LIVE]
    w("/* The snapshot a renderer process would publish after each frame: the queries it answers. */")
    w("static union { int i; uint32_t u; float f; int32_t i32; uint64_t u64; } rr_snap[RW_QUERY_COUNT];")
    w("static const uint8_t rr_snap_query[RW_QUERY_COUNT] = {" + ", ".join("1" if any(q == qi for q, _ in snap) else "0" for qi in range(len(queries))) + "};")
    w("static void rr_snap_take(void) {")
    for qi, f in snap:
        w(f"    rr_snap[{qi}].{SNAP_FIELD[f.ret]} = {f.name}();")
    w("}")
    snap_ids = {qi for qi, _ in snap}

    # ---- --render-process helper: remote queries (game side) and their server (the helper) ----
    def ret_io(f):
        if f.ret == "void":
            return None
        if f.ret == "const char *":
            return "str"
        if f.ret not in REMOTE_SCALARS:
            return "ptr"
        return f.ret
    w(f"#define RW_LATE_COUNT {sum(1 for f in queries if f.name in LATE)}")
    w("/* rr_rq_begin / rr_rq_call's kind: 0 a round trip, 1 the helper answers it in its per-frame ack (LATE), 2 the game")
    w(" * side keeps its first answer (CONST). */")
    for qi, f in enumerate(queries):
        if f.name in REMOTE_NULL:
            continue
        rio = ret_io(f)
        if rio == "ptr":
            sys.exit(f"gen_wire: {f.name}: a pointer result: REMOTE_NULL or a string")
        w("static " + f.proto("rr_rq_") + " {")
        kind = 1 if f.name in LATE else 2 if f.name in CONST else 0
        w(f"    RwWriter *rq_w = rr_rq_begin({qi}, {kind});")
        ins = [p for p in f.params if p.kind == "scalar"]
        if ins:
            w("    if (rq_w) {")
            for p in ins:
                fn = REMOTE_SCALARS[p.type][0]
                w(f"        {fn}(rq_w, ({'uint32_t' if p.type == 'size_t' else p.type}){p.name});")
            w("    }")
        else:
            w("    (void)rq_w;")
        w(f"    RwReader *rq_r = rr_rq_call({qi}, {kind});")
        if rio == "str":
            w(f"    const char *rr_v = rr_rq_str(rq_r, {qi});")
        elif rio:
            w(f"    const {f.ret} rr_v = ({f.ret}){REMOTE_SCALARS[f.ret][1]}(rq_r);")
        for name, _t, expr in remote_outputs(f, structs):
            w(f"    rr_rq_out(rq_r, {name}, {expr});")
        if rio:
            w("    return rr_v;")
        w("}")
    w("/* The helper: answers remote query qi (its arguments in r) into o. 0, or -1 (unknown query, short request). */")
    w("static int rw_serve_query(uint32_t qi, RwReader *rq_r, RwWriter *rq_o) {")
    w("    switch (qi) {")
    for qi, f in enumerate(queries):
        if f.name in REMOTE_NULL:
            continue
        rio = ret_io(f)
        w(f"    case {qi}: {{")
        for p in f.params:
            if p.kind == "scalar":
                w(f"        const {p.type} {p.name} = ({p.type}){REMOTE_SCALARS[p.type][1]}(rq_r);")
        for k, (name, t, expr) in enumerate(remote_outputs(f, structs)):
            w(f"        {t} *{name} = ({t} *)rr_rq_scratch({k}, {expr});")
        w("        if (rq_r->bad) return -1;")
        call = f"{f.name}({', '.join(p.name for p in f.params)})"
        if rio == "str":
            w(f"        const char *rr_v = {call};")
            w("        rw_blob(rq_o, rr_v, rr_v ? rw_str_bytes(rr_v) : 0, rr_v != NULL, RW_CAT_ARGS);")
        elif rio:
            w(f"        const {f.ret} rr_v = {call};")
            w(f"        {REMOTE_SCALARS[f.ret][0]}(rq_o, rr_v);")
        else:
            w(f"        {call};")
        for name, _t, expr in remote_outputs(f, structs):
            w(f"        rw_blob(rq_o, {name}, {expr}, 1, RW_CAT_ARGS);")
        w("        return 0;")
        w("    }")
    w("    default:")
    w("        return -1;")
    w("    }")
    w("}")
    w("/* The helper, after every frame: the LATE queries' replies, each as u32 query index + a plain blob. */")
    w("static void rw_serve_late(RwWriter *o) {")
    w("    RwReader rq_none;")
    w("    rw_reader(&rq_none, \"\", 0);")
    for qi, f in enumerate(queries):
        if f.name not in LATE:
            continue
        w("    {")
        w("        RwWriter t = {0};")
        w(f"        if (!rw_serve_query({qi}, &rq_none, &t) && !t.bad) {{")
        w(f"            rw_u32(o, {qi});")
        w("            rw_blob_plain(o, t.buf, t.n);")
        w("        } else rw_u32(o, RW_NULL);")
        w("        rw_free(&t);")
        w("    }")
    w("}")

    for qi, f in enumerate(queries):
        args = ", ".join(p.name for p in f.params)
        if f.name in REMOTE_NULL:
            remote = "return NULL;"
        elif f.name in GAME_ANSWER:
            remote = f"return rr_ga_{f.name}({args});"
        elif f.ret == "void":
            remote = f"{{ rr_rq_{f.name}({args}); return; }}"
        else:
            remote = f"return rr_rq_{f.name}({args});"
        w(f.proto("rr_") + " {")
        if qi in snap_ids:
            fld = SNAP_FIELD[f.ret]
            w("    if (rr_active) {")
            w(f"        rr_query({qi});")
            w("        if (rr_snap_on) {")
            w("            if (rr_snap_check) {")
            w("                rr_loop_sync();")
            w(f"                const {f.ret} rr_live = rr_remote ? rr_rq_{f.name}() : {f.name}();")
            w(f"                rr_snap_answer({qi}, memcmp(&rr_live, &rr_snap[{qi}].{fld}, sizeof rr_live) != 0);")
            w("            }")
            w(f"            return rr_snap[{qi}].{fld};")
            w("        }")
            w(f"        if (rr_remote) {remote}")
            w("        rr_loop_sync();")
            w("    }")
            w(f"    return {f.name}();")
        else:
            w("    if (rr_active) {")
            w(f"        rr_query({qi});")
            w(f"        if (rr_remote) {remote}")
            w("        rr_loop_sync();")
            w("    }")
            w(f"    {'return ' if f.ret != 'void' else ''}{f.name}({args});")
        w("}")
    for f in funcs:
        if f.name not in LOCAL:
            continue
        args = ", ".join(p.name for p in f.params)
        w(f.proto("rr_") + " {")
        w("    if (rr_active) {")
        if f.name in LOCAL_HOOK:
            w(f"        rr_pre_{f.name}({args});")
        if f.ret == "void":
            w(f"        if (rr_remote) {{ rr_rl_{f.name}({args}); return; }}")
        else:
            w(f"        if (rr_remote) return rr_rl_{f.name}({args});")
        w("        rr_loop_sync();")
        w("    }")
        w(f"    {'return ' if f.ret != 'void' else ''}{f.name}({args});")
        w("}")
    def encode(f, ind, loop):
        """The lines that write f's arguments (and its result) into rw_w."""
        out = []
        for p in f.params:
            key = (f.name, p.name)
            if key in OUTPUT:
                continue
            if p.kind == "scalar":
                fn = {"uint32_t": "rw_u32", "int": "rw_i32", "float": "rw_f32", "int32_t": "rw_i32", "uint64_t": "rw_u64"}[p.type]
                out.append(f"{fn}(rw_w, {p.name});")
            elif key in CODEC:
                codec, extra = CODEC[key]
                cast = "(const RMaterialMap *)" if codec == "map" else ""
                out.append(f"rw_put_{codec}(rw_w, {cast}{p.name}" + (f", {extra}" if extra else "") + ");")
            elif key in PAYLOAD:
                expr, cat = PAYLOAD[key]
                out.append(f"rw_blob(rw_w, {p.name}, {p.name} ? {expr} : 0, {p.name} != NULL, {cat});")
            else:
                out.append(f"rw_blob(rw_w, {p.name}, {p.name} ? sizeof({p.base()}) : 0, {p.name} != NULL, RW_CAT_ARGS);")
        if f.ret != "void":
            fn = {"uint32_t": "rw_u32", "int": "rw_i32", "float": "rw_f32", "int32_t": "rw_i32", "uint64_t": "rw_u64"}[f.ret]
            if loop:   # not known yet: the recording gets the decoder's result (rr_loop_result)
                out.append("rr_loop_result_at(rw_w);")
            val = "rr_ret" if not loop else "rr_how == RR_LOOP_KNOWN ? rr_ret : 0" if f.name in LOOP_ASK else "0"
            out.append(f"{fn}(rw_w, {val});")
        if f.name in FRAME_TIME:
            out.append(f"rw_f64(rw_w, {'rr_loop_frame_time()' if loop else 'renderer_frame_time()'});")
        out.append("rr_finish(rw_w);")
        return [ind + l for l in out]

    for f in recorded:
        args = ", ".join(p.name for p in f.params)
        idn = f"RW_ID_{f.name[9:].upper()}"
        sticky = "1" if f.name in STICKY else "2" if f.name in SAVED_LIST else "0"
        w(f.proto("rr_") + " {")
        if f.name in DRAW_STATE:
            assert f.ret == "void" and f.name not in PRE_HOOK | POST_HOOK | LOOP_ASK, f.name
            w("    if (rr_loop && rr_loop_on()) {   /* a draw-state setter: its payload straight to the slot */")
            w("        uint8_t rr_a[RR_DS_MAX];")
            w("        size_t rr_n = 0;")
            for p in f.params:
                key = (f.name, p.name)
                if p.kind == "scalar":
                    w(f"        rr_ds_put(rr_a, &rr_n, &{p.name}, sizeof {p.name});")
                else:
                    assert key not in PAYLOAD and key not in CODEC and p.kind == "ptr", key
                    w(f"        _Static_assert(sizeof({p.base()}) + 16 <= RR_DS_MAX, \"{p.base()} fits the setter buffer\");")
                    w(f"        rr_ds_blob(rr_a, &rr_n, {p.name}, sizeof({p.base()}));")
            w(f"        rr_ds_call(RW_ID_{f.name[9:].upper()}, rr_a, rr_n);")
            w("        return;")
            w("    }")
        # loopback: the call travels through the wire and the decoder makes it
        if f.name not in DRAW_STATE:
            w("    if (rr_loop && rr_loop_on()) {")
            if f.name in PRE_HOOK:
                w(f"        rr_pre_{f.name}({args});")
            for p in f.params:
                if (f.name, p.name) in OUTPUT:
                    w(f"        rr_loop_output({p.name});")
                if (f.name, p.name) in CODEC and CODEC[(f.name, p.name)][0] == "config":
                    w(f"        rr_loop_config({p.name});")
            if f.name in LOOP_ASK:
                assert f.ret != "void" and f.name not in POST_HOOK and f.name not in PRE_HOOK, f.name
                w(f"        {f.ret} rr_ret = 0;")
                w(f"        const int rr_how = rr_loop_ask_{f.name}({', '.join([args, '&rr_ret'] if args else ['&rr_ret'])});")
                w("        if (rr_how != RR_LOOP_SKIP) {")
                w(f"            RwWriter *rw_w = rr_start({idn}, {sticky});")
                w("            if (rw_w) {")
                for l in encode(f, "                ", True):
                    w(l)
                w("            }")
                w(f"            if (rr_how == RR_LOOP_WAIT) rr_ret = ({f.ret})rr_loop_result({idn});")
                w(f"            else rr_loop_after({idn});")
                w("        }")
                w(f"        rr_loop_told_{f.name}({', '.join([args, 'rr_ret'] if args else ['rr_ret'])});")
            else:
                w(f"        RwWriter *rw_w = rr_start({idn}, {sticky});")
                w("        if (rw_w) {")
                for l in encode(f, "            ", True):
                    w(l)
                w("        }")
                if f.ret != "void":
                    w(f"        const {f.ret} rr_ret = ({f.ret})rr_loop_result({idn});")
                else:
                    w(f"        rr_loop_after({idn});")
            if f.name in POST_HOOK:
                post = ", ".join([a for a in [args, "rr_ret" if f.ret != "void" else ""] if a])
                w(f"        rr_post_{f.name}({post});")
            if f.ret != "void":
                w("        return rr_ret;")
            else:
                w("        return;")
            w("    }")
        if f.name in PRE_HOOK:
            w(f"    if (rr_active) rr_pre_{f.name}({args});")
        w("    const double rr_t = rr_active ? rr_clock() : 0;")
        if f.ret == "void":
            w(f"    {f.name}({args});")
        else:
            w(f"    {f.ret} rr_ret = {f.name}({args});")
        w(f"    if (rr_active) rr_call_time({idn}, rr_t);")
        w(f"    RwWriter *rw_w = rr_active ? rr_start({idn}, {sticky}) : NULL;")
        w("    if (rw_w) {")
        for l in encode(f, "        ", False):
            w(l)
        w("    }")
        if f.name in POST_HOOK:
            post = ", ".join([a for a in [args, "rr_ret" if f.ret != "void" else ""] if a])
            w(f"    if (rr_active) rr_post_{f.name}({post});")
        if f.ret != "void":
            w("    return rr_ret;")
        w("}")
    w("#endif")
    w("")

    # ---- replay ----
    w("#ifdef RW_GEN_REPLAY")
    w("/* Makes the call of one record again (r: its payload). Returns 0, or -1 on a short record. */")
    w("static int rw_replay_call(RwReplay *rp, uint32_t id, RwReader *r) {")
    w("    switch (id) {")
    for f in recorded:
        w(f"    case RW_ID_{f.name[9:].upper()}: {{")
        args = []
        for p in f.params:
            key = (f.name, p.name)
            if key in OUTPUT:
                w(f"        {p.type} a_{p.name} = NULL;")
                w(f"        (void)a_{p.name};")
                args.append("a_" + p.name)
                continue
            if p.kind == "scalar":
                fn = {"uint32_t": "rw_ru32", "int": "rw_ri32", "float": "rw_rf32", "int32_t": "rw_ri32", "uint64_t": "rw_ru64"}[p.type]
                if key in HANDLE_PARAMS:
                    w(f"        {p.type} a_{p.name} = rw_map_handle(rp, {fn}(r));")
                else:
                    w(f"        {p.type} a_{p.name} = {fn}(r);")
            elif key in CODEC:
                c = CODEC[key][0]
                if c in CODEC_PTR:
                    w(f"        const void *a_{p.name} = rw_get_{c}(r);")
                elif c == "config":
                    w(f"        RendererConfig a_{p.name}_v;")
                    w(f"        int a_{p.name}_surf;")
                    w(f"        const RendererConfig *a_{p.name} = rw_get_config(r, &a_{p.name}_v, &a_{p.name}_surf) ? &a_{p.name}_v : NULL;")
                    w(f"        rw_replay_config(rp, &a_{p.name}_v, a_{p.name}_surf);")
                elif c == "map":
                    t = [q for q in f.params if q.name == p.name][0].base()
                    w(f"        {t} a_{p.name}_v;")
                    w(f"        const {t} *a_{p.name} = rw_get_map(r, (RMaterialMap *)&a_{p.name}_v) ? &a_{p.name}_v : NULL;")
                elif c == "skin_normals":
                    w(f"        RSkinNormals a_{p.name}_v;")
                    w(f"        const RSkinNormals *a_{p.name} = rw_get_skin_normals(r, &a_{p.name}_v) ? &a_{p.name}_v : NULL;")
            else:   # void * converts to the parameter's pointer type (C)
                w(f"        const void *a_{p.name} = rw_rblob(r, NULL);")
            args.append("a_" + p.name)
        call = f"{f.name}({', '.join(args)})"
        if f.name in FRAME_TIME:
            w("        const double a_frame_time = rw_rf64(r);")
        if f.ret == "void":
            w(f"        if (r->bad) return -1;")
            w(f"        rw_replay_before(rp, id);")
            if f.name in FRAME_TIME:
                w("        renderer_set_frame_time(a_frame_time);")
            w(f"        {call};")
            w(f"        rw_replay_after(rp, id, 0, 0);")
        else:
            fn = {"uint32_t": "rw_ru32", "int": "rw_ri32", "float": "rw_rf32", "int32_t": "rw_ri32", "uint64_t": "rw_ru64"}[f.ret]
            w(f"        const {f.ret} want = {fn}(r);")
            w(f"        if (r->bad) return -1;")
            w(f"        rw_replay_before(rp, id);")
            if f.name == "renderer_end":
                w("        const int got = rw_replay_end(rp, a_pitch_bytes);")
            elif f.name == "renderer_present":
                w("        const int got = rw_replay_present(rp);")
            elif f.name == "renderer_init":
                w(f"        const {f.ret} got = rw_replay_init(rp, a_w, a_h);")
            else:
                w(f"        const {f.ret} got = {call};")
            handle = "1" if f.name in HANDLE_RESULT else "0"
            w(f"        rw_replay_after(rp, id, {handle}, rw_result_differs(rp, id, {handle}, (double)want, (double)got));")
        w("        return 0;")
        w("    }")
    w("    default:")
    w("        return -1;")
    w("    }")
    w("}")
    w("#endif")
    return "\n".join(o) + "\n"


# Game-side files that call routed renderer functions without render_rec.h (their calls reach this
# process's renderer, which --render-process helper never initialises). Known gaps only, with why.
UNROUTED_OK = {
    "settings_model.c": "names renderer_taa_render_size in a comment only",
    "hle_settings.c": "known gap: renderer_set_parallax (live parallax) does not reach the render64 helper",
}
ROUTE_SKIP = {"render_rec.c", "render_wire.c", "r64_selftest.c"}   # the wire itself (r64_selftest drives both sides)


def check_routing(text):
    """Every runtime/src/*.c that calls a routed renderer function includes render_rec.h (which turns
    the calls into the rr_* wrappers): a file that does not talks to an uninitialised renderer in
    --render-process helper. Returns the offending files."""
    routed = set(re.findall(r"^#define (\w+) rr_\1$", text, re.M))
    bad = []
    for f in sorted(SRC.glob("*.c")):
        if f.name in ROUTE_SKIP or f.name in UNROUTED_OK:
            continue
        code = re.sub(r"/\*.*?\*/|//[^\n]*", "", f.read_text(errors="replace"), flags=re.S)
        if '#include "render_rec.h"' in code:
            continue
        used = sorted({m for m in re.findall(r"\b(renderer_\w+|gamma_\w+)\s*\(", code) if m in routed})
        if used:
            bad.append(f"{f.name} ({', '.join(used[:4])}{', ...' if len(used) > 4 else ''})")
    return bad


def main():
    text = gen()
    if "--check" in sys.argv:
        if not OUT.exists() or OUT.read_text() != text:
            sys.exit(f"gen_wire: {OUT} is stale: run python3 runtime/tools/render64/gen_wire.py")
        bad = check_routing(text)
        if bad:
            sys.exit("gen_wire: renderer calls not routed through the render64 wire (add #include \"render_rec.h\" after "
                     "renderer.h): " + "; ".join(bad))
        return
    OUT.write_text(text)
    print(f"gen_wire: wrote {OUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
