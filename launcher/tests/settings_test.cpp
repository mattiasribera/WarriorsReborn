/* Game-independent test of the in-game settings model against the launcher:
 * every profile.cpp key is a model setting and back, defaults, i18n ids, the command line of every
 * preset and of every value of every setting identical to build_argv, and the keys each preset sets.
 * And the launcher sync (W7): the pending file merge, the presets file, the game's environment, the
 * restart policy and a child that asks for a restart. No game data; scratch files go to
 * launcher/build/settings-scratch (next to this binary). */
#include "../src/command.h"
#include "../src/i18n.h"
#include "../src/keymap.h"
#include "../src/platform.h"
#include "../src/profile.h"
#include "../src/settings_sync.h"
#include "../../runtime/src/settings_model.h"
#include <cstdio>
#include <sys/stat.h>
#include <cstring>
#include <map>
#include <set>

using namespace launcher;

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static std::string join(const std::vector<std::string> &v) {
    std::string s;
    for (const std::string &a : v) s += (s.empty() ? "" : "|") + a;
    return s;
}

/* The [profile] section's key = value lines of p, as write_ini writes them (file order). */
static std::vector<std::pair<std::string, std::string>> ini_pairs(const Profile &p) {
    ProfileFile f;
    f.profiles.push_back(p);
    f.selected = p.name;
    std::string text = write_ini(f), section = "[profile " + p.name + "]\n";
    std::vector<std::pair<std::string, std::string>> out;
    size_t at = text.find(section);
    if (at == std::string::npos) return out;
    for (size_t pos = at + section.size(); pos < text.size();) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl - pos);
        pos = nl == std::string::npos ? text.size() : nl + 1;
        if (line.empty() || line[0] == '[') break;
        size_t eq = line.find(" = ");
        if (eq == std::string::npos) eq = line.find(" =");
        out.push_back({line.substr(0, eq), eq + 3 <= line.size() ? line.substr(eq + 3) : ""});
    }
    return out;
}

/* The model's values of p (every key of its ini section). */
static bool values_of(const Profile &p, SmValues &v) {
    if (sm_values_init(&v)) return false;
    bool ok = true;
    for (const auto &kv : ini_pairs(p))
        if (sm_set(&v, kv.first.c_str(), kv.second.c_str()) == -2) ok = false, printf("     sm_set %s = '%s' refused\n", kv.first.c_str(), kv.second.c_str());
    return ok;
}

static std::string model_argv(const Profile &p) {
    SmValues v;
    if (!values_of(p, v)) return "<values refused>";
    SmArgv a = {};
    std::string s = sm_build_argv(&v, &a) ? "<oom>" : "";
    std::vector<std::string> args;
    for (int i = 0; i < a.n; i++) args.push_back(a.a[i]);
    sm_argv_free(&a);
    sm_values_free(&v);
    return s + join(args);
}

static void test_keys() {
    Profile p;
    std::set<std::string> ini;
    for (const auto &kv : ini_pairs(p)) ini.insert(kv.first);
    int missing = 0, extra = 0;
    for (const std::string &k : ini)
        if (sm_find(k.c_str()) < 0) missing++, printf("     profile key %s has no model setting\n", k.c_str());
    for (int i = 0; i < sm_count(); i++)
        if (!ini.count(sm_setting(i)->key)) extra++, printf("     model setting %s is no profile key\n", sm_setting(i)->key);
    CHECK(ini.size() > 100 && !missing, "keys: every profile.cpp key (%zu) is a model setting (none excluded)", ini.size());
    CHECK(!extra, "keys: every model setting is a profile.cpp key");
    /* the file order of profile.cpp is not the model's (build_argv's): only the set matters */
    SmValues v;
    sm_values_init(&v);
    int differ = 0;
    for (const auto &kv : ini_pairs(p)) {
        const char *d = sm_get(&v, kv.first.c_str());
        if (kv.first != "preset" && (!d || kv.second != d)) differ++, printf("     default of %s: ini '%s', model '%s'\n", kv.first.c_str(), kv.second.c_str(), d ? d : "?");
    }
    CHECK(!differ, "keys: the model's defaults are Profile()'s");
    sm_values_free(&v);
}

