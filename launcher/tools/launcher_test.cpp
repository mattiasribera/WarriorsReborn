/* Game-independent test of the launcher core: profile file round trip (paths with spaces and
 * Windows drive letters, unknown keys and sections kept), the argv builder for each option with
 * defaults left out, quoting, copy validation against synthetic XBE headers written here, and a
 * child process with its output in a log file, and the title-image decoder on synthetic XPR0
 * textures and XBE sections. No game data. Run from any folder; scratch files
 * go to launcher/build/test-scratch (next to this binary). */
#include "../src/backup.h"
#include "../src/command.h"
#include "../src/i18n.h"
#include "../src/iso.h"
#include "../src/keymap.h"
#include "../src/padmap.h"
#include "../src/platform.h"
#include "../src/png.h"
#include "../src/profile.h"
#include "../src/report.h"
#include "../src/titleimage.h"
#include "../src/uiscale.h"
#include "../src/xbe_check.h"
#include <cstdio>
#include <cstring>

using namespace launcher;

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static std::string join(const std::vector<std::string> &v) {
    std::string s;
    for (const std::string &a : v) s += (s.empty() ? "" : "|") + a;
    return s;
}

/* The options after "<xbe> <disc>". */
static std::string opts(const Profile &p) {
    std::vector<std::string> a = build_argv(p);
    return join(std::vector<std::string>(a.begin() + (a.size() > 3 ? 3 : a.size()), a.end()));
}

static void test_ini_round_trip() {
    ProfileFile f;
    Profile a;
    a.name = "stable";
    a.xbrun = "C:\\Program Files\\Warriors Port\\xbrun.exe"; /* an older install */
    a.disc = "C:\\Warriors\\disc";
    a.hdd = "D:\\My Games\\warriors hdd";
    a.texture_pack = "/home/u/tex packs/v2";
    a.button_icons = "kbm";
    a.icon_pack = "C:\\Warriors\\icon pack";
    a.resolution = "1920x1080";
    a.msaa = 4;
    a.aniso = 16;
    a.widescreen = true;
    a.language = "es";
    a.frame_lock = false;
    a.extra_args = "--log-limit 100";
    a.unknown.push_back({"future_key", "some value"});
    Profile b;
    b.name = "test no lock";
    b.spawn_scale = 3;
    f.profiles = {a, b};
    f.selected = "test no lock";
    f.other_sections = {"[mods]", "layer = one"};

    std::string text = write_ini(f);
    ProfileFile g;
    std::vector<std::string> warn;
    CHECK(parse_ini(text, g, &warn), "round trip: parses its own output");
    CHECK(g.profiles.size() == 2, "round trip: 2 profiles (%zu)", g.profiles.size());
    if (g.profiles.size() != 2) return;
    const Profile &x = g.profiles[0];
    CHECK(x.name == "stable" && g.profiles[1].name == "test no lock", "round trip: names, incl. spaces");
    CHECK(x.xbrun == a.xbrun && x.disc == a.disc && x.hdd == a.hdd && x.texture_pack == a.texture_pack,
          "round trip: paths with spaces and drive letters (%s)", x.hdd.c_str());
    CHECK(x.resolution == "1920x1080" && x.msaa == 4 && x.aniso == 16 && x.widescreen && !x.fullscreen,
          "round trip: graphics");
    CHECK(x.button_icons == "kbm" && x.icon_pack == a.icon_pack && g.profiles[1].button_icons == "xbox",
          "round trip: button icons and icon pack folder (%s, %s)", x.button_icons.c_str(), x.icon_pack.c_str());
    CHECK(x.language == "es" && !x.frame_lock && x.rate_fixes && x.extra_args == "--log-limit 100",
          "round trip: language, gameplay, extra args");
    CHECK(g.profiles[1].spawn_scale == 3 && g.selected == "test no lock", "round trip: spawn scale, selected");
    CHECK(x.unknown.size() == 1 && x.unknown[0].first == "future_key" && x.unknown[0].second == "some value",
          "round trip: unknown key kept with its profile");
    CHECK(g.other_sections.size() == 2 && g.other_sections[0] == "[mods]", "round trip: unknown section kept");
    CHECK(write_ini(g) == text, "round trip: identical text on the second write");
    CHECK(warn.size() == 1 && warn[0].find("future_key") != std::string::npos,
          "round trip: unknown key reported (%zu warnings)", warn.size());
}

static void test_ini_parsing() {
    ProfileFile f;
    std::vector<std::string> warn;
    bool ok = parse_ini("# comment\r\n; other\r\n[profile  win ]\r\n  disc =  E:\\disc  \r\n"
                        "widescreen = yes\r\nmsaa = lots\r\n", f, &warn);
    CHECK(ok && f.profiles.size() == 1 && f.profiles[0].name == "win", "parse: CRLF, comments, trimmed section name");
    if (f.profiles.size() == 1) {
        CHECK(f.profiles[0].disc == "E:\\disc" && f.profiles[0].widescreen, "parse: trimmed value, yes = true");
        CHECK(f.profiles[0].msaa == 0 && warn.size() == 1, "parse: bad number reported, default kept (%zu)", warn.size());
    }
    ProfileFile e;
    CHECK(!parse_ini("[profile x]\njust text\n", e, nullptr), "parse: line without '=' is an error");
    ProfileFile u;
    u.profiles.push_back(Profile());
    u.profiles[0].name = "stable";
    CHECK(unique_profile_name(u, "stable") == "stable 2" && unique_profile_name(u, "new") == "new", "unique names");
}

static void test_argv() {
    Profile p;
    p.xbrun = "/opt/port/xbrun";
    p.disc = "/games/warriors/disc/";
    std::vector<std::string> a = build_argv(p);
    CHECK(join(a) == "/opt/port/xbrun|/games/warriors/disc/default.xbe|/games/warriors/disc",
          "argv: defaults give the bare command (%s)", join(a).c_str());
    CHECK(disc_xbe_path("C:\\Warriors\\disc") == "C:\\Warriors\\disc\\default.xbe", "argv: Windows disc path");

    struct Case { const char *what; void (*set)(Profile &); const char *want; };
    static const Case cases[] = {
        {"hdd", [](Profile &q) { q.hdd = "C:\\hdd"; }, "--hdd|C:\\hdd"},
        {"texture pack", [](Profile &q) { q.texture_pack = "/t p"; }, "--texture-pack|/t p"},
        {"resolution", [](Profile &q) { q.resolution = "1920x1080"; }, "--resolution|1920x1080"},
        {"render scale", [](Profile &q) { q.render_scale = 3; }, "--render-scale|3"},
        {"resolution wins", [](Profile &q) { q.resolution = "800x600"; q.render_scale = 2; }, "--resolution|800x600"},
        {"msaa", [](Profile &q) { q.msaa = 4; }, "--msaa|4"},
        {"aniso", [](Profile &q) { q.aniso = 16; }, "--aniso|16"},
        {"widescreen", [](Profile &q) { q.widescreen = true; }, "--widescreen"},
        {"fullscreen", [](Profile &q) { q.fullscreen = true; }, "--fullscreen"},
        {"present", [](Profile &q) { q.present = "fifo"; }, "--present|fifo"},
        {"language", [](Profile &q) { q.language = "de"; }, "--language|de"},
        {"spawn scale", [](Profile &q) { q.spawn_scale = 2; }, "--spawn-scale|2"},
        {"rate fixes", [](Profile &q) { q.rate_fixes = false; }, "--no-rate-fixes"},
        {"frame lock", [](Profile &q) { q.frame_lock = false; }, "--no-frame-lock"},
        {"input config", [](Profile &q) { q.input_config = "keys.cfg"; }, "--input-config|keys.cfg"},
        {"warp", [](Profile &q) { q.warp = "8:3"; }, "--warp|8:3"},
        {"audio", [](Profile &q) { q.audio = "null"; }, "--audio|null"},
        {"video", [](Profile &q) { q.video = "x11"; }, "--video|x11"},
        {"renderer", [](Profile &q) { q.renderer = "none"; }, "--renderer|none"},
        {"render targets", [](Profile &q) { q.render_targets = false; }, "--no-render-targets"},
        {"stats", [](Profile &q) { q.stats = true; }, "--stats"},
        {"trace", [](Profile &q) { q.trace = true; }, "--trace"},
        {"extra args", [](Profile &q) { q.extra_args = " --log-limit 5  \"--x y\" "; }, "--log-limit|5|--x y"},
        /* Values equal to the runtime defaults are left out, typed or empty. */
        {"default values", [](Profile &q) { q.language = ""; q.present = "auto"; q.audio = ""; q.video = "sdl";
                                            q.renderer = "vulkan"; q.render_scale = 1; q.msaa = 1; q.aniso = 1; }, ""},
        {"language en", [](Profile &q) { q.language = "en"; }, ""},
        /* Button-icon pack: goes before the profile's texture pack (first match wins). */
        {"icon pack", [](Profile &q) { q.icon_pack = "/g"; }, "--texture-pack|/g"},
        {"icon pack before texture pack", [](Profile &q) { q.icon_pack = "/g"; q.texture_pack = "/t"; },
         "--texture-pack|/g|--texture-pack|/t"},
        {"icon pack windows folder", [](Profile &q) { q.icon_pack = "C:\\W\\icons"; }, "--texture-pack|C:\\W\\icons"},
        {"unknown icons value", [](Profile &q) { q.button_icons = "nes"; }, ""},
        /* Keyboard/mouse icons are drawn by the runtime over the atlas in use. */
        {"kbm icons", [](Profile &q) { q.button_icons = "kbm"; q.icon_pack = "/g"; }, "--texture-pack|/g|--button-icons|kbm"},
        {"auto icons", [](Profile &q) { q.button_icons = "auto"; q.texture_pack = "/t"; }, "--texture-pack|/t|--button-icons|auto"},
        {"vibration off", [](Profile &q) { q.rumble = false; }, "--no-rumble"},
    };
    for (const Case &c : cases) {
        Profile q = p;
        c.set(q);
        std::string got = opts(q);
        CHECK(got == c.want, "argv: %s -> [%s]", c.what, got.c_str());
    }

    Profile full = p;
    full.hdd = "/h";
    full.widescreen = true;
    full.frame_lock = false;
    full.extra_args = "--stats";
    CHECK(opts(full) == "--hdd|/h|--widescreen|--no-frame-lock|--stats", "argv: several options in a fixed order (%s)",
          opts(full).c_str());
}

static void test_quoting() {
    CHECK(join(split_args("a  'b c' \"d e\" C:\\x\\y")) == "a|b c|d e|C:\\x\\y", "split: quotes group, backslashes literal");
    CHECK(quote_posix("plain/path") == "plain/path" && quote_posix("a b") == "'a b'" && quote_posix("it's") == "'it'\\''s'"
          && quote_posix("") == "''", "posix quoting");
    CHECK(quote_windows("C:\\a\\b") == "C:\\a\\b" && quote_windows("C:\\a b\\") == "\"C:\\a b\\\\\""
          && quote_windows("say \"hi\"") == "\"say \\\"hi\\\"\"" && quote_windows("") == "\"\"",
          "windows quoting (CommandLineToArgvW rules)");
    std::vector<std::string> argv = {"C:\\Port Dir\\TheWarriors.exe", "C:\\disc\\default.xbe", "C:\\disc", "--widescreen"};
    CHECK(display_command(argv, true) == "& \"C:\\Port Dir\\TheWarriors.exe\" C:\\disc\\default.xbe C:\\disc --widescreen",
          "display: PowerShell call operator for a quoted binary (%s)", display_command(argv, true).c_str());
    argv[0] = ".\\TheWarriors.exe";
    CHECK(display_command(argv, true) == ".\\TheWarriors.exe C:\\disc\\default.xbe C:\\disc --widescreen", "display: plain on Windows");
    CHECK(display_command({"/a b/xbrun", "/d/default.xbe", "/d"}, false) == "'/a b/xbrun' /d/default.xbe /d", "display: POSIX");
    CHECK(windows_command_line({"C:\\a b\\x.exe", "y"}) == "\"C:\\a b\\x.exe\" y", "windows command line");
}

/* A synthetic XBE header: magic, base, certificate address, UTF-16LE title. */
static std::string fake_xbe(const char *magic, const char *title, uint32_t cert_off = 0x178) {
    std::string f(0x1000, '\0');
    memcpy(&f[0], magic, 4);
    uint32_t base = 0x10000, cert = base + cert_off;
    memcpy(&f[0x104], &base, 4);
    memcpy(&f[0x118], &cert, 4);
    for (size_t i = 0; title[i] && i < 40; i++) f[cert_off + 0x0C + i * 2] = title[i];
    return f;
}

