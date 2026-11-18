/* Game-independent mod-layer test: mod.ini parsing, folder scan and load order, the --mod
 * options, and running hooks against a fake Lua C API (boot once per state, after.<script>
 * matching, a broken mod turned off while the others keep running, the stack restored). */
#include "../src/hle_mods.h"
#include "../src/colpatch.h"
#include "../src/meshover.h"
#include "../src/texlayer.h"
#include "../src/modini.h"
#include "../src/modcheck.h"
#include "../src/modsafe.h"
#include "../src/diaglog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

/* ---- fake Lua: a stack of (type, string) and the two globals the runner swaps ---- */
typedef struct { int t; char s[64]; } Val;
static Val stack[64];
static int top;
static Val g_err = {5, "game-handler"}, g_devlua_err = {1, ""};
static char ran[2048];   /* the chunk names run, space separated */

static int f_gettop(uint32_t L) { return top; }
static void f_settop(uint32_t L, int i) { top = i; }
static Val *at(int i) { return &stack[i < 0 ? top + i + 1 : i]; }
static void f_pushvalue(uint32_t L, int i) { stack[top + 1] = *at(i); top++; }
static int f_type(uint32_t L, int i) { return at(i)->t; }
static const char *f_tostring(uint32_t L, int i) { return at(i)->s; }
static Val *global(const char *n) { return !strcmp(n, "_ERRORMESSAGE") ? &g_err : !strcmp(n, "__devlua_err") ? &g_devlua_err : NULL; }
static void f_getglobal(uint32_t L, const char *n) { Val *g = global(n); stack[++top] = g ? *g : (Val){1, ""}; }
static void f_setglobal(uint32_t L, const char *n) { Val *g = global(n); if (g) *g = stack[top]; top--; }
static int f_dobuffer(uint32_t L, const char *b, size_t n, const char *name) {
    if (strstr(b, "_ERRORMESSAGE = function")) { g_err = (Val){5, "devlua-handler"}; g_devlua_err = (Val){1, ""}; return 0; }
    if (!strncmp(b, "MOD_ID", 6)) return 0;   /* the MOD_ID / MOD_SCRIPT prelude */
    size_t l = strlen(ran);
    snprintf(ran + l, sizeof ran - l, "%s%s", l ? " " : "", name);
    if (strstr(b, "error(")) { g_devlua_err = (Val){3, "boom"}; return 1; }
    stack[++top] = (Val){2, "1"};   /* a leftover result the runner must pop */
    return 0;
}
static const DevLuaApi api = {f_gettop, f_settop, f_pushvalue, f_type, f_tostring, f_getglobal, f_setglobal, f_dobuffer};

static char root[256];

static void put(const char *rel, const char *text) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    FILE *f = fopen(p, "wb");
    if (!f) { printf("FAIL cannot write %s\n", p); fails++; return; }
    fputs(text, f);
    fclose(f);
}
static void dir(const char *rel) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    mkdir(p, 0755);
}
static void count_warn(const char *msg, void *ud) { (*(int *)ud)++; }
static char problems[1024];   /* modcheck_files: "kind:name " */
static void note_problem(int kind, const char *name, void *ud) {
    size_t l = strlen(problems);
    snprintf(problems + l, sizeof problems - l, "%d:%s ", kind, name);
}
static char warnings[2048];   /* modcheck_report messages, one per line */
static void keep_warn(const char *msg, void *ud) {
    size_t l = strlen(warnings);
    snprintf(warnings + l, sizeof warnings - l, "%s\n", msg);
}
static int exists(const char *rel) {
    char p[512];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", root, rel);
    return stat(p, &st) == 0;
}
static int file_has(const char *rel, const char *text) {
    char p[512], b[512];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    return strstr(b, text) != NULL;
}
static const char *fake_profile = "";
static const char *get_profile(void) { return fake_profile; }
static char *slurp(const char *rel) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    static char b[96 * 1024];
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    return b;
}
static int parse_args(int argc, char **argv) {
    char err[192];
    for (int i = 1; i < argc; i++)
        if (mods_parse_arg(argc, argv, &i, err, sizeof err) != 1) return -1;
    return 0;
}