static void test_i18n_ids() {
    std::set<std::string> ids;
#define X(id, en, es) ids.insert(#id);
#include "../src/i18n_strings.h"
#undef X
    int bad = 0;
    for (int i = 0; i < sm_count(); i++) {
        const SmSetting *s = sm_setting(i);
        for (const char *id : {s->label, s->desc})
            if (id && !ids.count(id)) bad++, printf("     %s: no i18n id %s\n", s->key, id);
    }
    CHECK(!bad, "i18n: every label and description id is in i18n_strings.h (both languages)");
}

/* The golden file's profiles (launcher_test preset_outputs): same argv from the model. */
static void test_preset_argv() {
    const Preset presets[] = {Preset::Laptop, Preset::Balanced, Preset::Gaming, Preset::Ultra, Preset::Integrated};
    for (Preset preset : presets) {
        Profile p;
        p.name = "pinned";
        p.xbrun = "/opt/warriors/xbrun";
        p.disc = "/opt/warriors/disc";
        apply_preset(p, preset, "/opt/warriors");
        std::string want = join(build_argv(p)), got = model_argv(p);
        CHECK(got == want, "argv: preset %s identical to build_argv", preset_key(preset));
        if (got != want) printf("     want %s\n     got  %s\n", want.c_str(), got.c_str());
    }
}

/* Sample values of a setting's domain. */
static std::vector<std::string> samples(const SmSetting *s) {
    std::vector<std::string> out;
    std::string dom = s->domain;
    auto split = [&](const std::string &list) {
        for (size_t at = 0;;) {
            size_t bar = list.find('|', at);
            out.push_back(list.substr(at, bar == std::string::npos ? std::string::npos : bar - at));
            if (bar == std::string::npos) break;
            at = bar + 1;
        }
    };
    switch (s->type) {
    case SM_BOOL: out = {"true", "false"}; break;
    case SM_ENUM: split(dom); break;
    case SM_INT:
        if (dom.find("..") != std::string::npos) {
            int lo = atoi(dom.c_str()), hi = atoi(dom.c_str() + dom.find("..") + 2);
            out = {std::to_string(lo), std::to_string(hi), std::to_string((lo + hi) / 2), std::to_string(lo + 1)};
        } else split(dom);
        break;
    case SM_FLOAT: {
        float lo = (float)atof(dom.c_str()), hi = (float)atof(dom.c_str() + dom.find("..") + 2);
        out = {"-1", format_float(lo), format_float(hi), format_float((lo + hi) / 2)};
        break;
    }
    default:
        out = {"", "/x y/z", "C:\\Packs\\v3\\", "fps,cpu,gpu", "cpu", "bogus"};
        if (!strcmp(s->key, "extra_args")) out = {"", "--log-limit 5", "--a 'b c' \"d\""};
    }
    return out;
}

/* Every value of every setting on top of each base profile: the profile parsed back from its ini
 * text with that line changed gives the same command line from build_argv and from the model. */