static XbeCheck check_str(const std::string &s) { return check_xbe_bytes((const uint8_t *)s.data(), s.size()); }

static void test_xbe(const std::string &scratch) {
    XbeCheck good = check_str(fake_xbe("XBEH", "The Warriors"));
    CHECK(good.status == XbeStatus::Ok && good.title == "The Warriors", "xbe: good magic and title (%s)", good.title.c_str());
    CHECK(check_str(fake_xbe("MZxx", "The Warriors")).status == XbeStatus::NotXbe, "xbe: bad magic");
    XbeCheck wrong = check_str(fake_xbe("XBEH", "Other Game"));
    CHECK(wrong.status == XbeStatus::WrongTitle && wrong.title == "Other Game", "xbe: wrong title (%s)", wrong.title.c_str());
    CHECK(check_str(fake_xbe("XBEH", "The Warriors", 0x2000)).status == XbeStatus::NotXbe, "xbe: certificate out of file");
    CHECK(check_str(std::string("XBEH")).status == XbeStatus::NotXbe, "xbe: truncated header");

    std::string disc = path_join(scratch, "disc copy"), hdd = path_join(scratch, "hdd");
    make_dir(disc);
    make_dir(hdd);
    std::string xbe = path_join(disc, "default.xbe");
    write_file(xbe, fake_xbe("XBEH", "The Warriors"));
    std::string xbrun = path_join(scratch, "xbrun");
    write_file(xbrun, "#!/bin/sh\n");
    CHECK(check_xbe_file(xbe).status == XbeStatus::Ok, "xbe file: ok");
    CHECK(check_xbe_file(path_join(scratch, "nope.xbe")).status == XbeStatus::Missing, "xbe file: missing");

    Profile p;
    p.xbrun = xbrun;
    p.disc = disc;
    p.hdd = hdd;
    CopyCheck c = check_copy(p);
    CHECK(c.ready(), "copy: ready (xbrun %d disc %d hdd %d xbe %s)", c.xbrun_ok, c.disc_ok, c.hdd_ok,
          xbe_status_text(c.xbe.status));
    p.hdd = "";
    CHECK(check_copy(p).hdd_ok, "copy: empty hdd is ok (xbrun's ./hdd)");
    p.hdd = path_join(scratch, "no hdd");
    p.texture_pack = path_join(scratch, "no pack");
    c = check_copy(p);
    CHECK(!c.hdd_ok && !c.texpack_ok && !c.ready(), "copy: missing hdd and texture pack folders");
    p = Profile();
    p.disc = scratch;
    c = check_copy(p);
    CHECK(!c.xbrun_ok && c.disc_ok && c.xbe.status == XbeStatus::Missing, "copy: no xbrun, no default.xbe in disc");
}

static void test_child(const std::string &scratch) {
#ifndef _WIN32
    std::string log = path_join(scratch, "child.log");
    write_file(log, "");
    Child c;
    std::string err;
    bool ok = child_spawn(c, {"/bin/sh", "-c", "echo out; echo err >&2; pwd; exit 3"}, scratch, log, &err);
    CHECK(ok && c.running, "child: started (%s)", err.c_str());
    if (!ok) return;
    child_wait(c);
    std::string text;
    read_file(log, text, 4096);
    CHECK(!c.running && c.exit_code == 3, "child: exit code 3 (%d)", c.exit_code);
    CHECK(text.find("out\n") != std::string::npos && text.find("err\n") != std::string::npos
          && text.find(scratch) != std::string::npos, "child: stdout, stderr and cwd in the log");
    Child bad;
    CHECK(!child_spawn(bad, {path_join(scratch, "missing-binary")}, "", log, &err) && !err.empty(),
          "child: a missing binary is an error (%s)", err.c_str());
#endif
}

/* A first-run or New profile: the configuration validated as playable on Windows, with the
 * texture pack looked up in the install folder and left out when it is missing. */
static void test_recommended(const std::string &scratch) {
    std::string inst = path_join(scratch, "install");
    make_dir(inst);
    make_dir(path_join(inst, "texpack-v3"));
    Profile p = recommended_profile(inst);
    p.xbrun = path_join(inst, "xbrun");
    p.disc = "/games/disc";
    p.hdd = "/games/hdd";
    std::vector<std::string> a = build_argv(p);
    std::string want = join({p.xbrun, "/games/disc/default.xbe", "/games/disc", "--hdd", "/games/hdd",
                             "--texture-pack", path_join(inst, "texpack-v3"),
                             "--resolution", "1920x1080", "--widescreen", "--fullscreen", "--msaa", "4", "--aniso", "4",
                             "--stats", "--sim60", "--hud-hd-glass", "--supersample", "2",
                             "--shadow-scale", "2", "--shadow-soft", "--shadow-range", "--bloom", "--rain-hd", "--ssao"});
    CHECK(join(a) == want, "recommended: validated command line (%s)", display_command(a, false).c_str());

    Profile q = recommended_profile(path_join(scratch, "no-packs"));
    CHECK(q.texture_pack.empty() && q.button_icons == "xbox" && opts(q).find("--texture-pack") == std::string::npos,
          "recommended: a missing pack gives no --texture-pack (%s)", opts(q).c_str());
    CHECK(opts(Profile()).empty(), "recommended: a plain Profile still gives the bare command");
}

/* First run: TheWarriors.exe is preferred over the older xbrun.exe, next to the launcher or one
 * folder up (launcher/ subfolder); the POSIX build looks for xbrun. */
static void test_find_game_exe(const std::string &scratch) {
    std::string up = path_join(scratch, "find"), here = path_join(up, "launcher");
    make_dir(up);
    make_dir(here);
    for (const char *n : {"TheWarriors.exe", "xbrun.exe", "xbrun"}) {
        std::remove(path_join(here, n).c_str());
        std::remove(path_join(up, n).c_str());
    }
    CHECK(find_game_exe({here, up}, true).empty(), "find exe: none found");
    write_file(path_join(here, "xbrun.exe"), "MZ");
    CHECK(find_game_exe({here, up}, true) == path_join(here, "xbrun.exe"), "find exe: xbrun.exe fallback");
    write_file(path_join(up, "TheWarriors.exe"), "MZ");
    CHECK(find_game_exe({here, up}, true) == path_join(up, "TheWarriors.exe"),
          "find exe: TheWarriors.exe one folder up beats xbrun.exe next to the launcher");
    write_file(path_join(here, "TheWarriors.exe"), "MZ");
    CHECK(find_game_exe({here, up}, true) == path_join(here, "TheWarriors.exe"), "find exe: next to the launcher first");
    CHECK(find_game_exe({here, up}, false).empty(), "find exe: POSIX looks for xbrun only");
    write_file(path_join(up, "xbrun"), "#!/bin/sh\n");
    CHECK(find_game_exe({here, up}, false) == path_join(up, "xbrun"), "find exe: POSIX xbrun one folder up");
}


/* The options the launcher v2 controls cover: each one alone, then all together in their fixed
 * order (after the older options, before extra_args, so typed overrides still come last). */
static void test_argv_v2() {
    Profile p;
    p.xbrun = "/x";
    p.disc = "/d";
    struct Case { const char *what; void (*set)(Profile &); const char *want; };
    static const Case cases[] = {
        {"supersample", [](Profile &q) { q.supersample = 2; }, "--supersample|2"},
        {"shadow scale", [](Profile &q) { q.shadow_scale = 4; }, "--shadow-scale|4"},
        {"soft shadows", [](Profile &q) { q.shadow_soft = true; }, "--shadow-soft"},
        {"shadow range", [](Profile &q) { q.shadow_range = true; }, "--shadow-range"},
        {"frames in flight", [](Profile &q) { q.frames_in_flight = 2; }, "--frames-in-flight|2"},
        {"sharpen", [](Profile &q) { q.sharpen = 50; }, "--sharpen|50"},
        {"sharpen off", [](Profile &q) { q.sharpen = 0; }, ""},
        {"bloom", [](Profile &q) { q.bloom = true; }, "--bloom"},
        {"bloom strength", [](Profile &q) { q.bloom = true; q.bloom_strength = 2; }, "--bloom|--bloom-strength|2"},
        {"bloom strength needs bloom", [](Profile &q) { q.bloom_strength = 2; }, ""},
        {"better rain", [](Profile &q) { q.rain_hd = true; }, "--rain-hd"},
        {"ambient occlusion", [](Profile &q) { q.ssao = true; }, "--ssao"},
        {"ssao strength", [](Profile &q) { q.ssao = true; q.ssao_strength = 1.5f; }, "--ssao|--ssao-strength|1.5"},
        {"ssao strength needs ssao", [](Profile &q) { q.ssao_strength = 1.5f; }, ""},
        {"sim60", [](Profile &q) { q.sim60 = true; }, "--sim60"},
        {"hud hd", [](Profile &q) { q.hud_style = "hd"; }, "--hud-hd"},
        {"hud hd glass", [](Profile &q) { q.hud_style = "hd-glass"; }, "--hud-hd-glass"},
        {"hud glass", [](Profile &q) { q.hud_style = "glass"; }, "--hud-glass"},
        {"hud customize", [](Profile &q) { q.hud_style = "hd-glass"; q.hud_track_opacity = 0.3f; q.hud_fill_opacity = 0.75f;
                                           q.hud_frost = 6; q.hud_height = 1.5f; },
         "--hud-hd-glass|--hud-hd-track-opacity|0.3|--hud-hd-fill-opacity|0.75|--hud-hd-frost|6|--hud-hd-height|1.5"},
        {"hud customize needs an hd style", [](Profile &q) { q.hud_style = "original"; q.hud_frost = 6; }, ""},
        {"hud customize ignored for glass", [](Profile &q) { q.hud_style = "glass"; q.hud_height = 2; }, "--hud-glass"},
        {"spray time", [](Profile &q) { q.spray_time = 150; }, "--spray-time|150"},
        {"spray time default", [](Profile &q) { q.spray_time = 110; }, ""},
        {"text fixes off", [](Profile &q) { q.text_fixes = false; }, "--no-text-fixes"},
        {"v2 defaults", [](Profile &q) { q.supersample = 1; q.shadow_scale = 1; q.frames_in_flight = 0; q.hud_style = "original"; }, ""},
    };
    for (const Case &c : cases) {
        Profile q = p;
        c.set(q);
        std::string got = opts(q);
        CHECK(got == c.want, "argv v2: %s -> [%s]", c.what, got.c_str());
    }
    Profile all = p;
    all.stats = true;
    all.sim60 = all.shadow_soft = true;
    all.hud_style = "hd-glass";
    all.supersample = 2;
    all.shadow_scale = 2;
    all.frames_in_flight = 3;
    all.spray_time = 120;
    all.text_fixes = false;
    all.extra_args = "--hud-hd-frost 8";
    CHECK(opts(all) == "--stats|--sim60|--hud-hd-glass|--supersample|2|--shadow-scale|2|--shadow-soft|--frames-in-flight|3"
                       "|--spray-time|120|--no-text-fixes|--hud-hd-frost|8",
          "argv v2: fixed order, extra args last (%s)", opts(all).c_str());
}