int main(void) {
    ModInfo m;
    char err[192];
    /* ---- mod.ini ---- */
    const char *ini = "; comment\n[mod]\nid = extra-hats\nname = More hats fly\nname.es = Mas sombreros\n"
                      "description = Hats fly off\nversion = 1.0\nauthor = Us\nsaves = no\n"
                      "boot = boot.lua\nafter.config_preload2 = hats.lua\r\nafter.level* = lv.lua\n";
    CHECK(modini_parse(ini, strlen(ini), &m, err, sizeof err) == 0, "ini parses");
    CHECK(!strcmp(m.id, "extra-hats") && !strcmp(m.name, "More hats fly") && !strcmp(m.name_es, "Mas sombreros") &&
              !strcmp(m.version, "1.0") && !strcmp(m.author, "Us") && m.saves == 0, "ini fields");
    CHECK(m.nhooks == 3 && !m.hooks[0].script[0] && !strcmp(m.hooks[0].file, "boot.lua") &&
              !strcmp(m.hooks[1].script, "config_preload2") && !strcmp(m.hooks[1].file, "hats.lua") &&
              !strcmp(m.hooks[2].script, "level*"), "ini hooks in file order");
    CHECK(modini_parse("id = a\nversion = 1\n", 19, &m, err, sizeof err) < 0 && strstr(err, "name"), "missing name");
    CHECK(modini_parse("id = A B\nname = x\nversion = 1\n", 30, &m, err, sizeof err) < 0 && strstr(err, "id"), "bad id");
    const char *esc = "id = a\nname = x\nversion = 1\nboot = ../x.lua\n";
    CHECK(modini_parse(esc, strlen(esc), &m, err, sizeof err) < 0 && strstr(err, "boot"), "file outside the folder");
    const char *ms = "id = a\nname = x\nversion = 1\nmeshes = local\n";
    CHECK(modini_parse(ms, strlen(ms), &m, err, sizeof err) == 0 && !strcmp(m.meshes, "local"), "meshes folder");
    const char *mx = "id = a\nname = x\nversion = 1\nmeshes = ../x\n";
    CHECK(modini_parse(mx, strlen(mx), &m, err, sizeof err) < 0 && strstr(err, "meshes"), "meshes outside the folder");
    CHECK(modini_parse(ini, strlen(ini), &m, err, sizeof err) == 0 && !m.meshes[0], "no meshes folder by default");
    const char *sv = "id = a\nname = x\nversion = 1\nsaves = yes\n";
    CHECK(modini_parse(sv, strlen(sv), &m, err, sizeof err) == 0 && m.saves == 1, "saves = yes");
    CHECK(modini_match("level*", "level95") && modini_match("level95", "level95") && !modini_match("level95", "level9") &&
              !modini_match("level9", "level95") && modini_match("*", "x"), "patterns");

    /* ---- folder scan: sorted by folder, broken and duplicate ones skipped ---- */
    snprintf(root, sizeof root, "%s/mods_test_XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(root)) { printf("FAIL mkdtemp\n"); return 1; }
    dir("b-second"); dir("a-first"); dir("c-broken"); dir("d-dup"); dir("e-noini"); dir("f-bad");
    put("a-first/mod.ini", "id = first\nname = First\nversion = 1\nboot = boot.lua\nafter.level* = lv.lua\n");
    put("a-first/boot.lua", "x = 1");
    put("a-first/lv.lua", "y = 1");
    put("b-second/mod.ini", "id = second\nname = Second\nversion = 2\nafter.level95 = lv.lua\n");
    put("b-second/lv.lua", "z = 1");
    put("c-broken/mod.ini", "id = broken\nname = Broken\nversion = 1\nboot = boot.lua\nafter.level95 = lv.lua\n");
    put("c-broken/boot.lua", "error(\"boom\")");
    put("c-broken/lv.lua", "w = 1");
    put("d-dup/mod.ini", "id = first\nname = Dup\nversion = 1\n");
    put("f-bad/mod.ini", "name = no id\n");
    put("g-file.txt", "not a folder");
    ModInfo list[8];
    int warned = 0;
    CHECK(modini_scan(root, list, 8, count_warn, &warned) == 3, "scan: 3 valid mods");
    CHECK(!strcmp(list[0].id, "first") && !strcmp(list[1].id, "second") && !strcmp(list[2].id, "broken"),
          "scan order = folder names; duplicate id and invalid ini skipped");
    CHECK(warned == 2, "duplicate id and invalid ini reported");

    /* ---- options ---- */
    char *a1[] = {"x", "--mods", root, "--mod", "second,broken", "--mod", "first"};
    mods_reset();
    CHECK(parse_args(7, a1) == 0, "--mods/--mod parse");
    CHECK(mods_load("/nonexistent") == 3, "3 enabled mods loaded");
    CHECK(mods_count() == 3 && !strcmp(mods_get(0)->id, "first") && !strcmp(mods_get(1)->id, "second") &&
              !strcmp(mods_get(2)->id, "broken"), "load order follows folders, not the --mod order");
    char *a2[] = {"x", "--mods", root, "--mod", "nope"};
    mods_reset();
    CHECK(parse_args(5, a2) == 0 && mods_load(NULL) == 0, "unknown id: nothing loaded");
    char *a3[] = {"x", "--mods", root, "--mod", "all", "--no-mods"};
    mods_reset();
    CHECK(parse_args(6, a3) == 0 && mods_load(NULL) == 0, "--no-mods wins");
    char *a4[] = {"x", "--mod", "Bad Id"};
    mods_reset();
    CHECK(parse_args(3, a4) < 0, "--mod with an invalid id is rejected");
    char *a5[] = {"x", "--mods"};
    mods_reset();
    CHECK(parse_args(2, a5) < 0, "--mods without a value");

    /* ---- hooks against the fake Lua ---- */
    char *a6[] = {"x", "--mods", root, "--mod", "all"};
    mods_reset();
    CHECK(parse_args(5, a6) == 0 && mods_load(NULL) == 3, "all: 3 mods");
    top = 5;
    ran[0] = 0;
    mods_on_script(&api, 0x1000, "config_preload");
    CHECK(!strcmp(ran, "=first/boot.lua =broken/boot.lua"), "boot hooks once, in order");
    CHECK(top == 5 && !strcmp(g_err.s, "game-handler"), "stack and _ERRORMESSAGE restored");
    CHECK(mods_failed("broken") && !mods_failed("first"), "the broken mod is off, the others are not");
    ran[0] = 0;
    mods_on_script(&api, 0x1000, "level95.lua");
    CHECK(!strcmp(ran, "=first/lv.lua =second/lv.lua"), "after.level* and after.level95; broken mod skipped");
    ran[0] = 0;
    mods_on_script(&api, 0x1000, "=level95_coney.lua");
    CHECK(!strcmp(ran, "=first/lv.lua"), "prefix pattern only");
    ran[0] = 0;
    mods_on_script(&api, 0x2000, "config_preload");
    CHECK(!strcmp(ran, "=first/boot.lua"), "a new Lua state runs boot again");
    CHECK(top == 5, "stack still balanced");

    /* ---- mod data (ModSave / ModLoad): <hdd>/mods-data/<id>.json, per save profile ---- */
    {
        char v[MODS_VALUE_MAX + 8], e[300], big[MODS_VALUE_MAX + 2];
        struct stat st;
        mods_set_storage(NULL, get_profile);
        CHECK(mods_data_save("gym", "best.1", "9.5", e, sizeof e) < 0 && strstr(e, "hdd"), "data: no folder, no write");
        mods_set_storage(root, get_profile);
        CHECK(mods_data_load("gym", "best.1", v, sizeof v, e, sizeof e) == 0, "data: nothing stored yet");
        CHECK(mods_data_save("gym", "best.1", "9.5", e, sizeof e) == 0, "data: save before a profile loads");
        fake_profile = "1A2B3C4D5E6F";
        CHECK(mods_data_load("gym", "best.1", v, sizeof v, e, sizeof e) == 0, "data: another profile does not see it");
        CHECK(mods_data_save("gym", "best.1", "8.25", e, sizeof e) == 0 && mods_data_save("gym", "best.2", "say \"hi\"\n", e, sizeof e) == 0,
              "data: two keys in a profile");
        CHECK(mods_data_load("gym", "best.1", v, sizeof v, e, sizeof e) == 1 && !strcmp(v, "8.25"), "data: load in that profile");
        CHECK(mods_data_load("gym", "best.2", v, sizeof v, e, sizeof e) == 1 && !strcmp(v, "say \"hi\"\n"), "data: quotes and newlines round-trip");
        CHECK(mods_data_save("gym", "best.1", "7", e, sizeof e) == 0 && mods_data_load("gym", "best.1", v, sizeof v, e, sizeof e) == 1 &&
              !strcmp(v, "7"), "data: replace a value");
        fake_profile = "";
        CHECK(mods_data_load("gym", "best.1", v, sizeof v, e, sizeof e) == 1 && !strcmp(v, "9.5"), "data: profile \"none\" kept its value");
        fake_profile = "bad/../name";
        CHECK(mods_data_load("gym", "best.1", v, sizeof v, e, sizeof e) == 1 && !strcmp(v, "9.5"), "data: an invalid profile id is \"none\"");
        fake_profile = "1A2B3C4D5E6F";
        const char *txt = slurp("mods-data/gym.json");
        CHECK(txt && !strcmp(txt, "{\n  \"none\": {\n    \"best.1\": \"9.5\"\n  },\n  \"1A2B3C4D5E6F\": {\n    \"best.1\": \"7\",\n"
                                  "    \"best.2\": \"say \\\"hi\\\"\\u000a\"\n  }\n}\n"),
              "data: the JSON file, grouped by profile");
        CHECK(stat(root, &st) == 0 && slurp("mods-data/gym.json.tmp") == NULL, "data: no temporary file left");
        CHECK(mods_data_save("gym", "best.2", NULL, e, sizeof e) == 0 && mods_data_load("gym", "best.2", v, sizeof v, e, sizeof e) == 0,
              "data: nil removes a key");
        CHECK(mods_data_save("gym", "nope", NULL, e, sizeof e) == 0, "data: removing a missing key is fine");
        CHECK(mods_data_save("gym", "a b", "1", e, sizeof e) < 0 && strstr(e, "key"), "data: key with a space refused");
        CHECK(mods_data_save("gym", "../x", "1", e, sizeof e) < 0 && mods_data_save("gym", "", "1", e, sizeof e) < 0, "data: path-like and empty keys refused");
        CHECK(mods_data_load("gym", "x/y", v, sizeof v, e, sizeof e) < 0, "data: load checks the key too");
        CHECK(mods_data_save("Bad Id", "k", "1", e, sizeof e) < 0 && mods_data_save(NULL, "k", "1", e, sizeof e) < 0, "data: invalid mod id refused");
        memset(big, 'x', sizeof big - 1);
        big[sizeof big - 1] = 0;
        CHECK(mods_data_save("gym", "big", big, e, sizeof e) < 0 && strstr(e, "longer"), "data: value over the limit refused");
        int saved = 0;
        char k[16];
        for (int i = 0; i < 80; i++) {
            snprintf(k, sizeof k, "k%d", i);
            if (mods_data_save("cap", k, big + 1, e, sizeof e)) break;
            saved++;
        }
        CHECK(saved > 50 && saved < 80 && strstr(e, "larger"), "data: a file stops at 64 KiB");
        CHECK(stat(root, &st) == 0 && mods_data_load("cap", "k0", v, sizeof v, e, sizeof e) == 1, "data: the full file is still readable");
        CHECK(mods_data_save("other", "best.1", "1", e, sizeof e) == 0 && mods_data_load("gym", "best.1", v, sizeof v, e, sizeof e) == 1 &&
              !strcmp(v, "7"), "data: one file per mod");
        put("mods-data/gym.json", "{ \"x\": broken");
        CHECK(mods_data_save("gym", "best.1", "1", e, sizeof e) < 0 && strstr(e, "left as it is"), "data: unreadable file refused");
        txt = slurp("mods-data/gym.json");
        CHECK(txt && !strcmp(txt, "{ \"x\": broken"), "data: and not overwritten");
        CHECK(mods_data_load("gym", "best.1", v, sizeof v, e, sizeof e) < 0, "data: load reports it");
        put("mods-data/gym.json", "{\"P1\":{\"k\":\"caf\\u00e9 \\/\"}}");
        fake_profile = "P1";
        CHECK(mods_data_load("gym", "k", v, sizeof v, e, sizeof e) == 1 && !strcmp(v, "caf\xc3\xa9 /"), "data: compact JSON with escapes is read");
        mods_set_storage(NULL, NULL);
    }

    /* ---- mesh-override groups (ModMeshSet): subfolders of a mod's meshes folder, off at start ---- */
    dir("m"); dir("m/h-meshy"); dir("m/h-meshy/local"); dir("m/h-meshy/local/knocked"); dir("m/h-meshy/local/Bad Name");
    put("m/h-meshy/mod.ini", "id = meshy\nname = Meshy\nversion = 1\nmeshes = local\n");
    put("m/h-meshy/local/00000000000000aa.hide", "");
    put("m/h-meshy/local/knocked/00000000000000aa.hide", "");
    put("m/h-meshy/local/knocked/00000000000000bb-00000000000000cc.hide", "");
    put("m/h-meshy/local/Bad Name/00000000000000dd.hide", "");
    put("m/h-meshy/local/00000000000000ee@dirt-1.png", "");
    put("m/h-meshy/local/00000000000000ff@dirt-1.png", "");
    put("m/h-meshy/local/knocked/00000000000000ee@sub.png", "");   /* layers live in the folder itself */
    char mroot[300];
    snprintf(mroot, sizeof mroot, "%s/m", root);
    char *a7[] = {"x", "--mods", mroot, "--mod", "meshy"};
    mods_reset();
    CHECK(parse_args(5, a7) == 0 && mods_load(NULL) == 1 && meshover_count() == 3, "meshes: the folder and its valid group, 3 overrides");
    const MeshOverride *o = meshover_find(0xaa, 0, 0);
    CHECK(o && o->hide && !o->group && !meshover_find(0xbb, 0xcc, 1) && !meshover_find(0xdd, 0, 0),
          "groups start off: only the folder's own override is used");
    CHECK(mods_mesh_set("meshy/knocked", 1) == 2 && meshover_group_on("meshy/knocked") == 1, "ModMeshSet on: 2 overrides");
    o = meshover_find(0xaa, 0, 0);
    CHECK(o && o->group && meshover_find(0xbb, 0xcc, 1), "an enabled group wins over the folder's own override");
    CHECK(mods_mesh_set("meshy/knocked", 0) == 2 && meshover_group_on("meshy/knocked") == 0 && !meshover_find(0xbb, 0xcc, 1) &&
              !meshover_find(0xaa, 0, 0)->group, "ModMeshSet off: back to the folder's own");
    CHECK(mods_mesh_set("meshy/nope", 1) == -1 && mods_mesh_set("knocked", 1) == -1 && mods_mesh_set(NULL, 1) == -1,
          "unknown groups (the name includes the mod id)");
    /* ModMeshPoint: world points of a group by key; every point dropped when a new Lua state boots */
    const float pt[3] = {1, 2, 3};
    CHECK(mods_mesh_point("meshy/knocked", "4711", pt) == 1 && mods_mesh_point("meshy/knocked", "4712", pt) == 2 &&
              mods_mesh_point("meshy/knocked", "4711", NULL) == 1, "ModMeshPoint: set two, remove one");
    CHECK(mods_mesh_point("meshy/nope", "1", pt) == -1, "ModMeshPoint: unknown group");
    mods_on_script(&api, 0x3000, "config_preload");
    CHECK(!meshover_points_any(), "a new Lua state drops the points (their keys are the old state's handles)");
    CHECK(texlayer_count() == 2 && texlayer_on("meshy/dirt-1") == 0 && texlayer_on("meshy/sub") == -1,
          "texture layers: the folder's 2 files, off at start");
    CHECK(mods_tex_layer("meshy/dirt-1", 1) == 2 && texlayer_on("meshy/dirt-1") == 1 && texlayer_select(0xee) && texlayer_select(0xff) &&
              !texlayer_select(0xaa), "ModTexLayer on: its 2 textures get the layer");
    CHECK(mods_tex_layer("meshy/dirt-1", 0) == 2 && !texlayer_select(0xee), "ModTexLayer off");
    CHECK(mods_tex_layer("meshy/nope", 1) == -1 && mods_tex_layer("dirt-1", 1) == -1 && mods_tex_layer(NULL, 1) == -1,
          "unknown texture layers (the name includes the mod id)");
    mods_reset();
    CHECK(meshover_count() == 0 && meshover_group_on("meshy/knocked") == -1 && texlayer_count() == 0, "reset drops the overrides, groups and layers");

    /* ---- safety (modcheck.h): min_runtime, the folder manifest, leftovers and missing files ---- */
    const char *mr = "id = a\nname = x\nversion = 1\nmin_runtime = 1.1\nfiles = notes.txt, data*\n";
    CHECK(modini_parse(mr, strlen(mr), &m, err, sizeof err) == 0 && !strcmp(m.min_runtime, "1.1") &&
              !strcmp(m.files, "notes.txt, data*"), "min_runtime and files parse");
    const char *mrx = "id = a\nname = x\nversion = 1\nmin_runtime = new\n";
    CHECK(modini_parse(mrx, strlen(mrx), &m, err, sizeof err) < 0 && strstr(err, "min_runtime"), "min_runtime must be a version");
    const char *mrd = "id = a\nname = x\nversion = 1\nmin_runtime = 1..2\n";
    CHECK(modini_parse(mrd, strlen(mrd), &m, err, sizeof err) < 0, "min_runtime 1..2 is not a version");
    const char *fx = "id = a\nname = x\nversion = 1\nfiles = ok.txt, ../up.txt\n";
    CHECK(modini_parse(fx, strlen(fx), &m, err, sizeof err) < 0 && strstr(err, "files"), "files outside the folder");
    CHECK(modcheck_version_cmp("1.10", "1.9") == 1 && modcheck_version_cmp("1.1", "1.1.0") == 0 &&
              modcheck_version_cmp("1", "1.1") == -1 && modcheck_version_cmp("2.0", "10") == -1, "versions compare by number");
    ModInfo cm;
    memset(&cm, 0, sizeof cm);
    CHECK(modcheck_runtime_ok(&cm), "no min_runtime: loads");
    snprintf(cm.min_runtime, sizeof cm.min_runtime, "%s", MOD_RUNTIME_VERSION);
    CHECK(modcheck_runtime_ok(&cm), "min_runtime = this runtime: loads");
    snprintf(cm.min_runtime, sizeof cm.min_runtime, "99.0");
    CHECK(!modcheck_runtime_ok(&cm), "min_runtime newer than this runtime: refused");

    dir("s"); dir("s/a-stale"); dir("s/a-stale/meshes-v2"); dir("s/a-stale/meshes"); dir("s/b-new"); dir("s/c-nolua");
    put("s/a-stale/mod.ini", "id = stale\nname = Stale\nversion = 1.2\nmeshes = meshes-v2\nboot = boot.lua\n"
                             "after.level95 = boot.lua\nfiles = notes.txt, data*, readme.txt\n");
    put("s/a-stale/boot.lua", "x = 1");
    put("s/a-stale/old.lua", "x = 0");          /* version 1.1's file */
    put("s/a-stale/meshes/00000000000000aa.hide", "");   /* version 1.1's meshes folder */
    put("s/a-stale/data-1.bin", "");
    put("s/a-stale/notes.txt", "");
    put("s/a-stale/.hidden", "");
    put("s/b-new/mod.ini", "id = new\nname = New\nversion = 1\nmin_runtime = 99\nboot = boot.lua\n");
    put("s/b-new/boot.lua", "x = 1");
    put("s/c-nolua/mod.ini", "id = nolua\nname = No Lua\nversion = 1\nboot = boot.lua\nafter.level95 = gone.lua\n");
    put("s/c-nolua/boot.lua", "x = 1");
    char sroot[300];
    snprintf(sroot, sizeof sroot, "%s/s", root);
    ModInfo sm[4];
    CHECK(modini_scan(sroot, sm, 4, NULL, NULL) == 3, "safety: 3 mods");
    problems[0] = 0;
    CHECK(modcheck_files(sroot, &sm[0], note_problem, NULL) == 3 && !strcmp(problems, "1:meshes 1:old.lua 3:readme.txt "),
          "leftovers (an old meshes folder, an old Lua file) and a missing listed file; meshes-v2, data*, hidden names are fine");
    warnings[0] = 0;
    CHECK(modcheck_report(sroot, &sm[0], keep_warn, NULL) == 1 &&
              strstr(warnings, "stale: \"meshes\" in its folder is not part of version 1.2 (left from an older version?)") &&
              strstr(warnings, "stale: missing file readme.txt"), "leftovers are reported and the mod still loads");
    warnings[0] = 0;
    CHECK(modcheck_report(sroot, &sm[1], keep_warn, NULL) == 0 &&
              strstr(warnings, "new: needs mod runtime 99 or newer (this game has " MOD_RUNTIME_VERSION "); mod not loaded"),
          "min_runtime: refused with a clear message");
    warnings[0] = 0;
    CHECK(modcheck_report(sroot, &sm[2], keep_warn, NULL) == 0 &&
              strstr(warnings, "nolua: missing file gone.lua (named in its mod.ini); mod not loaded"),
          "a missing Lua file: refused");
    char *a8[] = {"x", "--mods", sroot, "--mod", "all"};
    mods_reset();
    CHECK(parse_args(5, a8) == 0 && mods_load(NULL) == 1 && !strcmp(mods_get(0)->id, "stale"),
          "mods_load: only the mod that passes the checks loads");
    mods_reset();

    /* ---- collision patches (colpatch.h): the collision key, its file in the folder, registration ---- */
    const char *ck = "id = a\nname = x\nversion = 1\ncollision = cage.col\n";
    CHECK(modini_parse(ck, strlen(ck), &m, err, sizeof err) == 0 && !strcmp(m.collision, "cage.col"), "collision key parses");
    const char *ckx = "id = a\nname = x\nversion = 1\ncollision = ../cage.col\n";
    CHECK(modini_parse(ckx, strlen(ckx), &m, err, sizeof err) < 0 &&
              strstr(err, "line 4: collision: \"../cage.col\" must be a file name inside the mod's folder"), "collision outside the folder");
    dir("c"); dir("c/a-col"); dir("c/b-gone"); dir("c/c-bad");
    put("c/a-col/mod.ini", "id = col\nname = Col\nversion = 1\ncollision = cage.col\n");
    put("c/a-col/cage.col", "level = level95\nfloor 0 0 1 1 0\n");
    put("c/b-gone/mod.ini", "id = gone\nname = Gone\nversion = 1\ncollision = cage.col\n");
    put("c/c-bad/mod.ini", "id = bad\nname = Bad\nversion = 1\ncollision = cage.col\n");
    put("c/c-bad/cage.col", "floor 0 0 1 1 0\n");
    char croot[300];
    snprintf(croot, sizeof croot, "%s/c", root);
    ModInfo cmods[3];
    CHECK(modini_scan(croot, cmods, 3, NULL, NULL) == 3, "collision: 3 mods");
    problems[0] = 0;
    CHECK(modcheck_files(croot, &cmods[0], note_problem, NULL) == 0, "the collision file is part of the mod (no leftover)");
    problems[0] = 0;
    CHECK(modcheck_files(croot, &cmods[1], note_problem, NULL) == 1 && !strcmp(problems, "3:cage.col "), "a missing collision file is reported");
    char *ac1[] = {"x", "--mods", croot, "--mod", "all"};
    mods_reset();
    colpatch_reset();
    CHECK(parse_args(5, ac1) == 0 && mods_load(NULL) == 3 && colpatch_count() == 1,
          "mods_load registers the valid patch; mods with a missing or bad patch still load without it");
    mods_reset();
    colpatch_reset();
    char *ac2[] = {"x", "--mods", croot, "--mod", "bad"};
    CHECK(parse_args(5, ac2) == 0 && mods_load(NULL) == 1 && colpatch_count() == 0, "only enabled mods' patches are registered");
    mods_reset();

    /* ---- save backup (modsafe.h): <hdd>/mods-backups/<stamp>/save + backup.txt, newest 2 kept ---- */
    dir("hdd"); dir("hdd/partition1"); dir("hdd/partition1/UDATA"); dir("hdd/partition1/UDATA/5454009b");
    dir("hdd/partition1/UDATA/5454009b/1A2B3C4D5E6F"); dir("hdd/partition1/UDATA/other");
    char hdd[300], out[1300];
    snprintf(hdd, sizeof hdd, "%s/hdd", root);
    CHECK(modsafe_backup(hdd, "20261117-100000", 2, out, sizeof out) == MODSAFE_NOSAVE && modsafe_count(hdd) == 0,
          "backup: an empty save folder is not backed up");
    put("hdd/partition1/UDATA/5454009b/TitleMeta.xbx", "meta");
    put("hdd/partition1/UDATA/5454009b/1A2B3C4D5E6F/SaveMeta.xbx", "save v1");
    put("hdd/partition1/UDATA/other/x", "another title");
    CHECK(modsafe_backup(hdd, "20261117-100000", 2, out, sizeof out) == MODSAFE_DONE && strstr(out, "/mods-backups/20261117-100000") &&
              file_has("hdd/mods-backups/20261117-100000/save/1A2B3C4D5E6F/SaveMeta.xbx", "save v1") &&
              file_has("hdd/mods-backups/20261117-100000/save/TitleMeta.xbx", "meta") &&
              file_has("hdd/mods-backups/20261117-100000/backup.txt", "kind = mods\nfiles = 2\nbytes = 11\n") &&
              !exists("hdd/mods-backups/20261117-100000/save/other") && !exists("hdd/mods-backups/20261117-100000.partial"),
          "backup: the title's save folder only, in the launcher's layout");
    CHECK(modsafe_backup(hdd, "20261117-100100", 2, out, sizeof out) == MODSAFE_UNCHANGED && strstr(out, "20261117-100000") &&
              modsafe_count(hdd) == 1, "backup: an unchanged save is not copied again");
    put("hdd/partition1/UDATA/5454009b/1A2B3C4D5E6F/SaveMeta.xbx", "save v2!");
    CHECK(modsafe_backup(hdd, "20261117-100000", 2, out, sizeof out) == MODSAFE_DONE && strstr(out, "20261117-100000-2") &&
              modsafe_count(hdd) == 2, "backup: a changed save is copied; a taken name gets -2");
    dir("hdd/mods-backups/20261117-090000.partial");   /* a copy cut short */
    put("hdd/partition1/UDATA/5454009b/1A2B3C4D5E6F/SaveMeta.xbx", "save v3!!");
    CHECK(modsafe_backup(hdd, "20261117-110000", 2, out, sizeof out) == MODSAFE_DONE && modsafe_count(hdd) == 2 &&
              !exists("hdd/mods-backups/20261117-100000") && exists("hdd/mods-backups/20261117-100000-2") &&
              exists("hdd/mods-backups/20261117-110000") && !exists("hdd/mods-backups/20261117-090000.partial"),
          "backup: the newest 2 are kept; unfinished copies are removed");
    CHECK(file_has("hdd/partition1/UDATA/5454009b/1A2B3C4D5E6F/SaveMeta.xbx", "save v3!!") &&
              file_has("hdd/partition1/UDATA/other/x", "another title"), "backup: the live save is only read");

    /* mods_safety_start: backs up only when a loaded mod has saves = yes; the crash-context line */
    dir("p"); dir("p/a-money"); dir("p/b-look");
    put("p/a-money/mod.ini", "id = money\nname = Money\nversion = 1.0\nsaves = yes\nboot = boot.lua\n");
    put("p/a-money/boot.lua", "x = 1");
    put("p/b-look/mod.ini", "id = look\nname = Look\nversion = 2.1\nboot = boot.lua\n");
    put("p/b-look/boot.lua", "x = 1");
    char proot[300], ctx[512];
    snprintf(proot, sizeof proot, "%s/p", root);
    put("hdd/partition1/UDATA/5454009b/1A2B3C4D5E6F/SaveMeta.xbx", "save v4!!!");
    char *a9[] = {"x", "--mods", proot, "--mod", "look"};
    mods_reset();
    dlog_reset();
    CHECK(parse_args(5, a9) == 0 && mods_load(NULL) == 1, "safety start: a mod that does not change the save");
    mods_safety_start(hdd);
    dlog_context_format(ctx, sizeof ctx, 0);
    CHECK(modsafe_count(hdd) == 2 && exists("hdd/mods-backups/20261117-110000") && strstr(ctx, "[crash-context] mods: look 2.1\n"),
          "safety start: no backup without a save-changing mod; the mods are in the crash context");
    char *a10[] = {"x", "--mods", proot, "--mod", "money,look"};
    mods_reset();
    CHECK(parse_args(5, a10) == 0 && mods_load(NULL) == 2, "safety start: a save-changing mod");
    mods_safety_start(hdd);
    dlog_context_format(ctx, sizeof ctx, 0);
    CHECK(modsafe_count(hdd) == 3 && strstr(ctx, "[crash-context] mods: money 1.0, look 2.1\n"),
          "safety start: backed up before the game (MODSAFE_KEEP = 5 kept)");
    mods_reset();
    dlog_reset();

    /* The bundled example mods (mods/ at the repository root, found from runtime/ or the root):
     * every mod.ini parses and names files that exist; the hats and trophies mods hook boot
     * (they rewrite the game's own CfgObj calls) before their later hooks. */
    const char *bundled = NULL;
    struct stat sb;
    if (stat("../mods/10-extra-hats/mod.ini", &sb) == 0) bundled = "../mods";
    else if (stat("mods/10-extra-hats/mod.ini", &sb) == 0) bundled = "mods";
    if (!bundled) printf("note: bundled mods/ not found from here; skipped\n");
    else {
        ModInfo bm[16];
        int bw = 0, n = modini_scan(bundled, bm, 16, count_warn, &bw);
        CHECK(n == 13 && bw == 0, "bundled: 13 valid mods");
        for (int i = 0; i < n; i++)
            for (int h = 0; h < bm[i].nhooks; h++) {
                char p[400];
                snprintf(p, sizeof p, "%s/%s/%s", bundled, bm[i].folder, bm[i].hooks[h].file);
                CHECK(stat(p, &sb) == 0, "bundled: every hook file exists");
            }
        for (int i = 0; i < n; i++) {
            problems[0] = 0;
            int np = modcheck_files(bundled, &bm[i], note_problem, NULL);
            if (np) printf("FAIL bundled %s: %s\n", bm[i].id, problems);
            CHECK(np == 0 && modcheck_runtime_ok(&bm[i]), "bundled: no leftovers or missing files, loadable by this runtime");
        }
        CHECK(n >= 2 && !strcmp(bm[0].id, "extra-hats") && bm[0].nhooks == 2 && !bm[0].hooks[0].script[0] &&
              !strcmp(bm[0].hooks[1].script, "config_preload2"), "bundled: extra-hats = boot, after.config_preload2");
        CHECK(n >= 2 && !strcmp(bm[1].id, "hideout-trophies") && bm[1].nhooks == 2 && !bm[1].hooks[0].script[0] &&
              !strcmp(bm[1].hooks[0].file, "config.lua") && !strcmp(bm[1].hooks[1].script, "level95") && !bm[1].saves &&
              !strcmp(bm[1].meshes, "meshes-v1"),
              "bundled: hideout-trophies = boot config.lua, after.level95, saves = no, meshes = meshes-v1");
        CHECK(n == 13 && !strcmp(bm[10].id, "dirty-clothes") && bm[10].nhooks == 3 && !bm[10].hooks[0].script[0] &&
                  !strcmp(bm[10].hooks[1].script, "level*") && !strcmp(bm[10].hooks[2].script, "level95") && !bm[10].saves &&
                  !strcmp(bm[10].meshes, "layers-v1"),
              "bundled: dirty-clothes = boot clothes.lua, after.level* start.lua, after.level95 hideout.lua, saves = no, meshes = layers-v1");
        CHECK(n == 13 && !strcmp(bm[6].id, "gym-records") && bm[6].nhooks == 2 && !strcmp(bm[6].hooks[0].script, "level95") &&
              !strcmp(bm[6].hooks[1].script, "level95_workout") && !strcmp(bm[6].hooks[0].file, "records.lua") && !bm[6].saves,
              "bundled: gym-records = after.level95 and after.level95_workout, saves = no");
        CHECK(n == 13 && !strcmp(bm[9].id, "scars") && bm[9].nhooks == 2 && !strcmp(bm[9].hooks[0].file, "scars.lua") &&
              !strcmp(bm[9].hooks[1].file, "scars.lua") && !strcmp(bm[9].hooks[1].script, "level*") && !bm[9].hooks[0].script[0] && !bm[9].saves,
              "bundled: scars = boot and after.level* scars.lua, saves = no");
        CHECK(n == 13 && !strcmp(bm[8].id, "wounds") && bm[8].nhooks == 1 && !bm[8].hooks[0].script[0] &&
              !strcmp(bm[8].hooks[0].file, "wounds.lua") && !bm[8].saves && !strcmp(bm[8].min_runtime, "1.3"),
              "bundled: wounds = boot wounds.lua, saves = no, min_runtime 1.3");
        CHECK(n == 13 && !strcmp(bm[11].id, "police-memory") && bm[11].nhooks == 1 && !bm[11].hooks[0].script[0] &&
              !strcmp(bm[11].hooks[0].file, "police_memory.lua") && !bm[11].saves,
              "bundled: police-memory = boot police_memory.lua, saves = no");
    }

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -r -- '%s'", root);
    if (system(cmd) != 0) printf("note: could not remove %s\n", root);
    printf(fails ? "%d FAILED\n" : "ALL OK\n", fails);
    return fails ? 1 : 0;
}