static void test_value_sweep() {
    std::vector<Profile> bases;
    for (Preset preset : {Preset::Custom, Preset::Laptop, Preset::Balanced, Preset::Gaming, Preset::Ultra, Preset::Integrated}) {
        Profile p;
        p.name = "sweep";
        p.xbrun = "/opt/w/xbrun";
        p.disc = "/opt/w/disc";
        apply_preset(p, preset, "");
        bases.push_back(p);
    }
    Profile tex = bases[3];   /* sharp faces over the pack, the glyph pack, neon masks */
    tex.texture_pack = "/opt/w/texpack";
    tex.glyph_packs = "/opt/w/glyph-packs";
    tex.button_icons = "ps-black";
    tex.neon_masks = "/opt/w/neon-masks";
    tex.mods = "a,b";
    bases.push_back(tex);
    int runs = 0, bad = 0;
    for (const Profile &base : bases) {
        ProfileFile f;
        f.profiles.push_back(base);
        f.selected = base.name;
        std::string text = write_ini(f);
        for (int i = 0; i < sm_count(); i++) {
            const SmSetting *s = sm_setting(i);
            if (!strcmp(s->key, "preset")) continue;
            std::string head = std::string("\n") + s->key + " = ";
            size_t at = text.find(head);
            if (at == std::string::npos) { bad++; continue; }
            size_t end = text.find('\n', at + 1);
            for (const std::string &value : samples(s)) {
                std::string t = text.substr(0, at) + head + value + text.substr(end);
                ProfileFile g;
                parse_ini(t, g, nullptr);
                if (g.profiles.size() != 1) { bad++; continue; }
                runs++;
                std::string want = join(build_argv(g.profiles[0])), got = model_argv(g.profiles[0]);
                if (got != want) {
                    bad++;
                    if (bad < 8) printf("     %s = '%s':\n     want %s\n     got  %s\n", s->key, value.c_str(), want.c_str(), got.c_str());
                }
            }
        }
    }
    CHECK(runs > 2000 && !bad, "argv: %d profiles (every value of every setting on 7 bases) identical to build_argv (%d differ)", runs, bad);
}

/* SM_PRESET is set exactly for the keys apply_preset decides: the same value from two different
 * starting profiles in at least one preset. */
static void test_preset_coverage() {
    Profile a, b;
    a.name = b.name = "cover";
    /* b: every model setting at a value other than a's, where the domain has one */
    SmValues va;
    values_of(a, va);
    std::string text;
    {
        ProfileFile f;
        f.profiles.push_back(b);
        f.selected = b.name;
        text = write_ini(f);
    }
    for (int i = 0; i < sm_count(); i++) {
        const SmSetting *s = sm_setting(i);
        for (const std::string &v : samples(s)) {
            if (v == va.v[i] || !strcmp(s->key, "preset")) continue;
            std::string head = std::string("\n") + s->key + " = ";
            size_t at = text.find(head), end = text.find('\n', at + 1);
            text = text.substr(0, at) + head + v + text.substr(end);
            break;
        }
    }
    ProfileFile g;
    parse_ini(text, g, nullptr);
    b = g.profiles[0];
    b.name = a.name;
    SmValues vb;
    values_of(b, vb);
    std::set<std::string> covered;
    for (Preset preset : {Preset::Laptop, Preset::Balanced, Preset::Gaming, Preset::Ultra, Preset::Integrated}) {
        Profile ra = a, rb = b;
        apply_preset(ra, preset, "");
        apply_preset(rb, preset, "");
        SmValues xa, xb;
        values_of(ra, xa);
        values_of(rb, xb);
        for (int i = 0; i < sm_count(); i++)
            if (strcmp(va.v[i], vb.v[i]) && !strcmp(xa.v[i], xb.v[i])) covered.insert(sm_setting(i)->key);
        sm_values_free(&xa);
        sm_values_free(&xb);
    }
    int bad = 0;
    for (int i = 0; i < sm_count(); i++) {
        const SmSetting *s = sm_setting(i);
        bool flagged = (s->flags & SM_PRESET) != 0, cov = covered.count(s->key) != 0;
        if (!strcmp(s->key, "skip_intro") || !strcmp(s->key, "preset")) continue;   /* a Game option / the preset itself */
        if (flagged != cov) bad++, printf("     %s: SM_PRESET %d, set by a preset %d\n", s->key, flagged, cov);
    }
    CHECK(!bad && covered.size() > 50, "presets: SM_PRESET marks the %zu keys apply_preset sets", covered.size());
    sm_values_free(&va);
    sm_values_free(&vb);
}

/* ---- W7: launcher sync ---- */

static ProfileFile two_profiles() {
    ProfileFile f;
    Profile a, b;
    a.name = "main";
    apply_preset(a, Preset::Laptop, "");
    a.preset = preset_key(detect_preset(a));
    a.unknown.push_back({"future_key", "kept"});
    b.name = "other";
    f.profiles = {a, b};
    f.selected = "main";
    f.other_sections = {"[future]", "x = 1"};
    return f;
}