static void test_ini_v2() {
    ProfileFile f;
    Profile a;
    a.name = "v2";
    a.supersample = 4;
    a.shadow_scale = 2;
    a.shadow_soft = true;
    a.frames_in_flight = 1;
    a.sim60 = true;
    a.hud_style = "hd";
    a.hud_track_opacity = 0.25f;
    a.hud_height = 2;
    a.spray_time = 130;
    a.text_fixes = false;
    f.profiles = {a};
    f.selected = "v2";
    f.settings.setup_done = 1;
    f.settings.close_on_start = true;
    f.settings.show_advanced = true;
    f.settings.last_page = "graphics";
    f.settings.last_log = "C:\\W\\logs\\v2-1.log";
    f.settings.unknown.push_back({"future", "x"});
    std::string text = write_ini(f);
    ProfileFile g;
    std::vector<std::string> warn;
    CHECK(parse_ini(text, g, &warn) && g.profiles.size() == 1, "ini v2: parses");
    if (g.profiles.size() != 1) return;
    const Profile &x = g.profiles[0];
    CHECK(x.supersample == 4 && x.shadow_scale == 2 && x.shadow_soft && x.frames_in_flight == 1 && x.sim60,
          "ini v2: graphics fields");
    CHECK(x.hud_style == "hd" && x.hud_track_opacity == 0.25f && x.hud_fill_opacity < 0 && x.hud_frost < 0 && x.hud_height == 2,
          "ini v2: hud fields (track %g fill %g)", x.hud_track_opacity, x.hud_fill_opacity);
    CHECK(x.spray_time == 130 && !x.text_fixes && x.preset == "custom", "ini v2: gameplay fields, preset %s", x.preset.c_str());
    CHECK(g.settings.setup_done == 1 && g.settings.close_on_start && g.settings.show_advanced &&
          g.settings.last_page == "graphics" && g.settings.last_log == f.settings.last_log,
          "ini v2: [launcher] settings");
    CHECK(g.settings.unknown.size() == 1 && g.settings.unknown[0].first == "future", "ini v2: unknown launcher key kept");
    CHECK(write_ini(g) == text, "ini v2: identical text on the second write");
    CHECK(text.find("preset = custom") != std::string::npos, "ini v2: preset written");

    ProfileFile old;
    parse_ini("[launcher]\nselected = a\n[profile a]\ndisc = /d\n", old, nullptr);
    CHECK(old.settings.setup_done == -1 && !old.settings.close_on_start && !old.settings.show_advanced &&
          old.settings.last_page.empty(), "ini v2: an older file has no launcher settings (setup_done unknown)");
    CHECK(old.profiles.size() == 1 && old.profiles[0].spray_time == 110 && old.profiles[0].text_fixes &&
          old.profiles[0].hud_style == "original" && old.profiles[0].supersample == 1, "ini v2: older profile gets the runtime defaults");
    CHECK(write_ini(old).find("setup_done") == std::string::npos, "ini v2: unknown setup state is not written");
}

/* An older profile with the covered options typed in extra_args: loading moves them into the
 * new fields, removes them from extra_args, and the command line stays the same. The fixture is
 * the user's command validated on Windows. */
static void test_migration() {
    std::string ini = "[launcher]\nselected = mine\n[profile mine]\nxbrun = /inst/TheWarriors.exe\ndisc = /games/disc\n"
                      "texture_pack = /inst/texpack-v3\nicon_pack = /inst/icons\n"
                      "resolution = 1920x1080\nmsaa = 4\naniso = 4\nwidescreen = true\nfullscreen = true\nlanguage = es\n"
                      "stats = true\nextra_args = --sim60 --hud-hd-glass --supersample 2 --shadow-scale 2 --shadow-soft --shadow-range --bloom --rain-hd --ssao\n";
    ProfileFile f;
    std::vector<std::string> warn;
    CHECK(parse_ini(ini, f, &warn) && f.profiles.size() == 1, "migration: parses");
    if (f.profiles.size() != 1) return;
    const Profile &p = f.profiles[0];
    std::string want = "--texture-pack|/inst/icons|--texture-pack|/inst/texpack-v3|--resolution|1920x1080|"
                       "--widescreen|--fullscreen|--msaa|4|--aniso|4|--language|es|--stats|--sim60|--hud-hd-glass|"
                       "--supersample|2|--shadow-scale|2|--shadow-soft|--shadow-range|--bloom|--rain-hd|--ssao";
    CHECK(opts(p) == want, "migration: the user's command is unchanged (%s)", opts(p).c_str());
    CHECK(p.extra_args.empty(), "migration: extra_args emptied ('%s')", p.extra_args.c_str());
    CHECK(p.sim60 && p.hud_style == "hd-glass" && p.supersample == 2 && p.shadow_scale == 2 && p.shadow_soft,
          "migration: fields set");
    CHECK(p.preset == "gaming", "migration: detected as the Quality preset (%s)", p.preset.c_str());
    ProfileFile g;
    parse_ini(write_ini(f), g, nullptr);
    CHECK(g.profiles.size() == 1 && opts(g.profiles[0]) == want && g.profiles[0].extra_args.empty(),
          "migration: saved and reloaded, same command");

    struct Case { const char *what, *extra, *left; };
    static const Case cases[] = {
        {"other args kept, quoted ones requoted", "--log-limit 5 --frames-in-flight 3 \"--x y\"", "--log-limit 5 \"--x y\""},
        {"invalid values stay typed", "--supersample 3 --shadow-scale 9 --frames-in-flight 0", "--supersample 3 --shadow-scale 9 --frames-in-flight 0"},
        {"a hud option before the preset keeps it typed", "--hud-hd-frost 6 --hud-hd-glass", "--hud-hd-frost 6 --hud-hd-glass"},
        {"hud overrides after the preset stay typed", "--hud-hd-glass --hud-hd-frost 6", "--hud-hd-frost 6"},
        {"gameplay options", "--spray-time 150 --no-text-fixes --sim60 --no-sim60", ""},
        {"missing value stays typed", "--supersample", "--supersample"},
    };
    for (const Case &c : cases) {
        Profile q;
        q.xbrun = "/x";
        q.disc = "/d";
        q.extra_args = c.extra;
        migrate_extra_args(q);
        CHECK(q.extra_args == c.left, "migration: %s -> '%s'", c.what, q.extra_args.c_str());
    }
    Profile q;
    q.extra_args = "--spray-time 150 --no-text-fixes --sim60 --no-sim60 --frames-in-flight 3";
    migrate_extra_args(q);
    CHECK(q.spray_time == 150 && !q.text_fixes && !q.sim60 && q.frames_in_flight == 3, "migration: last value wins");
    q = Profile();
    q.hud_style = "hd";
    q.hud_frost = 9;
    q.extra_args = "--hud-hd-glass";
    std::string old_cmd = opts(q);
    migrate_extra_args(q);
    CHECK(q.hud_style == "hd-glass" && q.hud_frost < 0 && opts(q) == "--hud-hd-glass",
          "migration: the hd glass preset resets the overrides it sets (%s -> %s)", old_cmd.c_str(), opts(q).c_str());
    q = Profile();
    q.hud_style = "glass";
    q.extra_args = "--hud-hd-glass";
    migrate_extra_args(q);
    CHECK(q.hud_style == "glass" && q.extra_args == "--hud-hd-glass", "migration: hd glass on top of glass stays typed");
}

static void test_presets(const std::string &scratch) {
    std::string inst = path_join(scratch, "install");
    Profile l;
    apply_preset(l, Preset::Laptop, inst);
    CHECK(opts(l) == "--texture-pack|" + path_join(inst, "texpack-v3") + "|--resolution|1920x1080|--widescreen|--fullscreen|"
                     "--aniso|4|--sim60|--hud-hd-glass|--frames-in-flight|2",
          "preset: laptop (%s)", opts(l).c_str());
    CHECK(detect_preset(l) == Preset::Laptop, "preset: laptop detected");
    Profile g;
    apply_preset(g, Preset::Gaming, inst);
    CHECK(opts(g) == "--texture-pack|" + path_join(inst, "texpack-v3") + "|--resolution|1920x1080|--widescreen|--fullscreen|"
                     "--msaa|4|--aniso|4|--sim60|--hud-hd-glass|--supersample|2|--shadow-scale|2|--shadow-soft|--shadow-range|--bloom|--rain-hd|--ssao",
          "preset: gaming (%s)", opts(g).c_str());
    CHECK(detect_preset(g) == Preset::Gaming && detect_preset(recommended_profile(inst)) == Preset::Gaming,
          "preset: gaming detected, recommended profile is gaming");
    CHECK(recommended_profile(inst).extra_args.empty(), "preset: recommended profile has no extra_args");
    Profile nb = g;
    nb.bloom = false;
    CHECK(detect_preset(nb) == Preset::Custom, "preset: Quality without bloom is custom");
    nb = g;
    nb.bloom_strength = 2;
    CHECK(detect_preset(nb) == Preset::Custom, "preset: a changed bloom strength is custom");
    apply_preset(nb, Preset::Laptop, inst);
    CHECK(!nb.bloom && nb.bloom_strength < 0 && detect_preset(nb) == Preset::Laptop, "preset: Performance turns bloom off");
    nb = g;
    nb.rain_hd = false;
    CHECK(detect_preset(nb) == Preset::Custom, "preset: Quality without better rain is custom");
    apply_preset(nb, Preset::Gaming, inst);
    CHECK(nb.rain_hd && detect_preset(nb) == Preset::Gaming, "preset: Quality turns better rain on");
    apply_preset(nb, Preset::Laptop, inst);
    CHECK(!nb.rain_hd && detect_preset(nb) == Preset::Laptop, "preset: Performance turns better rain off");
    nb = g;
    nb.ssao = false;
    CHECK(detect_preset(nb) == Preset::Custom, "preset: Quality without ambient occlusion is custom");
    nb = g;
    nb.ssao_strength = 1.2f;
    CHECK(detect_preset(nb) == Preset::Custom, "preset: a changed ambient occlusion strength is custom");
    apply_preset(nb, Preset::Gaming, inst);
    CHECK(nb.ssao && nb.ssao_strength < 0 && detect_preset(nb) == Preset::Gaming, "preset: Quality turns ambient occlusion on");
    apply_preset(nb, Preset::Laptop, inst);
    CHECK(!nb.ssao && detect_preset(nb) == Preset::Laptop, "preset: Performance turns ambient occlusion off");
    nb = g;
    nb.shadow_range = false;
    CHECK(detect_preset(nb) == Preset::Custom, "preset: Quality without shadow range is custom");
    apply_preset(nb, Preset::Laptop, inst);
    CHECK(!nb.shadow_range && detect_preset(nb) == Preset::Laptop, "preset: Performance turns shadow range off");
    g.msaa = 2;
    CHECK(detect_preset(g) == Preset::Custom, "preset: an edited covered field is custom");
    CHECK(detect_preset(Profile()) == Preset::Custom, "preset: runtime defaults are custom");
    Profile t;
    t.texture_pack = "/mine";
    apply_preset(t, Preset::Laptop, path_join(scratch, "no-packs"));
    CHECK(t.texture_pack == "/mine", "preset: a chosen texture pack is kept");
    apply_preset(l, Preset::Gaming, inst);
    CHECK(detect_preset(l) == Preset::Gaming && l.frames_in_flight == 0, "preset: laptop -> gaming clears frames in flight");
    CHECK(std::string(preset_key(Preset::Laptop)) == "laptop" && preset_from_key("gaming") == Preset::Gaming &&
          preset_from_key("x") == Preset::Custom, "preset: keys");
}

/* --capture's PNG: signature, IHDR, one stored deflate block, the well-known IEND CRC. */
static void test_png() {
    const uint8_t px[8] = {255, 0, 0, 255, 0, 255, 0, 128};
    std::string png = encode_png_rgba(2, 1, px, 8);
    size_t want = 8 + (12 + 13) + (12 + 2 + 5 + 9 + 4) + 12;
    CHECK(png.size() == want && png.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0, "png: size %zu (want %zu), signature",
          png.size(), want);
    CHECK(png.compare(png.size() - 8, 8, std::string("IEND\xae\x42\x60\x82", 8)) == 0, "png: IEND chunk and CRC");
    CHECK(png.compare(12, 4, "IHDR") == 0 && (uint8_t)png[19] == 2 && (uint8_t)png[23] == 1 && png[24] == 8 && png[25] == 6,
          "png: IHDR 2x1 RGBA8");
    CHECK(png.find(std::string("\x00\xff\x00\x00\xff\x00\xff\x00\x80", 9)) != std::string::npos, "png: filter byte + raw row");
}

/* ---- XDVDFS disc images ---- */

/* A synthetic XDVDFS image, built here (no game data): the volume descriptor at sector 32, the
 * root directory at sector 33, a Media directory at sector 34 and the file data from sector 40
 * (default.xbe takes two sectors).
 * Each directory table chains its entries through the right-subtree offsets. */
struct FakeEntry { std::string name; uint8_t attr; uint32_t sector, size; };
#define SECTOR 2048

static std::string dir_table(const std::vector<FakeEntry> &entries) {
    std::string t;
    for (size_t i = 0; i < entries.size(); i++) {
        const FakeEntry &e = entries[i];
        size_t len = (14 + e.name.size() + 3) & ~(size_t)3;
        uint16_t left = 0, right = i + 1 < entries.size() ? (uint16_t)((t.size() + len) / 4) : 0;
        std::string d(len, '\0');
        memcpy(&d[0], &left, 2);
        memcpy(&d[2], &right, 2);
        memcpy(&d[4], &e.sector, 4);
        memcpy(&d[8], &e.size, 4);
        d[12] = (char)e.attr;
        d[13] = (char)e.name.size();
        memcpy(&d[14], e.name.data(), e.name.size());
        t += d;
    }
    return t;
}

static void put(std::string &img, uint32_t sector, const std::string &data) {
    size_t at = (size_t)sector * SECTOR;
    if (img.size() < at + data.size()) img.resize(at + data.size(), '\0');
    memcpy(&img[at], data.data(), data.size());
}

struct FakeImage {
    std::string bytes;
    size_t root_at = 33 * SECTOR, media_at = 34 * SECTOR; /* where the tables start */
};

static FakeImage fake_image(const std::string &xbe, const std::string &root_name0 = "default.xbe") {
    FakeImage f;
    std::string a = "alpha data", big(5000, 'b');
    std::vector<FakeEntry> media = {{"a.bin", 0x20, 43, (uint32_t)a.size()}, {"empty", 0x10, 0, 0},
                                    {"big file.dat", 0x80, 44, (uint32_t)big.size()}};
    std::string mt = dir_table(media);
    std::vector<FakeEntry> root = {{root_name0, 0x20, 40, (uint32_t)xbe.size()}, {"Media", 0x10, 34, (uint32_t)mt.size()},
                                   {"zero.txt", 0x20, 0, 0}};
    std::string rt = dir_table(root);
    std::string vd(SECTOR, '\0');
    memcpy(&vd[0], "MICROSOFT*XBOX*MEDIA", 20);
    uint32_t rs = 33, rsz = (uint32_t)rt.size();
    memcpy(&vd[20], &rs, 4);
    memcpy(&vd[24], &rsz, 4);
    memcpy(&vd[0x7EC], "MICROSOFT*XBOX*MEDIA", 20);
    put(f.bytes, 32, vd);
    put(f.bytes, 33, rt);
    put(f.bytes, 34, mt);
    put(f.bytes, 40, xbe);
    put(f.bytes, 43, a);
    put(f.bytes, 44, big);
    f.bytes.resize(48 * SECTOR, '\0');
    return f;
}

/* Writes the image at partition (sparse below it). */
static bool write_image(const std::string &path, const std::string &bytes, long partition = 0) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fseek(f, partition, SEEK_SET) == 0 && fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    return fclose(f) == 0 && ok;
}

static std::string slurp(const std::string &path) {
    std::string s;
    read_file(path, s, 1 << 20);
    return s;
}

static bool open_fails(const std::string &scratch, const std::string &bytes, const char *what) {
    std::string path = path_join(scratch, "bad.iso");
    write_image(path, bytes);
    IsoImage img;
    std::string err;
    bool failed = !iso_open(path, img, &err) && !err.empty();
    CHECK(failed, "iso malformed: %s is a clean error (%s)", what, err.c_str());
    std::remove(path.c_str());
    return failed;
}

static void test_iso(const std::string &scratch) {
    std::string xbe = fake_xbe("XBEH", "The Warriors");
    FakeImage f = fake_image(xbe);
    std::string iso = path_join(scratch, "disc image.iso");
    write_image(iso, f.bytes);

    IsoImage img;
    std::string err;
    CHECK(iso_open(iso, img, &err), "iso: opens a trimmed image (%s)", err.c_str());
    CHECK(img.partition == 0 && img.image_size == f.bytes.size(), "iso: partition at 0, size %llu",
          (unsigned long long)img.image_size);
    CHECK(img.files.size() == 4 && img.dirs.size() == 2, "iso: 4 files, 2 folders (%zu, %zu)", img.files.size(), img.dirs.size());
    CHECK(img.total_bytes == xbe.size() + 10 + 5000, "iso: total bytes %llu", (unsigned long long)img.total_bytes);
    const IsoFile *big = iso_find(img, "MEDIA/Big File.dat");
    CHECK(big && big->path == "Media/big file.dat" && big->size == 5000 && big->offset == 44 * SECTOR,
          "iso: find is case-insensitive, nested path and offset");
    CHECK(iso_check_xbe(img).status == XbeStatus::Ok, "iso: default.xbe in the image is The Warriors");

    /* Extraction, then a resumed one that only redoes the changed and missing files. */
    std::string dest = path_join(scratch, "extracted game");
    for (const char *rel : {"default.xbe", "zero.txt", "Media/a.bin", "Media/big file.dat"}) /* a previous run */
        std::remove(path_join(dest, rel).c_str());
    CHECK(iso_bytes_needed(img, dest) == img.total_bytes, "iso: nothing extracted yet needs all bytes");
    IsoProgress prog;
    IsoResult r = iso_extract(img, dest, &prog, &err);
    CHECK(r == IsoResult::Ok && prog.files == 4 && prog.bytes == img.total_bytes && prog.skipped == 0,
          "iso extract: %u files, %llu bytes (%s)", (unsigned)prog.files, (unsigned long long)prog.bytes.load(), err.c_str());
    std::string media = path_join(dest, "Media");
    CHECK(slurp(path_join(dest, "default.xbe")) == xbe && slurp(path_join(media, "a.bin")) == "alpha data" &&
          slurp(path_join(media, "big file.dat")) == std::string(5000, 'b') && is_dir(path_join(media, "empty")) &&
          is_file(path_join(dest, "zero.txt")), "iso extract: contents, empty folder and empty file");
    CHECK(check_xbe_file(path_join(dest, "default.xbe")).status == XbeStatus::Ok, "iso extract: usable default.xbe");
    CHECK(iso_bytes_needed(img, dest) == 0, "iso: a complete copy needs no bytes");
    write_file(path_join(media, "a.bin"), "short");
    std::remove(path_join(media, "big file.dat").c_str());
    CHECK(iso_bytes_needed(img, dest) == 5010, "iso: changed and missing files are needed (%llu)",
          (unsigned long long)iso_bytes_needed(img, dest));
    IsoProgress again;
    r = iso_extract(img, dest, &again, &err);
    CHECK(r == IsoResult::Ok && again.skipped == 2 && again.files == 4 && slurp(path_join(media, "a.bin")) == "alpha data"
          && slurp(path_join(media, "big file.dat")).size() == 5000,
          "iso extract: resumed, %u of 4 files skipped", (unsigned)again.skipped);
    std::string after = slurp(iso);
    CHECK(after == f.bytes, "iso: the image is never modified");

    IsoProgress cancelled;
    cancelled.cancel = true;
    std::string dest2 = path_join(scratch, "cancelled");
    CHECK(iso_extract(img, dest2, &cancelled, &err) == IsoResult::Cancelled && !is_file(path_join(dest2, "default.xbe")),
          "iso extract: cancel leaves no partial file");

    /* A full (Redump-style) image: the game partition at 0x18300000, sparse below. */
    std::string full = path_join(scratch, "full.iso");
    write_image(full, f.bytes, 0x18300000);
    IsoImage fimg;
    CHECK(iso_open(full, fimg, &err) && fimg.partition == 0x18300000 && fimg.files.size() == 4,
          "iso: full image, partition at 0x18300000 (%s)", err.c_str());
    const IsoFile *fx = iso_find(fimg, "default.xbe");
    CHECK(fx && fx->offset == 0x18300000ull + 40 * SECTOR && iso_check_xbe(fimg).status == XbeStatus::Ok,
          "iso: full image file offsets include the partition");
    std::remove(full.c_str());

    /* Another title: refused by the title check. */
    std::string other = path_join(scratch, "other.iso");
    write_image(other, fake_image(fake_xbe("XBEH", "Other Game")).bytes);
    IsoImage oimg;
    XbeCheck oc = iso_open(other, oimg, &err) ? iso_check_xbe(oimg) : XbeCheck();
    CHECK(oc.status == XbeStatus::WrongTitle && oc.title == "Other Game", "iso: another title is refused (%s)", oc.title.c_str());
    write_image(other, fake_image(xbe, "game.xbe").bytes);
    oimg = IsoImage();
    CHECK(iso_open(other, oimg, &err) && iso_check_xbe(oimg).status == XbeStatus::Missing, "iso: no default.xbe");
    std::remove(other.c_str());

    /* Malformed images. */
    std::string b = f.bytes;
    memset(&b[32 * SECTOR], 0, 20);
    open_fails(scratch, b, "no XDVDFS magic");
    open_fails(scratch, f.bytes.substr(0, 20 * SECTOR), "truncated before the volume descriptor");
    b = f.bytes;
    uint32_t far_sector = 100000;
    memcpy(&b[32 * SECTOR + 20], &far_sector, 4);
    open_fails(scratch, b, "root directory past the end");
    b = f.bytes;
    { /* the third root entry's left subtree points back at the second, whose right is the third */
        size_t second = 28, third = 48;
        uint16_t to_second = (uint16_t)(second / 4);
        memcpy(&b[f.root_at + third], &to_second, 2);
    }
    open_fails(scratch, b, "a cycle in the directory tree");
    b = f.bytes;
    uint16_t past = 0x4000;
    memcpy(&b[f.root_at + 2], &past, 2);
    open_fails(scratch, b, "subtree offset past the directory");
    b = f.bytes;
    b[f.root_at + 13] = (char)200;
    open_fails(scratch, b, "name past the directory");
    b = f.bytes;
    memcpy(&b[f.media_at + 14], "..\0\0\0", 5);
    b[f.media_at + 13] = 2;
    open_fails(scratch, b, "a '..' name");
    b = f.bytes;
    b[f.media_at + 14 + 1] = '/';
    open_fails(scratch, b, "a name with a slash");
    b = f.bytes;
    b[f.media_at + 14] = 'C', b[f.media_at + 15] = ':';
    open_fails(scratch, b, "a drive-letter name");
    b = f.bytes;
    uint32_t huge = 0x7FFFFFFF;
    memcpy(&b[f.media_at + 8], &huge, 4);
    open_fails(scratch, b, "file data past the end of the image");
    b = f.bytes;
    { /* the 'empty' folder in Media points back at Media: a folder that contains itself */
        size_t empty = f.media_at + 20;
        uint32_t media_sector = 34, media_size;
        memcpy(&media_size, &f.bytes[f.root_at + 28 + 8], 4);
        memcpy(&b[empty + 4], &media_sector, 4);
        memcpy(&b[empty + 8], &media_size, 4);
    }
    open_fails(scratch, b, "a folder that contains itself");
    std::remove(iso.c_str());
}

/* ---- Title image (synthetic XPR0 textures, no game data) ---- */

static void put_le32(std::string &s, size_t at, uint32_t v) {
    if (s.size() < at + 4) s.resize(at + 4, '\0');
    for (int i = 0; i < 4; i++) s[at + i] = (char)((v >> (8 * i)) & 0xFF);
}

/* An XPR0 bundle with one w x h texture (log2 sizes) of the colour format fmt; header 64 bytes. */
static std::string fake_xpr(uint32_t fmt, int lw, int lh, const std::string &texels) {
    std::string x = "XPR0";
    put_le32(x, 4, (uint32_t)(64 + texels.size()));
    put_le32(x, 8, 64);
    put_le32(x, 12, 0x00040001);                                           /* Common: texture, 1 ref */
    put_le32(x, 24, fmt << 8 | 2 << 4 | 1 << 16 | (uint32_t)lw << 20 | (uint32_t)lh << 24); /* Format */
    x.resize(64, '\0');
    return x + texels;
}

static std::string dxt1_block(uint16_t c0, uint16_t c1, uint32_t idx) {
    std::string b(8, '\0');
    b[0] = (char)(c0 & 0xFF), b[1] = (char)(c0 >> 8), b[2] = (char)(c1 & 0xFF), b[3] = (char)(c1 >> 8);
    put_le32(b, 4, idx);
    return b;
}

static const uint8_t *px_at(const Image &im, int x, int y) { return &im.rgba[((size_t)y * im.w + x) * 4]; }