static std::string pending_text(const std::string &profile, const std::string &lines) {
    return std::string(SM_PENDING_MAGIC) + "\n[profile " + profile + "]\n" + lines;
}

static void test_merge_pending() {
    ProfileFile f = two_profiles();
    const std::string before_other = write_ini(f).substr(write_ini(f).find("[profile other]"));
    /* the game's own writer: only the changed menu keys */
    SmValues start, now;
    values_of(f.profiles[0], start);
    sm_values_copy(&now, &start);
    sm_set(&now, "bloom", "true");
    sm_set(&now, "music_volume", "-6");
    sm_set(&now, "xbrun", "/not/a/menu/key");
    char buf[4096];
    int n = sm_pending_format(buf, sizeof buf, "main", &start, &now);
    CHECK(n > 0 && n < (int)sizeof buf && std::string(buf).find("xbrun") == std::string::npos, "merge: the game writes only changed menu keys");
    std::vector<std::string> warn;
    CHECK(merge_pending(f, buf, &warn) == 2 && warn.empty(), "merge: both changed keys set (%zu warnings)", warn.size());
    const Profile &m = f.profiles[0];
    CHECK(m.bloom && m.music_volume == -6 && m.xbrun.empty() && m.aniso == 4 && m.sim60, "merge: only those keys changed in the profile");
    CHECK(m.preset == "custom" && detect_preset(m) == Preset::Custom, "merge: the preset re-detected (laptop + bloom = custom)");
    CHECK(write_ini(f).substr(write_ini(f).find("[profile other]")) == before_other, "merge: the other profile and the unknown section untouched");
    CHECK(m.unknown.size() == 1 && m.unknown[0].second == "kept", "merge: the profile's unknown keys kept");
    sm_values_free(&start);
    sm_values_free(&now);

    warn.clear();
    int k = merge_pending(f, pending_text("main", "future_key = 1\nxbrun = /evil\nbloom = maybe\nmsaa = 3\n  ssao = on  \n# a comment\n\nmsaa = 4\n"), &warn);
    CHECK(k == 2 && f.profiles[0].ssao && f.profiles[0].msaa == 4 && f.profiles[0].bloom && f.profiles[0].xbrun.empty() &&
          f.profiles[0].unknown.size() == 1, "merge: unknown, non-menu and bad values ignored, the rest set (%d)", k);
    CHECK(warn.size() == 4, "merge: one warning per ignored line (%zu)", warn.size());

    const std::string ini = write_ini(f);
    const char *corrupt[] = {
        "",
        "garbage\n",
        "[profile main]\nbloom = true\n",                                   /* no magic line */
        SM_PENDING_MAGIC "\nbloom = true\n",                                /* no section */
        SM_PENDING_MAGIC "\n[profile main]\nbloom true\n",                  /* a line without '=' */
        SM_PENDING_MAGIC "\n[profile main]\nbloom = true\n[profile other]\nssao = true\n",   /* two sections */
        SM_PENDING_MAGIC "\n[profile nobody]\nbloom = false\n",             /* no such profile */
        SM_PENDING_MAGIC "\n[launcher]\nselected = other\n",                /* not a profile section */
    };
    int bad = 0;
    for (const char *t : corrupt)
        if (merge_pending(f, t, nullptr) != -1 || write_ini(f) != ini) bad++;
    CHECK(!bad, "merge: %zu corrupt files rejected, the profiles unchanged", sizeof corrupt / sizeof corrupt[0]);
}