static void test_titleimage(const std::string &scratch) {
    Image im;
    std::string err;
    std::string x = fake_xpr(0x0C, 2, 2, dxt1_block(0xF800, 0x001F, 0xE4)); /* row 0: c0 c1 2/3 1/3 */
    bool ok = xpr_decode((const uint8_t *)x.data(), x.size(), im, &err);
    CHECK(ok && im.w == 4 && im.h == 4, "xpr: DXT1 4x4 (%s)", err.c_str());
    if (ok) {
        const uint8_t *a = px_at(im, 0, 0), *b = px_at(im, 1, 0), *c = px_at(im, 2, 0), *d = px_at(im, 0, 1);
        CHECK(a[0] == 255 && a[1] == 0 && a[2] == 0 && a[3] == 255 && b[0] == 0 && b[2] == 255, "xpr: DXT1 endpoints red, blue");
        CHECK(c[0] == 170 && c[2] == 85 && d[0] == 255 && d[2] == 0, "xpr: DXT1 2/3 blend, index 0 elsewhere");
    }
    x = fake_xpr(0x0C, 2, 2, dxt1_block(0x001F, 0xF800, 0xC0)); /* c0 <= c1: index 3 is transparent */
    ok = xpr_decode((const uint8_t *)x.data(), x.size(), im, &err);
    CHECK(ok && px_at(im, 3, 0)[3] == 0 && px_at(im, 0, 0)[3] == 255, "xpr: DXT1 1-bit alpha");

    std::string texels(4 * 4 * 4, '\0'); /* swizzled A8R8G8B8 4x4: B G R A, value = offset */
    for (int i = 0; i < 16; i++) texels[i * 4] = (char)i, texels[i * 4 + 2] = (char)(100 + i), texels[i * 4 + 3] = (char)200;
    x = fake_xpr(0x06, 2, 2, texels);
    ok = xpr_decode((const uint8_t *)x.data(), x.size(), im, &err);
    CHECK(ok && px_at(im, 1, 0)[2] == 1 && px_at(im, 0, 1)[2] == 2 && px_at(im, 1, 1)[2] == 3 && px_at(im, 2, 0)[2] == 4 &&
              px_at(im, 0, 2)[2] == 8 && px_at(im, 3, 3)[2] == 15,
          "xpr: A8R8G8B8 unswizzled (Morton order, x first)");
    CHECK(ok && px_at(im, 2, 0)[0] == 104 && px_at(im, 2, 0)[3] == 200, "xpr: BGRA to RGBA");

    x = fake_xpr(0x0C, 4, 4, dxt1_block(0xF800, 0xF800, 0)); /* 16x16 needs 16 blocks */
    CHECK(!xpr_decode((const uint8_t *)x.data(), x.size(), im, &err) && err.find("past the end") != std::string::npos,
          "xpr: truncated data rejected");
    x = fake_xpr(0x05, 2, 2, std::string(64, '\0'));
    CHECK(!xpr_decode((const uint8_t *)x.data(), x.size(), im, &err) && err.find("0x05") != std::string::npos,
          "xpr: unsupported format named (%s)", err.c_str());
    CHECK(!xpr_decode((const uint8_t *)"XPR1", 4, im, nullptr), "xpr: bad magic rejected");

    /* An XBE with a $$XTIMAGE section of a 16x16 red DXT1 texture. */
    std::string red;
    for (int i = 0; i < 16; i++) red += dxt1_block(0xF800, 0xF800, 0);
    std::string xpr = fake_xpr(0x0C, 4, 4, red), xbe = "XBEH";
    const uint32_t base = 0x10000;
    put_le32(xbe, 0x104, base);
    put_le32(xbe, 0x108, 0x400);           /* size of headers */
    put_le32(xbe, 0x11C, 2);               /* sections */
    put_le32(xbe, 0x120, base + 0x200);
    put_le32(xbe, 0x200 + 20, base + 0x300); /* .text, no data */
    put_le32(xbe, 0x238 + 12, 0x400);
    put_le32(xbe, 0x238 + 16, (uint32_t)xpr.size());
    put_le32(xbe, 0x238 + 20, base + 0x310);
    xbe.resize(0x400, '\0');
    memcpy(&xbe[0x300], ".text", 6);
    memcpy(&xbe[0x310], "$$XTIMAGE", 10);
    size_t off = 0, len = 0;
    CHECK(xbe_section((const uint8_t *)xbe.data(), xbe.size(), "$$XTIMAGE", off, len) && off == 0x400 && len == xpr.size(),
          "xbe: $$XTIMAGE section found");
    CHECK(!xbe_section((const uint8_t *)xbe.data(), xbe.size(), "$$XTIMAG", off, len), "xbe: section names match whole");
    xbe += xpr;
    std::string xbe_path = path_join(scratch, "title.xbe"), cache = path_join(scratch, "cache");
    write_file(xbe_path, xbe);
    Image bg;
    ok = title_background(xbe_path, cache, bg, &err);
    CHECK(ok && bg.w == 384 && bg.h == 384, "background: 384x384 from a 16x16 title (%s)", err.c_str());
    if (ok) { /* uniform red, desaturated by 0.15 and darkened to 0.6 */
        const uint8_t *c = px_at(bg, 192, 192);
        CHECK(c[0] >= 135 && c[0] <= 139 && c[1] >= 5 && c[1] <= 9 && c[2] == c[1] && c[3] == 255, "background: %d %d %d", c[0],
              c[1], c[2]);
    }
    Image again;
    write_file(xbe_path, xbe.substr(0, 0x400)); /* the section now points past the end: only the cache can answer */
    CHECK(!title_background(xbe_path, cache, again, nullptr), "background: a broken XBE fails");
    write_file(xbe_path, xbe);
    CHECK(title_background(xbe_path, cache, again, nullptr) && again.rgba == bg.rgba, "background: cached PNG read back");
    write_file(xbe_path, xbe.substr(0, 0x300) + std::string(0x100, '\0'));
    CHECK(!title_background(xbe_path, cache, again, &err) && err.find("no $$XTIMAGE") != std::string::npos,
          "background: no section (%s)", err.c_str());

    /* A game folder with a boot splash: XBoxWad.idx + Main.wad1 holding a 32x32 DXT3 texture whose
     * top 24 rows are opaque red (the picture) and the rest transparent (padding). */
    std::string disc = path_join(scratch, "disc");
    make_dir(disc);
    write_file(path_join(disc, "default.xbe"), xbe);
    std::string tex;
    for (int b = 0; b < 64; b++) tex += std::string(8, b < 48 ? '\xff' : '\0') + dxt1_block(0xF800, 0xF800, 0);
    std::string splash(0xA0, '\0');
    put_le32(splash, 0x2C, (uint32_t)tex.size());
    splash += tex;
    size_t res = splash.size();
    put_le32(splash, res, 0x00040001);
    put_le32(splash, res + 12, 0x0E << 8 | 2 << 4 | 1 << 16 | 5 << 20 | 5 << 24);
    splash.resize(res + 20, '\0');
    std::string idx;
    put_le32(idx, 0, 2);            /* wads */
    put_le32(idx, 4, 2);            /* files */
    put_le32(idx, 12, 20);          /* wad table */
    put_le32(idx, 16, 20 + 2 * 36); /* file table */
    idx.resize(20 + 2 * 36 + 2 * 16, '\0');
    memcpy(&idx[20], "System.wad0", 11);
    memcpy(&idx[56], "Main.wad1", 9);
    put_le32(idx, 92, 1u << 24 | 0), put_le32(idx, 96, 10), put_le32(idx, 104, 0x12345678); /* another file */
    put_le32(idx, 108, 1u << 24 | 2), put_le32(idx, 112, (uint32_t)splash.size()), put_le32(idx, 120, SPLASH_HASH);
    write_file(path_join(disc, "XBoxWad.idx"), idx);
    write_file(path_join(disc, "Main.wad1"), std::string(2048, '\0') + splash);
    std::string wad;
    uint64_t woff = 0;
    uint32_t wsize = 0;
    CHECK(wad_find(disc, SPLASH_HASH, wad, woff, wsize, &err) && wad == path_join(disc, "Main.wad1") && woff == 2048 &&
              wsize == splash.size(),
          "wad: splash entry found (%s)", err.c_str());
    CHECK(!wad_find(disc, 0xDEADBEEF, wad, woff, wsize, nullptr), "wad: unknown hash");
    ok = splash_decode((const uint8_t *)splash.data(), splash.size(), im, &err);
    CHECK(ok && im.w == 32 && im.h == 24 && px_at(im, 5, 23)[0] == 255 && px_at(im, 5, 23)[3] == 255,
          "splash: DXT3 decoded and cropped to its opaque rows (%s)", err.c_str());
    CHECK(!splash_decode((const uint8_t *)splash.data(), splash.size() - 1, im, nullptr), "splash: truncated resource");
    std::string source;
    ok = game_background(disc, cache, bg, &source, &err);
    CHECK(ok && source == "boot splash" && bg.w == 768 && bg.h == 576, "background: splash first, %dx%d", bg.w, bg.h);
    remove_file(path_join(disc, "XBoxWad.idx"));
    ok = game_background(disc, cache, bg, &source, &err);
    CHECK(ok && source == "title image" && bg.w == 384, "background: title image without the splash");
    remove_file(path_join(disc, "default.xbe"));
    CHECK(!game_background(disc, cache, bg, &source, &err) && err.find("splash:") != std::string::npos &&
              err.find("title image:") != std::string::npos,
          "background: neither (%s)", err.c_str());

    Image wide;
    wide.w = 20, wide.h = 10;
    wide.rgba.assign(20 * 10 * 4, 255);
    Image b = blur_darken(wide, 64, 4, 1, 0);
    CHECK(b.w == 64 && b.h == 32 && b.rgba[0] == 255, "blur: aspect kept, white stays white");

    const uint8_t px[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24};
    std::string png = encode_png_rgba(3, 2, px, 12);
    int w = 0, h = 0;
    std::vector<uint8_t> back;
    CHECK(decode_png_rgba(png, w, h, back) && w == 3 && h == 2 && back.size() == 24 && memcmp(back.data(), px, 24) == 0,
          "png: decode round trip");
    png[45] ^= 1;
    CHECK(!decode_png_rgba(png, w, h, back), "png: bad CRC rejected");
}

/* ---- UI strings ---- */

/* The printf conversions of a format ("%s", "%d"; "%%" and a "%" before a space are text). */
static std::string conversions(const char *f) {
    std::string out;
    for (; *f; f++)
        if (f[0] == '%' && (f[1] == 's' || f[1] == 'd')) out += f[1], f++;
    return out;
}

static void test_i18n() {
    int missing = 0, mismatched = 0;
    for (int i = 0; i < (int)S::Count; i++) {
        const char *en = tr((S)i, Lang::En), *es = tr((S)i, Lang::Es);
        if (!en || !es || !*en || !*es) missing++, printf("     id %d has no text in a language\n", i);
        else if (conversions(en) != conversions(es)) mismatched++, printf("     id %d: \"%s\" vs \"%s\"\n", i, en, es);
    }
    CHECK(missing == 0, "i18n: all %d strings have English and Spanish", (int)S::Count);
    CHECK(mismatched == 0, "i18n: placeholders match between the languages");
    set_ui_lang(Lang::Es);
    CHECK(std::string(tr(S::play)) == "JUGAR" && trf(S::setup_step, 2) == "Configuración: paso 2 de 4", "i18n: Spanish tr/trf");
    set_ui_lang(Lang::En);
    CHECK(trf(S::setup_step, 3) == "Setup: step 3 of 4", "i18n: English trf");
    CHECK(lang_from_setting("es") == Lang::Es && lang_from_setting("en") == Lang::En && lang_from_setting("x") == system_lang(),
          "i18n: setting, else the system's language");
    ProfileFile f, g;
    f.profiles.push_back(Profile());
    f.settings.ui_language = "es";
    CHECK(parse_ini(write_ini(f), g, nullptr) && g.settings.ui_language == "es" && g.settings.unknown.empty(),
          "ini: ui_language round trip");
}

static void test_keymap(const std::string &scratch) {
    KeyMap d;
    CHECK(d.binding['w'] == CTL_LS_UP && d.binding[MOUSE_LEFT] == CTL_X && d.binding[' '] == CTL_A && d.binding['g'] == CTL_SPIN &&
          d.binding[KEY_LSHIFT] == CTL_RT && d.binding[KEY_ESCAPE] == -1, "keymap: the runtime's PC layout");
    std::vector<int> x = d.keys_for(CTL_X);
    CHECK(x.size() == 2 && x[0] == MOUSE_LEFT && x[1] == 'r', "keymap: keys of an action, mouse first");
    CHECK(key_config_name(KEY_LSHIFT) == std::string("lshift") && key_from_config_name("Mouse.Right") == MOUSE_RIGHT &&
          key_from_config_name("escape") == -1 && key_from_config_name("f1") == -1 && key_from_config_name("f12") == KEY_F1 + 11,
          "keymap: config names (case-insensitive, escape/f1 reserved)");
    CHECK(key_display_name('e') == "E" && key_display_name(KEY_LSHIFT) == "Shift" && key_display_name(' ') == "Space" &&
          key_display_name(MOUSE_RIGHT) == "Mouse right" && key_display_name(KEY_F1 + 4) == "F5", "keymap: display names");

    std::vector<std::string> warn;
    KeyMap m = parse_keymap("# comment\nkey.E = b\nMOUSE.X1 = y\nmouse.sensitivity = 1.5\nkey.v = none\n"
                            "key.escape = a\nkey.zz = a\nkey.h = bogus\nnot a line\nspin.rate = 3\n", &warn);
    CHECK(m.binding['e'] == CTL_B && m.binding[MOUSE_X1] == CTL_Y && m.binding['v'] == -1 && m.binding['w'] == CTL_LS_UP,
          "keymap: file lines change the defaults");
    CHECK(warn.size() == 4 && m.get("mouse.sensitivity") == "1.5" && m.get("spin.rate") == "3" && m.get("mouse.deadzone") == "0.15" &&
          m.settings.size() == d.settings.size(), "keymap: 4 invalid lines reported, settings replace the defaults (%d)", (int)warn.size());
    KeyMap r = parse_keymap(write_keymap(m), &warn);
    CHECK(r == m, "keymap: write -> parse round trip");
    KeyMap e = parse_keymap("", nullptr);
    CHECK(e == d && parse_keymap(write_keymap(d), nullptr) == d, "keymap: empty file = defaults, defaults round trip");

    CHECK(m.bind('q', CTL_A) == CTL_LT && m.binding['q'] == CTL_A && m.keys_for(CTL_LT).empty(),
          "keymap: binding a used key moves it and reports its previous action");
    CHECK(m.bind(KEY_ESCAPE, CTL_A) == -2 && m.bind(KEY_F1, CTL_A) == -2 && m.bind(MOUSE_MIDDLE, CTL_RTHUMB) == CTL_RTHUMB,
          "keymap: reserved keys refused, rebinding the same action is no conflict");
    m.reset();
    CHECK(m == d && m.get("mouse.sensitivity") == "1.0", "keymap: reset to defaults");

    std::string path = path_join(scratch, "keys.cfg");
    KeyMap s;
    s.bind('z', CTL_X);
    s.unbind(MOUSE_LEFT);
    std::string err;
    KeyMap back;
    CHECK(save_keymap(path, s, &err) && load_keymap(path, back, nullptr) && back == s, "keymap: save + load (%s)", err.c_str());
    CHECK(!load_keymap(path_join(scratch, "missing.cfg"), back, nullptr), "keymap: missing file");
    Profile kp;
    CHECK(keymap_path(kp).empty(), "keymap path: none without a game program");
    kp.xbrun = "/opt/w/TheWarriors";
    CHECK(keymap_path(kp) == "/opt/w/warriors-input.cfg", "keymap path: the runtime's default next to the game (%s)", keymap_path(kp).c_str());
    kp.input_config = "keys/mine.cfg";
    CHECK(keymap_path(kp) == "/opt/w/keys/mine.cfg", "keymap path: input_config relative to the game folder");
    kp.input_config = "/k.cfg";
    CHECK(keymap_path(kp) == "/k.cfg", "keymap path: absolute input_config");
    CHECK(pad_db_path(kp) == "/opt/w/gamecontrollerdb.txt" && pad_db_path(Profile()).empty(),
          "pad mappings: gamecontrollerdb.txt next to the game program (%s)", pad_db_path(kp).c_str());
    /* The defaults as the launcher writes them (tests/test_launcher.py feeds this file to the
     * runtime's input_map_test, which checks it restates the runtime's own defaults). */
    CHECK(save_keymap(path_join(scratch, "keys-defaults.cfg"), KeyMap(), &err), "keymap: defaults file written");
}

/* A synthetic save tree: <hdd>/partition1/UDATA/5454009b with a profile folder, plus a file of
 * another title and one on the partition root that must never be touched. */
static void write_save(const std::string &hdd, const std::string &profile_bytes) {
    std::string udata = path_join(path_join(hdd, "partition1"), "UDATA");
    std::string save = path_join(udata, SAVE_TITLE_ID), prof = path_join(save, "0123456789AB");
    make_dirs(prof);
    write_file(path_join(save, "TitleMeta.xbx"), "title meta");
    write_file(path_join(prof, "profile.dat"), profile_bytes);
    write_file(path_join(prof, "SaveMeta.xbx"), "save meta");
    make_dirs(path_join(udata, "4d530004"));
    write_file(path_join(path_join(udata, "4d530004"), "other.dat"), "another title");
    write_file(path_join(hdd, "partition1.marker"), "outside");
}

static void remove_tree_test(const std::string &path) {
    std::vector<DirEntry> list;
    if (!list_dir(path, list)) return;
    for (const DirEntry &e : list)
        if (e.dir) remove_tree_test(path_join(path, e.name));
        else remove_file(path_join(path, e.name));
    remove_dir(path);
}

static void test_backup(const std::string &scratch) {
    std::string base = path_join(scratch, "backup"), hdd = path_join(base, "hdd"), data = path_join(base, "data");
    remove_tree_test(base);
    make_dirs(base);
    std::string save = save_folder(hdd), root = backup_root(data, save), made, err, text;
    CHECK(save == path_join(path_join(path_join(hdd, "partition1"), "UDATA"), SAVE_TITLE_ID), "backup: save folder under partition1/UDATA/<title>");
    CHECK(root.compare(0, data.size(), data) == 0 && root.find("save-") != std::string::npos && root != backup_root(data, save + "x"),
          "backup: one backup folder per save folder");

    CHECK(backup_save(save, root, "20261031-100000", 5, "play", false, &made, &err) == BackupResult::NoSave &&
              list_backups(root).empty(), "backup: no save yet, nothing made");
    write_save(hdd, "progress 1");
    SaveScan sc = scan_save(save);
    CHECK(sc.exists && sc.files == 3 && sc.bytes == 29 && sc.signature != 0,
          "backup: scan counts the title's files (%d files, %llu bytes)", sc.files, (unsigned long long)sc.bytes);
    BackupResult r = backup_save(save, root, "20261031-100000", 5, "play", false, &made, &err);
    std::vector<BackupInfo> list = list_backups(root);
    CHECK(r == BackupResult::Done && list.size() == 1 && list[0].name == "20261031-100000" && list[0].kind == "play" &&
              list[0].files == sc.files && list[0].bytes == sc.bytes && list[0].signature == sc.signature,
          "backup: first backup made (%s)", err.c_str());
    CHECK(read_file(path_join(path_join(path_join(made, "save"), "0123456789AB"), "profile.dat"), text, 100) && text == "progress 1" &&
              !is_dir(path_join(path_join(made, "save"), "4d530004")) && !is_file(path_join(path_join(made, "save"), "partition1.marker")),
          "backup: only the title's folder is copied");
    CHECK(backup_save(save, root, "20261031-100100", 5, "play", false, &made, &err) == BackupResult::Unchanged &&
              list_backups(root).size() == 1, "backup: an unchanged save is skipped");
    CHECK(backup_date("20261031-100000") == "2026-10-31 10:00:00" && backup_date("odd") == "odd", "backup: dates from names");

    /* rotation: keep 3 */
    for (int i = 2; i <= 5; i++) {
        write_save(hdd, "progress " + std::to_string(i) + std::string(i, '!'));
        backup_save(save, root, "20261031-10000" + std::to_string(i), 3, "play", false, &made, &err);
    }
    list = list_backups(root);
    CHECK(list.size() == 3 && list[0].name == "20261031-100005" && list[2].name == "20261031-100003",
          "backup: rotation keeps the newest 3 (%zu, newest %s)", list.size(), list.empty() ? "-" : list[0].name.c_str());
    /* the same second twice: a suffix */
    CHECK(backup_save(save, root, "20261031-100005", 0, "play", true, &made, &err) == BackupResult::Done &&
              made.size() > 2 && made.compare(made.size() - 2, 2, "-2") == 0 && list_backups(root).size() == 4,
          "backup: a taken name gets -2, keep 0 deletes nothing");
    /* an unfinished copy is ignored and removed by the next backup */
    make_dirs(path_join(root, "20261031-090000.partial"));
    CHECK(list_backups(root).size() == 4, "backup: unfinished copies are not listed");

    /* restore the oldest (progress 3) */
    list = list_backups(root);
    BackupInfo oldest = list.back();
    std::string undo;
    bool ok = restore_backup(save, root, oldest, "20261031-110000", &undo, &err);
    CHECK(ok && read_file(path_join(path_join(save, "0123456789AB"), "profile.dat"), text, 100) && text == "progress 3!!!",
          "backup: restore puts the backup back (%s)", err.c_str());
    CHECK(read_file(path_join(path_join(path_join(path_join(hdd, "partition1"), "UDATA"), "4d530004"), "other.dat"), text, 100) &&
              text == "another title" && is_file(path_join(hdd, "partition1.marker")),
          "backup: restore leaves other titles and the partition alone");
    list = list_backups(root);
    CHECK(!undo.empty() && list.size() == 5 && list[0].kind == "restore" && list[0].path == undo, "backup: the save before the restore is kept");
    std::vector<DirEntry> sib;
    list_dir(path_dir(save), sib);
    bool clean = true;
    for (const DirEntry &e : sib) clean &= e.name == SAVE_TITLE_ID || e.name == "4d530004";
    CHECK(clean, "backup: no temporary folder left next to the save");
    /* undo: restore the pre-restore backup */
    std::string undo2;
    ok = restore_backup(save, root, list[0], "20261031-110100", &undo2, &err);
    CHECK(ok && read_file(path_join(path_join(save, "0123456789AB"), "profile.dat"), text, 100) && text == "progress 5!!!!!" &&
              scan_save(save).files == 3, "backup: restoring the pre-restore backup undoes the restore");
    /* a backup that disappeared: the live save stays */
    BackupInfo gone = oldest;
    gone.path = path_join(root, "missing");
    CHECK(!restore_backup(save, root, gone, "20261031-110200", &undo2, &err) &&
              read_file(path_join(path_join(save, "0123456789AB"), "profile.dat"), text, 100) && text == "progress 5!!!!!",
          "backup: a failed restore keeps the current save");
    remove_tree_test(base);

    ProfileFile f, g;
    f.profiles.push_back(Profile());
    CHECK(write_ini(f).find("backup_") == std::string::npos && f.settings.backup_saves && f.settings.backup_count == BACKUP_COUNT_DEFAULT,
          "backup: on by default, 5 kept, not written");
    f.settings.backup_saves = false;
    f.settings.backup_count = 12;
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(!g.settings.backup_saves && g.settings.backup_count == 12 && warn.empty(), "backup: settings round trip");
    parse_ini("[launcher]\nbackup_count = 40\n[profile a]\n", g, &warn);
    CHECK(g.settings.backup_count == BACKUP_COUNT_DEFAULT && !warn.empty(), "backup: a count outside 1-20 is refused");
}