static void test_merge_file(const std::string &scratch) {
    make_dirs(scratch);
    const std::string ini = path_join(scratch, "profiles.ini"), pending = pending_path_for(ini), bad = pending + ".bad";
    ProfileFile f = two_profiles();
    save_ini(ini, f, nullptr);
    remove_file(pending), remove_file(bad);
    std::string note, text, before;
    read_file(ini, before, 1 << 20);
    CHECK(sync_merge_and_save(f, ini, &note) == SyncMerge::None && read_file(ini, text, 1 << 20) && text == before,
          "merge file: no pending file, the profiles file untouched");
    write_file(pending, "not a pending file\n");
    CHECK(sync_merge_and_save(f, ini, &note) == SyncMerge::Corrupt && read_file(ini, text, 1 << 20) && text == before &&
          !is_file(pending) && is_file(bad), "merge file: a corrupt file leaves the profiles file and moves to .bad");
    write_file(pending, pending_text("main", "lamp_lights = true\nfuture = 2\n"));
    SyncMerge r = sync_merge_and_save(f, ini, &note);
    ProfileFile g;
    CHECK(r == SyncMerge::Merged && !is_file(pending) && load_ini(ini, g, nullptr, nullptr) && g.profiles.size() == 2 &&
          g.profiles[0].lamp_lights && !g.profiles[1].lamp_lights && g.other_sections.size() == 2 && g.profiles[0].unknown.size() == 1,
          "merge file: merged and saved, the pending file removed, unknown keys and sections kept");
    CHECK(note.find("1 change(s)") != std::string::npos && note.find("future") != std::string::npos, "merge file: the note (%s)", note.c_str());
}

static void test_presets_file() {
    std::string t = presets_text("");
    int ok = 0;
    for (Preset preset : {Preset::Laptop, Preset::Integrated, Preset::Balanced, Preset::Gaming, Preset::Ultra}) {
        std::string head = std::string("[preset ") + preset_key(preset) + "]\n";
        size_t at = t.find(head);
        if (at == std::string::npos) continue;
        size_t end = t.find("\n[preset ", at + 1);
        std::string body = t.substr(at + head.size(), end == std::string::npos ? std::string::npos : end - at - head.size());
        for (Preset from : {Preset::Custom, Preset::Ultra, Preset::Laptop}) {   /* from any profile, the menu's preset choice */
            ProfileFile f;
            Profile p;
            p.name = "x";
            apply_preset(p, from, "");
            p.bloom_strength = 2;   /* custom touches the preset clears */
            p.ssao = !p.ssao;
            f.profiles.push_back(p);
            if (merge_pending(f, pending_text("x", body), nullptr) > 10 && detect_preset(f.profiles[0]) == preset) ok++;
            else printf("     %s from %s: %s\n", preset_key(preset), preset_key(from), preset_key(detect_preset(f.profiles[0])));
        }
    }
    CHECK(ok == 15, "presets file: each preset's section applied over 3 profiles is detected as that preset (%d/15)", ok);
    CHECK(t.find("[preset gaming]") != std::string::npos && t.substr(t.find("[preset gaming]")).find("draw_distance") > t.substr(t.find("[preset gaming]")).find("[preset ultra]") &&
          t.find("draw_distance = 2") != std::string::npos && t.find("texture_pack") == std::string::npos && t.find("skip_intro") == std::string::npos,
          "presets file: Quality leaves the draw distance, packs and skip_intro are no preset keys");
}

static void test_env_and_restart() {
    auto env = sync_env("/w/warriors-launcher.ini", "my profile", "es", true);
    std::map<std::string, std::string> m(env.begin(), env.end());
    CHECK(m[SM_ENV_INI] == "/w/warriors-launcher.ini" && m[SM_ENV_PROFILE] == "my profile" &&
          m[SM_ENV_PENDING] == "/w/warriors-launcher.ini.pending" && m[SM_ENV_PRESETS] == "/w/warriors-launcher.ini.presets" &&
          m[SM_ENV_UI_LANGUAGE] == "es" && m[SM_ENV_RESTART] == "1" && env.size() == 6, "env: the six variables");
    CHECK(sync_env("i", "p", "en", false)[5].second == "0", "env: no restart with close on start");
    RestartPolicy r;
    CHECK(!r.after_exit(0, 100) && !r.after_exit(1, 5) && !r.after_exit(SM_EXIT_RESTART - 1, 5), "restart: other exit codes end the chain");
    CHECK(r.after_exit(SM_EXIT_RESTART, 120) && r.after_exit(SM_EXIT_RESTART, 120) && r.after_exit(SM_EXIT_RESTART, 45),
          "restart: a settled game restarts every time");
    int quick = 0;
    while (quick < 10 && r.after_exit(SM_EXIT_RESTART, 2)) quick++;
    CHECK(quick == RESTART_MAX_IN_ROW, "restart: %d quick restarts in a row, then no more (no loop on repeated failure)", quick);
    CHECK(!r.after_exit(SM_EXIT_RESTART, 2) && !r.after_exit(0, 2) && r.after_exit(SM_EXIT_RESTART, 2),
          "restart: a normal exit resets the count");
    std::vector<std::string> a = {"x", "--warp", "m1"};
    add_skip_intro(a);
    add_skip_intro(a);
    CHECK(join(a) == "x|--warp|m1|--skip-intro", "restart: --skip-intro added once");
}