static void test_report(const std::string &scratch) {
    std::string log = path_join(scratch, "report.log"), body;
    for (int i = 1; i <= 300; i++) body += "line " + std::to_string(i) + (i % 2 ? "\r\n" : "\n");
    write_file(log, body);
    std::vector<std::string> tail;
    CHECK(read_log_tail(log, 50, tail) && tail.size() == 50 && tail[0] == "line 251" && tail[49] == "line 300",
          "report: the last 50 lines, CR LF stripped (%zu, %s)", tail.size(), tail.empty() ? "-" : tail[0].c_str());
    CHECK(!read_log_tail(path_join(scratch, "no-such.log"), 10, tail), "report: a missing log");
    CHECK(log_level("FAULT at 0x1234") == LogLevel::Error && log_level("[audio] cannot open device") == LogLevel::Error &&
              log_level("[texpack] warning: 3 skipped") == LogLevel::Warn && log_level("[start] reading x") == LogLevel::Info,
          "report: line levels");

    std::vector<ScrubRule> rules = scrub_rules({{"/home/jdoe/Games/Warriors/disc", "<game>"}, {"/home/jdoe/Games/Warriors/hdd", "<save>"},
                                                {"C:\\Users\\jdoe\\Warriors", "<game-folder>"}},
                                               "/home/jdoe", "jdoe", "jdoe-desktop");
    std::string in = "xbrun /home/jdoe/Games/Warriors/disc/default.xbe /home/jdoe/Games/Warriors/disc --hdd /home/jdoe/Games/Warriors/hdd\n"
                     "texpack /home/jdoe/packs/x, user jdoe on jdoe-desktop, JDOE-DESKTOP\n"
                     "win c:/users/JDOE/warriors/TheWarriors.exe and C:\\Users\\jdoe\\Documents\n"
                     "jdoes and xjdoe stay, [renderer] Vulkan on NVIDIA\n";
    std::string out = scrub(in, rules);
    CHECK(out.find("<game>/default.xbe <game> --hdd <save>") != std::string::npos, "report: game paths become labels");
    CHECK(out.find("texpack ~/packs/x") != std::string::npos && out.find("user <user> on <host>, <host>") != std::string::npos,
          "report: home, user and machine names");
    CHECK(out.find("win <game-folder>/TheWarriors.exe") != std::string::npos && out.find("C:\\Users\\<user>\\Documents") != std::string::npos,
          "report: Windows paths, either separator, any case");
    CHECK(out.find("jdoes and xjdoe stay") != std::string::npos, "report: names only as whole words");
    CHECK(scrub("ab and xy", scrub_rules({}, "", "ab", "xy")) == "ab and xy", "report: names shorter than 3 are kept");

    ReportInfo info;
    info.launcher_version = "v1.2-3-gabc";
    info.os = "Linux 6.1 (x86_64)";
    info.command = "/home/jdoe/Games/Warriors/xbrun /home/jdoe/Games/Warriors/disc/default.xbe";
    info.log.push_back("# warriors-launcher: /home/jdoe/Games/Warriors/xbrun ...");
    info.log.push_back("[start] xbrun v9.9-1-gdef");
    info.log.push_back("[renderer] Vulkan on NVIDIA GeForce RTX 3060 (driver 580.95.5), 1920x1080 (back buffer 640x480), 4 samples");
    for (int i = 0; i < 250; i++) info.log.push_back("frame " + std::to_string(i));
    std::string rep = bug_report(info, rules);
    CHECK(rep.find("Launcher: v1.2-3-gabc") != std::string::npos && rep.find("Runtime: v9.9-1-gdef") != std::string::npos &&
              rep.find("GPU: NVIDIA GeForce RTX 3060 (driver 580.95.5)") != std::string::npos && rep.find("OS: Linux 6.1") != std::string::npos,
          "report: versions, OS and GPU");
    CHECK(rep.find("frame 249") != std::string::npos && rep.find("\nframe 50\n") != std::string::npos && rep.find("\nframe 49\n") == std::string::npos &&
              rep.find("jdoe") == std::string::npos, "report: last 200 lines, scrubbed");
}

static void test_sharpen_key() {
    ProfileFile f, g;
    Profile p;
    p.sharpen = 75;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].sharpen == 75, "sharpen: ini round trip");
    Profile m;
    m.extra_args = "--sharpen 30 --sharpen 101";
    migrate_extra_args(m);
    CHECK(m.sharpen == 30 && m.extra_args == "--sharpen 101", "sharpen: moved out of extra_args, a bad value stays typed (%d, %s)", m.sharpen,
          m.extra_args.c_str());
}

static void test_bloom_key() {
    ProfileFile f, g;
    Profile p;
    p.bloom = true;
    p.bloom_strength = 0.75f;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].bloom && g.profiles[0].bloom_strength == 0.75f && warn.empty(), "bloom: ini round trip");
    CHECK(!Profile().bloom && Profile().bloom_strength < 0, "bloom: off by default, strength left out");
    Profile m;
    m.extra_args = "--bloom --bloom-strength 1.2 --bloom-strength 9";
    migrate_extra_args(m);
    CHECK(m.bloom && m.bloom_strength == 1.2f && m.extra_args == "--bloom-strength 9",
          "bloom: moved out of extra_args, a bad value stays typed (%.2f, %s)", m.bloom_strength, m.extra_args.c_str());
    m.extra_args = "--no-bloom";
    migrate_extra_args(m);
    CHECK(!m.bloom && m.extra_args.empty(), "bloom: --no-bloom moved too");
}

static void test_rain_key() {
    ProfileFile f, g;
    Profile p;
    p.rain_hd = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].rain_hd && warn.empty(), "rain: ini round trip");
    CHECK(!Profile().rain_hd, "rain: off by default");
    Profile m;
    m.extra_args = "--rain-hd --rain-hd-density 2";
    migrate_extra_args(m);
    CHECK(m.rain_hd && m.extra_args == "--rain-hd-density 2", "rain: moved out of extra_args, the density stays typed (%s)", m.extra_args.c_str());
    CHECK(opts(m) == "--rain-hd|--rain-hd-density|2", "rain: the density still follows --rain-hd (%s)", opts(m).c_str());
    m.extra_args = "--no-rain-hd";
    migrate_extra_args(m);
    CHECK(!m.rain_hd && m.extra_args.empty(), "rain: --no-rain-hd moved too");
}

static void test_ssao_key() {
    ProfileFile f, g;
    Profile p;
    p.ssao = true;
    p.ssao_strength = 1.25f;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].ssao && g.profiles[0].ssao_strength == 1.25f && warn.empty(), "ssao: ini round trip");
    CHECK(!Profile().ssao && Profile().ssao_strength < 0, "ssao: off by default, strength left out");
    Profile m;
    m.extra_args = "--ssao --ssao-strength 1.5 --ssao-strength 7 --ssao-radius 2";
    migrate_extra_args(m);
    CHECK(m.ssao && m.ssao_strength == 1.5f && m.extra_args == "--ssao-strength 7 --ssao-radius 2",
          "ssao: moved out of extra_args, a bad value and the radius stay typed (%.2f, %s)", m.ssao_strength, m.extra_args.c_str());
    m.extra_args = "--no-ssao";
    migrate_extra_args(m);
    CHECK(!m.ssao && m.extra_args.empty(), "ssao: --no-ssao moved too");
}

static void test_shadow_range_key() {
    ProfileFile f, g;
    Profile p;
    p.shadow_range = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].shadow_range && warn.empty(), "shadow range: ini round trip");
    CHECK(!Profile().shadow_range, "shadow range: off by default");
    Profile m;
    m.extra_args = "--shadow-range --shadow-range-dist 4";
    migrate_extra_args(m);
    CHECK(m.shadow_range && m.extra_args == "--shadow-range-dist 4" && opts(m) == "--shadow-range|--shadow-range-dist|4",
          "shadow range: the flag moves out of extra_args, the distance stays typed (%s)", opts(m).c_str());
    m.extra_args = "--no-shadow-range";
    migrate_extra_args(m);
    CHECK(!m.shadow_range && m.extra_args.empty(), "shadow range: --no-shadow-range moved too");
}

static void test_uiscale() {
    CHECK(ui_scale_from_setting("") == 0 && ui_scale_from_setting("auto") == 0 && ui_scale_from_setting("1.25") == 1.25f &&
              ui_scale_from_setting("1.5") == 1.5f && ui_scale_from_setting("1") == 1 && ui_scale_from_setting("3") == 0,
          "uiscale: settings (unknown = auto)");
    CHECK(std::string(ui_scale_setting(0)) == "auto" && std::string(ui_scale_setting(1.25f)) == "1.25", "uiscale: written values");
    CHECK(auto_ui_scale(96, 1366, 728) == 1 && auto_ui_scale(0, 1920, 1040) == 1, "uiscale: auto at 96 dpi / unknown");
    CHECK(auto_ui_scale(144, 2560, 1400) == 1.5f && auto_ui_scale(120, 1920, 1040) == 1.25f, "uiscale: auto follows 150%% / 125%% dpi");
    CHECK(auto_ui_scale(192, 3840, 2120) == 2 && auto_ui_scale(300, 3840, 2120) == 2, "uiscale: auto 2x on a 200%% 4K screen, never more");
    CHECK(auto_ui_scale(144, 1366, 728) == 1 && auto_ui_scale(144, 1920, 1040) == 1.5f && auto_ui_scale(144, 1600, 860) == 1.25f,
          "uiscale: auto lowered until the window fits (%.2f %.2f %.2f)", auto_ui_scale(144, 1366, 728), auto_ui_scale(144, 1920, 1040),
          auto_ui_scale(144, 1600, 860));
    int w, h, mw, mh;
    ui_window_size(1.5f, 1366, 728, w, h, mw, mh);
    CHECK(w <= 1366 && h <= 728 && mw <= w && mh <= h && mw >= 1300 && mh >= 640, "uiscale: 1.5x on 1366x768 fits (%dx%d, min %dx%d)", w, h, mw, mh);
    ui_window_size(1, 2560, 1400, w, h, mw, mh);
    CHECK(w == UI_DEFAULT_W && h == UI_DEFAULT_H && mw == UI_MIN_W && mh == UI_MIN_H, "uiscale: 1x keeps the default size");
}

/* The controller mapper: SDL mapping syntax, the gamecontrollerdb.txt merge, and the capture from
 * synthetic raw joystick sequences. */
static PadRaw raw(int buttons, std::vector<int> axes, int hats = 1) {
    PadRaw r;
    r.buttons.assign(buttons, false);
    r.axes = axes;
    r.hats.assign(hats, 0);
    return r;
}
static int target(const char *field) {
    for (int i = 0; i < PAD_TARGET_COUNT; i++)
        if (!strcmp(PAD_TARGETS[i].field, field)) return i;
    return -1;
}
/* Feeds frames until one returns a binding; "" when none did. */
static std::string feed_all(PadCapture &c, const std::vector<PadRaw> &frames, PadTargetKind kind) {
    PadBinding b;
    for (const PadRaw &f : frames)
        if (c.feed(f, kind, b)) return b.str();
    return "";
}