/* F10 opens the in-game settings menu: reserved like Escape and F1. */
static void test_f10_reserved() {
    KeyMap m;
    std::vector<std::string> warn;
    KeyMap p = parse_keymap("key.f10 = a\nkey.f9 = b\n", &warn);
    CHECK(key_reserved(KEY_F1 + 9) && !key_reserved(KEY_F1 + 8) && m.bind(KEY_F1 + 9, CTL_A) == -2 && m.bind(KEY_F1 + 8, CTL_A) != -2,
          "keymap: F10 reserved, F9 still bindable");
    CHECK(p.binding[KEY_F1 + 9] != CTL_A && p.binding[KEY_F1 + 8] == CTL_B && warn.size() == 1, "keymap: a key.f10 line is refused with a warning");
    CHECK(std::string(tr(S::key_reserved, Lang::En)).find("F10") != std::string::npos &&
          std::string(tr(S::key_reserved, Lang::Es)).find("F10") != std::string::npos, "keymap: the refusal names F10 in both languages");
}

#ifndef _WIN32
/* A child started like the game: it sees the variables, writes a pending file, asks for a restart. */
static void test_child_restart(const std::string &scratch) {
    const std::string ini = path_join(scratch, "child.ini"), script = path_join(scratch, "fake-game.sh"),
                      seen = path_join(scratch, "seen.txt"), log = path_join(scratch, "child.log");
    remove_file(seen), remove_file(pending_path_for(ini));
    write_file(script, "#!/bin/sh\nprintf '%s|%s|%s|%s' \"$WARRIORS_PROFILE\" \"$WARRIORS_SETTINGS_PENDING\" \"$WARRIORS_RESTART\" \"$*\" > '" + seen +
                           "'\nprintf '" SM_PENDING_MAGIC "\\n[profile main]\\nbloom = true\\n' > \"$WARRIORS_SETTINGS_PENDING\"\nexit 86\n");
    chmod(script.c_str(), 0755);
    ProfileFile f = two_profiles();
    save_ini(ini, f, nullptr);
    CHECK(apply_env(sync_env(ini, "main", "en", true)), "child: environment set");
    Child c;
    std::vector<std::string> argv = {script, "--x"};
    add_skip_intro(argv);
    std::string err, text;
    CHECK(child_spawn(c, argv, "", log, &err), "child: started (%s)", err.c_str());
    child_wait(c);
    RestartPolicy r;
    read_file(seen, text, 4096);
    CHECK(c.exit_code == SM_EXIT_RESTART && text == "main|" + pending_path_for(ini) + "|1|--x --skip-intro", "child: exit %d, saw '%s'", c.exit_code, text.c_str());
    CHECK(sync_merge_and_save(f, ini, nullptr) == SyncMerge::Merged && f.profiles[0].bloom && r.after_exit(c.exit_code, 1),
          "child: its pending change merged, relaunch wanted");
}
#endif

int main() {
    test_keys();
    test_i18n_ids();
    test_preset_argv();
    test_value_sweep();
    test_preset_coverage();
    const std::string scratch = path_join(exe_dir(), "settings-scratch");
    test_merge_pending();
    test_merge_file(scratch);
    test_presets_file();
    test_env_and_restart();
    test_f10_reserved();
#ifndef _WIN32
    test_child_restart(scratch);
#endif
    if (failures) printf("%d FAILED\n", failures);
    else printf("ALL OK\n");
    return failures ? 1 : 0;
}