static void test_padmap() {
    CHECK(PAD_TARGETS[PAD_TARGET_COUNT - 1].field && target("a") == 0 && target("righty") >= 0 && target("guide") >= 0 &&
          PAD_TARGETS[target("guide")].optional && !PAD_TARGETS[target("a")].optional, "padmap: the wizard's targets");
    PadBinding b;
    CHECK(b.str().empty(), "padmap: an unset binding is empty");
    b.kind = PadBinding::Button, b.index = 3;
    CHECK(b.str() == "b3", "padmap: button -> b3 (%s)", b.str().c_str());
    b = PadBinding(), b.kind = PadBinding::Hat, b.index = 0, b.hat_mask = 4;
    CHECK(b.str() == "h0.4", "padmap: hat down -> h0.4 (%s)", b.str().c_str());
    b = PadBinding(), b.kind = PadBinding::Axis, b.index = 2, b.half = 1;
    CHECK(b.str() == "+a2", "padmap: positive half axis -> +a2 (%s)", b.str().c_str());
    b.half = -1;
    CHECK(b.str() == "-a2", "padmap: negative half axis -> -a2 (%s)", b.str().c_str());
    b.half = 0;
    CHECK(b.str() == "a2", "padmap: whole axis -> a2 (%s)", b.str().c_str());
    b.invert = true;
    CHECK(b.str() == "a2~", "padmap: inverted axis -> a2~ (%s)", b.str().c_str());
    PadBinding p = b, m = b;
    p.half = 1, p.invert = false, m.half = -1, m.invert = false;
    CHECK(!pad_same_input(p, m) && pad_same_input(p, b) && pad_same_input(m, m), "padmap: axis halves are different inputs");

    PadBinding binds[PAD_TARGET_COUNT];
    binds[target("a")].kind = PadBinding::Button, binds[target("a")].index = 0;
    binds[target("lefttrigger")].kind = PadBinding::Axis, binds[target("lefttrigger")].index = 2;
    binds[target("righttrigger")].kind = PadBinding::Axis, binds[target("righttrigger")].index = 5, binds[target("righttrigger")].half = 1;
    binds[target("dpup")].kind = PadBinding::Hat, binds[target("dpup")].hat_mask = 1;
    binds[target("lefty")].kind = PadBinding::Axis, binds[target("lefty")].index = 1, binds[target("lefty")].invert = true;
    std::string line = pad_mapping_line("030000005e0400008e02000014010000", "Pad, Pro", binds, "Linux");
    CHECK(line == "030000005e0400008e02000014010000,Pad  Pro,a:b0,dpup:h0.1,lefttrigger:a2,lefty:a1~,righttrigger:+a5,platform:Linux,",
          "padmap: mapping line (%s)", line.c_str());

    const std::string g1 = "03000000aaaa0000bbbb000000000000", g2 = "03000000cccc0000dddd000000000000";
    std::string db = "# user mappings\n" + g1 + ",One,a:b0,platform:Linux,\n\n" + g2 + ",Two,a:b1,platform:Linux,\r\n" + g1 +
                     ",One,a:b5,platform:Windows,\n";
    std::string merged = pad_db_merge(db, g1, "Linux", g1 + ",One,a:b9,platform:Linux,");
    CHECK(merged == "# user mappings\n" + g1 + ",One,a:b9,platform:Linux,\n\n" + g2 + ",Two,a:b1,platform:Linux,\r\n" + g1 +
                        ",One,a:b5,platform:Windows,\n",
          "padmap: merge replaces the same GUID + platform, keeps comments and the others");
    CHECK(pad_db_find(merged, "03000000AAAA0000BBBB000000000000", "Linux") == g1 + ",One,a:b9,platform:Linux," &&
          pad_db_find(merged, g1, "Windows") == g1 + ",One,a:b5,platform:Windows," && pad_db_find(merged, g1, "Mac OS X").empty(),
          "padmap: find by GUID (any case) and platform");
    std::string removed = pad_db_merge(merged, g1, "Linux", "");
    CHECK(removed == "# user mappings\n\n" + g2 + ",Two,a:b1,platform:Linux,\r\n" + g1 + ",One,a:b5,platform:Windows,\n",
          "padmap: removing drops only that line");
    std::string added = pad_db_merge("# no newline at end", g2, "Linux", g2 + ",Two,a:b2,platform:Linux,");
    CHECK(added == "# no newline at end\n" + g2 + ",Two,a:b2,platform:Linux,\n", "padmap: merge appends a new line");
    CHECK(pad_db_merge("", g1, "Linux", "x") == "x\n", "padmap: merge into an empty file");
    std::string any = pad_db_merge(g1 + ",Old,a:b0,\n" + g1 + ",Dup,a:b0,platform:Linux,\n", g1, "Linux", g1 + ",New,a:b1,platform:Linux,");
    CHECK(any == g1 + ",New,a:b1,platform:Linux,\n", "padmap: a line without platform and a duplicate are replaced once");
    CHECK(pad_db_find(g1 + ",Old,a:b0,\n", g1, "Windows") == g1 + ",Old,a:b0,", "padmap: a line without platform matches any");
    CHECK(std::string(pad_platform()) ==
#ifdef _WIN32
          "Windows",
#else
          "Linux",
#endif
          "padmap: platform name");

    /* capture: a button, after everything was let go */
    PadCapture c;
    PadRaw rest = raw(12, {0, 0, -32768, 0, 0, 0});
    PadRaw held = rest;
    held.buttons[0] = true;
    c.start(held);   /* A still held from the click that started the wizard */
    CHECK(feed_all(c, {held, held}, PadTargetKind::Button).empty() && c.waiting_release(), "padmap: a held button waits for release");
    PadRaw b6 = rest;
    b6.buttons[6] = true;
    CHECK(feed_all(c, {rest, b6}, PadTargetKind::Button) == "b6", "padmap: button 6 captured");
    CHECK(feed_all(c, {b6, b6}, PadTargetKind::Button).empty(), "padmap: the same press does not count for the next step");
    /* noise under the threshold is ignored; a hat direction counts for a button */
    PadRaw noise = rest;
    noise.axes[0] = 3000, noise.axes[1] = -6000, noise.axes[2] = -32768 + 5000;
    PadRaw hat = rest;
    hat.hats[0] = 8;
    CHECK(feed_all(c, {rest, noise, noise, hat}, PadTargetKind::Button) == "h0.8", "padmap: noise ignored, hat left -> h0.8");
    PadRaw diag = rest;
    diag.hats[0] = 1 | 2;
    CHECK(feed_all(c, {rest, diag, diag}, PadTargetKind::Button).empty(), "padmap: a diagonal is not one direction");

    /* a trigger resting at -32768: the whole axis, taken when it is let go */
    c.start(rest);
    PadRaw lt = rest;
    lt.axes[2] = 32767;
    PadRaw half = rest;
    half.axes[2] = 0;
    CHECK(feed_all(c, {rest, half, lt}, PadTargetKind::Trigger).empty(), "padmap: a pressed trigger is taken when let go");
    CHECK(feed_all(c, {half, rest}, PadTargetKind::Trigger) == "a2", "padmap: trigger resting at -32768 -> a2");
    /* a trigger resting at 0 (pressed to 32767), and one going negative */
    PadRaw rest0 = raw(12, {0, 0, 0, 0, 0, 0});
    c.start(rest0);
    PadRaw rt = rest0;
    rt.axes[5] = 32767;
    CHECK(feed_all(c, {rest0, rt, rt, rest0}, PadTargetKind::Trigger) == "+a5", "padmap: trigger resting at 0 -> +a5");
    PadRaw neg = rest0;
    neg.axes[2] = -32768;
    CHECK(feed_all(c, {rest0, neg, rest0}, PadTargetKind::Trigger) == "-a2", "padmap: both triggers on one axis -> -a2");
    /* a trigger at rest 0 until first touched, then resting at -32768: the whole axis */
    PadRaw lowrest = rest0;
    lowrest.axes[4] = -32768;
    PadRaw up4 = rest0;
    up4.axes[4] = 32767;
    PadRaw mid4 = rest0;
    mid4.axes[4] = -20000;
    CHECK(feed_all(c, {rest0, up4, mid4, lowrest}, PadTargetKind::Trigger) == "a4", "padmap: rest learnt when the trigger settles at -32768");
    PadRaw up4b = lowrest;
    up4b.axes[4] = 32767;
    CHECK(feed_all(c, {lowrest, up4b, lowrest}, PadTargetKind::Trigger) == "a4", "padmap: the learnt rest is kept");
    /* a trigger resting at 32767 (pressed to -32768): inverted */
    PadRaw high = rest0;
    high.axes[3] = 32767;
    c.start(high);
    PadRaw down3 = high;
    down3.axes[3] = -32768;
    CHECK(feed_all(c, {high, down3, high}, PadTargetKind::Trigger) == "a3~", "padmap: trigger resting at 32767 -> a3~");
    /* a trigger that is a button, and one that reports a button and an axis */
    c.start(rest);
    PadRaw b7 = rest;
    b7.buttons[7] = true;
    std::vector<PadRaw> only_button(15, b7);
    only_button[0] = rest;
    CHECK(feed_all(c, only_button, PadTargetKind::Trigger) == "b7", "padmap: a digital trigger -> b7");
    PadRaw both = b7;
    both.axes[2] = 32767;
    CHECK(feed_all(c, {rest, b7, both, both, rest}, PadTargetKind::Trigger) == "a2", "padmap: a trigger's axis wins over its button");
    /* sticks: the whole axis, inverted when it moves the other way */
    c.start(rest0);
    PadRaw right = rest0;
    right.axes[0] = 30000, right.axes[1] = 4000;
    CHECK(feed_all(c, {rest0, right, rest0}, PadTargetKind::StickX) == "a0", "padmap: stick right -> a0");
    PadRaw upward = rest0;
    upward.axes[1] = -32768;
    CHECK(feed_all(c, {rest0, upward, rest0}, PadTargetKind::StickY) == "a1", "padmap: stick up (negative) -> a1");
    PadRaw inv = rest0;
    inv.axes[3] = 32767;
    CHECK(feed_all(c, {rest0, inv, rest0}, PadTargetKind::StickY) == "a3~", "padmap: stick up read as positive -> a3~");
    PadRaw press = rest0;
    press.buttons[1] = true;
    CHECK(feed_all(c, {rest0, press, rest0}, PadTargetKind::StickX).empty(), "padmap: a button does not count for a stick");
    /* a D-pad on axes: half an axis */
    CHECK(feed_all(c, {rest0, neg, rest0}, PadTargetKind::Button) == "-a2", "padmap: D-pad on an axis -> -a2");
}

/* The Controllers panel: per-controller name and player in the ini, --pad-slot on the command line,
 * one controller's mapping line set up or reset without touching the other's (made-up GUIDs). */
static void test_controllers() {
    const std::string ga = "03000000aaaa00000100000000000000", gb = "05000000bbbb00000200000000000000";
    CHECK(controller_key("03000000AAAA00000100000000000000", 1) == ga && controller_key(ga, 2) == ga + "#2",
          "controllers: key is the GUID in lower case, #K from the second identical one");
    CHECK(pad_guid_short(ga) == "aaaa:0001" && pad_guid_short("03000000101800000100000000000000") == "1810:0001",
          "controllers: short GUID is vendor:product (%s)", pad_guid_short(ga).c_str());
    CHECK(pad_guid_short("78696e70757401000000000000000000") == "78696e70" && pad_guid_short("abc") == "abc",
          "controllers: other GUIDs: their first 8 digits (%s)", pad_guid_short("78696e70757401000000000000000000").c_str());

    ProfileFile f;
    f.profiles.push_back(Profile());
    f.selected = "default";
    controller_pref(f, ga).name = "Generic USB pad";
    set_controller_player(f, ga, 2);
    set_controller_player(f, controller_key(ga, 2), 1);
    set_controller_player(f, gb, 0);    /* Auto and no name: not written */
    CHECK(f.controllers.size() == 3, "controllers: three entries (%zu)", f.controllers.size());
    set_controller_player(f, gb, 2);    /* player 2 moves to B; A goes back to Auto */
    CHECK(find_controller(f, ga) && find_controller(f, ga)->player == 0 && find_controller(f, gb)->player == 2,
          "controllers: a player belongs to one controller");
    set_controller_player(f, ga, 3);
    std::string text = write_ini(f);
    CHECK(text.find("[controller " + ga + "]\nname = Generic USB pad\nplayer = 3\n") != std::string::npos &&
              text.find("[controller " + ga + "#2]\nplayer = 1\n") != std::string::npos,
          "controllers: [controller GUID] sections");
    ProfileFile g;
    std::vector<std::string> warn;
    CHECK(parse_ini(text + "[controller " + gb + "]\nplayer = 7\nfuture = x\n", g, &warn) && warn.size() == 2,
          "controllers: a bad player and an unknown key are reported (%zu)", warn.size());
    const ControllerPref *a = find_controller(g, ga), *a2 = find_controller(g, ga + "#2"), *b = find_controller(g, gb);
    CHECK(a && a->name == "Generic USB pad" && a->player == 3 && a2 && a2->player == 1 && b && b->player == 2,
          "controllers: round trip of names and players (a repeated section adds to the first)");
    CHECK(write_ini(g).find("future = x") != std::string::npos, "controllers: unknown keys kept");
    ProfileFile h;
    parse_ini(text, h, nullptr);
    CHECK(write_ini(h) == text, "controllers: identical text on the second write");

    std::vector<std::string> args = pad_slot_args(f.controllers);
    CHECK(args == std::vector<std::string>({"--pad-slot", "1=" + ga + "#2", "--pad-slot", "2=" + gb, "--pad-slot", "3=" + ga}),
          "controllers: --pad-slot per player in player order (%zu)", args.size());
    Profile p;
    p.xbrun = "/w/xbrun";
    p.disc = "/w/disc";
    p.extra_args = "--stats";
    std::vector<std::string> argv = build_argv(p, f.controllers);
    CHECK(argv.size() == 10 && argv[3] == "--pad-slot" && argv.back() == "--stats", "controllers: the slots come before the extra args");
    CHECK(build_argv(p, {}) == build_argv(p), "controllers: none chosen: the plain command line");

    /* Setting up controller A's buttons replaces only A's line; resetting B removes only B's. */
    std::string db = "# mine\n" + ga + ",Pad A,a:b0,platform:Linux,\n" + gb + ",Pad B,a:b1,platform:Linux,\n";
    std::string set = pad_db_merge(db, ga, "Linux", ga + ",Pad A,a:b2,platform:Linux,");
    CHECK(set == "# mine\n" + ga + ",Pad A,a:b2,platform:Linux,\n" + gb + ",Pad B,a:b1,platform:Linux,\n",
          "controllers: setting up one controller keeps the other's line");
    CHECK(pad_db_merge(set, gb, "Linux", "") == "# mine\n" + ga + ",Pad A,a:b2,platform:Linux,\n",
          "controllers: resetting one controller removes only its line");
}

int main() {
    std::string scratch = path_join(exe_dir(), "test-scratch");
    make_dir(scratch);
    test_ini_round_trip();
    test_ini_parsing();
    test_argv();
    test_quoting();
    test_xbe(scratch);
    test_child(scratch);
    test_recommended(scratch);
    test_find_game_exe(scratch);
    test_argv_v2();
    test_ini_v2();
    test_migration();
    test_presets(scratch);
    test_png();
    test_iso(scratch);
    test_titleimage(scratch);
    test_i18n();
    test_keymap(scratch);
    test_backup(scratch);
    test_report(scratch);
    test_sharpen_key();
    test_bloom_key();
    test_rain_key();
    test_ssao_key();
    test_shadow_range_key();
    test_uiscale();
    test_padmap();
    test_controllers();
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("ALL OK\n");
    return 0;
}
