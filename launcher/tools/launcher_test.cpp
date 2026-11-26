/* Game-independent test of the launcher core: profile file round trip (paths with spaces and
 * Windows drive letters, unknown keys and sections kept), the argv builder for each option with
 * defaults left out, quoting, copy validation against synthetic XBE headers written here, and a
 * child process with its output in a log file, and the title-image decoder on synthetic XPR0
 * textures and XBE sections. No game data. Run from any folder; scratch files
 * go to launcher/build/test-scratch (next to this binary). */
#include "../src/async.h"
#include "../src/backup.h"
#include "../src/command.h"
#include "../src/gpu.h"
#include "../src/i18n.h"
#include "../src/iso.h"
#include "../src/keymap.h"
#include "../src/mods.h"
#include "../src/padmap.h"
#include "../src/platform.h"
#include "../src/png.h"
#include "../src/profile.h"
#include "../src/report.h"
#include "../src/titleimage.h"
#include "../src/uirules.h"
#include "../src/uiscale.h"
#include "../src/vram.h"
#include "../src/xbe_check.h"
#include <cstdio>
#include <cstdlib>
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
    a.sound_pack = "C:\\Warriors\\sound pack";
    a.button_icons = "ps-white";
    a.glyph_packs = "C:\\Warriors\\glyph packs";
    a.icon_pack = "C:\\Warriors\\icon pack";
    a.resolution = "1920x1080";
    a.msaa = 4;
    a.aniso = 16;
    a.widescreen = true;
    a.language = "es";
    a.frame_lock = false;
    a.refresh_match = false;
    a.gamma_ramp = false;
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
    CHECK(x.sound_pack == a.sound_pack, "round trip: sound pack (%s)", x.sound_pack.c_str());
    CHECK(x.resolution == "1920x1080" && x.msaa == 4 && x.aniso == 16 && x.widescreen && !x.fullscreen,
          "round trip: graphics");
    CHECK(x.button_icons == "ps-white" && x.glyph_packs == a.glyph_packs && g.profiles[1].button_icons == "xbox",
          "round trip: button icons and glyph packs folder (%s, %s)", x.button_icons.c_str(), x.glyph_packs.c_str());
    CHECK(x.icon_pack == a.icon_pack, "round trip: the v0.1.8 icon pack folder (%s)", x.icon_pack.c_str());
    CHECK(x.language == "es" && !x.frame_lock && x.rate_fixes && x.extra_args == "--log-limit 100",
          "round trip: language, gameplay, extra args");
    CHECK(!x.refresh_match && g.profiles[1].refresh_match && text.find("refresh_match = false") != std::string::npos,
          "round trip: refresh_match saved off, on by default");
    CHECK(!x.gamma_ramp && g.profiles[1].gamma_ramp && text.find("gamma_ramp = false") != std::string::npos,
          "round trip: gamma_ramp saved off, on by default");
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
        {"sound pack", [](Profile &q) { q.sound_pack = "/s p"; }, "--sound-pack|/s p"},
        {"resolution", [](Profile &q) { q.resolution = "1920x1080"; }, "--resolution|1920x1080"},
        {"render scale", [](Profile &q) { q.render_scale = 3; }, "--render-scale|3"},
        {"resolution wins", [](Profile &q) { q.resolution = "800x600"; q.render_scale = 2; }, "--resolution|800x600"},
        {"msaa", [](Profile &q) { q.msaa = 4; }, "--msaa|4"},
        {"aniso", [](Profile &q) { q.aniso = 16; }, "--aniso|16"},
        {"widescreen", [](Profile &q) { q.widescreen = true; }, "--widescreen"},
        {"movie hd off", [](Profile &q) { q.movie_hd = false; }, "--no-movie-hd"},
        {"gamma ramp off", [](Profile &q) { q.gamma_ramp = false; }, "--no-gamma-ramp"},
        {"fullscreen", [](Profile &q) { q.fullscreen = true; }, "--fullscreen"},
        {"refresh match off", [](Profile &q) { q.refresh_match = false; }, "--no-refresh-match"},
        {"refresh match off, fullscreen", [](Profile &q) { q.fullscreen = true; q.refresh_match = false; }, "--fullscreen|--no-refresh-match"},
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
        {"64-bit renderer", [](Profile &q) { q.render64 = true; }, "--render-process|helper"},
        {"stats", [](Profile &q) { q.stats = true; }, "--stats"},
        {"load profile", [](Profile &q) { q.load_profile = true; }, "--load-profile"},
        {"trace", [](Profile &q) { q.trace = true; }, "--trace"},
        {"extra args", [](Profile &q) { q.extra_args = " --log-limit 5  \"--x y\" "; }, "--log-limit|5|--x y"},
        /* Values equal to the runtime defaults are left out, typed or empty. */
        {"default values", [](Profile &q) { q.language = ""; q.present = "auto"; q.audio = ""; q.video = "sdl";
                                            q.renderer = "vulkan"; q.render_scale = 1; q.msaa = 1; q.aniso = 1; }, ""},
        {"language en", [](Profile &q) { q.language = "en"; }, ""},
        /* Button icons: the glyph pack goes before the profile's texture pack (first match wins). */
        /* The user's own icon atlas (icon_pack, a v0.1.8 key) goes first, before any glyph pack. */
        {"icon pack", [](Profile &q) { q.icon_pack = "/i"; q.texture_pack = "/t"; }, "--texture-pack|/i|--texture-pack|/t"},
        {"icon pack before glyph pack", [](Profile &q) { q.icon_pack = "/i"; q.button_icons = "ps-black"; q.glyph_packs = "/g"; },
         "--texture-pack|/i|--texture-pack|/g/ps-black"},
        {"ps black", [](Profile &q) { q.button_icons = "ps-black"; q.glyph_packs = "/g"; }, "--texture-pack|/g/ps-black"},
        {"ps white before texture pack", [](Profile &q) { q.button_icons = "ps-white"; q.glyph_packs = "/g/"; q.texture_pack = "/t"; },
         "--texture-pack|/g/ps-white|--texture-pack|/t"},
        {"ps glyphs windows folder", [](Profile &q) { q.button_icons = "ps-black"; q.glyph_packs = "C:\\W\\glyphs\\"; },
         "--texture-pack|C:\\W\\glyphs\\ps-black"},
        {"xbox icons ignore the folder", [](Profile &q) { q.button_icons = "xbox"; q.glyph_packs = "/g"; q.texture_pack = "/t"; },
         "--texture-pack|/t"},
        /* No glyph pack folder: the runtime paints the PlayStation icons itself (--button-icons ps-*). */
        {"ps icons without a folder", [](Profile &q) { q.button_icons = "ps-black"; }, "--button-icons|ps-black"},
        {"ps white without a folder", [](Profile &q) { q.button_icons = "ps-white"; q.texture_pack = "/t"; },
         "--texture-pack|/t|--button-icons|ps-white"},
        {"ps color without a folder", [](Profile &q) { q.button_icons = "ps-color"; }, "--button-icons|ps-color"},
        {"unknown icons value", [](Profile &q) { q.button_icons = "nes"; q.glyph_packs = "/g"; }, ""},
        /* Keyboard/mouse icons are drawn by the runtime: no glyph pack. */
        {"kbm icons", [](Profile &q) { q.button_icons = "kbm"; q.glyph_packs = "/g"; }, "--button-icons|kbm"},
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
 * glyph and texture packs looked up in the install folder and left out when they are missing. */
static void test_recommended(const std::string &scratch) {
    std::string inst = path_join(scratch, "install"), glyphs = path_join(inst, "glyph-packs");
    make_dir(inst);
    make_dir(glyphs);
    make_dir(path_join(glyphs, "ps-black"));
    make_dir(path_join(inst, "texpack-v3"));
    make_dir(path_join(inst, "neon-masks"));
    Profile p = recommended_profile(inst);
    p.xbrun = path_join(inst, "xbrun");
    p.disc = "/games/disc";
    p.hdd = "/games/hdd";
    std::vector<std::string> a = build_argv(p);
    std::string want = join({p.xbrun, "/games/disc/default.xbe", "/games/disc", "--hdd", "/games/hdd",
                             "--texture-pack", path_join(glyphs, "ps-black"), "--texture-pack", path_join(inst, "texpack-v3"),
                             "--texture-pack", path_join(inst, "neon-masks"),
                             "--resolution", "1920x1080", "--widescreen", "--fullscreen", "--msaa", "2", "--taa", "--aniso", "16",
                             "--stats", "--sim60", "--hud-hd-glass", "--taa-upscale", "ultra", "--dynres", "auto", "--vrs", "auto",
                             "--shadow-scale", "2", "--shadow-soft", "--shadow-range", "--bloom", "--rain-hd", "--rain-hd-puddles", "--wet-light-streaks", "--materials", "--damp-streets", "--ssao", "--ssao-mode", "gtao",
                             "--contact-shadows", "--lamp-lights", "--neon-glow", "--neon-reflections", "--char-lighting", "--hair", "--skin", "--eyes", "--cloth", "--world-lighting", "--normal-maps", "--dry-highlights", "--parallax", "--fire-light", "--level-lights", "--world-shadows", "--light-shafts", "--volfog", "--volfog-quality", "high", "--filmic", "--auto-exposure", "--cutscene-dof", "--sky-hd", "--sharp-faces", "--skip-intro"});
    CHECK(join(a) == want, "recommended: validated command line (%s)", display_command(a, false).c_str());

    Profile q = recommended_profile(path_join(scratch, "no-packs"));
    CHECK(q.texture_pack.empty() && q.button_icons == "xbox" && opts(q).find("--texture-pack") == std::string::npos,
          "recommended: missing packs give no --texture-pack (%s)", opts(q).c_str());
    CHECK(opts(Profile()).empty(), "recommended: a plain Profile still gives the bare command");
}

/* The PlayStation button icons from the glyph pack folder (odd: launcher glyph auto): in auto mode
 * a PlayStation controller connected at launch gets <glyph_packs>/ps-black before the texture pack;
 * the folder may be picked as the one holding ps-black, a ps-* folder itself or a folder holding
 * glyph-packs (nested glyph-packs/glyph-packs). */
static void test_glyph_auto(const std::string &scratch) {
    std::string inst = path_join(scratch, "glyph-auto"), glyphs = path_join(inst, "glyph-packs");
    make_dir(inst);
    make_dir(glyphs);
    for (const char *s : {"ps-black", "ps-color", "ps-white"}) make_dir(path_join(glyphs, s));
    std::string nested = path_join(inst, "nested"), inner = path_join(nested, "glyph-packs");
    make_dir(nested);
    make_dir(inner);
    make_dir(path_join(inner, "ps-black"));
    Profile p;
    p.xbrun = path_join(inst, "xbrun");
    p.disc = "/games/disc";
    p.texture_pack = "/games/texpack";
    p.glyph_packs = glyphs;
    p.button_icons = "auto";
    std::string ps_black = path_join(glyphs, "ps-black");

    std::vector<std::string> a = build_argv(p);
    add_glyph_pack(a, auto_glyph_pack(p, true));
    std::string want = join({p.xbrun, "/games/disc/default.xbe", "/games/disc", "--texture-pack", ps_black, "--texture-pack",
                             "/games/texpack", "--button-icons", "auto"});
    CHECK(join(a) == want, "glyphs: auto + pack + PlayStation pad passes ps-black before the texture pack (%s)", join(a).c_str());

    std::vector<std::string> b = build_argv(p);
    add_glyph_pack(b, auto_glyph_pack(p, false));
    CHECK(join(b) == join(build_argv(p)) && join(b).find("ps-black") == std::string::npos,
          "glyphs: auto + pack without a PlayStation pad is unchanged (%s)", join(b).c_str());

    Profile x = p;
    x.button_icons = "xbox";
    CHECK(auto_glyph_pack(x, true).empty(), "glyphs: Xbox icons chosen stay Xbox with a PlayStation pad");
    Profile none = p;
    none.glyph_packs.clear();
    CHECK(auto_glyph_pack(none, true).empty(), "glyphs: auto without a pack folder adds nothing");
    Profile missing = p;
    missing.glyph_packs = path_join(inst, "nowhere");
    CHECK(auto_glyph_pack(missing, true).empty(), "glyphs: auto with a folder holding no pack adds nothing");

    Profile ps = p;
    ps.button_icons = "ps-black";
    std::string ps_want = join({p.xbrun, "/games/disc/default.xbe", "/games/disc", "--texture-pack", ps_black, "--texture-pack",
                                "/games/texpack"});
    std::vector<std::string> c = build_argv(ps);
    add_glyph_pack(c, auto_glyph_pack(ps, true));
    CHECK(join(build_argv(ps)) == ps_want && join(c) == ps_want, "glyphs: ps-black + pack folder unchanged, no second pack (%s)",
          join(c).c_str());

    std::vector<std::string> d = {"x", "--hdd", "h"};
    add_glyph_pack(d, "g/ps-black");
    CHECK(join(d) == "x|--hdd|h|--texture-pack|g/ps-black", "glyphs: without other packs the glyph pack is appended (%s)",
          join(d).c_str());

    Profile pick = p;
    CHECK(find_glyph_packs(pick) == glyphs, "glyphs: the folder holding ps-black resolves to itself");
    pick.glyph_packs = glyphs + "/";
    CHECK(find_glyph_packs(pick) == glyphs, "glyphs: a trailing separator resolves (%s)", find_glyph_packs(pick).c_str());
    pick.glyph_packs = path_join(glyphs, "ps-white");
    CHECK(find_glyph_packs(pick) == glyphs, "glyphs: a ps-* folder picked resolves to its parent (%s)", find_glyph_packs(pick).c_str());
    pick.glyph_packs = nested;
    CHECK(find_glyph_packs(pick) == inner, "glyphs: a folder holding glyph-packs resolves to it (%s)", find_glyph_packs(pick).c_str());
    pick.glyph_packs = path_join(inst, "nowhere");
    CHECK(find_glyph_packs(pick).empty(), "glyphs: a folder without packs resolves to nothing");
    pick.glyph_packs = "glyph-packs";   /* relative: next to the game program */
    CHECK(find_glyph_packs(pick) == "glyph-packs", "glyphs: a relative folder resolves from the game program's folder (%s)",
          find_glyph_packs(pick).c_str());
    pick.glyph_packs = "nested";
    CHECK(find_glyph_packs(pick) == path_join("nested", "glyph-packs"), "glyphs: a relative nested folder keeps its relative form (%s)",
          find_glyph_packs(pick).c_str());

    /* No pack folder (or one without the chosen style): the runtime's own PlayStation icons; a pack
     * folder that holds the style keeps winning. */
    Profile auto_none = p;
    auto_none.glyph_packs.clear();
    std::vector<std::string> f = build_argv(auto_none);
    add_auto_ps_icons(f, auto_none, true);
    CHECK(join(f) == join(build_argv(auto_none)) + "|--pad-icons|ps-black",
          "glyphs: auto + PlayStation pad without a pack folder asks the runtime for ps-black icons (%s)", join(f).c_str());
    std::vector<std::string> g = build_argv(auto_none);
    add_auto_ps_icons(g, auto_none, false);
    CHECK(join(g) == join(build_argv(auto_none)), "glyphs: auto without a PlayStation pad stays as it was (%s)", join(g).c_str());
    std::vector<std::string> h = build_argv(p);
    add_auto_ps_icons(h, p, true);
    CHECK(join(h) == join(a), "glyphs: auto + PlayStation pad + pack folder still uses the pack (%s)", join(h).c_str());
    Profile xb = auto_none;
    xb.button_icons = "xbox";
    std::vector<std::string> k = build_argv(xb);
    add_auto_ps_icons(k, xb, true);
    CHECK(join(k) == join(build_argv(xb)), "glyphs: Xbox icons chosen get no --pad-icons with a PlayStation pad");

    Profile gone = p;
    gone.button_icons = "ps-white";
    gone.glyph_packs = path_join(inst, "nowhere");
    Profile r = resolve_button_icons(gone);
    CHECK(r.glyph_packs.empty() && opts(r) == "--texture-pack|/games/texpack|--button-icons|ps-white",
          "glyphs: a missing pack folder falls back to the runtime's icons (%s)", opts(r).c_str());
    Profile only_black = p;
    only_black.button_icons = "ps-white";
    only_black.glyph_packs = nested;   /* holds glyph-packs/ps-black only */
    r = resolve_button_icons(only_black);
    CHECK(opts(r) == "--texture-pack|/games/texpack|--button-icons|ps-white",
          "glyphs: a pack folder without the chosen style falls back to the runtime's icons (%s)", opts(r).c_str());
    Profile has = p;
    has.button_icons = "ps-black";
    has.glyph_packs = path_join(glyphs, "ps-black");   /* a ps-* folder picked */
    r = resolve_button_icons(has);
    CHECK(r.glyph_packs == glyphs && opts(r) == "--texture-pack|" + ps_black + "|--texture-pack|/games/texpack",
          "glyphs: an existing pack folder wins over the runtime's icons (%s)", opts(r).c_str());

    Profile nest = p;
    nest.glyph_packs = nested;
    std::vector<std::string> e = build_argv(nest);
    add_glyph_pack(e, auto_glyph_pack(nest, true));
    CHECK(join(e).find("--texture-pack|" + path_join(inner, "ps-black")) != std::string::npos,
          "glyphs: auto + PlayStation pad uses the resolved nested folder (%s)", join(e).c_str());
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

/* The AppImage (tools/appimage): WARRIORS_DATA_DIR moves the game's folder (relative profile paths,
 * the folder it runs in) to the writable data folder, and the first run finds the xbrun next to the
 * launcher inside the image by its bare name. Unset, everything stays as before. */
static void test_appimage_data_dir(const std::string &scratch) {
    std::string root = path_join(scratch, "appimage"), data = path_join(root, "data"), image = path_join(root, "bin");
    make_dir(root);
    make_dir(data);
    make_dir(image);
    std::remove(path_join(root, "xbrun").c_str());
    std::remove(path_join(data, "xbrun").c_str());
    write_file(path_join(image, "xbrun"), "#!/bin/sh\n");
    Profile p;
    p.xbrun = "/opt/port/xbrun";

    unsetenv("WARRIORS_DATA_DIR");
    CHECK(data_dir_env().empty(), "appimage: no data folder outside the AppImage");
    CHECK(game_dir(p) == "/opt/port", "appimage: unset, the game's folder is the executable's (%s)", game_dir(p).c_str());
    CHECK(mods_path(p) == "/opt/port/mods", "appimage: unset, mods next to the game program (%s)", mods_path(p).c_str());
    CHECK(first_run_game_exe(data, image, false).empty(), "appimage: unset, the launcher's folder is not searched");
    setenv("WARRIORS_DATA_DIR", "", 1);
    CHECK(data_dir_env().empty() && game_dir(p) == "/opt/port", "appimage: empty counts as unset");

    setenv("WARRIORS_DATA_DIR", data.c_str(), 1);
    CHECK(data_dir_env() == data, "appimage: the data folder (%s)", data_dir_env().c_str());
    CHECK(game_dir(p) == data, "appimage: the game runs in the data folder (%s)", game_dir(p).c_str());
    CHECK(xbrun_path(p) == "/opt/port/xbrun", "appimage: the executable path itself is unchanged");
    CHECK(mods_path(p) == path_join(data, "mods"), "appimage: mods in the data folder (%s)", mods_path(p).c_str());
    CHECK(keymap_path(p) == path_join(data, "warriors-input.cfg"), "appimage: key map in the data folder (%s)",
          keymap_path(p).c_str());
    std::string first = first_run_game_exe(data, image, false);
    CHECK(first == "xbrun", "appimage: first run, the bare name of the xbrun next to the launcher (%s)", first.c_str());
    Profile q;
    q.xbrun = first;
    CHECK(xbrun_path(q) == path_join(exe_dir(), "xbrun"), "appimage: the bare name follows the launcher's folder (%s)",
          xbrun_path(q).c_str());
    write_file(path_join(data, "xbrun"), "#!/bin/sh\n");
    CHECK(first_run_game_exe(data, image, false) == path_join(data, "xbrun"),
          "appimage: an xbrun next to the profiles file still comes first");
    std::remove(path_join(data, "xbrun").c_str());
    unsetenv("WARRIORS_DATA_DIR");
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
        {"fsr quality", [](Profile &q) { q.fsr = "quality"; }, "--fsr|quality"},
        {"fsr off", [](Profile &q) { q.fsr = "off"; }, ""},
        {"taa upscale quality", [](Profile &q) { q.taa_upscale = "quality"; }, "--taa-upscale|quality"},
        {"taa upscale off", [](Profile &q) { q.taa_upscale = "off"; }, ""},
        {"shadow scale", [](Profile &q) { q.shadow_scale = 4; }, "--shadow-scale|4"},
        {"effects and particles more", [](Profile &q) { q.fx_scale = 2; }, "--fx-scale|2"},
        {"effects and particles maximum", [](Profile &q) { q.fx_scale = 4; }, "--fx-scale|4"},
        {"effects and particles normal", [](Profile &q) { q.fx_scale = 1; }, ""},
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
        {"ssao classic left out", [](Profile &q) { q.ssao = true; q.ssao_mode = "classic"; q.gtao_quality = "low"; }, "--ssao"},
        {"ssao gtao", [](Profile &q) { q.ssao = true; q.ssao_mode = "gtao"; }, "--ssao|--ssao-mode|gtao"},
        {"ssao gtao quality", [](Profile &q) { q.ssao = true; q.ssao_strength = 1.5f; q.ssao_mode = "gtao"; q.gtao_quality = "medium"; },
         "--ssao|--ssao-strength|1.5|--ssao-mode|gtao|--gtao-quality|medium"},
        {"ssao mode needs ssao", [](Profile &q) { q.ssao_mode = "gtao"; q.gtao_quality = "low"; }, ""},
        {"ssgi with gtao", [](Profile &q) { q.ssao = true; q.ssao_mode = "gtao"; q.ssgi = true; q.ssgi_quality = "low"; },
         "--ssao|--ssao-mode|gtao|--ssgi|low"},
        {"ssgi strength", [](Profile &q) { q.ssao = true; q.ssao_mode = "gtao"; q.ssgi = true; q.ssgi_quality = "high"; q.ssgi_strength = 2; },
         "--ssao|--ssao-mode|gtao|--ssgi|high|--ssgi-strength|2"},
        {"ssgi needs gtao", [](Profile &q) { q.ssao = true; q.ssgi = true; q.ssgi_strength = 2; }, "--ssao"},
        {"ssgi needs ssao", [](Profile &q) { q.ssao_mode = "gtao"; q.ssgi = true; }, ""},
        {"ssgi off passes nothing", [](Profile &q) { q.ssao = true; q.ssao_mode = "gtao"; q.ssgi_quality = "high"; q.ssgi_strength = 2; },
         "--ssao|--ssao-mode|gtao"},
        {"ssgi unknown quality left out", [](Profile &q) { q.ssao = true; q.ssao_mode = "gtao"; q.ssgi = true; q.ssgi_quality = "ultra"; },
         "--ssao|--ssao-mode|gtao"},
        {"filmic", [](Profile &q) { q.filmic = true; }, "--filmic"},
        {"auto exposure", [](Profile &q) { q.auto_exposure = true; }, "--auto-exposure"},
        {"auto exposure after filmic", [](Profile &q) { q.filmic = q.auto_exposure = true; }, "--filmic|--auto-exposure"},
        {"cutscene dof", [](Profile &q) { q.cutscene_dof = true; }, "--cutscene-dof"},
        {"taa", [](Profile &q) { q.taa = true; }, "--taa"},
        {"taa after msaa", [](Profile &q) { q.msaa = 2; q.taa = true; }, "--msaa|2|--taa"},
        {"frame_gen", [](Profile &q) { q.frame_gen = true; }, "--frame-gen"},
        {"frame_gen after taa", [](Profile &q) { q.taa = true; q.frame_gen = true; }, "--taa|--frame-gen"},
        {"cutscene dof after auto exposure", [](Profile &q) { q.filmic = q.auto_exposure = q.cutscene_dof = true; }, "--filmic|--auto-exposure|--cutscene-dof"},
        {"lamp lights", [](Profile &q) { q.lamp_lights = true; }, "--lamp-lights"},
        {"lamp lights strength", [](Profile &q) { q.lamp_lights = true; q.lamp_lights_strength = 2; }, "--lamp-lights|--lamp-lights-strength|2"},
        {"lamp lights strength needs lamp lights", [](Profile &q) { q.lamp_lights_strength = 2; }, ""},
        {"neon glow", [](Profile &q) { q.neon_glow = true; }, "--neon-glow"},
        {"neon glow strength", [](Profile &q) { q.neon_glow = true; q.neon_glow_strength = 2; }, "--neon-glow|--neon-glow-strength|2"},
        {"neon glow strength needs neon glow", [](Profile &q) { q.neon_glow_strength = 2; }, ""},
        {"lamp lights after ssao", [](Profile &q) { q.ssao = true; q.lamp_lights = true; },
         "--ssao|--lamp-lights"},
        {"soft particles", [](Profile &q) { q.soft_particles = true; }, "--soft-particles"},
        {"soft particles after lamp lights", [](Profile &q) { q.ssao = true; q.lamp_lights = true; q.soft_particles = true; },
         "--ssao|--lamp-lights|--soft-particles"},
        {"sim60", [](Profile &q) { q.sim60 = true; }, "--sim60"},
        {"hud hd", [](Profile &q) { q.hud_style = "hd"; }, "--hud-hd"},
        {"hud hd glass", [](Profile &q) { q.hud_style = "hd-glass"; }, "--hud-hd-glass"},
        {"hud glass", [](Profile &q) { q.hud_style = "glass"; }, "--hud-glass"},
        {"hud customize", [](Profile &q) { q.hud_style = "hd-glass"; q.hud_track_opacity = 0.3f; q.hud_fill_opacity = 0.75f;
                                           q.hud_frost = 6; q.hud_height = 1.5f; },
         "--hud-hd-glass|--hud-hd-track-opacity|0.3|--hud-hd-fill-opacity|0.75|--hud-hd-frost|6|--hud-hd-height|1.5"},
        {"hud customize needs an hd style", [](Profile &q) { q.hud_style = "original"; q.hud_frost = 6; }, ""},
        {"hud customize ignored for glass", [](Profile &q) { q.hud_style = "glass"; q.hud_height = 2; }, "--hud-glass"},
        {"draw distance original", [](Profile &q) { q.draw_distance = 1; }, ""},
        {"draw distance far", [](Profile &q) { q.draw_distance = 2; }, "--draw-distance|1.5|--prop-detail|2"},
        {"draw distance farther", [](Profile &q) { q.draw_distance = 3; }, "--draw-distance|2|--prop-detail|4"},
        {"draw distance max", [](Profile &q) { q.draw_distance = 4; }, "--draw-distance|3|--prop-detail|8"},
        {"draw distance out of range", [](Profile &q) { q.draw_distance = 9; }, ""},
        {"spray time", [](Profile &q) { q.spray_time = 150; }, "--spray-time|150"},
        {"spray time default", [](Profile &q) { q.spray_time = 110; }, ""},
        {"text fixes off", [](Profile &q) { q.text_fixes = false; }, "--no-text-fixes"},
        {"script fixes off", [](Profile &q) { q.script_fixes = false; }, "--no-script-fixes"},
        {"smart allies", [](Profile &q) { q.smart_allies = true; }, "--smart-allies"},
        {"enemy tactics", [](Profile &q) { q.enemy_tactics = true; }, "--enemy-tactics"},
        {"volumes", [](Profile &q) { q.dialogue_volume = 6; q.music_volume = -10; q.sfx_volume = -3; },
         "--dialogue-volume|6|--music-volume|-10|--sfx-volume|-3"},
        {"volumes at 0 dB left out", [](Profile &q) { q.dialogue_volume = 0; q.music_volume = 0; q.sfx_volume = 0; }, ""},
        {"headphones binaural", [](Profile &q) { q.audio_hrtf = true; }, "--audio-hrtf"},
        {"air absorption and wall occlusion off", [](Profile &q) { q.audio_air = false; q.audio_occlusion = false; },
         "--no-audio-air|--no-audio-occlusion"},
        {"location-based reverb off", [](Profile &q) { q.audio_reverb_geo = false; }, "--no-audio-reverb-geo"},
        {"sound around corners off", [](Profile &q) { q.audio_pathing = false; }, "--no-audio-pathing"},
        {"volumes clamped to -40..12", [](Profile &q) { q.dialogue_volume = 30; q.sfx_volume = -99; },
         "--dialogue-volume|12|--sfx-volume|-40"},
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
    a.script_fixes = false;
    a.dialogue_volume = 8;
    a.music_volume = -12;
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
    CHECK(x.spray_time == 130 && !x.text_fixes && !x.script_fixes && x.preset == "custom", "ini v2: gameplay fields, preset %s", x.preset.c_str());
    CHECK(x.dialogue_volume == 8 && x.music_volume == -12 && x.sfx_volume == 0, "ini v2: volumes (%d %d %d)", x.dialogue_volume,
          x.music_volume, x.sfx_volume);
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
    CHECK(old.profiles.size() == 1 && old.profiles[0].spray_time == 110 && old.profiles[0].text_fixes && old.profiles[0].script_fixes &&
          old.profiles[0].hud_style == "original" && old.profiles[0].supersample == 1, "ini v2: older profile gets the runtime defaults");
    CHECK(write_ini(old).find("setup_done") == std::string::npos, "ini v2: unknown setup state is not written");
}

/* An older profile with the covered options typed in extra_args: loading moves them into the
 * new fields, removes them from extra_args, and the command line stays the same. The fixture is
 * the user's command validated on Windows. */
static void test_migration() {
    std::string ini = "[launcher]\nselected = mine\n[profile mine]\nxbrun = /inst/TheWarriors.exe\ndisc = /games/disc\n"
                      "texture_pack = /inst/texpack-v3\nbutton_icons = ps-black\nglyph_packs = /inst/glyph-packs\n"
                      "resolution = 1920x1080\nmsaa = 4\naniso = 4\nwidescreen = true\nfullscreen = true\nlanguage = es\n"
                      "stats = true\nextra_args = --sim60 --hud-hd-glass --supersample 2 --shadow-scale 2 --shadow-soft --shadow-range --bloom --rain-hd --ssao --soft-particles\n";
    ProfileFile f;
    std::vector<std::string> warn;
    CHECK(parse_ini(ini, f, &warn) && f.profiles.size() == 1, "migration: parses");
    if (f.profiles.size() != 1) return;
    const Profile &p = f.profiles[0];
    std::string want = "--texture-pack|/inst/glyph-packs/ps-black|--texture-pack|/inst/texpack-v3|--resolution|1920x1080|"
                       "--widescreen|--fullscreen|--msaa|4|--aniso|4|--language|es|--stats|--sim60|--hud-hd-glass|"
                       "--supersample|2|--shadow-scale|2|--shadow-soft|--shadow-range|--bloom|--rain-hd|--ssao|--soft-particles|"
                       "--skip-intro"; /* skip_intro is newer than the file: on (the only addition) */
    CHECK(opts(p) == want, "migration: the user's command is unchanged (%s)", opts(p).c_str());
    CHECK(p.extra_args.empty(), "migration: extra_args emptied ('%s')", p.extra_args.c_str());
    CHECK(p.sim60 && p.hud_style == "hd-glass" && p.supersample == 2 && p.shadow_scale == 2 && p.shadow_soft,
          "migration: fields set");
    CHECK(p.preset == "custom", "migration: supersample 2 is no longer a preset, so custom (%s)", p.preset.c_str());
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
        {"gameplay options", "--spray-time 150 --no-text-fixes --no-script-fixes --sim60 --no-sim60", ""},
        {"volumes move, out of range or decimal ones stay typed", "--dialogue-volume 6 --music-volume -50 --sfx-volume 2.5",
         "--music-volume -50 --sfx-volume 2.5"},
        {"effects scale moves, a decimal one stays typed", "--fx-scale 4 --fx-scale 2.5 --fx-scale 5", "--fx-scale 2.5 --fx-scale 5"},
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
    q.extra_args = "--spray-time 150 --no-text-fixes --script-fixes --no-script-fixes --sim60 --no-sim60 --frames-in-flight 3";
    migrate_extra_args(q);
    CHECK(q.spray_time == 150 && !q.text_fixes && !q.script_fixes && !q.sim60 && q.frames_in_flight == 3, "migration: last value wins");
    q = Profile();
    q.extra_args = "--dialogue-volume 4 --music-volume -6 --sfx-volume -2";
    migrate_extra_args(q);
    CHECK(q.dialogue_volume == 4 && q.music_volume == -6 && q.sfx_volume == -2 && q.extra_args.empty(),
          "migration: volumes into their fields (%d %d %d)", q.dialogue_volume, q.music_volume, q.sfx_volume);
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
                     "--aniso|4|--sim60|--hud-hd-glass|--frames-in-flight|2|--skip-intro",
          "preset: laptop (%s)", opts(l).c_str());
    CHECK(detect_preset(l) == Preset::Laptop, "preset: laptop detected");
    Profile g;
    apply_preset(g, Preset::Gaming, inst);
    CHECK(opts(g) == "--texture-pack|" + path_join(inst, "texpack-v3") + "|--texture-pack|" + path_join(inst, "neon-masks") + "|--resolution|1920x1080|--widescreen|--fullscreen|"
                     "--msaa|2|--taa|--aniso|16|--sim60|--hud-hd-glass|--taa-upscale|ultra|--dynres|auto|--vrs|auto|--shadow-scale|2|--shadow-soft|--shadow-range|--bloom|--rain-hd|--rain-hd-puddles|--wet-light-streaks|--materials|--damp-streets|--ssao|--ssao-mode|gtao|"
                     "--contact-shadows|--lamp-lights|--neon-glow|--neon-reflections|--char-lighting|--hair|--skin|--eyes|--cloth|--world-lighting|--normal-maps|--dry-highlights|--parallax|--fire-light|--level-lights|--world-shadows|--light-shafts|--volfog|--volfog-quality|high|--filmic|--auto-exposure|--cutscene-dof|--sky-hd|--sharp-faces|--skip-intro",
          "preset: gaming (%s)", opts(g).c_str());
    /* Ultra: Quality at full resolution with the lamps' character shadows. */
    Profile u;
    apply_preset(u, Preset::Ultra, inst);
    CHECK(opts(u) == "--texture-pack|" + path_join(inst, "texpack-v3") + "|--texture-pack|" + path_join(inst, "neon-masks") + "|--resolution|1920x1080|--widescreen|--fullscreen|"
                     "--msaa|2|--taa|--aniso|16|--sim60|--hud-hd-glass|--vrs|auto|--shadow-scale|2|--shadow-soft|--shadow-range|--bloom|--rain-hd|--rain-hd-puddles|--wet-light-streaks|--materials|--damp-streets|--ssao|--ssao-mode|gtao|"
                     "--contact-shadows|--lamp-lights|--neon-glow|--neon-reflections|--char-lighting|--hair|--skin|--eyes|--cloth|--world-lighting|--normal-maps|--dry-highlights|--parallax|--fire-light|--level-lights|--lamp-shadows|--world-shadows|--light-shafts|--volfog|--volfog-quality|high|--filmic|--auto-exposure|--cutscene-dof|--sky-hd|--sharp-faces|--skip-intro",
          "preset: ultra (%s)", opts(u).c_str());
    CHECK(detect_preset(u) == Preset::Ultra && std::string(preset_key(Preset::Ultra)) == "ultra" && preset_from_key("ultra") == Preset::Ultra,
          "preset: ultra detected, key ultra");
    Profile gu = g;
    gu.taa_upscale = "off";
    CHECK(detect_preset(gu) == Preset::Custom, "preset: Quality at full resolution without the lamp shadows is custom");
    CHECK(std::string(tr(S::preset_ultra, Lang::En)) == "Ultra" && std::string(tr(S::preset_ultra_desc, Lang::Es)) != "",
          "preset: ultra title and description");
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
    nb.lamp_lights = false;
    CHECK(detect_preset(nb) == Preset::Custom, "preset: Quality without lamp lights is custom");
    nb = g;
    nb.lamp_lights_strength = 2;
    CHECK(detect_preset(nb) == Preset::Custom, "preset: a changed lamp lights strength is custom");
    apply_preset(nb, Preset::Gaming, inst);
    CHECK(nb.lamp_lights && nb.lamp_lights_strength < 0 && detect_preset(nb) == Preset::Gaming, "preset: Quality turns lamp lights on");
    apply_preset(nb, Preset::Laptop, inst);
    CHECK(!nb.lamp_lights && detect_preset(nb) == Preset::Laptop, "preset: Performance turns lamp lights off");
    nb = g;
    nb.char_lighting = false;
    CHECK(detect_preset(nb) == Preset::Custom, "preset: Quality without character lighting is custom");
    apply_preset(nb, Preset::Gaming, inst);
    CHECK(nb.char_lighting && detect_preset(nb) == Preset::Gaming, "preset: Quality turns character lighting on");
    apply_preset(nb, Preset::Balanced, inst);
    CHECK(nb.char_lighting && !nb.lamp_lights && detect_preset(nb) == Preset::Balanced, "preset: Balanced turns character lighting on (no lamp lights)");
    apply_preset(nb, Preset::Laptop, inst);
    CHECK(!nb.char_lighting && detect_preset(nb) == Preset::Laptop, "preset: Performance turns character lighting off");
    nb = g;
    nb.world_lighting = false;
    CHECK(detect_preset(nb) == Preset::Custom, "preset: Quality without world lighting is custom");
    apply_preset(nb, Preset::Gaming, inst);
    CHECK(nb.world_lighting && detect_preset(nb) == Preset::Gaming, "preset: Quality turns world lighting on");
    apply_preset(nb, Preset::Balanced, inst);
    CHECK(!nb.world_lighting && detect_preset(nb) == Preset::Balanced, "preset: Balanced leaves world lighting off");
    apply_preset(nb, Preset::Laptop, inst);
    CHECK(!nb.world_lighting && detect_preset(nb) == Preset::Laptop, "preset: Performance leaves world lighting off");
    nb = g;
    nb.fire_light = false;
    CHECK(detect_preset(nb) == Preset::Custom, "preset: Quality without fire light is custom");
    apply_preset(nb, Preset::Gaming, inst);
    CHECK(nb.fire_light && detect_preset(nb) == Preset::Gaming, "preset: Quality turns fire light on");
    apply_preset(nb, Preset::Balanced, inst);
    CHECK(!nb.fire_light && detect_preset(nb) == Preset::Balanced, "preset: Balanced leaves fire light off");
    apply_preset(nb, Preset::Laptop, inst);
    CHECK(!nb.fire_light && detect_preset(nb) == Preset::Laptop, "preset: Performance leaves fire light off");
    nb = u;
    nb.lamp_shadows = false;
    CHECK(detect_preset(nb) == Preset::Custom, "preset: Ultra without lamp shadows is custom");
    apply_preset(nb, Preset::Ultra, inst);
    CHECK(nb.lamp_shadows && detect_preset(nb) == Preset::Ultra, "preset: Ultra turns lamp shadows on");
    apply_preset(nb, Preset::Gaming, inst);
    CHECK(!nb.lamp_shadows && detect_preset(nb) == Preset::Gaming, "preset: Quality leaves lamp shadows off");
    apply_preset(nb, Preset::Balanced, inst);
    CHECK(!nb.lamp_shadows && detect_preset(nb) == Preset::Balanced, "preset: Balanced leaves lamp shadows off");
    apply_preset(nb, Preset::Laptop, inst);
    CHECK(!nb.lamp_shadows && detect_preset(nb) == Preset::Laptop, "preset: Performance leaves lamp shadows off");
    nb = g;
    nb.world_shadows = false;
    CHECK(detect_preset(nb) == Preset::Custom, "preset: Quality without world shadows is custom");
    apply_preset(nb, Preset::Gaming, inst);
    CHECK(nb.world_shadows && !nb.world_shadows_low && detect_preset(nb) == Preset::Gaming, "preset: Quality turns world shadows on (full map)");
    nb.world_shadows_low = true;
    CHECK(detect_preset(nb) == Preset::Custom, "preset: Quality with the low-cost world shadow map is custom");
    apply_preset(nb, Preset::Balanced, inst);
    CHECK(!nb.world_shadows && detect_preset(nb) == Preset::Balanced, "preset: Balanced leaves world shadows off");
    nb.world_shadows_low = true;
    CHECK(detect_preset(nb) == Preset::Balanced, "preset: the low-cost toggle alone (world shadows off) keeps the preset");
    apply_preset(nb, Preset::Laptop, inst);
    CHECK(!nb.world_shadows && detect_preset(nb) == Preset::Laptop, "preset: Performance leaves world shadows off");
    nb = g;
    nb.light_shafts = false;
    CHECK(detect_preset(nb) == Preset::Custom, "preset: Quality without light shafts is custom");
    apply_preset(nb, Preset::Gaming, inst);
    CHECK(nb.light_shafts && detect_preset(nb) == Preset::Gaming, "preset: Quality turns light shafts on");
    apply_preset(nb, Preset::Balanced, inst);
    CHECK(!nb.light_shafts && detect_preset(nb) == Preset::Balanced, "preset: Balanced leaves light shafts off");
    apply_preset(nb, Preset::Laptop, inst);
    CHECK(!nb.light_shafts && detect_preset(nb) == Preset::Laptop, "preset: Performance leaves light shafts off");
    nb.light_shafts_cheap = true;
    CHECK(detect_preset(nb) == Preset::Laptop, "preset: the cheap shafts choice is not part of the match");
    nb = g;
    nb.volfog = false;
    CHECK(detect_preset(nb) == Preset::Custom, "preset: Quality without volumetric fog is custom");
    apply_preset(nb, Preset::Gaming, inst);
    CHECK(nb.volfog && nb.volfog_quality.empty() && detect_preset(nb) == Preset::Gaming, "preset: Quality turns volumetric fog on (high)");
    apply_preset(nb, Preset::Balanced, inst);
    CHECK(!nb.volfog && detect_preset(nb) == Preset::Balanced, "preset: Balanced leaves volumetric fog off");
    apply_preset(nb, Preset::Laptop, inst);
    CHECK(!nb.volfog && detect_preset(nb) == Preset::Laptop, "preset: Performance leaves volumetric fog off");
    nb.volfog_quality = "low";
    CHECK(detect_preset(nb) == Preset::Laptop, "preset: the volumetric fog quality is not part of the match");
    /* Soft particles are opt-in: no preset turns them on, and turning them on makes any preset Custom. */
    for (Preset pr : {Preset::Laptop, Preset::Balanced, Preset::Gaming}) {
        nb = g;
        nb.soft_particles = true;
        apply_preset(nb, pr, inst);
        CHECK(!nb.soft_particles && detect_preset(nb) == pr, "preset: %s turns soft particles off", preset_key(pr));
        nb.soft_particles = true;
        CHECK(detect_preset(nb) == Preset::Custom, "preset: %s with soft particles on is custom", preset_key(pr));
    }
    CHECK(!recommended_profile(inst).soft_particles, "preset: a new profile has soft particles off");
    nb = g;
    nb.filmic = false;
    CHECK(detect_preset(nb) == Preset::Custom, "preset: Quality without the filmic look is custom");
    apply_preset(nb, Preset::Gaming, inst);
    CHECK(nb.filmic && detect_preset(nb) == Preset::Gaming, "preset: Quality turns the filmic look on");
    apply_preset(nb, Preset::Laptop, inst);
    CHECK(!nb.filmic && detect_preset(nb) == Preset::Laptop, "preset: Performance turns the filmic look off");
    nb = g;
    nb.sky_hd = false;
    CHECK(detect_preset(nb) == Preset::Custom, "preset: Quality without the enhanced sky is custom");
    apply_preset(nb, Preset::Gaming, inst);
    CHECK(nb.sky_hd && detect_preset(nb) == Preset::Gaming, "preset: Quality turns the enhanced sky on");
    apply_preset(nb, Preset::Laptop, inst);
    CHECK(!nb.sky_hd && detect_preset(nb) == Preset::Laptop, "preset: Performance turns the enhanced sky off");
    nb = g;
    nb.neon_glow = false;
    CHECK(detect_preset(nb) == Preset::Custom, "preset: Quality without neon glow is custom");
    apply_preset(nb, Preset::Gaming, inst);
    CHECK(nb.neon_glow && detect_preset(nb) == Preset::Gaming, "preset: Quality turns neon glow on");
    apply_preset(nb, Preset::Balanced, inst);
    CHECK(nb.neon_glow && detect_preset(nb) == Preset::Balanced, "preset: Balanced keeps neon glow on");
    apply_preset(nb, Preset::Laptop, inst);
    CHECK(!nb.neon_glow && detect_preset(nb) == Preset::Laptop, "preset: Performance turns neon glow off");
    nb = g;
    nb.neon_glow_strength = 2;
    CHECK(detect_preset(nb) == Preset::Custom, "preset: a neon glow strength is custom");
    nb = g;
    nb.sharp_faces = false;
    CHECK(detect_preset(nb) == Preset::Custom, "preset: Quality without sharp faces is custom");
    apply_preset(nb, Preset::Gaming, inst);
    CHECK(nb.sharp_faces && detect_preset(nb) == Preset::Gaming, "preset: Quality turns sharp faces on");
    apply_preset(nb, Preset::Laptop, inst);
    CHECK(!nb.sharp_faces && detect_preset(nb) == Preset::Laptop, "preset: Performance turns sharp faces off");
    nb = g;
    nb.shadow_range = false;
    CHECK(detect_preset(nb) == Preset::Custom, "preset: Quality without shadow range is custom");
    apply_preset(nb, Preset::Laptop, inst);
    CHECK(!nb.shadow_range && detect_preset(nb) == Preset::Laptop, "preset: Performance turns shadow range off");
    g.msaa = 4;
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

/* Balanced sits between Performance and Quality: TAA (no MSAA), soft shadows at scale 1, bloom,
 * better rain, neon glow, the filmic look and the enhanced sky, no ambient occlusion or lamp lights, and the mildest farther draw distance and effect pools. */
/* Each preset's argv and saved ini, pinned to tests/preset_outputs.golden: the Display page's
 * layout (gui.cpp) only arranges the rows, so a launcher UI change must leave these byte-identical.
 * The install folder is a fixed path that does not exist (no texture pack is picked). */
static std::string preset_outputs() {
    std::string out;
    const Preset presets[] = {Preset::Laptop, Preset::Balanced, Preset::Gaming, Preset::Ultra, Preset::Integrated};
    for (Preset preset : presets) {
        Profile p;
        p.name = "pinned";
        p.xbrun = "/opt/warriors/xbrun";
        p.disc = "/opt/warriors/disc";
        apply_preset(p, preset, "/opt/warriors");
        ProfileFile f;
        f.profiles.push_back(p);
        f.selected = p.name;
        out += "=== " + std::to_string((int)preset) + "\nARGV " + join(build_argv(p)) + "\nINI\n" + write_ini(f);
    }
    return out;
}

static void test_preset_outputs_pinned() {
    std::string golden, path = path_join(path_join(path_join(exe_dir(), ".."), "tests"), "preset_outputs.golden");
    if (!is_file(path))   /* the x86_64 build in build/linux64/ (make linux64) */
        path = path_join(path_join(path_join(path_join(exe_dir(), ".."), ".."), "tests"), "preset_outputs.golden");
    CHECK(read_file(path, golden, 1 << 20), "preset outputs: golden file readable (%s)", path.c_str());
    std::string actual = preset_outputs();
    CHECK(actual == golden, "preset outputs: argv and ini of every preset match tests/preset_outputs.golden");
    if (actual != golden) { /* the actual output next to the test binary, to diff against the golden file */
        FILE *f = fopen(path_join(exe_dir(), "preset_outputs.actual").c_str(), "wb");
        if (f) { fwrite(actual.data(), 1, actual.size(), f); fclose(f); }
    }
}

static void test_balanced_preset(const std::string &scratch) {
    std::string inst = path_join(scratch, "install");
    Profile b;
    apply_preset(b, Preset::Balanced, inst);
    CHECK(opts(b) == "--texture-pack|" + path_join(inst, "texpack-v3") + "|--texture-pack|" + path_join(inst, "neon-masks") + "|--resolution|1920x1080|--widescreen|--fullscreen|"
                     "--taa|--aniso|4|--sim60|--hud-hd-glass|--taa-upscale|quality|--dynres|auto|--shadow-soft|--bloom|--rain-hd|--rain-hd-puddles|--neon-glow|--char-lighting|--filmic|--auto-exposure|--cutscene-dof|--sky-hd|--sharp-faces|"
                     "--draw-distance|1.5|--prop-detail|2|--fx-scale|2|--skip-intro",
          "balanced: command line (%s)", opts(b).c_str());
    CHECK(detect_preset(b) == Preset::Balanced, "balanced: detected");
    CHECK(std::string(preset_key(Preset::Balanced)) == "balanced" && preset_from_key("balanced") == Preset::Balanced,
          "balanced: key");
    Profile e = b;
    e.draw_distance = 1;
    CHECK(detect_preset(e) == Preset::Custom, "balanced: draw distance as shipped is custom");
    e = b;
    e.fx_scale = 4;
    CHECK(detect_preset(e) == Preset::Custom, "balanced: other effect pools are custom");
    e = b;
    e.ssao = true;
    CHECK(detect_preset(e) == Preset::Custom, "balanced: with ambient occlusion is custom");
    e = b;
    e.filmic = false;
    CHECK(b.filmic && detect_preset(e) == Preset::Custom, "balanced: without the filmic look is custom");
    e = b;
    e.sky_hd = false;
    CHECK(b.sky_hd && detect_preset(e) == Preset::Custom, "balanced: without the enhanced sky is custom");
    e = b;
    e.sharp_faces = false;
    CHECK(b.sharp_faces && detect_preset(e) == Preset::Custom, "balanced: without sharp faces is custom");
    e = b;
    e.lamp_lights = true;
    CHECK(!b.lamp_lights && detect_preset(e) == Preset::Custom, "balanced: with lamp lights is custom");
    /* Performance goes back to the shipped draw distance and pools; Quality keeps them. */
    Profile l = b;
    apply_preset(l, Preset::Laptop, inst);
    CHECK(detect_preset(l) == Preset::Laptop && l.draw_distance == 1 && l.fx_scale == 1 && l.msaa == 0 && !l.bloom &&
              !l.filmic && !l.sky_hd && !l.lamp_lights,
          "balanced -> laptop: shipped draw distance and pools, no msaa, bloom, filmic look, enhanced sky or lamp lights");
    Profile g = b;
    apply_preset(g, Preset::Gaming, inst);
    CHECK(detect_preset(g) == Preset::Gaming && g.msaa == 2 && g.taa && g.ssao && g.lamp_lights && g.filmic && g.sky_hd, "balanced -> gaming");
    Profile r = recommended_profile(inst, Preset::Balanced);
    CHECK(detect_preset(r) == Preset::Balanced && r.preset == "balanced" && r.stats, "balanced: recommended profile");
    CHECK(detect_preset(recommended_profile(inst, Preset::Laptop)) == Preset::Laptop, "recommended: laptop on request");
}

static GpuAdapter gpu(const char *name, uint32_t vendor, double vram_gb, GpuKind kind = GpuKind::Unknown) {
    GpuAdapter a;
    a.name = name;
    a.vendor_id = vendor;
    a.vram = (uint64_t)(vram_gb * 1024 * 1024 * 1024);
    a.kind = kind;
    return a;
}

/* The first-run preset from the graphics card: fake adapter descriptions as DXGI (no kind) and
 * Vulkan (with a kind) report them. */
static void test_gpu_classifier() {
    const uint32_t NV = 0x10de, AMD = 0x1002, INTEL = 0x8086, MS = 0x1414;
    struct Case { const char *what; GpuAdapter a; GpuClass want; };
    const Case cases[] = {
        {"GTX 1050 2 GB", gpu("NVIDIA GeForce GTX 1050", NV, 2), GpuClass::Low},
        {"GTX 1650 4 GB", gpu("NVIDIA GeForce GTX 1650", NV, 4), GpuClass::Mid},
        {"RTX 3060 12 GB", gpu("NVIDIA GeForce RTX 3060", NV, 12), GpuClass::High},
        {"RX 6600 8 GB", gpu("AMD Radeon RX 6600", AMD, 8), GpuClass::High},
        {"RX 560 4 GB just under (3.98 GB)", gpu("Radeon RX 560 Series", AMD, 3.98), GpuClass::Low},
        {"Intel UHD 620 (DXGI 128 MB)", gpu("Intel(R) UHD Graphics 620", INTEL, 0.125), GpuClass::Integrated},
        {"Intel Iris Xe, no dedicated memory", gpu("Intel(R) Iris(R) Xe Graphics", INTEL, 0), GpuClass::Integrated},
        {"Intel Arc A770 16 GB", gpu("Intel(R) Arc(TM) A770 Graphics", INTEL, 16), GpuClass::High},
        {"AMD APU with a 2 GB carve-out", gpu("AMD Radeon(TM) Vega 8 Graphics", AMD, 2), GpuClass::Integrated},
        {"AMD 780M with an 8 GB carve-out", gpu("AMD Radeon 780M Graphics", AMD, 8), GpuClass::Integrated},
        {"Vulkan integrated AMD", gpu("AMD Radeon Graphics (RADV RENOIR)", AMD, 0.5, GpuKind::Integrated), GpuClass::Integrated},
        {"Vulkan discrete AMD", gpu("AMD Radeon RX 7600 (RADV NAVI33)", AMD, 8, GpuKind::Discrete), GpuClass::High},
        {"NVIDIA with unknown memory", gpu("NVIDIA GeForce GTX 960", NV, 0), GpuClass::Unknown},
        {"an unknown vendor", gpu("VMware SVGA 3D", 0x15ad, 1), GpuClass::Unknown},
        {"the software renderer", gpu("Microsoft Basic Render Driver", MS, 0), GpuClass::Unknown},
        {"Vulkan llvmpipe", gpu("llvmpipe (LLVM 15.0.7, 256 bits)", 0x10005, 2, GpuKind::Software), GpuClass::Unknown},
    };
    for (const Case &c : cases)
        CHECK(classify_gpu(c.a) == c.want, "gpu: %s is %s (got %s)", c.what, gpu_class_key(c.want), gpu_class_key(classify_gpu(c.a)));

    CHECK(preset_for_gpu(GpuClass::Integrated) == Preset::Laptop && preset_for_gpu(GpuClass::Low) == Preset::Balanced &&
          preset_for_gpu(GpuClass::Mid) == Preset::Gaming && preset_for_gpu(GpuClass::High) == Preset::Gaming &&
          preset_for_gpu(GpuClass::Unknown) == Preset::Balanced, "gpu: class -> preset");

    /* Optimus laptop: the game runs on the discrete card. */
    GpuChoice c = choose_gpu({gpu("Intel(R) UHD Graphics 630", INTEL, 0.125), gpu("NVIDIA GeForce RTX 3050 Laptop GPU", NV, 4)});
    CHECK(c.found && c.cls == GpuClass::Mid && c.adapter.vendor_id == NV, "gpu: Optimus picks the discrete card (%s)", c.adapter.name.c_str());
    c = choose_gpu({gpu("Microsoft Basic Render Driver", MS, 0), gpu("Intel(R) UHD Graphics 620", INTEL, 0.125)});
    CHECK(c.found && c.cls == GpuClass::Integrated, "gpu: the software renderer is left out");
    c = choose_gpu({gpu("NVIDIA GeForce GTX 1050", NV, 2), gpu("NVIDIA GeForce RTX 4070", NV, 12)});
    CHECK(c.cls == GpuClass::High, "gpu: two cards: the one with more memory");
    c = choose_gpu({});
    CHECK(!c.found && c.cls == GpuClass::Unknown && preset_for_gpu(c.cls) == Preset::Balanced, "gpu: nothing found -> Balanced");
    c = choose_gpu({gpu("Microsoft Basic Render Driver", MS, 0)});
    CHECK(!c.found && c.cls == GpuClass::Unknown, "gpu: only the software renderer -> unknown");

    /* 32-bit DXGI tops out near 4 GB: "4+ GB" is mid, not low. */
    GpuAdapter capped = gpu("NVIDIA GeForce RTX 3070", NV, 4);
    capped.vram_at_least = true;
    CHECK(classify_gpu(capped) == GpuClass::Mid && gpu_vram_text(capped) == "4+ GB", "gpu: capped memory (%s)", gpu_vram_text(capped).c_str());
    CHECK(gpu_vram_text(gpu("x", NV, 2)) == "2 GB" && gpu_vram_text(gpu("x", NV, 0.5)) == "512 MB" &&
          gpu_vram_text(gpu("x", NV, 3.98)) == "4 GB" && gpu_vram_text(gpu("x", NV, 0)).empty(), "gpu: memory text");

    CHECK(gpu_display_name("Intel(R) UHD Graphics 620 (KBL GT2)") == "Intel UHD Graphics 620" &&
          gpu_display_name("AMD Radeon(TM) RX 6600 (RADV NAVI23)") == "AMD Radeon RX 6600" &&
          gpu_display_name("NVIDIA GeForce GTX 1050") == "NVIDIA GeForce GTX 1050" &&
          gpu_display_name("  ").empty(), "gpu: display name (%s)", gpu_display_name("Intel(R) UHD Graphics 620 (KBL GT2)").c_str());

    /* Why, in the launcher's language. */
    Lang was = ui_lang();
    set_ui_lang(Lang::Es);
    c = choose_gpu({gpu("NVIDIA GeForce GTX 1050", NV, 2)});
    std::string why = gpu_reason(c, preset_for_gpu(c.cls));
    CHECK(why == "Elegimos Equilibrado para tu NVIDIA GeForce GTX 1050 (2 GB).", "gpu: reason, low (%s)", why.c_str());
    c = choose_gpu({gpu("Intel(R) UHD Graphics 620", INTEL, 0.125)});
    why = gpu_reason(c, preset_for_gpu(c.cls));
    CHECK(why == "Elegimos Rendimiento para tu Intel UHD Graphics 620 (gráfica integrada).", "gpu: reason, integrated (%s)", why.c_str());
    why = gpu_reason(choose_gpu({}), Preset::Balanced);
    CHECK(why == "No pudimos identificar tu tarjeta gráfica, así que elegimos Equilibrado.", "gpu: reason, nothing found (%s)", why.c_str());
    c = choose_gpu({gpu("NVIDIA GeForce GTX 960", NV, 0)});
    why = gpu_reason(c, preset_for_gpu(c.cls));
    CHECK(why == "Elegimos Equilibrado: no sabemos cuánta memoria tiene tu NVIDIA GeForce GTX 960.", "gpu: reason, memory unknown (%s)", why.c_str());
    set_ui_lang(Lang::En);
    c = choose_gpu({gpu("NVIDIA GeForce RTX 3060", NV, 12)});
    why = gpu_reason(c, preset_for_gpu(c.cls));
    CHECK(why == "We picked Quality for your NVIDIA GeForce RTX 3060 (12 GB).", "gpu: reason, English (%s)", why.c_str());
    set_ui_lang(was);

    /* The probe itself must not crash on this machine (whatever it finds). */
    std::vector<GpuAdapter> found = probe_gpus();
    GpuChoice here = choose_gpu(found);
    printf("info gpu probe: %zu adapter(s), chose \"%s\" (%s, %s, device %04x, driver %s, budget %llu MB, bc %d)\n", found.size(),
           here.adapter.name.c_str(), gpu_class_key(here.cls), gpu_tier_key(here.tier), here.adapter.device_id,
           here.adapter.driver.c_str(), (unsigned long long)(here.adapter.budget >> 20), here.adapter.bc);
}

static GpuAdapter gpu_id(const char *name, uint32_t vendor, uint32_t device, double vram_gb, GpuKind kind = GpuKind::Discrete) {
    GpuAdapter a = gpu(name, vendor, vram_gb, kind);
    a.device_id = device;
    return a;
}

/* A profile saved with the retired "iris-xe" preset key loads as Custom with every saved value
 * intact: the same command line and the same ini once written back. */
static void test_retired_iris_xe_key() {
    Profile x;
    x.name = "old";
    apply_preset(x, Preset::Laptop, "");
    x.fsr = "quality";
    x.aniso = 16;
    x.shadow_soft = x.shadow_range = x.bloom = x.rain_hd = x.rain_puddles = x.wet_light_streaks = x.materials = true;
    x.lamp_lights = x.char_lighting = x.world_lighting = x.fire_light = x.level_lights = x.level_lights_cheap = true;
    x.world_shadows = x.world_shadows_low = x.filmic = x.auto_exposure = x.cutscene_dof = x.sky_hd = x.sharp_faces = true;
    x.draw_distance = 1, x.fx_scale = 2;
    ProfileFile f;
    f.profiles.push_back(x);
    f.selected = x.name;
    const std::string written = write_ini(f);
    /* write_ini writes the detected preset: put the old key in its place, as an older launcher saved it. */
    std::string text = written;
    const size_t at = text.find("\npreset = "), end = at == std::string::npos ? at : text.find('\n', at + 1);
    CHECK(end != std::string::npos, "retired iris-xe: preset line written");
    if (end == std::string::npos) return;
    text.replace(at, end - at, "\npreset = iris-xe");
    ProfileFile g;
    CHECK(parse_ini(text, g, nullptr) && g.profiles.size() == 1, "retired iris-xe: parses");
    if (g.profiles.size() != 1) return;
    const Profile &y = g.profiles[0];
    CHECK(y.preset == "custom" && preset_from_key("iris-xe") == Preset::Custom, "retired iris-xe: loads as custom (%s)", y.preset.c_str());
    CHECK(join(build_argv(y)) == join(build_argv(x)), "retired iris-xe: command line intact (%s)", join(build_argv(y)).c_str());
    CHECK(write_ini(g) == written && written.find("\npreset = custom\n") != std::string::npos, "retired iris-xe: every saved value intact");
}

/* Intel integrated graphics (Iris Xe, the integrated Arc, UHD) are T0; Iris Xe and the integrated Arc
 * get the integrated graphics preset, UHD keeps Performance. */
static void test_intel_integrated_recommendation() {
    const uint32_t INTEL = 0x8086;
    GpuTierTable none;
    for (const char *name : {"Intel(R) Iris(R) Xe Graphics", "Intel(R) Arc(TM) Graphics", "Intel(R) UHD Graphics 620"}) {
        const std::string want = strstr(name, "UHD") ? "laptop" : "integrated";
        GpuAdapter a = gpu_id(name, INTEL, 0x9a49, 0.125, GpuKind::Integrated);
        a.driver = "101.6913";
        GpuDetection d;
        CHECK(refresh_gpu_detection(d, choose_gpu({a}, none), none) && d.tier == "t0" && d.recommended == want,
              "intel integrated: %s recommends %s (got %s, %s)", name, want.c_str(), d.recommended.c_str(), d.tier.c_str());
    }
}

/* Integrated graphics: the profile tuned by hand on an Iris Xe laptop (about 20-25 fps to 55-60):
 * 1600x900 with balanced temporal upscaling and dynamic resolution, MSAA, contact shadows, damp
 * streets and lamp shadows off, low volumetric fog, the rest of the lighting on. */
static void test_integrated_preset() {
    Profile p;
    apply_preset(p, Preset::Integrated, "/nonexistent");
    const std::string o = "|" + opts(p) + "|";
    for (const char *want : {"|--resolution|1600x900|", "|--widescreen|", "|--fullscreen|", "|--taa|", "|--aniso|16|", "|--sim60|",
                             "|--taa-upscale|balanced|", "|--dynres|auto|", "|--shadow-soft|", "|--shadow-range|", "|--bloom|",
                             "|--rain-hd|", "|--wet-light-streaks|", "|--materials|", "|--ssao|", "|--ssao-mode|gtao|",
                             "|--gtao-quality|medium|", "|--lamp-lights|", "|--char-lighting|", "|--hair|", "|--skin|", "|--eyes|",
                             "|--cloth|", "|--world-lighting|", "|--normal-maps|", "|--dry-highlights|", "|--parallax|", "|--fire-light|",
                             "|--level-lights|", "|--level-lights-max|12|", "|--world-shadows|", "|--world-shadows-size|1024|",
                             "|--volfog|", "|--volfog-quality|low|", "|--soft-particles|", "|--filmic|", "|--auto-exposure|",
                             "|--cutscene-dof|", "|--sky-hd|", "|--skip-intro|"})
        CHECK(o.find(want) != std::string::npos, "integrated preset: %s in the command line (%s)", want, o.c_str());
    for (const char *never : {"|--msaa|", "|--fsr|", "|--contact-shadows|", "|--damp-streets|", "|--lamp-shadows|", "|--light-shafts|",
                              "|--supersample|", "|--fps-overlay|", "|--frames-in-flight|"})
        CHECK(o.find(never) == std::string::npos, "integrated preset: no %s (%s)", never, o.c_str());
    CHECK(detect_preset(p) == Preset::Integrated, "integrated preset: detected");
    CHECK(std::string(preset_key(Preset::Integrated)) == "integrated" && preset_from_key("integrated") == Preset::Integrated,
          "integrated preset: key");
    /* Every other preset stays itself, and leaving the integrated preset is a full switch. */
    for (Preset pr : {Preset::Laptop, Preset::Balanced, Preset::Gaming, Preset::Ultra}) {
        Profile q = p;
        apply_preset(q, pr, "/nonexistent");
        CHECK(detect_preset(q) == pr, "integrated preset: -> %s detected", preset_key(pr));
        apply_preset(q, Preset::Integrated, "/nonexistent");
        CHECK(join(build_argv(q)) == join(build_argv(p)) && detect_preset(q) == Preset::Integrated,
              "integrated preset: %s -> integrated gives the same command line", preset_key(pr));
    }
    /* Its own choices are part of the match. */
    Profile e = p;
    e.volfog_quality = "high";
    CHECK(detect_preset(e) == Preset::Custom, "integrated preset: other fog quality is custom");
    e = p;
    e.resolution = "1920x1080";
    CHECK(detect_preset(e) == Preset::Custom, "integrated preset: full HD is custom");
    e = p;
    e.taa_upscale = "quality";
    CHECK(detect_preset(e) == Preset::Custom, "integrated preset: another upscale is custom");
    /* A profile saved as integrated loads back as integrated. */
    ProfileFile f;
    p.name = "igpu";
    f.profiles.push_back(p);
    f.selected = p.name;
    ProfileFile g;
    CHECK(parse_ini(write_ini(f), g, nullptr) && g.profiles.size() == 1 && g.profiles[0].preset == "integrated" &&
              detect_preset(g.profiles[0]) == Preset::Integrated && write_ini(g) == write_ini(f),
          "integrated preset: ini round trip");
}

/* First run: a fake device list with the stronger integrated graphics (Iris Xe / Iris Plus, Intel's
 * integrated Arc, Radeon 680M and newer RDNA) recommends the integrated preset; Intel UHD / HD,
 * Vega APUs, unrecognised integrated graphics, a discrete card (Optimus included), a weak discrete
 * T0 card or graphics without BC textures do not. */
static void test_integrated_detection() {
    const uint32_t NV = 0x10de, AMD = 0x1002, INTEL = 0x8086;
    const GpuTierTable &table = builtin_gpu_tiers();
    GpuAdapter iris = gpu_id("Intel(R) Iris(R) Xe Graphics", INTEL, 0x9a49, 0.125, GpuKind::Integrated);
    iris.driver = "101.6913";
    iris.bc = 1;
    GpuAdapter iris_no_bc = iris;
    iris_no_bc.bc = 0;
    auto igpu = [](const char *name, uint32_t vendor) { return gpu_id(name, vendor, 0x1234, 0.5, GpuKind::Integrated); };
    struct Case {
        const char *what;
        std::vector<GpuAdapter> devices;
        Preset want;
    } cases[] = {
        {"Vulkan Iris Xe", {iris}, Preset::Integrated},
        {"Linux Iris Xe (ANV)", {igpu("Intel(R) Xe Graphics (TGL GT2)", INTEL)}, Preset::Integrated},
        {"Iris Plus", {igpu("Intel(R) Iris(R) Plus Graphics", INTEL)}, Preset::Integrated},
        {"Meteor Lake Arc", {igpu("Intel(R) Arc(TM) Graphics", INTEL)}, Preset::Integrated},
        {"Lunar Lake Arc 140V", {igpu("Intel(R) Arc(TM) 140V GPU (16GB)", INTEL)}, Preset::Integrated},
        {"Radeon 680M", {igpu("AMD Radeon(TM) 680M", AMD)}, Preset::Integrated},
        {"Radeon 780M", {igpu("AMD Radeon(TM) 780M Graphics", AMD)}, Preset::Integrated},
        {"Radeon 780M (DXGI, kind guessed)", {gpu("AMD Radeon 780M Graphics", AMD, 8)}, Preset::Integrated},
        {"Radeon 880M", {igpu("AMD Radeon(TM) 880M", AMD)}, Preset::Integrated},
        {"Radeon 890M", {igpu("AMD Radeon(TM) 890M Graphics", AMD)}, Preset::Integrated},
        {"Radeon 8060S", {igpu("AMD Radeon(TM) 8060S Graphics", AMD)}, Preset::Integrated},
        {"Linux 680M (RADV REMBRANDT)", {igpu("AMD Radeon Graphics (RADV REMBRANDT)", AMD)}, Preset::Integrated},
        {"Linux 780M (RADV PHOENIX)", {igpu("AMD Radeon Graphics (RADV PHOENIX)", AMD)}, Preset::Integrated},
        {"Intel UHD 620 (Vulkan)", {igpu("Intel(R) UHD Graphics 620", INTEL)}, Preset::Laptop},
        {"Intel UHD 620 (DXGI, kind guessed)", {gpu("Intel(R) UHD Graphics 620", INTEL, 0.125)}, Preset::Laptop},
        {"Intel HD 520", {igpu("Intel(R) HD Graphics 520", INTEL)}, Preset::Laptop},
        {"Radeon Vega 8", {igpu("AMD Radeon(TM) Vega 8 Graphics", AMD)}, Preset::Laptop},
        {"Radeon Vega 8 (DXGI, kind guessed)", {gpu("AMD Radeon(TM) Vega 8 Graphics", AMD, 2)}, Preset::Laptop},
        {"Linux Vega APU (RADV RENOIR)", {igpu("AMD Radeon Graphics (RADV RENOIR)", AMD)}, Preset::Laptop},
        {"unrecognised integrated", {igpu("AMD Radeon(TM) Graphics", AMD)}, Preset::Laptop},
        {"Optimus: Iris Xe and an RTX 3050", {iris, gpu_id("NVIDIA GeForce RTX 3050 Laptop GPU", NV, 0x25a2, 4)}, Preset::Balanced},
        {"discrete T0 GeForce MX", {gpu_id("NVIDIA GeForce MX450", NV, 0x1f97, 2)}, Preset::Laptop},
        {"Iris Xe without BC textures", {iris_no_bc}, Preset::Laptop},
        {"nothing found", {}, Preset::Balanced},
    };
    for (const Case &c : cases) {
        GpuChoice choice = choose_gpu(c.devices, table);
        CHECK(preset_for_choice(choice) == c.want, "igpu detection: %s -> %s (got %s)", c.what, preset_key(c.want),
              preset_key(preset_for_choice(choice)));
        GpuDetection d;
        if (refresh_gpu_detection(d, choice, table))
            CHECK(gpu_detected_preset(d) == c.want && d.recommended == preset_key(c.want), "igpu detection: %s stored as %s (got %s)",
                  c.what, preset_key(c.want), d.recommended.c_str());
    }
    /* A detection stored by an older table (integrated -> laptop) is worked out again. */
    GpuDetection old;
    CHECK(refresh_gpu_detection(old, choose_gpu({iris}, table), table), "igpu detection: first store");
    old.recommended = "laptop";
    old.table = 3;
    CHECK(table.version > 3 && refresh_gpu_detection(old, choose_gpu({iris}, table), table) && old.recommended == "integrated",
          "igpu detection: an older table's laptop recommendation becomes integrated (%s, table %d)", old.recommended.c_str(), table.version);
    /* The one line that says so, in both languages. */
    Lang was = ui_lang();
    GpuChoice c = choose_gpu({iris}, table);
    set_ui_lang(Lang::Es);
    std::string why = gpu_reason(c, preset_for_choice(c));
    CHECK(why == "Detectamos gráficos integrados (Intel Iris Xe Graphics): elegimos el ajuste recomendado.", "igpu detection: reason (%s)",
          why.c_str());
    set_ui_lang(Lang::En);
    why = gpu_reason(c, preset_for_choice(c));
    CHECK(why == "Integrated graphics detected (Intel Iris Xe Graphics): we picked the recommended preset.",
          "igpu detection: reason, English (%s)", why.c_str());
    /* Integrated graphics with another preset (no BC textures) keep the plain line. */
    why = gpu_reason(c, Preset::Laptop);
    CHECK(why == "We picked Performance for your Intel Iris Xe Graphics (integrated graphics).", "igpu detection: reason, other preset (%s)",
          why.c_str());
    set_ui_lang(was);
}

/* The versioned tier table (data/gpu_tiers.txt, embedded) and the tier -> preset rule. */
static void test_gpu_tiers() {
    const uint32_t NV = 0x10de, AMD = 0x1002, INTEL = 0x8086;

    /* The parser: version, name and id rules, demote rules; errors name the line. */
    GpuTierTable t;
    std::string err;
    CHECK(parse_gpu_tiers("# c\nversion 3\nT2 nvidia name rtx 20##\nT3 any id 0x2200-0x22ff\ndemote amd name rx ####m\n", t, &err) &&
              t.version == 3 && t.rules.size() == 3 && t.rules[0].tier == GpuTier::T2 && t.rules[0].vendor == NV &&
              t.rules[0].pattern == "rtx 20##" && t.rules[1].kind == GpuTierRule::Id && t.rules[1].vendor == 0 &&
              t.rules[1].lo == 0x2200 && t.rules[1].hi == 0x22ff && t.rules[2].kind == GpuTierRule::Demote,
          "tiers: parse (%s)", err.c_str());
    CHECK(!parse_gpu_tiers("T2 nvidia name x\n", t, &err) && err.find("version") != std::string::npos, "tiers: version is required (%s)", err.c_str());
    CHECK(!parse_gpu_tiers("version 1\nT9 nvidia name x\n", t, &err) && err.find("line 2") != std::string::npos, "tiers: bad tier (%s)", err.c_str());
    CHECK(!parse_gpu_tiers("version 1\nT1 sega name x\n", t, &err), "tiers: bad vendor");
    CHECK(!parse_gpu_tiers("version 1\nT1 nvidia id 0x20-0x10\n", t, &err), "tiers: empty id range");
    CHECK(!parse_gpu_tiers("version 1\nT1 nvidia name\n", t, &err), "tiers: empty pattern");

    /* Patterns: lower case, anywhere in the name, # a digit, ? any character, * any run. */
    CHECK(gpu_name_matches("rtx 3070", "nvidia geforce rtx 3070 ti") && gpu_name_matches("rx ####m", "amd radeon rx 6800m") &&
              !gpu_name_matches("rx ####m", "amd radeon rx 6800") && gpu_name_matches("arc a7*graphics", "intel arc a770 graphics") &&
              gpu_name_matches("gtx 10?0", "nvidia geforce gtx 1060 6gb") && !gpu_name_matches("rtx 40#0", "rtx 4"),
          "tiers: patterns");

    /* The built-in table parses and covers the three vendors. */
    const GpuTierTable &b = builtin_gpu_tiers();
    CHECK(b.version >= 1 && b.rules.size() > 20, "tiers: built-in table (version %d, %zu rules)", b.version, b.rules.size());
    struct Case { const char *what; GpuAdapter a; GpuTier want; };
    const Case cases[] = {
        {"GT 1030", gpu("NVIDIA GeForce GT 1030", NV, 2, GpuKind::Discrete), GpuTier::T0},
        {"MX450", gpu("NVIDIA GeForce MX450", NV, 2, GpuKind::Discrete), GpuTier::T0},
        {"GTX 1050", gpu("NVIDIA GeForce GTX 1050", NV, 2, GpuKind::Discrete), GpuTier::T1},
        {"GTX 1650 4 GB", gpu("NVIDIA GeForce GTX 1650", NV, 4, GpuKind::Discrete), GpuTier::T1},
        {"GTX 1660 SUPER", gpu("NVIDIA GeForce GTX 1660 SUPER", NV, 6, GpuKind::Discrete), GpuTier::T2},
        {"RTX 2050 laptop", gpu("NVIDIA GeForce RTX 2050", NV, 4, GpuKind::Discrete), GpuTier::T2},
        {"RTX 2060", gpu("NVIDIA GeForce RTX 2060", NV, 6, GpuKind::Discrete), GpuTier::T2},
        {"RTX 3060", gpu("NVIDIA GeForce RTX 3060", NV, 12, GpuKind::Discrete), GpuTier::T2},
        {"RTX 3070", gpu("NVIDIA GeForce RTX 3070", NV, 8, GpuKind::Discrete), GpuTier::T3},
        {"RTX 4060", gpu("NVIDIA GeForce RTX 4060", NV, 8, GpuKind::Discrete), GpuTier::T2},
        {"RTX 4060 Ti", gpu("NVIDIA GeForce RTX 4060 Ti", NV, 8, GpuKind::Discrete), GpuTier::T3},
        {"RTX 4090", gpu("NVIDIA GeForce RTX 4090", NV, 24, GpuKind::Discrete), GpuTier::T3},
        {"RTX 3050 Laptop (demoted)", gpu("NVIDIA GeForce RTX 3050 Laptop GPU", NV, 4, GpuKind::Discrete), GpuTier::T1},
        {"RTX 3070 Laptop (demoted)", gpu("NVIDIA GeForce RTX 3070 Laptop GPU", NV, 8, GpuKind::Discrete), GpuTier::T2},
        {"RX 580", gpu("Radeon RX 580 Series", AMD, 8), GpuTier::T1},
        {"RX 6600 (RADV)", gpu("AMD Radeon RX 6600 (RADV NAVI23)", AMD, 8, GpuKind::Discrete), GpuTier::T2},
        {"RX 6700 XT", gpu("AMD Radeon RX 6700 XT", AMD, 12, GpuKind::Discrete), GpuTier::T3},
        {"RX 7900 XTX", gpu("AMD Radeon RX 7900 XTX", AMD, 24, GpuKind::Discrete), GpuTier::T3},
        {"RX 6800M (demoted)", gpu("AMD Radeon RX 6800M", AMD, 12, GpuKind::Discrete), GpuTier::T2},
        {"Radeon 780M", gpu("AMD Radeon 780M Graphics", AMD, 8), GpuTier::T0},
        {"Intel UHD 620", gpu("Intel(R) UHD Graphics 620", INTEL, 0.125), GpuTier::T0},
        {"Intel Arc integrated", gpu("Intel(R) Arc(TM) Graphics", INTEL, 0.5, GpuKind::Integrated), GpuTier::T0},
        {"Arc A380", gpu("Intel(R) Arc(TM) A380 Graphics", INTEL, 6, GpuKind::Discrete), GpuTier::T1},
        {"Arc A770", gpu("Intel(R) Arc(TM) A770 Graphics", INTEL, 16, GpuKind::Discrete), GpuTier::T2},
        {"Arc B580", gpu("Intel(R) Arc(TM) B580 Graphics", INTEL, 12, GpuKind::Discrete), GpuTier::T2},
        /* no name (the DRM fallback): the device ID */
        {"nameless GA104", gpu_id("NVIDIA", NV, 0x2484, 8), GpuTier::T3},
        {"nameless GP107", gpu_id("NVIDIA", NV, 0x1c82, 4), GpuTier::T1},
        {"nameless TU117 (GTX 1650)", gpu_id("NVIDIA", NV, 0x1f82, 4), GpuTier::T1},
        /* in no rule: the VRAM rule */
        {"unknown NVIDIA 2 GB", gpu("NVIDIA Quadro P400", NV, 2, GpuKind::Discrete), GpuTier::T1},
        {"unknown NVIDIA 6 GB", gpu("NVIDIA Future 9000", NV, 6, GpuKind::Discrete), GpuTier::T2},
        {"unknown NVIDIA 16 GB", gpu("NVIDIA Future 9900", NV, 16, GpuKind::Discrete), GpuTier::T3},
        {"unknown memory", gpu("NVIDIA Future 9000", NV, 0, GpuKind::Discrete), GpuTier::Unknown},
        /* a fast name with little memory is capped at T1 */
        {"RTX 2060 with 3 GB", gpu("NVIDIA GeForce RTX 2060", NV, 3, GpuKind::Discrete), GpuTier::T1},
        {"software", gpu("llvmpipe (LLVM 15.0.7, 256 bits)", 0x10005, 2, GpuKind::Software), GpuTier::Unknown},
    };
    for (const Case &c : cases)
        CHECK(gpu_tier(b, c.a) == c.want, "tiers: %s is %s (got %s)", c.what, gpu_tier_key(c.want), gpu_tier_key(gpu_tier(b, c.a)));
    GpuAdapter no_bc = gpu("NVIDIA GeForce RTX 3070", NV, 8, GpuKind::Discrete);
    no_bc.bc = 0;
    CHECK(gpu_tier(b, no_bc) == GpuTier::T0, "tiers: no BC textures -> T0");

    /* T0 Performance, T1 Balanced, T2 Quality, T3 Ultra. */
    CHECK(preset_for_tier(GpuTier::T0) == Preset::Laptop && preset_for_tier(GpuTier::T1) == Preset::Balanced &&
              preset_for_tier(GpuTier::T2) == Preset::Gaming && preset_for_tier(GpuTier::T3) == Preset::Ultra &&
              preset_for_tier(GpuTier::Unknown) == Preset::Balanced, "tiers: tier -> preset");
    GpuTier back = GpuTier::Unknown;
    CHECK(gpu_tier_from_key("t2", back) && back == GpuTier::T2 && !gpu_tier_from_key("t7", back), "tiers: keys");
    GpuChoice c = choose_gpu({gpu("Intel(R) UHD Graphics 630", INTEL, 0.125), gpu("NVIDIA GeForce RTX 3070", NV, 8, GpuKind::Discrete)});
    CHECK(c.found && c.tier == GpuTier::T3, "tiers: choose_gpu fills the tier (%s)", gpu_tier_key(c.tier));

    /* Driver versions: NVIDIA's 10/8/8/6 bits, Intel's Windows 18.14, VK_MAKE_VERSION otherwise. */
    CHECK(decode_driver_version(NV, (566u << 22) | (36u << 14), true) == "566.36", "driver: NVIDIA (%s)",
          decode_driver_version(NV, (566u << 22) | (36u << 14), true).c_str());
    CHECK(decode_driver_version(NV, (550u << 22) | (120u << 14), false) == "550.120", "driver: NVIDIA Linux");
    CHECK(decode_driver_version(INTEL, (101u << 14) | 5762u, true) == "101.5762", "driver: Intel Windows (%s)",
          decode_driver_version(INTEL, (101u << 14) | 5762u, true).c_str());
    CHECK(decode_driver_version(INTEL, (24u << 22) | (2u << 12) | 8u, false) == "24.2.8", "driver: Mesa");
    CHECK(decode_driver_version(AMD, (2u << 22) | (0u << 12) | 302u, true) == "2.0.302", "driver: AMD");
    CHECK(decode_driver_version(AMD, 0, true).empty(), "driver: none");

    /* The detection in the ini: recomputed only when the card, the driver or the table changes. */
    GpuAdapter rtx = gpu_id("NVIDIA GeForce RTX 2060", NV, 0x1f08, 6);
    rtx.driver = "566.36";
    rtx.budget = 5ull << 30;
    rtx.bc = 1;
    CHECK(gpu_identity(rtx) == "10de:1f08:566.36", "detect: identity (%s)", gpu_identity(rtx).c_str());
    GpuDetection d;
    GpuChoice now = choose_gpu({rtx});
    CHECK(refresh_gpu_detection(d, now, b) && d.id == "10de:1f08:566.36" && d.name == "NVIDIA GeForce RTX 2060" &&
              d.driver == "566.36" && d.tier == "t2" && d.recommended == "gaming" && d.vram_mb == 6144 && d.budget_mb == 5120 &&
              d.bc == 1 && d.table == b.version,
          "detect: first detection stored (%s %s %s)", d.id.c_str(), d.tier.c_str(), d.recommended.c_str());
    d.recommended = "balanced"; /* as an older table had it: kept while nothing changed */
    CHECK(!refresh_gpu_detection(d, now, b) && d.recommended == "balanced", "detect: same card and driver -> kept");
    rtx.driver = "572.16";
    CHECK(refresh_gpu_detection(d, choose_gpu({rtx}), b) && d.driver == "572.16" && d.recommended == "gaming", "detect: new driver -> recomputed");
    GpuTierTable newer = b;
    newer.version = b.version + 1;
    CHECK(refresh_gpu_detection(d, choose_gpu({rtx}), newer) && d.table == newer.version, "detect: new table -> recomputed");
    CHECK(!refresh_gpu_detection(d, choose_gpu({}), b) && d.id == "10de:1f08:572.16", "detect: a failed probe keeps the stored card");
    CHECK(gpu_detected_preset(d) == Preset::Gaming && gpu_detected_preset(GpuDetection()) == Preset::Balanced, "detect: recommended preset");

    /* The line on the wizard's graphics step and the Display page. */
    Lang was = ui_lang();
    set_ui_lang(Lang::Es);
    std::string line = gpu_detected_line(d);
    CHECK(line == "Detectamos: NVIDIA GeForce RTX 2060 (6 GB) — recomendado: Calidad", "detect: line (%s)", line.c_str());
    line = gpu_detected_line(GpuDetection());
    CHECK(line == "No pudimos identificar tu tarjeta gráfica — recomendado: Equilibrado", "detect: line, nothing (%s)", line.c_str());
    set_ui_lang(Lang::En);
    line = gpu_detected_line(d);
    CHECK(line == "Detected: NVIDIA GeForce RTX 2060 (6 GB) — recommended: Quality", "detect: line, English (%s)", line.c_str());
    set_ui_lang(was);

    /* ini round trip of the [launcher] gpu_* keys */
    ProfileFile f;
    f.selected = "a";
    f.profiles.push_back(Profile());
    f.profiles[0].name = "a";
    f.settings.gpu = d;
    std::string text = write_ini(f);
    ProfileFile g;
    std::vector<std::string> warn;
    CHECK(parse_ini(text, g, &warn) && warn.empty() && g.settings.gpu.id == d.id && g.settings.gpu.name == d.name &&
              g.settings.gpu.driver == d.driver && g.settings.gpu.tier == d.tier && g.settings.gpu.recommended == d.recommended &&
              g.settings.gpu.vram_mb == d.vram_mb && g.settings.gpu.budget_mb == d.budget_mb && g.settings.gpu.bc == d.bc &&
              g.settings.gpu.table == d.table && g.settings.unknown.empty(),
          "detect: ini round trip");
    CHECK(write_ini(ProfileFile()).find("gpu_") == std::string::npos, "detect: nothing detected, no gpu_ keys");
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

/* A UTF-8 byte sequence starting at s[i] that is a letter: ASCII a-z / A-Z or a Latin-1 letter
 * (C3 80-BF except the x and / signs); "¿", "¡", "«", "—" and the like are not. */
static bool letter_at(const std::string &s, size_t i) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (c == 0xC3 && i + 1 < s.size()) return (unsigned char)s[i + 1] != 0x97 && (unsigned char)s[i + 1] != 0xB7;
    return false;
}

/* The first voseo form (Rioplatense "vos": querés, elegí, mostrá...) among the words of a Spanish
 * text, or "" when it is neutral Spanish (tú). The launcher speaks neutral Spanish. */
static std::string voseo_word(const char *text) {
    static const char *const FORMS[] = {
        "vos", "sos", "querés", "podés", "tenés", "sabés", "hacés", "ponés", "usás", "jugás", "elegís", "necesitás",
        "preferís", "decís", "venís", "salís", "elegí", "mirá", "probá", "hacé", "poné", "usá", "mostrá", "tocá",
        "apretá", "pulsá", "presioná", "cambiá", "seleccioná", "abrí", "escribí", "andá", "vení", "decí", "salí", "tené",
        "dejá", "activá", "desactivá", "instalá", "copiá", "buscá", "esperá", "conectá", "revisá", "guardá", "indicá",
        "configurá", "reiniciá", "cerrá", "pegá", "iniciá", "marcá", "ejecutá", "extraé", "volvé", "empezá", "jugá",
        "fijate", "asegurate", "acordate", "fijá", "elegilo", "elegila", "probalo", "probala", "cambialo", "mirala"};
    std::string low = text;
    for (size_t i = 0; i < low.size(); i++) {
        unsigned char c = (unsigned char)low[i];
        if (c >= 'A' && c <= 'Z') low[i] = (char)(c + 32);
        else if (c == 0xC3 && i + 1 < low.size() && (unsigned char)low[i + 1] >= 0x80 && (unsigned char)low[i + 1] <= 0x9E &&
                 (unsigned char)low[i + 1] != 0x97)
            low[i + 1] = (char)((unsigned char)low[i + 1] + 0x20), i++;
    }
    for (const char *form : FORMS) {
        size_t n = strlen(form);
        for (size_t at = low.find(form); at != std::string::npos; at = low.find(form, at + 1)) {
            bool start = at == 0 || !(letter_at(low, at - 1) || (at >= 2 && letter_at(low, at - 2) && (unsigned char)low[at - 2] == 0xC3));
            bool end = at + n >= low.size() || !letter_at(low, at + n);
            if (start && end) return form;
        }
    }
    return "";
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
    CHECK(voseo_word("¿Querés ver?") == "querés" && voseo_word("Mostrá los FPS") == "mostrá" && voseo_word("elegí abajo") == "elegí" &&
              voseo_word("¿Quieres ver?").empty() && voseo_word("Elige abajo; más detrás; vosotros, versos") == "" &&
              voseo_word("Fijate.") == "fijate",
          "i18n: the voseo check finds voseo and passes neutral Spanish");
    int voseo = 0;
    for (int i = 0; i < (int)S::Count; i++) {
        std::string w = voseo_word(tr((S)i, Lang::Es));
        if (!w.empty()) voseo++, printf("     id %d uses voseo (\"%s\"): \"%s\"\n", i, w.c_str(), tr((S)i, Lang::Es));
    }
    CHECK(voseo == 0, "i18n: every Spanish string is neutral Spanish (tú, no voseo): %d with voseo", voseo);
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
    /* Gym: mash_a on H and the mouse wheel, mash.rate, as the runtime. */
    std::vector<int> mk = d.keys_for(CTL_MASH);
    CHECK(mk.size() == 2 && mk[0] == MOUSE_WHEEL && mk[1] == 'h' && d.get("mash.rate") == "12" && key_is_mouse(MOUSE_WHEEL) &&
          key_config_name(MOUSE_WHEEL) && std::string(key_config_name(MOUSE_WHEEL)) == "mouse.wheel" &&
          key_from_config_name("Mouse.Wheel") == MOUSE_WHEEL && control_from_config_name("MASH_A") == CTL_MASH &&
          std::string(control_config_name(CTL_MASH)) == "mash_a" && key_display_name(MOUSE_WHEEL) == "Mouse wheel",
          "keymap: mash_a on the mouse wheel and H, mash.rate 12");
    std::vector<std::string> wwarn;
    KeyMap w = parse_keymap("mouse.wheel = b\nmash.rate = 20\nkey.h = none\n", &wwarn);
    CHECK(wwarn.empty() && w.binding[MOUSE_WHEEL] == CTL_B && w.binding['h'] == -1 && w.get("mash.rate") == "20" &&
          parse_keymap(write_keymap(w), nullptr) == w && w.bind(MOUSE_WHEEL, CTL_SPIN) == -2 && w.bind(MOUSE_WHEEL, CTL_MASH) == CTL_B,
          "keymap: wheel and mash.rate lines round trip; the spin cannot go on the wheel (%d)", (int)wwarn.size());
    KeyMap e = parse_keymap("", nullptr);
    CHECK(e == d && parse_keymap(write_keymap(d), nullptr) == d, "keymap: empty file = defaults, defaults round trip");

    CHECK(m.bind('q', CTL_A) == CTL_LT && m.binding['q'] == CTL_A && m.keys_for(CTL_LT).empty(),
          "keymap: binding a used key moves it and reports its previous action");
    CHECK(m.bind(KEY_ESCAPE, CTL_A) == -2 && m.bind(KEY_F1, CTL_A) == -2 && m.bind(MOUSE_MIDDLE, CTL_RTHUMB) == CTL_RTHUMB,
          "keymap: reserved keys refused, rebinding the same action is no conflict");
    m.reset();
    CHECK(m == d && m.get("mouse.sensitivity") == "2.0" && m.get("mouse.sensitivity_x").empty() && m.get("mouse.sensitivity_y").empty(), "keymap: reset to defaults (mouse 2.0, axes follow it)");

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

/* Controller button remap (pad.* lines of the key map file). */
static void test_pad_remap(const std::string &scratch) {
    KeyMap d;
    bool ident = !d.swap_sticks && !d.pad_remapped();
    for (int b = 0; b < PAD_BUTTONS; b++) ident &= d.pad[b] == b && d.pad_button_for(b) == b;
    CHECK(ident && write_keymap(d).find("\npad.") == std::string::npos, "pad remap: none by default, no pad lines written");
    KeyMap m;
    CHECK(m.pad_assign(CTL_X, CTL_A) == CTL_A && m.pad[CTL_A] == CTL_X && m.pad[CTL_X] == CTL_A && m.pad_button_for(CTL_X) == CTL_A &&
          m.pad_button_for(CTL_A) == CTL_X, "pad remap: light attack on A swaps with X (the button that did it takes A's action)");
    CHECK(m.pad_assign(CTL_START, CTL_RT) == CTL_RT && m.pad[CTL_RT] == CTL_START && m.pad[CTL_START] == CTL_RT,
          "pad remap: Start on RT swaps the trigger and Start");
    CHECK(m.pad_assign(CTL_UP, CTL_UP) == CTL_UP && m.pad[CTL_UP] == CTL_UP && m.pad_assign(CTL_A, 99) == -2 && m.pad_assign(CTL_LS_UP, CTL_A) == -2,
          "pad remap: the same button is no change, sticks and unknown buttons refused");
    m.swap_sticks = true;
    std::string text = write_keymap(m);
    CHECK(text.find("pad.a = x\n") != std::string::npos && text.find("pad.x = a\n") != std::string::npos &&
          text.find("pad.right_trigger = start\n") != std::string::npos && text.find("pad.swap_sticks = true\n") != std::string::npos &&
          text.find("pad.b =") == std::string::npos, "pad remap: only the changed buttons written, in the runtime's names");
    std::vector<std::string> warn;
    KeyMap r = parse_keymap(text, &warn);
    CHECK(r == m && warn.empty(), "pad remap: write -> parse round trip (%d warnings)", (int)warn.size());
    r = parse_keymap("pad.a = none\npad.left_stick_up = a\npad.b = spin_left_stick\npad.swap_sticks = maybe\nPAD.Y = B\n", &warn);
    CHECK(warn.size() == 3 && r.pad[CTL_A] == -1 && r.pad[CTL_Y] == CTL_B && r.pad_button_for(CTL_A) == -1 && r.pad_button_for(CTL_B) == CTL_B,
          "pad remap: none, case-insensitive; sticks, spin and bad values reported (%d)", (int)warn.size());
    KeyMap k = m;
    k.reset_keys();
    CHECK(k.pad_remapped() && k.pad[CTL_A] == CTL_X && k.binding['w'] == CTL_LS_UP, "pad remap: the keyboard reset keeps the controller map");
    k.pad_reset();
    CHECK(!k.pad_remapped() && k == d, "pad remap: controller reset to defaults");
    m.reset();
    CHECK(m == d, "pad remap: full reset");
    std::string path = path_join(scratch, "keys-pad.cfg"), err;
    KeyMap s, back;
    s.pad_assign(CTL_B, CTL_Y);
    CHECK(save_keymap(path, s, &err) && load_keymap(path, back, nullptr) && back == s && back.pad[CTL_Y] == CTL_B,
          "pad remap: save + load");
    bool btn[15] = {};
    float axis[6] = {};
    CHECK(pad_input_pressed(btn, axis) == -1, "pad capture: nothing pressed");
    btn[9] = true;   /* SDL left shoulder */
    CHECK(pad_input_pressed(btn, axis) == CTL_WHITE, "pad capture: LB is the White button");
    btn[9] = false;
    btn[5] = true;   /* Guide: not a game button */
    axis[5] = 0.7f;
    CHECK(pad_input_pressed(btn, axis) == CTL_RT, "pad capture: Guide ignored, RT past half way");
    axis[5] = 0.3f;
    btn[13] = true;
    CHECK(pad_input_pressed(btn, axis) == CTL_LEFT, "pad capture: D-pad left; a light trigger is no press");
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

/* Neon glow (--neon-glow): saved as neon_glow / neon_glow_strength / neon_masks; the mask pack is
 * passed as a --texture-pack after the texture pack, only while neon glow is on. */
static void test_neon_glow_key() {
    ProfileFile f, g;
    Profile p;
    p.neon_glow = true;
    p.neon_glow_strength = 1.5f;
    p.texture_pack = "/packs/tex";
    p.neon_masks = "/packs/neon-masks";
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].neon_glow && g.profiles[0].neon_glow_strength == 1.5f &&
              g.profiles[0].neon_masks == "/packs/neon-masks" && warn.empty(),
          "neon glow: ini round trip");
    CHECK(!Profile().neon_glow && Profile().neon_masks.empty(), "neon glow: off by default, no mask pack");
    std::string on = join(build_argv(p));
    CHECK(on.find("--texture-pack|/packs/tex|--texture-pack|/packs/neon-masks") != std::string::npos &&
              on.find("--neon-glow|--neon-glow-strength|1.5") != std::string::npos,
          "neon glow: the mask pack after the texture pack, then the flag (%s)", on.c_str());
    p.neon_glow = false;
    std::string off = join(build_argv(p));
    CHECK(off.find("neon") == std::string::npos, "neon glow: nothing while off (%s)", off.c_str());
    Profile m;
    m.extra_args = "--neon-glow --neon-glow-strength 2 --ssao-radius 1";
    migrate_extra_args(m);
    CHECK(m.neon_glow && m.neon_glow_strength == 2 && m.extra_args == "--ssao-radius 1", "neon glow: moved out of extra_args (%s)",
          m.extra_args.c_str());
    CHECK(std::string(tr(S::neon_glow)) != "" && std::string(tr(S::neon_glow_desc)) != "" && std::string(tr(S::neon_glow_strength)) != "",
          "neon glow: label and description");
}

static void test_sharp_faces_key() {
    ProfileFile f, g;
    Profile p;
    p.sharp_faces = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].sharp_faces && warn.empty(), "sharp faces: ini round trip");
    CHECK(!Profile().sharp_faces, "sharp faces: off by default");
    CHECK(join(build_argv(p)).find("--sharp-faces") != std::string::npos &&
              join(build_argv(Profile())).find("--sharp-faces") == std::string::npos,
          "sharp faces: --sharp-faces only when on");
    Profile m;
    m.extra_args = "--sharp-faces --ssao-radius 1";
    migrate_extra_args(m);
    CHECK(m.sharp_faces && m.extra_args == "--ssao-radius 1", "sharp faces: moved out of extra_args (%s)", m.extra_args.c_str());
    m.extra_args = "--no-sharp-faces";
    migrate_extra_args(m);
    CHECK(!m.sharp_faces && m.extra_args.empty(), "sharp faces: --no-sharp-faces moved too");
    CHECK(std::string(tr(S::sharp_faces)) != "" && std::string(tr(S::sharp_faces_desc)) != "", "sharp faces: label and description");
}

/* Show FPS (--fps-overlay, Display): saved as fps_overlay, off by default and in every preset (applying
 * one turns it off; it is not part of preset matching, so turning it on keeps the preset), passed as
 * --fps-overlay; typed in extra_args it moves (with --fps-overlay-corner, now a control too). */
static void test_fps_overlay_key() {
    ProfileFile f, g;
    Profile p;
    p.fps_overlay = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].fps_overlay && warn.empty(), "show fps: ini round trip");
    CHECK(write_ini(f).find("fps_overlay = true") != std::string::npos, "show fps: saved as fps_overlay");
    CHECK(!Profile().fps_overlay, "show fps: off by default");
    CHECK(join(build_argv(p)).find("|--fps-overlay") != std::string::npos, "show fps: --fps-overlay when on");
    CHECK(join(build_argv(Profile())).find("fps-overlay") == std::string::npos, "show fps: nothing passed when off");
    for (Preset preset : {Preset::Laptop, Preset::Balanced, Preset::Gaming}) {
        Profile q = p;
        apply_preset(q, preset, "");
        CHECK(!q.fps_overlay, "show fps: off in the %s preset", preset_key(preset));
        q.fps_overlay = true;
        CHECK(detect_preset(q) == preset, "show fps: turning it on keeps the %s preset", preset_key(preset));
    }
    Profile m;
    m.extra_args = "--fps-overlay --fps-overlay-corner br";
    migrate_extra_args(m);
    CHECK(m.fps_overlay && m.fps_overlay_corner == "br" && m.extra_args.empty(), "show fps: moved out of extra_args with its corner (%s)",
          m.extra_args.c_str());
    m.extra_args = "--no-fps-overlay";
    migrate_extra_args(m);
    CHECK(!m.fps_overlay && m.extra_args.empty(), "show fps: --no-fps-overlay moved too");
    CHECK(std::string(tr(S::perf_overlay_show, Lang::Es)) == "Mostrar contador" && std::string(tr(S::perf_overlay_show, Lang::En)) == "Show counter",
          "show fps: the toggle's labels (Mostrar contador / Show counter)");
}

/* The on-screen counter's items and corner: saved as fps_overlay_items
 * and fps_overlay_corner; an older profile with the counter on and no items keeps FPS + ms (plain
 * --fps-overlay); turning it on from the launcher picks FPS only. The one-time question
 * (perf_overlay_asked) is offered once to a profile without the counter. */
static void test_perf_overlay() {
    Profile p;
    CHECK(p.fps_overlay_items.empty() && p.fps_overlay_corner.empty() && fps_overlay_mask(p) == FPS_OVERLAY_FPS + FPS_OVERLAY_MS,
          "perf overlay: no items saved = fps and ms (as before)");
    enable_fps_overlay(p);
    CHECK(p.fps_overlay && p.fps_overlay_items == "fps" && fps_overlay_mask(p) == FPS_OVERLAY_FPS, "perf overlay: turned on = FPS only");
    std::string a = join(build_argv(p));
    CHECK((a + "|").find("|--fps-overlay|--fps-overlay-items|fps|") != std::string::npos, "perf overlay: FPS only passes --fps-overlay-items fps (%s)",
          a.c_str());
    set_fps_overlay_mask(p, FPS_OVERLAY_FPS | FPS_OVERLAY_MS | FPS_OVERLAY_CPU | FPS_OVERLAY_GPU);
    CHECK(p.fps_overlay_items == "fps,ms,cpu,gpu", "perf overlay: all four saved in order (%s)", p.fps_overlay_items.c_str());
    CHECK(join(build_argv(p)).find("|--fps-overlay-items|fps,ms,cpu,gpu") != std::string::npos, "perf overlay: all four passed");
    set_fps_overlay_mask(p, FPS_OVERLAY_GPU | FPS_OVERLAY_MS);
    CHECK(p.fps_overlay_items == "ms,gpu" && join(build_argv(p)).find("|--fps-overlay-items|ms,gpu") != std::string::npos,
          "perf overlay: ms + gpu (%s)", p.fps_overlay_items.c_str());
    set_fps_overlay_mask(p, FPS_OVERLAY_FPS | FPS_OVERLAY_MS);
    CHECK(join(build_argv(p)).find("fps-overlay-items") == std::string::npos, "perf overlay: fps + ms is the runtime's default: no items flag");
    Profile legacy;
    legacy.fps_overlay = true;
    a = join(build_argv(legacy));
    CHECK(a.find("|--fps-overlay") != std::string::npos && a.find("fps-overlay-items") == std::string::npos &&
              a.find("fps-overlay-corner") == std::string::npos,
          "perf overlay: an older profile with Show FPS on passes --fps-overlay alone, as before");
    enable_fps_overlay(legacy);
    CHECK(legacy.fps_overlay_items.empty(), "perf overlay: turning it on again keeps an older profile's items");
    p.fps_overlay_corner = "br";
    CHECK(join(build_argv(p)).find("|--fps-overlay-corner|br") != std::string::npos, "perf overlay: corner passed");
    p.fps_overlay_corner = "tl";
    CHECK(join(build_argv(p)).find("fps-overlay-corner") == std::string::npos, "perf overlay: top left is the default: no corner flag");
    Profile off = p;
    off.fps_overlay = false;
    off.fps_overlay_corner = "br";
    CHECK(join(build_argv(off)).find("fps-overlay") == std::string::npos, "perf overlay: off passes nothing (items and corner kept)");
    /* round trip */
    ProfileFile f, g;
    p.fps_overlay_items = "fps,cpu";
    p.fps_overlay_corner = "br";
    f.profiles.push_back(p);
    CHECK(write_ini(f).find("perf_overlay_asked") == std::string::npos, "perf overlay: not asked yet = not written");
    f.settings.perf_overlay_asked = true;
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].fps_overlay_items == "fps,cpu" && g.profiles[0].fps_overlay_corner == "br" &&
              g.settings.perf_overlay_asked && warn.empty(),
          "perf overlay: items, corner and the question's answer round trip");
    for (Preset preset : {Preset::Laptop, Preset::Balanced, Preset::Gaming}) {
        Profile q = p;
        apply_preset(q, preset, "");
        CHECK(!q.fps_overlay, "perf overlay: off in the %s preset", preset_key(preset));
    }
    /* typed options move */
    Profile m;
    m.extra_args = "--fps-overlay --fps-overlay-items cpu,gpu --fps-overlay-corner tr";
    migrate_extra_args(m);
    CHECK(m.fps_overlay && m.fps_overlay_items == "cpu,gpu" && m.fps_overlay_corner == "tr" && m.extra_args.empty(),
          "perf overlay: typed items and corner move (%s)", m.extra_args.c_str());
    m.extra_args = "--fps-overlay-items gpu,fps";
    migrate_extra_args(m);
    CHECK(m.fps_overlay_items == "fps,gpu" && m.extra_args.empty(), "perf overlay: typed items saved in the runtime's order");
    m.extra_args = "--fps-overlay-items vram --fps-overlay-corner middle";
    migrate_extra_args(m);
    CHECK(m.fps_overlay_items == "fps,gpu" && m.fps_overlay_corner == "tr" && m.extra_args == "--fps-overlay-items vram --fps-overlay-corner middle",
          "perf overlay: values the runtime rejects stay typed (%s)", m.extra_args.c_str());
    /* the one-time question */
    ProfileFile q;
    Profile none;
    CHECK(perf_overlay_offer(q, none), "perf overlay: offered to a profile without the counter");
    Profile with;
    with.fps_overlay = true;
    CHECK(!perf_overlay_offer(q, with), "perf overlay: not offered when the counter is already on");
    q.settings.perf_overlay_asked = true;
    CHECK(!perf_overlay_offer(q, none), "perf overlay: asked once only");
    CHECK(std::string(tr(S::perf_overlay_section, Lang::Es)) == "Contador en pantalla" && std::string(tr(S::perf_overlay_show, Lang::Es)) == "Mostrar contador" &&
              std::string(tr(S::perf_overlay_fps, Lang::Es)) == "FPS" &&
              std::string(tr(S::perf_overlay_ms, Lang::Es)) == "Milisegundos por cuadro (16,6 ms)" &&
              std::string(tr(S::perf_overlay_cpu, Lang::Es)) == "Uso de CPU (%)" && std::string(tr(S::perf_overlay_gpu, Lang::Es)) == "Uso de GPU (%)" &&
              std::string(tr(S::perf_overlay_question, Lang::Es)) == "¿Quieres ver un contador de FPS en pantalla?" &&
              std::string(tr(S::perf_overlay_new, Lang::Es)) == "Nuevo: contador de rendimiento en pantalla" &&
              std::string(tr(S::perf_overlay_configure, Lang::Es)) == "Configurar" && std::string(tr(S::perf_overlay_no_thanks, Lang::Es)) == "No, gracias",
          "perf overlay: the Spanish labels");
    CHECK(std::string(tr(S::perf_overlay_section, Lang::En)) == "On-screen counter" && std::string(tr(S::perf_overlay_cpu, Lang::En)) == "CPU usage (%)",
          "perf overlay: the English labels");
}

/* Sharp faces over the texture pack (--sharp-faces-over-pack): saved as sharp_faces_over_pack, off by
 * default and in every preset (applying one turns it off; it is not part of preset matching), offered
 * and passed only with sharp faces on and a texture pack chosen; typed in extra_args it moves. */
static void test_sharp_faces_over_pack_key() {
    ProfileFile f, g;
    Profile p;
    p.sharp_faces = true;
    p.texture_pack = "packs/hd";
    p.sharp_faces_over_pack = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].sharp_faces_over_pack && warn.empty(), "faces over pack: ini round trip");
    CHECK(!Profile().sharp_faces_over_pack, "faces over pack: off by default");
    CHECK(sharp_faces_over_pack_offered(p), "faces over pack: offered with sharp faces and a texture pack");
    Profile no_pack = p, no_faces = p;
    no_pack.texture_pack = "";
    no_faces.sharp_faces = false;
    CHECK(!sharp_faces_over_pack_offered(no_pack) && !sharp_faces_over_pack_offered(no_faces),
          "faces over pack: hidden without a texture pack or without sharp faces");
    const std::string flag = "|--sharp-faces-over-pack";
    CHECK(join(build_argv(p)).find(flag) != std::string::npos, "faces over pack: flag passed when on and offered");
    CHECK(join(build_argv(no_pack)).find(flag) == std::string::npos && join(build_argv(no_faces)).find(flag) == std::string::npos,
          "faces over pack: no flag when the option is hidden");
    Profile off = p;
    off.sharp_faces_over_pack = false;
    CHECK(join(build_argv(off)).find("over-pack") == std::string::npos, "faces over pack: nothing passed when off");
    for (Preset preset : {Preset::Laptop, Preset::Balanced, Preset::Gaming}) {
        Profile q = p;
        apply_preset(q, preset, "");
        CHECK(!q.sharp_faces_over_pack, "faces over pack: off in the %s preset", preset_key(preset));
        q.sharp_faces_over_pack = true;
        CHECK(detect_preset(q) == preset, "faces over pack: turning it on keeps the %s preset", preset_key(preset));
    }
    Profile m;
    m.extra_args = "--sharp-faces-over-pack --ssao-radius 1";
    migrate_extra_args(m);
    CHECK(m.sharp_faces_over_pack && m.extra_args == "--ssao-radius 1", "faces over pack: moved out of extra_args (%s)",
          m.extra_args.c_str());
    m.extra_args = "--no-sharp-faces-over-pack";
    migrate_extra_args(m);
    CHECK(!m.sharp_faces_over_pack && m.extra_args.empty(), "faces over pack: --no-sharp-faces-over-pack moved too");
    CHECK(std::string(tr(S::sharp_faces_over_pack, Lang::En)) == "Sharp faces over texture pack" &&
              std::string(tr(S::sharp_faces_over_pack, Lang::Es)) == "Caras nítidas por encima del pack de texturas",
          "faces over pack: English and Spanish labels");
    CHECK(std::string(tr(S::sharp_faces_over_pack_desc, Lang::En)) != "" &&
              std::string(tr(S::sharp_faces_over_pack_desc, Lang::Es)) != "",
          "faces over pack: descriptions");
}

static void test_sky_hd_key() {
    ProfileFile f, g;
    Profile p;
    p.sky_hd = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].sky_hd && warn.empty(), "enhanced sky: ini round trip");
    CHECK(!Profile().sky_hd, "enhanced sky: off by default");
    CHECK(join(build_argv(p)).find("--sky-hd") != std::string::npos && join(build_argv(Profile())).find("--sky-hd") == std::string::npos,
          "enhanced sky: --sky-hd only when on");
    Profile m;
    m.extra_args = "--sky-hd --ssao-radius 1";
    migrate_extra_args(m);
    CHECK(m.sky_hd && m.extra_args == "--ssao-radius 1", "enhanced sky: moved out of extra_args (%s)", m.extra_args.c_str());
    m.extra_args = "--no-sky-hd";
    migrate_extra_args(m);
    CHECK(!m.sky_hd && m.extra_args.empty(), "enhanced sky: --no-sky-hd moved too");
}

/* Soft particles are opt-in (Display, off in every preset), saved as soft_particles_opt_in. The
 * soft_particles key written before v0.1.4 (when the Quality preset turned them on and they faded
 * graffiti tags) stays retired: such a profile loads with them off, quietly, keeps its preset and
 * the old key is not written back. --soft-particles / --no-soft-particles typed in extra_args move
 * into the option (the distance stays typed). */
static bool has_soft_particles(const Profile &p) {
    for (const std::string &a : build_argv(p))
        if (a == "--soft-particles") return true;
    return false;
}

static void test_soft_particles_key() {
    ProfileFile f, g;
    Profile p;
    p.soft_particles = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].soft_particles && warn.empty(), "soft particles: ini round trip");
    CHECK(write_ini(f).find("soft_particles_opt_in = true") != std::string::npos, "soft particles: saved as soft_particles_opt_in");
    CHECK(!Profile().soft_particles && !has_soft_particles(Profile()), "soft particles: off by default");
    CHECK(has_soft_particles(p), "soft particles: --soft-particles when on");

    ProfileFile o, h;
    Profile q;
    apply_preset(q, Preset::Gaming, "");
    q.name = "q";
    o.profiles.push_back(q);
    std::string ini = write_ini(o) + "soft_particles = true\n";
    warn.clear();
    CHECK(parse_ini(ini, h, &warn) && h.profiles.size() == 1, "soft particles: a profile with the pre-v0.1.4 key parses");
    if (h.profiles.size() != 1) return;
    CHECK(warn.empty(), "soft particles: the retired key gives no warning (%zu)", warn.size());
    CHECK(!has_soft_particles(h.profiles[0]), "soft particles: the pre-v0.1.4 key loads with them off (%s)", opts(h.profiles[0]).c_str());
    CHECK(h.profiles[0].preset == "gaming", "soft particles: the saved Quality profile is still Quality (%s)", h.profiles[0].preset.c_str());
    CHECK(write_ini(h).find("soft_particles =") == std::string::npos, "soft particles: the retired key is not written back");

    Profile m;
    m.extra_args = "--soft-particles --soft-particles-distance 1";
    migrate_extra_args(m);
    CHECK(m.soft_particles && m.extra_args == "--soft-particles-distance 1",
          "soft particles: moved out of extra_args, the distance stays typed (%s)", m.extra_args.c_str());
    m.extra_args = "--no-soft-particles";
    migrate_extra_args(m);
    CHECK(!m.soft_particles && m.extra_args.empty(), "soft particles: --no-soft-particles moved too");
    CHECK(std::string(tr(S::soft_particles, Lang::Es)) == "Partículas suaves", "soft particles: the toggle's label in Spanish");
}

/* Sharp text (--sharp-fonts, on in the runtime by default and in every preset): only
 * --no-sharp-fonts is written, when it is off; a profile saved before the option existed loads with
 * it on and keeps its preset; turning it off makes any preset Custom. */
/* Positioned (3D) sound: the Sound card's three toggles. */
static void test_audio_3d_keys() {
    Profile d;
    CHECK(!d.audio_hrtf && d.audio_air && d.audio_occlusion && d.audio_reverb_geo && d.audio_pathing,
          "3D sound: binaural off, air absorption, occlusion, location-based reverb and sound around corners on by default");
    std::string def = join(build_argv(d));
    CHECK(def.find("audio-") == std::string::npos, "3D sound: nothing written at the defaults (%s)", def.c_str());
    Profile p;
    p.audio_hrtf = true;
    p.audio_air = false;
    p.audio_occlusion = false;
    p.audio_reverb_geo = false;
    p.audio_pathing = false;
    ProfileFile f, g;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].audio_hrtf && !g.profiles[0].audio_air && !g.profiles[0].audio_occlusion && !g.profiles[0].audio_reverb_geo && !g.profiles[0].audio_pathing && warn.empty(),
          "3D sound: ini round trip");
    for (Preset pr : {Preset::Laptop, Preset::Balanced, Preset::Gaming}) {
        Profile q = p;
        apply_preset(q, pr, "/inst");
        CHECK(q.audio_hrtf && !q.audio_air && !q.audio_occlusion && !q.audio_reverb_geo && !q.audio_pathing && detect_preset(q) == pr, "3D sound: %s keeps the player's choice and still matches",
              preset_key(pr));
    }
    /* a profile written before the options: no audio_* keys */
    ProfileFile of;
    of.profiles.push_back(recommended_profile("/inst", Preset::Gaming));
    std::string ini = write_ini(of);
    for (const char *line : {"audio_hrtf = false\n", "audio_air = true\n", "audio_occlusion = true\n", "audio_reverb_geo = true\n", "audio_pathing = true\n"}) {
        size_t at = ini.find(line);
        CHECK(at != std::string::npos, "3D sound: %.*s written", (int)strlen(line) - 1, line);
        if (at != std::string::npos) ini.erase(at, strlen(line));
    }
    ProfileFile og;
    parse_ini(ini, og, nullptr);
    CHECK(og.profiles.size() == 1 && !og.profiles[0].audio_hrtf && og.profiles[0].audio_air && og.profiles[0].audio_occlusion && og.profiles[0].audio_reverb_geo &&
              og.profiles[0].audio_pathing && og.profiles[0].preset == "gaming",
          "3D sound: an older profile loads with the defaults and stays Quality");
    Profile m;
    m.extra_args = "--audio-hrtf --no-audio-air --ssao-radius 1 --no-audio-occlusion --no-audio-reverb-geo --no-audio-pathing";
    migrate_extra_args(m);
    CHECK(m.audio_hrtf && !m.audio_air && !m.audio_occlusion && !m.audio_reverb_geo && !m.audio_pathing && m.extra_args == "--ssao-radius 1", "3D sound: flags moved out of extra_args (%s)",
          m.extra_args.c_str());
    CHECK(std::string(tr(S::audio_hrtf, Lang::Es)) == "Auriculares (sonido 3D binaural)" &&
              std::string(tr(S::audio_hrtf, Lang::En)) == "Headphones (binaural 3D sound)" && std::string(tr(S::audio_occlusion, Lang::Es)) != "" &&
              std::string(tr(S::audio_air, Lang::Es)) != "" && std::string(tr(S::audio_hrtf_desc, Lang::Es)) != "" &&
              std::string(tr(S::audio_reverb_geo, Lang::Es)) == "Reverberación según el lugar" &&
              std::string(tr(S::audio_reverb_geo, Lang::En)) == "Location-based reverb" && std::string(tr(S::audio_reverb_geo_desc, Lang::Es)) != "" &&
              std::string(tr(S::audio_pathing, Lang::Es)) == "Sonido que rodea esquinas" &&
              std::string(tr(S::audio_pathing, Lang::En)) == "Sound around corners" && std::string(tr(S::audio_pathing_desc, Lang::Es)) != "",
          "3D sound: the toggles' labels in Spanish and English");
}

static void test_sharp_fonts_key() {
    CHECK(Profile().sharp_fonts, "sharp text: on by default");
    std::string def = join(build_argv(Profile()));
    CHECK(def.find("sharp-fonts") == std::string::npos, "sharp text: nothing written while on (%s)", def.c_str());
    Profile p;
    p.sharp_fonts = false;
    CHECK(join(build_argv(p)).find("--no-sharp-fonts") != std::string::npos, "sharp text: --no-sharp-fonts when off");
    ProfileFile f, g;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && !g.profiles[0].sharp_fonts && warn.empty(), "sharp text: ini round trip (off)");
    for (Preset pr : {Preset::Laptop, Preset::Balanced, Preset::Gaming}) {
        Profile q;
        q.sharp_fonts = false;
        apply_preset(q, pr, "/inst");
        CHECK(q.sharp_fonts && detect_preset(q) == pr, "sharp text: %s turns it on", preset_key(pr));
        q.sharp_fonts = false;
        CHECK(detect_preset(q) == Preset::Custom, "sharp text: %s without it is custom", preset_key(pr));
    }
    /* a Quality profile written before the option: no sharp_fonts key */
    Profile old = recommended_profile("/inst", Preset::Gaming);
    ProfileFile of;
    of.profiles.push_back(old);
    std::string ini = write_ini(of), line = "sharp_fonts = true\n";
    size_t at = ini.find(line);
    CHECK(at != std::string::npos, "sharp text: the key is written");
    if (at != std::string::npos) ini.erase(at, line.size());
    ProfileFile og;
    parse_ini(ini, og, nullptr);
    CHECK(og.profiles.size() == 1 && og.profiles[0].sharp_fonts && og.profiles[0].preset == "gaming",
          "sharp text: an older Quality profile loads with it on and stays Quality");
    Profile m;
    m.extra_args = "--no-sharp-fonts --ssao-radius 1";
    migrate_extra_args(m);
    CHECK(!m.sharp_fonts && m.extra_args == "--ssao-radius 1", "sharp text: --no-sharp-fonts moved out of extra_args (%s)",
          m.extra_args.c_str());
    m.extra_args = "--sharp-fonts";
    migrate_extra_args(m);
    CHECK(m.sharp_fonts && m.extra_args.empty(), "sharp text: --sharp-fonts moved too");
    CHECK(std::string(tr(S::sharp_fonts, Lang::Es)) == "Texto nítido" && std::string(tr(S::sharp_fonts, Lang::En)) == "Sharp text",
          "sharp text: the toggle's label in Spanish and English");
}

/* Skip intro videos (--skip-intro, off in the runtime and in a plain Profile, on in every preset,
 * so in every new profile): --skip-intro is written while on, nothing when off; a profile saved
 * before the option existed loads with it on; it is a Game option, so turning it off keeps the
 * graphics preset; typed in extra_args it moves into the option. */
static void test_skip_intro_key() {
    CHECK(!Profile().skip_intro && recommended_profile("/inst", Preset::Laptop).skip_intro,
          "skip intro: off in a plain Profile, on in a new profile");
    Profile p;
    p.skip_intro = true;
    CHECK(join(build_argv(p)).find("|--skip-intro") != std::string::npos, "skip intro: --skip-intro while on");
    p.skip_intro = false;
    CHECK(join(build_argv(p)).find("skip-intro") == std::string::npos, "skip intro: nothing written when off");
    ProfileFile f, g;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && !g.profiles[0].skip_intro && warn.empty(), "skip intro: ini round trip (off)");
    for (Preset pr : {Preset::Laptop, Preset::Balanced, Preset::Gaming}) {
        Profile q;
        q.skip_intro = false;
        apply_preset(q, pr, "/inst");
        CHECK(q.skip_intro && detect_preset(q) == pr, "skip intro: %s turns it on", preset_key(pr));
        q.skip_intro = false;
        CHECK(detect_preset(q) == pr, "skip intro: %s without it keeps the preset", preset_key(pr));
    }
    /* a Balanced profile written before the option: no skip_intro key */
    Profile old = recommended_profile("/inst", Preset::Balanced);
    ProfileFile of;
    of.profiles.push_back(old);
    std::string ini = write_ini(of), line = "skip_intro = true\n";
    size_t at = ini.find(line);
    CHECK(at != std::string::npos, "skip intro: the key is written");
    if (at != std::string::npos) ini.erase(at, line.size());
    ProfileFile og;
    parse_ini(ini, og, nullptr);
    CHECK(og.profiles.size() == 1 && og.profiles[0].skip_intro && og.profiles[0].preset == "balanced",
          "skip intro: an older profile loads with it on and keeps its preset");
    Profile m;
    m.extra_args = "--no-skip-intro --ssao-radius 1";
    migrate_extra_args(m);
    CHECK(!m.skip_intro && m.extra_args == "--ssao-radius 1", "skip intro: --no-skip-intro moved out of extra_args (%s)",
          m.extra_args.c_str());
    m.extra_args = "--skip-intro";
    migrate_extra_args(m);
    CHECK(m.skip_intro && m.extra_args.empty(), "skip intro: --skip-intro moved too");
    CHECK(std::string(tr(S::skip_intro, Lang::Es)) == "Saltar videos de inicio" &&
              std::string(tr(S::skip_intro, Lang::En)) == "Skip intro videos" && std::string(tr(S::skip_intro_desc)) != "",
          "skip intro: the toggle's label in Spanish and English");
}

static void test_filmic_key() {
    ProfileFile f, g;
    Profile p;
    p.filmic = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].filmic && warn.empty(), "filmic: ini round trip");
    CHECK(!Profile().filmic, "filmic: off by default");
    Profile m;
    m.extra_args = "--filmic --filmic-strength 0.5";
    migrate_extra_args(m);
    CHECK(m.filmic && m.extra_args == "--filmic-strength 0.5", "filmic: moved out of extra_args, the strength stays typed (%s)",
          m.extra_args.c_str());
    m.extra_args = "--no-filmic";
    migrate_extra_args(m);
    CHECK(!m.filmic && m.extra_args.empty(), "filmic: --no-filmic moved too");
    CHECK(std::string(tr(S::filmic, Lang::Es)) == "Look cinematográfico" && std::string(tr(S::filmic, Lang::En)) == "Cinematic look",
          "filmic: the toggle's label in Spanish and English");
}

static void test_auto_exposure_key() {
    ProfileFile f, g;
    Profile p;
    p.auto_exposure = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].auto_exposure && warn.empty(), "auto exposure: ini round trip");
    CHECK(!Profile().auto_exposure, "auto exposure: off by default");
    Profile m;
    m.extra_args = "--auto-exposure --auto-exposure-strength 0.5";
    migrate_extra_args(m);
    CHECK(m.auto_exposure && m.extra_args == "--auto-exposure-strength 0.5", "auto exposure: moved out of extra_args, the strength stays typed (%s)",
          m.extra_args.c_str());
    m.extra_args = "--no-auto-exposure";
    migrate_extra_args(m);
    CHECK(!m.auto_exposure && m.extra_args.empty(), "auto exposure: --no-auto-exposure moved too");
    CHECK(std::string(tr(S::auto_exposure, Lang::Es)) == "Adaptación de la vista" && std::string(tr(S::auto_exposure, Lang::En)) == "Eye adaptation" &&
              std::string(tr(S::auto_exposure_desc, Lang::Es)) != "",
          "auto exposure: the toggle's label in Spanish and English");
    for (Preset preset : {Preset::Gaming, Preset::Balanced, Preset::Laptop}) {
        Profile q;
        apply_preset(q, preset, "");
        CHECK(q.auto_exposure == (preset != Preset::Laptop) && q.auto_exposure == q.filmic,
              "auto exposure: on with the filmic look in preset %d (Performance off)", (int)preset);
        q.auto_exposure = !q.auto_exposure;
        CHECK(detect_preset(q) == Preset::Custom, "auto exposure: toggled in preset %d is custom", (int)preset);
    }
}

/* --anim-interp: the key round-trips, is off by default, moves out of
 * extra_args, its toggle's labels, its argv after --sim60, and the presets (see apply_preset). */
#define ANIM_PRESET_WANT(p) false
static void test_anim_interp_key() {
    ProfileFile f, g;
    Profile p;
    p.anim_interp = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].anim_interp && warn.empty(), "anim_interp: ini round trip");
    CHECK(!Profile().anim_interp, "anim_interp: off by default");
    Profile m;
    m.extra_args = "--anim-interp --frames 9";
    migrate_extra_args(m);
    CHECK(m.anim_interp && m.extra_args == "--frames 9", "anim_interp: moved out of extra_args, the rest stays (%s)", m.extra_args.c_str());
    m.extra_args = "--no-anim-interp";
    migrate_extra_args(m);
    CHECK(!m.anim_interp && m.extra_args.empty(), "anim_interp: --no-anim-interp moved too");
    CHECK(std::string(tr(S::anim_interp, Lang::Es)) == "Animaciones suaves" && std::string(tr(S::anim_interp, Lang::En)) == "Smooth animation" &&
              std::string(tr(S::anim_interp_desc, Lang::Es)) != "" && std::string(tr(S::anim_interp_desc, Lang::En)) != "",
          "anim_interp: the toggle's label in Spanish and English");
    Profile q;
    q.sim60 = true;
    q.anim_interp = true;
    std::string joined;
    for (const std::string &a : build_argv(q)) joined += a + "|";
    CHECK(joined.find("--sim60|--anim-interp|") != std::string::npos, "anim_interp: --anim-interp after --sim60 (%s)", joined.c_str());
    for (Preset preset : {Preset::Gaming, Preset::Balanced, Preset::Laptop}) {
        Profile r;
        apply_preset(r, preset, "");
        CHECK(r.anim_interp == ANIM_PRESET_WANT(preset), "anim_interp: preset %d (%d)", (int)preset, r.anim_interp);
        r.anim_interp = !r.anim_interp;
        CHECK(detect_preset(r) == Preset::Custom, "anim_interp: toggled in preset %d is custom", (int)preset);
    }
}

/* --taa: the key round-trips, is off by default, moves out of extra_args (the
 * sharpness stays typed), its toggle's labels, and the presets (see apply_preset). */
#define TAA_PRESET_WANT(p) ((p) != Preset::Laptop)
#define TAA_PRESET_MSAA(p) ((p) == Preset::Gaming ? 2 : 0)
static void test_taa_key() {
    ProfileFile f, g;
    Profile p;
    p.taa = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].taa && warn.empty(), "taa: ini round trip");
    CHECK(!Profile().taa, "taa: off by default");
    Profile m;
    m.extra_args = "--taa --taa-sharpness 0.5";
    migrate_extra_args(m);
    CHECK(m.taa && m.taa_sharpness == 0.5f && m.extra_args.empty(), "taa: moved out of extra_args with its sharpness (%s)", m.extra_args.c_str());
    m.extra_args = "--no-taa";
    migrate_extra_args(m);
    CHECK(!m.taa && m.extra_args.empty(), "taa: --no-taa moved too");
    CHECK(std::string(tr(S::taa, Lang::Es)) == "Antialiasing temporal (TAA)" && std::string(tr(S::taa, Lang::En)) == "Temporal anti-aliasing (TAA)" &&
              std::string(tr(S::taa_desc, Lang::Es)) != "" && std::string(tr(S::taa_desc, Lang::En)) != "",
          "taa: the toggle's label in Spanish and English");
    for (Preset preset : {Preset::Gaming, Preset::Balanced, Preset::Laptop}) {
        Profile q;
        apply_preset(q, preset, "");
        CHECK(q.taa == TAA_PRESET_WANT(preset) && q.msaa == TAA_PRESET_MSAA(preset), "taa: preset %d (taa %d, msaa %d)", (int)preset, q.taa, q.msaa);
        q.taa = !q.taa;
        CHECK(detect_preset(q) == Preset::Custom, "taa: toggled in preset %d is custom", (int)preset);
    }
}

/* --frame-gen: the key round-trips, is off by default and in every preset (a
 * preset leaves it as it is: it depends on the screen, not the card), moves out of extra_args, and
 * its toggle's labels in Spanish and English. */
static void test_frame_gen_key() {
    ProfileFile f, g;
    Profile p;
    p.frame_gen = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].frame_gen && warn.empty(), "frame_gen: ini round trip");
    CHECK(!Profile().frame_gen, "frame_gen: off by default");
    Profile m;
    m.extra_args = "--frame-gen --stats";
    migrate_extra_args(m);
    CHECK(m.frame_gen && m.extra_args == "--stats", "frame_gen: moved out of extra_args (%s)", m.extra_args.c_str());
    m.extra_args = "--no-frame-gen";
    migrate_extra_args(m);
    CHECK(!m.frame_gen && m.extra_args.empty(), "frame_gen: --no-frame-gen moved too");
    CHECK(std::string(tr(S::frame_gen, Lang::Es)) == "Generación de fotogramas (120 Hz)" &&
              std::string(tr(S::frame_gen, Lang::En)) == "Frame generation (120 Hz)" && std::string(tr(S::frame_gen_desc, Lang::Es)) != "" &&
              std::string(tr(S::frame_gen_desc, Lang::En)) != "",
          "frame_gen: the toggle's label in Spanish and English");
    for (Preset preset : {Preset::Gaming, Preset::Balanced, Preset::Laptop, Preset::Ultra}) {
        Profile q;
        apply_preset(q, preset, "");
        CHECK(!q.frame_gen, "frame_gen: off in preset %d", (int)preset);
        q.frame_gen = true;
        apply_preset(q, preset, "");
        CHECK(q.frame_gen && detect_preset(q) == preset, "frame_gen: a preset keeps it and is still detected (preset %d)", (int)preset);
    }
}

/* --taa-upscale: the key round-trips, is off by default, moves out of extra_args
 * (an unknown mode stays typed), and its row's labels in Spanish and English. */
static void test_taa_upscale_key() {
    ProfileFile f, g;
    Profile p;
    p.taa_upscale = "balanced";
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].taa_upscale == "balanced" && warn.empty(), "taa upscale: ini round trip");
    CHECK(Profile().taa_upscale == "off", "taa upscale: off by default");
    Profile m;
    m.extra_args = "--taa-upscale performance --sharpen 20x";
    migrate_extra_args(m);
    CHECK(m.taa_upscale == "performance" && m.extra_args == "--sharpen 20x", "taa upscale: moved out of extra_args (%s, %s)",
          m.taa_upscale.c_str(), m.extra_args.c_str());
    m.extra_args = "--taa-upscale ultra";
    migrate_extra_args(m);
    CHECK(m.taa_upscale == "ultra" && m.extra_args.empty(), "taa upscale: ultra moved out of extra_args (%s)", m.extra_args.c_str());
    m.extra_args = "--taa-upscale 1.4";
    migrate_extra_args(m);
    CHECK(m.taa_upscale == "ultra" && m.extra_args == "--taa-upscale 1.4", "taa upscale: a typed ratio stays typed (%s)", m.extra_args.c_str());
    CHECK(std::string(tr(S::taa_upscale, Lang::Es)) == "Escalado temporal (TAA)" && std::string(tr(S::taa_upscale, Lang::En)) == "Temporal upscaling (TAA)" &&
              std::string(tr(S::taa_upscale_desc, Lang::Es)) != "" && std::string(tr(S::taa_upscale_desc, Lang::En)) != "",
          "taa upscale: the row's label in Spanish and English");
}

/* --taa-upscale native: the "Native (AA)" pill's value round-trips, moves out
 * of extra_args, reaches the argv with either upscaler and with dynamic resolution, picking an
 * upscaler keeps it (it is temporal upscaling's ceiling, not off), and its label in both languages. */
static void test_taa_upscale_native() {
    ProfileFile f, g;
    Profile p;
    p.taa_upscale = "native";
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].taa_upscale == "native" && warn.empty(), "taa upscale native: ini round trip");
    Profile m;
    m.extra_args = "--taa-upscale native --sharpen 20x";
    migrate_extra_args(m);
    CHECK(m.taa_upscale == "native" && m.extra_args == "--sharpen 20x", "taa upscale native: moved out of extra_args (%s, %s)", m.taa_upscale.c_str(),
          m.extra_args.c_str());
    Profile q;
    choose_fsr(q, "quality");
    choose_taa_upscale(q, "native");
    q.dynres = true;
    CHECK(q.fsr == "off" && join(build_argv(q)).find("--taa-upscale|native|") != std::string::npos &&
              join(build_argv(q)).find("--dynres|auto") != std::string::npos && join(build_argv(q)).find("--upscaler") == std::string::npos,
          "taa upscale native: replaces FSR 1, reaches the argv with dynamic resolution (%s)", join(build_argv(q)).c_str());
    choose_upscaler(q, "fsr3");
    CHECK(q.taa_upscale == "native" && std::string(upscaler_shown(q)) == "fsr3" &&
              join(build_argv(q)).find("--taa-upscale|native|--upscaler|fsr3") != std::string::npos,
          "taa upscale native: AMD FSR 3.1 keeps native (FSR 3.1 Native AA; %s)", join(build_argv(q)).c_str());
    CHECK(std::string(tr(S::taa_upscale_native, Lang::Es)) == "Nativo (AA)" && std::string(tr(S::taa_upscale_native, Lang::En)) == "Native (AA)",
          "taa upscale native: the pill's label in Spanish and English");
}

/* --upscaler: the key round-trips, is taa by default, is passed only as
 * fsr3 and only with temporal upscaling, moves out of extra_args (an unknown value stays typed), a
 * preset with it is Custom, and its row's labels in Spanish and English. */
static void test_upscaler_key() {
    ProfileFile f, g;
    Profile p;
    p.upscaler = "fsr3";
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].upscaler == "fsr3" && warn.empty(), "upscaler: ini round trip");
    CHECK(Profile().upscaler == "taa", "upscaler: taa by default");
    Profile q;
    q.taa_upscale = "quality";
    CHECK(join(build_argv(q)).find("--upscaler") == std::string::npos, "upscaler: taa is not passed (%s)", join(build_argv(q)).c_str());
    q.upscaler = "fsr3";
    CHECK(join(build_argv(q)).find("--taa-upscale|quality|--upscaler|fsr3") != std::string::npos, "upscaler: fsr3 passed after --taa-upscale (%s)",
          join(build_argv(q)).c_str());
    q.taa_upscale = "off";
    CHECK(join(build_argv(q)).find("--upscaler") == std::string::npos, "upscaler: not passed without temporal upscaling");
    Profile m;
    m.extra_args = "--upscaler fsr3 --sharpen 20x";
    migrate_extra_args(m);
    CHECK(m.upscaler == "fsr3" && m.extra_args == "--sharpen 20x", "upscaler: moved out of extra_args (%s, %s)", m.upscaler.c_str(), m.extra_args.c_str());
    m.extra_args = "--upscaler dlss";
    migrate_extra_args(m);
    CHECK(m.upscaler == "fsr3" && m.extra_args == "--upscaler dlss", "upscaler: an unknown value stays typed (%s)", m.extra_args.c_str());
    Profile r;
    apply_preset(r, Preset::Gaming, "");
    r.upscaler = "fsr3";
    CHECK(detect_preset(r) == Preset::Custom, "upscaler: fsr3 on a preset is Custom");
    CHECK(std::string(tr(S::upscaler, Lang::Es)) == "Escalado temporal" && std::string(tr(S::upscaler, Lang::En)) == "Temporal upscaler" &&
              std::string(tr(S::upscaler_taa, Lang::Es)) == "Warriors (TAA)" && std::string(tr(S::upscaler_fsr3, Lang::En)) == "AMD FSR 3.1" &&
              std::string(tr(S::upscaler_desc, Lang::Es)) != "" && std::string(tr(S::upscaler_desc, Lang::En)) != "",
          "upscaler: the row's labels in Spanish and English");
}

/* Display > Image's upscaler pills (user report 2026-11-19: "la opcion de FSR 3.1 desaparece ...
 * activas TAA eliges algo de TAA y no vuelve a aparecer nada FSR3.1"): one upscaler at a time, and
 * AMD FSR 3.1 is always offered. Picking it from any state ends in --upscaler fsr3 with temporal
 * upscaling on; FSR 1 hides only the pill's selection, not the choice. */
static void test_upscaler_pills() {
    Profile d; /* the default profile: temporal upscaling off, no pill shown selected */
    CHECK(std::string(upscaler_shown(d)).empty(), "upscaler pills: none selected while temporal upscaling is off");
    choose_upscaler(d, "fsr3");
    CHECK(d.taa_upscale == "quality" && d.upscaler == "fsr3" && d.fsr == "off" && std::string(upscaler_shown(d)) == "fsr3",
          "upscaler pills: FSR 3.1 from off turns temporal upscaling on at quality (%s, %s)", d.taa_upscale.c_str(), d.upscaler.c_str());
    CHECK(join(build_argv(d)).find("--taa-upscale|quality|--upscaler|fsr3") != std::string::npos, "upscaler pills: fsr3 reaches the argv (%s)",
          join(build_argv(d)).c_str());

    Profile f; /* FSR 1 on */
    choose_fsr(f, "quality");
    CHECK(f.fsr == "quality" && f.taa_upscale == "off", "upscaler pills: FSR 1 picked");
    choose_upscaler(f, "fsr3");
    CHECK(f.fsr == "off" && f.taa_upscale == "quality" && f.upscaler == "fsr3", "upscaler pills: FSR 3.1 replaces FSR 1 (%s, %s)", f.fsr.c_str(),
          f.taa_upscale.c_str());
    const std::string fa = join(build_argv(f));
    CHECK(fa.find("--fsr") == std::string::npos && fa.find("--upscaler|fsr3") != std::string::npos, "upscaler pills: argv after FSR 1 (%s)", fa.c_str());

    Profile q; /* the Quality preset: ultra temporal upscaling with Warriors (TAA) */
    apply_preset(q, Preset::Gaming, "");
    CHECK(std::string(upscaler_shown(q)) == "taa", "upscaler pills: Quality shows Warriors (TAA)");
    choose_upscaler(q, "fsr3");
    CHECK(q.taa_upscale == "ultra" && join(build_argv(q)).find("--taa-upscale|ultra|--upscaler|fsr3") != std::string::npos,
          "upscaler pills: FSR 3.1 keeps Quality's ultra mode (%s)", join(build_argv(q)).c_str());
    choose_fsr(q, "balanced"); /* FSR 1 turns temporal upscaling off: FSR 3.1 is kept, not passed, not shown */
    CHECK(q.taa_upscale == "off" && q.upscaler == "fsr3" && std::string(upscaler_shown(q)).empty() &&
              join(build_argv(q)).find("--upscaler") == std::string::npos,
          "upscaler pills: FSR 1 after FSR 3.1 (%s)", join(build_argv(q)).c_str());
    choose_taa_upscale(q, "performance"); /* back to temporal upscaling: FSR 3.1 again */
    CHECK(q.fsr == "off" && std::string(upscaler_shown(q)) == "fsr3" &&
              join(build_argv(q)).find("--taa-upscale|performance|--upscaler|fsr3") != std::string::npos,
          "upscaler pills: temporal upscaling picked again brings FSR 3.1 back (%s)", join(build_argv(q)).c_str());
    choose_upscaler(q, "taa");
    CHECK(q.upscaler == "taa" && q.taa_upscale == "performance" && join(build_argv(q)).find("--upscaler") == std::string::npos,
          "upscaler pills: Warriors (TAA) keeps the mode");
    choose_taa_upscale(q, "off");
    CHECK(q.taa_upscale == "off" && q.fsr == "off", "upscaler pills: temporal upscaling off leaves FSR 1 off");
}

/* --vrs auto: the key round-trips, is off by default, is passed as --vrs auto,
 * moves out of extra_args (a typed quality / performance stays typed), the presets set it as decided
 * by measurement (on in Quality and Ultra, off in Performance and Balanced), flipping it is Custom, an old
 * profile saved as a preset takes the preset's value, and its row's labels in Spanish and English. */
#define VRS_PRESET_ON(pr) ((pr) == Preset::Gaming || (pr) == Preset::Ultra)
static void test_vrs_key() {
    CHECK(!Profile().vrs, "vrs: off by default");
    ProfileFile f, g;
    Profile p;
    p.vrs = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].vrs && warn.empty(), "vrs: ini round trip");
    Profile q;
    CHECK(opts(q).find("--vrs") == std::string::npos, "vrs: left out when off (%s)", opts(q).c_str());
    q.vrs = true;
    CHECK(opts(q).find("--vrs|auto") != std::string::npos, "vrs: passed as --vrs auto (%s)", opts(q).c_str());
    Profile m;
    m.extra_args = "--vrs auto --vrs performance";
    migrate_extra_args(m);
    CHECK(m.vrs && m.extra_args == "--vrs performance", "vrs: auto moved out of extra_args, a typed mode stays typed (%s)", m.extra_args.c_str());
    for (Preset pr : {Preset::Laptop, Preset::Balanced, Preset::Gaming, Preset::Ultra}) {
        Profile a;
        apply_preset(a, pr, "/nonexistent");
        CHECK(a.vrs == VRS_PRESET_ON(pr) && detect_preset(a) == pr, "vrs: preset %s sets it %s", preset_key(pr), a.vrs ? "on" : "off");
        a.vrs = !a.vrs;
        CHECK(detect_preset(a) == Preset::Custom, "vrs: flipping it in preset %s is Custom", preset_key(pr));
    }
    ProfileFile old;
    parse_ini("[profile old]\npreset = gaming\n", old, &warn);
    Profile gq;
    apply_preset(gq, Preset::Gaming, "");
    CHECK(old.profiles.size() == 1 && old.profiles[0].vrs == gq.vrs, "vrs: a Quality profile saved before the key takes the preset's value");
    CHECK(std::string(tr(S::vrs, Lang::Es)) == "Sombreado de tasa variable (VRS)" && std::string(tr(S::vrs, Lang::En)) == "Variable rate shading (VRS)" &&
              std::string(tr(S::vrs_desc, Lang::Es)) != "" && std::string(tr(S::vrs_desc, Lang::En)) != "",
          "vrs: the row's label in Spanish and English");
}

/* --dynres auto: the key round-trips, is off by default, is passed only
 * with temporal upscaling, moves out of extra_args, the presets that upscale turn it on (Ultra and
 * Performance at full size do not), flipping it is Custom, an old profile saved as a preset takes the
 * preset's value, and its row's labels; the benchmark's arguments and report name. */
static void test_dynres_key() {
    CHECK(!Profile().dynres, "dynres: off by default");
    ProfileFile f, g;
    Profile p;
    p.dynres = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].dynres && warn.empty(), "dynres: ini round trip");
    Profile q;
    q.dynres = true;
    CHECK(opts(q).find("--dynres") == std::string::npos, "dynres: left out without temporal upscaling (%s)", opts(q).c_str());
    q.taa_upscale = "quality";
    CHECK(opts(q).find("--taa-upscale|quality") != std::string::npos && opts(q).find("--dynres|auto") != std::string::npos,
          "dynres: passed as --dynres auto with temporal upscaling (%s)", opts(q).c_str());
    Profile m;
    m.extra_args = "--dynres auto --dynres 12";
    migrate_extra_args(m);
    CHECK(m.dynres && m.extra_args == "--dynres 12", "dynres: auto moved out of extra_args, a typed target stays typed (%s)", m.extra_args.c_str());
    for (Preset pr : {Preset::Laptop, Preset::Balanced, Preset::Gaming, Preset::Ultra}) {
        Profile a;
        apply_preset(a, pr, "/nonexistent");
        CHECK(a.dynres == (a.taa_upscale != "off") && detect_preset(a) == pr, "dynres: preset %s sets it %s (upscaling %s)", preset_key(pr),
              a.dynres ? "on" : "off", a.taa_upscale.c_str());
        a.dynres = !a.dynres;
        CHECK(detect_preset(a) == Preset::Custom, "dynres: flipping it in preset %s is Custom", preset_key(pr));
    }
    ProfileFile old;
    parse_ini("[profile old]\npreset = gaming\n", old, &warn);
    Profile gq;
    apply_preset(gq, Preset::Gaming, "");
    CHECK(old.profiles.size() == 1 && old.profiles[0].dynres == gq.dynres, "dynres: a Quality profile saved before the key takes the preset's value");
    CHECK(std::string(tr(S::dynres, Lang::Es)) == "Resolución dinámica (mantener 60 fps)" &&
              std::string(tr(S::dynres, Lang::En)) == "Dynamic resolution (hold 60 fps)" && std::string(tr(S::dynres_desc, Lang::Es)) != "",
          "dynres: the row's label in Spanish and English");
    std::vector<std::string> argv = {"xbrun", "--taa"};
    add_bench_args(argv, "logs/bench-a.txt");
    CHECK(argv.size() == 5 && argv[1] == "--taa" && argv[2] == "--bench" && argv[3] == "--bench-out" && argv[4] == "logs/bench-a.txt",
          "bench: --bench --bench-out FILE after the profile's arguments");
    CHECK(bench_report_name("My PC/1", "20261116-120000") == "bench-My_PC_1-20261116-120000.txt" && bench_report_name("", "x") == "bench-profile-x.txt",
          "bench: the report name (%s)", bench_report_name("My PC/1", "20261116-120000").c_str());
    CHECK(std::string(tr(S::bench_run, Lang::Es)) == "Probar rendimiento" && std::string(tr(S::bench_run, Lang::En)) == "Run benchmark",
          "bench: the button's label in Spanish and English");
}

/* --taa-sharpness: the key round-trips, is left out by default and without
 * TAA, moves out of extra_args, a preset clears it (a set value is Custom), and its row's labels. */
static void test_taa_sharpness_key() {
    CHECK(Profile().taa_sharpness < 0, "taa sharpness: left out by default");
    ProfileFile f, g;
    Profile p;
    p.taa_sharpness = 0.75f;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].taa_sharpness == 0.75f && warn.empty(), "taa sharpness: ini round trip");
    Profile q;
    q.taa_sharpness = 0.6f;
    CHECK(opts(q).find("--taa-sharpness") == std::string::npos, "taa sharpness: left out without TAA (%s)", opts(q).c_str());
    q.taa = true;
    CHECK(opts(q).find("--taa|") != std::string::npos && opts(q).find("--taa-sharpness|0.6") != std::string::npos, "taa sharpness: passed with TAA (%s)",
          opts(q).c_str());
    q.taa = false;
    q.taa_upscale = "ultra";
    CHECK(opts(q).find("--taa-upscale|ultra|--taa-sharpness|0.6") != std::string::npos, "taa sharpness: passed with temporal upscaling (%s)",
          opts(q).c_str());
    Profile m;
    m.extra_args = "--taa-sharpness 0.5 --taa-sharpness 2";
    migrate_extra_args(m);
    CHECK(m.taa_sharpness == 0.5f && m.extra_args == "--taa-sharpness 2", "taa sharpness: moved out of extra_args, an out-of-range value stays typed (%.2f, %s)",
          m.taa_sharpness, m.extra_args.c_str());
    Profile gq;
    apply_preset(gq, Preset::Gaming, "/nonexistent");
    CHECK(gq.taa_sharpness < 0 && detect_preset(gq) == Preset::Gaming, "taa sharpness: the Quality preset leaves it to the runtime");
    gq.taa_sharpness = 0.9f;
    CHECK(detect_preset(gq) == Preset::Custom, "taa sharpness: a set value is Custom");
    apply_preset(gq, Preset::Gaming, "/nonexistent");
    CHECK(gq.taa_sharpness < 0, "taa sharpness: a preset clears it");
    CHECK(std::string(tr(S::taa_sharpness, Lang::Es)) == "Nitidez del TAA" && std::string(tr(S::taa_sharpness, Lang::En)) == "TAA sharpness" &&
              std::string(tr(S::taa_sharpness_desc, Lang::Es)) != "" && std::string(tr(S::taa_sharpness_desc, Lang::En)) != "",
          "taa sharpness: the row's label in Spanish and English");
}

/* --saturation and --hdr: the keys round-trip, saturation 1 and HDR off
 * are left out, the peak and paper white only with HDR and when not their default, they move out of
 * extra_args, presets keep them (a display preference, not part of a preset's match), and the rows'
 * labels in both languages. */
static void test_hdr_saturation_keys() {
    Profile d;
    CHECK(d.saturation == 1 && d.hdr == "off" && d.hdr_peak == 1000 && d.hdr_paper_white == 200 && opts(d).find("--saturation") == std::string::npos &&
              opts(d).find("--hdr") == std::string::npos,
          "hdr/saturation: defaults (1, off, 1000, 200) left out (%s)", opts(d).c_str());
    ProfileFile f, g;
    Profile p;
    p.saturation = 1.3f;
    p.hdr = "auto";
    p.hdr_peak = 800;
    p.hdr_paper_white = 250;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].saturation == 1.3f && g.profiles[0].hdr == "auto" && g.profiles[0].hdr_peak == 800 &&
              g.profiles[0].hdr_paper_white == 250 && warn.empty(),
          "hdr/saturation: ini round trip");
    CHECK(opts(p).find("--saturation|1.3|--hdr|auto|--hdr-peak|800|--hdr-paper-white|250") != std::string::npos, "hdr/saturation: passed (%s)",
          opts(p).c_str());
    Profile q;
    q.hdr_peak = 600;
    q.saturation = 0.8f;
    CHECK(opts(q).find("--hdr") == std::string::npos && opts(q).find("--saturation|0.8") != std::string::npos,
          "hdr/saturation: the peak needs HDR, saturation alone (%s)", opts(q).c_str());
    Profile m;
    m.extra_args = "--saturation 1.5 --hdr on --hdr-peak 1500 --hdr-paper-white 180 --saturation 3";
    migrate_extra_args(m);
    CHECK(m.saturation == 1.5f && m.hdr == "on" && m.hdr_peak == 1500 && m.hdr_paper_white == 180 && m.extra_args == "--saturation 3",
          "hdr/saturation: moved out of extra_args, an out-of-range value stays typed (%s)", m.extra_args.c_str());
    Profile gq;
    gq.saturation = 1.4f;
    gq.hdr = "auto";
    apply_preset(gq, Preset::Gaming, "/nonexistent");
    CHECK(gq.saturation == 1.4f && gq.hdr == "auto" && detect_preset(gq) == Preset::Gaming, "hdr/saturation: a preset keeps them and still matches");
    CHECK(std::string(tr(S::saturation, Lang::Es)) == "Saturación" && std::string(tr(S::saturation, Lang::En)) == "Saturation" &&
              std::string(tr(S::hdr_paper_white, Lang::Es)) == "Blanco de papel HDR" && std::string(tr(S::hdr_auto, Lang::Es)) == "Automático" &&
              std::string(tr(S::hdr_desc, Lang::Es)) != "" && std::string(tr(S::hdr_peak_desc, Lang::En)) != "",
          "hdr/saturation: the rows' labels in Spanish and English");
}

static void test_cutscene_dof_key() {
    ProfileFile f, g;
    Profile p;
    p.cutscene_dof = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].cutscene_dof && warn.empty(), "cutscene dof: ini round trip");
    CHECK(!Profile().cutscene_dof, "cutscene dof: off by default");
    Profile m;
    m.extra_args = "--cutscene-dof --cutscene-dof-strength 1.5";
    migrate_extra_args(m);
    CHECK(m.cutscene_dof && m.extra_args == "--cutscene-dof-strength 1.5", "cutscene dof: moved out of extra_args, the strength stays typed (%s)",
          m.extra_args.c_str());
    m.extra_args = "--no-cutscene-dof";
    migrate_extra_args(m);
    CHECK(!m.cutscene_dof && m.extra_args.empty(), "cutscene dof: --no-cutscene-dof moved too");
    CHECK(std::string(tr(S::cutscene_dof, Lang::Es)) == "Profundidad de campo en escenas" &&
              std::string(tr(S::cutscene_dof, Lang::En)) == "Cutscene depth of field" && std::string(tr(S::cutscene_dof_desc, Lang::Es)) != "",
          "cutscene dof: the toggle's label in Spanish and English");
    for (Preset preset : {Preset::Gaming, Preset::Balanced, Preset::Laptop}) {
        Profile q;
        apply_preset(q, preset, "");
        CHECK(q.cutscene_dof == (preset != Preset::Laptop), "cutscene dof: preset %d (on in Quality and Balanced, off in Performance)",
              (int)preset);
        q.cutscene_dof = !q.cutscene_dof;
        CHECK(detect_preset(q) == Preset::Custom, "cutscene dof: toggled in preset %d is custom", (int)preset);
    }
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
    /* --ssao-mode, --gtao-quality */
    CHECK(Profile().ssao_mode == "classic" && Profile().gtao_quality == "high", "ssao mode: classic, gtao quality high by default");
    f.profiles[0].ssao_mode = "gtao";
    f.profiles[0].gtao_quality = "low";
    g = ProfileFile();
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].ssao_mode == "gtao" && g.profiles[0].gtao_quality == "low" && warn.empty(),
          "ssao mode: ini round trip");
    m = Profile();
    m.extra_args = "--ssao --ssao-mode gtao --gtao-quality medium --ssao-mode hbao --gtao-quality ultra";
    migrate_extra_args(m);
    CHECK(m.ssao && m.ssao_mode == "gtao" && m.gtao_quality == "medium" && m.extra_args == "--ssao-mode hbao --gtao-quality ultra",
          "ssao mode: moved out of extra_args, bad values stay typed (%s %s, %s)", m.ssao_mode.c_str(), m.gtao_quality.c_str(), m.extra_args.c_str());
    /* Quality: GTAO High (stable under TAA with the temporal pass); the
     * presets without ambient occlusion keep classic for when it is turned on. */
    for (Preset pr : {Preset::Laptop, Preset::Balanced, Preset::Gaming}) {
        Profile q;
        apply_preset(q, pr, "/opt/warriors");
        const bool gtao = pr == Preset::Gaming;
        CHECK(q.ssao == gtao && q.ssao_mode == (gtao ? "gtao" : "classic") && q.gtao_quality == "high", "ssao mode: preset %d is %s", (int)pr,
              gtao ? "gtao high" : "classic (ssao off)");
        CHECK(!gtao || join(build_argv(q)).find("--ssao-mode|gtao") != std::string::npos, "ssao mode: the Quality preset passes --ssao-mode gtao");
        q.ssao_mode = gtao ? "classic" : "gtao";
        CHECK(detect_preset(q) == (q.ssao ? Preset::Custom : pr), "ssao mode: the other method in preset %d is %s", (int)pr,
              q.ssao ? "custom" : "unchanged (ssao off)");
        q.ssao_mode = gtao ? "gtao" : "classic";
        q.gtao_quality = "low";
        CHECK(detect_preset(q) == (gtao ? Preset::Custom : pr), "ssao mode: the gtao quality %s preset %d", gtao ? "leaves" : "alone does not leave",
              (int)pr);
    }
    /* --ssgi: off by default; ini round trip; moved out of extra_args (off too);
     * off in every preset (a faint effect for its cost), and turning it on leaves the preset where
     * GTAO is on; passed only with GTAO. */
    CHECK(!Profile().ssgi && Profile().ssgi_quality == "medium" && Profile().ssgi_strength < 0, "ssgi: off, medium, strength left out by default");
    f.profiles[0].ssgi = true;
    f.profiles[0].ssgi_quality = "high";
    f.profiles[0].ssgi_strength = 1.5f;
    g = ProfileFile();
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].ssgi && g.profiles[0].ssgi_quality == "high" && g.profiles[0].ssgi_strength == 1.5f && warn.empty(),
          "ssgi: ini round trip");
    m = Profile();
    m.extra_args = "--ssgi low --ssgi-strength 2 --ssgi ultra --ssgi-strength 9";
    migrate_extra_args(m);
    CHECK(m.ssgi && m.ssgi_quality == "low" && m.ssgi_strength == 2.0f && m.extra_args == "--ssgi ultra --ssgi-strength 9",
          "ssgi: moved out of extra_args, bad values stay typed (%s %.1f, %s)", m.ssgi_quality.c_str(), m.ssgi_strength, m.extra_args.c_str());
    m.extra_args = "--ssgi off";
    migrate_extra_args(m);
    CHECK(!m.ssgi && m.extra_args.empty(), "ssgi: --ssgi off moved too");
    for (Preset pr : {Preset::Laptop, Preset::Balanced, Preset::Gaming, Preset::Ultra, Preset::Integrated}) {
        Profile q;
        apply_preset(q, pr, "/opt/warriors");
        const bool on = false;
        const char *want = "medium";
        CHECK(q.ssgi == on && ssgi_used(q) == on && (!on || q.ssgi_quality == want), "ssgi: preset %d is %s", (int)pr, on ? want : "off");
        const std::string argv = join(build_argv(q));
        CHECK((argv.find("--ssgi|") != std::string::npos) == on, "ssgi: preset %d %s --ssgi", (int)pr, on ? "passes" : "leaves out");
        q.ssgi = !q.ssgi;
        CHECK(detect_preset(q) == (ssgi_used(q) || on ? Preset::Custom : pr), "ssgi: toggling it in preset %d leaves the preset (or not without GTAO)",
              (int)pr);
        q.ssgi = on;
        if (on) {
            q.ssgi_strength = 2;
            CHECK(detect_preset(q) == Preset::Custom, "ssgi: a strength set in preset %d is custom", (int)pr);
        }
    }
}


/* Contact shadows and puddles: ini keys, command line (puddles only with better rain), presets
 * (Quality on, Performance off), migration out of extra_args and the toggles' labels. */
static void test_contact_puddles_keys() {
    ProfileFile f;
    Profile p;
    p.contact_shadows = true;
    p.rain_hd = true;
    p.rain_puddles = true;
    f.profiles.push_back(p);
    ProfileFile g;
    std::vector<std::string> warn;
    std::string err;
    CHECK(parse_ini(write_ini(f), g, &warn) && g.profiles.size() == 1 && g.profiles[0].contact_shadows && g.profiles[0].rain_puddles &&
              warn.empty(),
          "contact/puddles: ini round trip");
    CHECK(!Profile().contact_shadows && !Profile().rain_puddles, "contact/puddles: off by default");
    std::string o = opts(p);
    CHECK(o.find("--contact-shadows") != std::string::npos && o.find("--rain-hd|--rain-hd-puddles") != std::string::npos, "contact/puddles: flags (%s)",
          o.c_str());
    p.rain_hd = false;
    CHECK(opts(p).find("--rain-hd-puddles") == std::string::npos, "puddles: left out without better rain");
    Profile q;
    apply_preset(q, Preset::Gaming, "");
    CHECK(q.contact_shadows && q.rain_puddles && detect_preset(q) == Preset::Gaming, "preset: Quality turns contact shadows and puddles on");
    Profile nq = q;
    nq.contact_shadows = false;
    CHECK(detect_preset(nq) == Preset::Custom, "preset: Quality without contact shadows is custom");
    nq = q;
    nq.rain_puddles = false;
    CHECK(detect_preset(nq) == Preset::Custom, "preset: Quality without puddles is custom");
    apply_preset(q, Preset::Balanced, "");
    CHECK(!q.contact_shadows && q.rain_puddles && detect_preset(q) == Preset::Balanced, "preset: Balanced has puddles, no contact shadows");
    apply_preset(q, Preset::Laptop, "");
    CHECK(!q.contact_shadows && !q.rain_puddles && detect_preset(q) == Preset::Laptop, "preset: Performance turns them off");
    Profile m;
    m.extra_args = "--contact-shadows --rain-hd-puddles --contact-shadows-strength 1";
    migrate_extra_args(m);
    CHECK(m.contact_shadows && m.rain_puddles && m.extra_args == "--contact-shadows-strength 1", "contact/puddles: moved out of extra_args (%s)",
          m.extra_args.c_str());
    m.extra_args = "--no-contact-shadows --no-rain-hd-puddles";
    migrate_extra_args(m);
    CHECK(!m.contact_shadows && !m.rain_puddles && m.extra_args.empty(), "contact/puddles: the --no- forms moved too");
    CHECK(std::string(tr(S::contact_shadows, Lang::Es)) == "Sombras de contacto" && std::string(tr(S::contact_shadows, Lang::En)) == "Contact shadows" &&
              std::string(tr(S::rain_puddles, Lang::Es)) == "Charcos" && std::string(tr(S::rain_puddles, Lang::En)) == "Puddles",
          "contact/puddles: the toggles' labels in Spanish and English");
}

/* Neon reflections (--neon-reflections): saved as neon_reflections, passed only with the neon glow
 * (after it), on in Quality only; a profile saved before it existed as Quality loads with it on and
 * keeps its preset. */
static void test_neon_reflections_key() {
    ProfileFile f, g;
    Profile p;
    p.neon_reflections = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    CHECK(parse_ini(write_ini(f), g, &warn) && g.profiles.size() == 1 && g.profiles[0].neon_reflections && warn.empty(), "neon reflections: ini round trip");
    CHECK(write_ini(f).find("neon_reflections = true") != std::string::npos, "neon reflections: saved as neon_reflections");
    CHECK(!Profile().neon_reflections, "neon reflections: off by default");
    CHECK(opts(p).find("--neon-reflections") == std::string::npos, "neon reflections: left out without the neon glow (%s)", opts(p).c_str());
    p.neon_glow = true;
    CHECK(opts(p).find("--neon-glow|--neon-reflections") != std::string::npos, "neon reflections: the flag after the neon glow (%s)", opts(p).c_str());
    p.neon_reflections = false;
    CHECK(opts(p).find("--neon-reflections") == std::string::npos, "neon reflections: left out when off");
    for (Preset preset : {Preset::Gaming, Preset::Balanced, Preset::Laptop}) {
        Profile q;
        apply_preset(q, preset, "");
        CHECK(q.neon_reflections == (preset == Preset::Gaming), "neon reflections: preset %d (on in Quality, off in Balanced and Performance)", (int)preset);
        q.neon_reflections = !q.neon_reflections;
        CHECK(detect_preset(q) == Preset::Custom, "neon reflections: toggled in preset %d is custom", (int)preset);
        Profile o;   /* an older file without the key: the saved preset's value */
        apply_preset(o, preset, "");
        o.preset = preset_key(preset);
        ProfileFile of, og;
        of.profiles.push_back(o);
        std::string ini = write_ini(of), key = o.neon_reflections ? "\nneon_reflections = true\n" : "\nneon_reflections = false\n";
        const size_t at = ini.find(key);
        CHECK(at != std::string::npos, "neon reflections: key written");
        if (at != std::string::npos) ini.erase(at + 1, key.size() - 1);
        CHECK(parse_ini(ini, og, nullptr) && og.profiles.size() == 1 && og.profiles[0].neon_reflections == o.neon_reflections &&
                  og.profiles[0].preset == preset_key(preset),
              "neon reflections: a %s profile saved before the key keeps its preset", preset_key(preset));
    }
    Profile m;
    m.extra_args = "--neon-reflections --neon-reflections-strength 1.5";
    migrate_extra_args(m);
    CHECK(m.neon_reflections && m.extra_args == "--neon-reflections-strength 1.5",
          "neon reflections: moved out of extra_args, the strength stays typed (%s)", m.extra_args.c_str());
    m.extra_args = "--no-neon-reflections";
    migrate_extra_args(m);
    CHECK(!m.neon_reflections && m.extra_args.empty(), "neon reflections: --no-neon-reflections moved too");
    CHECK(std::string(tr(S::neon_reflections, Lang::Es)) == "Reflejos de neón" && std::string(tr(S::neon_reflections, Lang::En)) == "Neon reflections" &&
              std::string(tr(S::neon_reflections_desc, Lang::Es)) != "" && std::string(tr(S::neon_reflections_desc, Lang::En)) != "",
          "neon reflections: the toggle's label in Spanish and English");
}
/* Reflections (--reflections classic|sssr): saved as reflections, classic by default and in every
 * preset (SSSR is an option, not a preset default), passed only as sssr and only
 * with better rain or damp streets (an unknown saved value is left out), moved out of extra_args (an
 * unknown mode stays typed), and the row's labels in Spanish and English. */
static void test_reflections_key() {
    ProfileFile f, g;
    Profile p;
    p.reflections = "sssr";
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    CHECK(parse_ini(write_ini(f), g, &warn) && g.profiles.size() == 1 && g.profiles[0].reflections == "sssr" && warn.empty(), "reflections: ini round trip");
    CHECK(write_ini(f).find("reflections = sssr") != std::string::npos, "reflections: saved as reflections");
    CHECK(Profile().reflections == "classic", "reflections: classic by default");
    CHECK(opts(p).find("--reflections") == std::string::npos, "reflections: left out without better rain or damp streets (%s)", opts(p).c_str());
    p.rain_hd = true;
    CHECK(opts(p).find("--reflections|sssr") != std::string::npos, "reflections: passed with better rain (%s)", opts(p).c_str());
    p.rain_hd = false;
    p.damp_streets = true;
    CHECK(opts(p).find("--reflections|sssr") != std::string::npos, "reflections: passed with damp streets (%s)", opts(p).c_str());
    p.reflections = "classic";
    CHECK(opts(p).find("--reflections") == std::string::npos, "reflections: classic left out (the default)");
    p.reflections = "mirror";
    CHECK(opts(p).find("--reflections") == std::string::npos, "reflections: an unknown saved value left out");
    for (Preset preset : {Preset::Gaming, Preset::Balanced, Preset::Laptop}) {
        Profile q;
        q.reflections = "sssr";
        apply_preset(q, preset, "");
        CHECK(q.reflections == "classic", "reflections: preset %d sets classic", (int)preset);
        CHECK(detect_preset(q) == preset, "reflections: preset %d detected", (int)preset);
        q.reflections = "sssr";
        CHECK(detect_preset(q) == Preset::Custom, "reflections: sssr in preset %d is custom", (int)preset);
    }
    Profile m;
    m.extra_args = "--reflections sssr --reflections-quality high";
    migrate_extra_args(m);
    CHECK(m.reflections == "sssr" && m.extra_args == "--reflections-quality high", "reflections: moved out of extra_args, the quality stays typed (%s, %s)",
          m.reflections.c_str(), m.extra_args.c_str());
    m.extra_args = "--reflections ultra";
    migrate_extra_args(m);
    CHECK(m.reflections == "sssr" && m.extra_args == "--reflections ultra", "reflections: an unknown mode stays typed (%s)", m.extra_args.c_str());
    CHECK(std::string(tr(S::reflections, Lang::Es)) == "Reflejos" && std::string(tr(S::reflections, Lang::En)) == "Reflections" &&
              std::string(tr(S::reflections_classic, Lang::Es)) == "Clásicos" && std::string(tr(S::reflections_sssr, Lang::Es)) == "SSSR" &&
              std::string(tr(S::reflections_desc, Lang::Es)) != "" && std::string(tr(S::reflections_desc, Lang::En)) != "",
          "reflections: the row's labels in Spanish and English");
}
/* Damp streets (--damp-streets): saved as damp_streets, passed with or without better rain, on in
 * Quality only; a profile saved before it existed as Quality loads with it on and keeps its preset. */
static void test_damp_streets_key() {
    ProfileFile f, g;
    Profile p;
    p.damp_streets = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    CHECK(parse_ini(write_ini(f), g, &warn) && g.profiles.size() == 1 && g.profiles[0].damp_streets && warn.empty(), "damp streets: ini round trip");
    CHECK(write_ini(f).find("damp_streets = true") != std::string::npos, "damp streets: saved as damp_streets");
    CHECK(!Profile().damp_streets, "damp streets: off by default");
    CHECK(opts(p).find("--damp-streets") != std::string::npos, "damp streets: the flag without better rain (%s)", opts(p).c_str());
    p.rain_hd = true;
    CHECK(opts(p).find("--damp-streets") != std::string::npos, "damp streets: the flag with better rain too");
    p.damp_streets = false;
    CHECK(opts(p).find("--damp-streets") == std::string::npos, "damp streets: left out when off");
    for (Preset preset : {Preset::Gaming, Preset::Balanced, Preset::Laptop}) {
        Profile q;
        apply_preset(q, preset, "");
        CHECK(q.damp_streets == (preset == Preset::Gaming), "damp streets: preset %d (on in Quality, off in Balanced and Performance)", (int)preset);
        q.damp_streets = !q.damp_streets;
        CHECK(detect_preset(q) == Preset::Custom, "damp streets: toggled in preset %d is custom", (int)preset);
        Profile o;   /* an older file without the key: the saved preset's value */
        apply_preset(o, preset, "");
        o.preset = preset_key(preset);
        ProfileFile of, og;
        of.profiles.push_back(o);
        std::string ini = write_ini(of), key = o.damp_streets ? "\ndamp_streets = true\n" : "\ndamp_streets = false\n";
        const size_t at = ini.find(key);
        CHECK(at != std::string::npos, "damp streets: key written");
        if (at != std::string::npos) ini.erase(at + 1, key.size() - 1);
        CHECK(parse_ini(ini, og, nullptr) && og.profiles.size() == 1 && og.profiles[0].damp_streets == o.damp_streets &&
                  og.profiles[0].preset == preset_key(preset),
              "damp streets: a %s profile saved before the key keeps its preset", preset_key(preset));
    }
    Profile m;
    m.extra_args = "--damp-streets --damp-streets-strength 1.5";
    migrate_extra_args(m);
    CHECK(m.damp_streets && m.extra_args == "--damp-streets-strength 1.5", "damp streets: moved out of extra_args, the strength stays typed (%s)",
          m.extra_args.c_str());
    m.extra_args = "--no-damp-streets";
    migrate_extra_args(m);
    CHECK(!m.damp_streets && m.extra_args.empty(), "damp streets: --no-damp-streets moved too");
    CHECK(std::string(tr(S::damp_streets, Lang::Es)) == "Calles húmedas" && std::string(tr(S::damp_streets, Lang::En)) == "Damp streets" &&
              std::string(tr(S::damp_streets_desc, Lang::Es)) != "" && std::string(tr(S::damp_streets_desc, Lang::En)) != "",
          "damp streets: the toggle's label in Spanish and English");
}

/* Wet ground by material (--materials): saved as materials, passed only with better rain; a profile
 * saved before it existed as Quality loads with it on and keeps its preset. */
static void test_materials_key() {
    ProfileFile f, g;
    Profile p;
    p.rain_hd = p.materials = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    CHECK(parse_ini(write_ini(f), g, &warn) && g.profiles.size() == 1 && g.profiles[0].materials && warn.empty(), "materials: ini round trip");
    CHECK(write_ini(f).find("materials = true") != std::string::npos, "materials: saved as materials");
    CHECK(!Profile().materials, "materials: off by default");
    CHECK(opts(p).find("--rain-hd|--materials") != std::string::npos, "materials: the flag after better rain (%s)", opts(p).c_str());
    p.rain_hd = false;
    CHECK(opts(p).find("--materials") == std::string::npos, "materials: left out without better rain");
    for (Preset preset : {Preset::Gaming, Preset::Balanced, Preset::Laptop}) {
        Profile q;
        apply_preset(q, preset, "");
        CHECK(q.materials == (preset == Preset::Gaming),
              "materials: preset %d (on in Quality, off in Balanced and Performance)", (int)preset);
        q.materials = !q.materials;
        CHECK(detect_preset(q) == Preset::Custom, "materials: toggled in preset %d is custom", (int)preset);
        /* An older file without the key: the saved preset's value. */
        Profile o;
        apply_preset(o, preset, "");
        o.preset = preset_key(preset);
        ProfileFile of, og;
        of.profiles.push_back(o);
        std::string ini = write_ini(of), key = o.materials ? "\nmaterials = true\n" : "\nmaterials = false\n";
        const size_t at = ini.find(key);
        CHECK(at != std::string::npos, "materials: key written");
        if (at != std::string::npos) ini.erase(at + 1, key.size() - 1);
        CHECK(parse_ini(ini, og, nullptr) && og.profiles.size() == 1 && og.profiles[0].materials == o.materials && og.profiles[0].preset == preset_key(preset),
              "materials: a %s profile saved before the key keeps its preset", preset_key(preset));
    }
    Profile m;
    m.extra_args = "--materials --materials-strength 1.5";
    migrate_extra_args(m);
    CHECK(m.materials && m.extra_args == "--materials-strength 1.5", "materials: moved out of extra_args, the strength stays typed (%s)", m.extra_args.c_str());
    m.extra_args = "--no-materials";
    migrate_extra_args(m);
    CHECK(!m.materials && m.extra_args.empty(), "materials: --no-materials moved too");
    CHECK(std::string(tr(S::materials, Lang::Es)) == "Suelo mojado según el material" && std::string(tr(S::materials, Lang::En)) == "Wet ground by material" &&
              std::string(tr(S::materials_desc, Lang::Es)) != "",
          "materials: the toggle's label in Spanish and English");
}

/* Relief (--normal-maps): saved as normal_maps, on in Quality only; a profile saved before it
 * existed as Quality loads with it on and keeps its preset; a typed --normal-maps moves into it. */
static void test_normal_maps_key() {
    ProfileFile f, g;
    Profile p;
    p.normal_maps = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    CHECK(parse_ini(write_ini(f), g, &warn) && g.profiles.size() == 1 && g.profiles[0].normal_maps && warn.empty(), "relief: ini round trip");
    CHECK(write_ini(f).find("normal_maps = true") != std::string::npos, "relief: saved as normal_maps");
    CHECK(!Profile().normal_maps, "relief: off by default");
    CHECK(opts(p) == "--normal-maps", "relief: the flag (%s)", opts(p).c_str());
    CHECK(opts(Profile()).find("--normal-maps") == std::string::npos, "relief: left out when off");
    for (Preset preset : {Preset::Gaming, Preset::Balanced, Preset::Laptop}) {
        Profile q;
        apply_preset(q, preset, "");
        CHECK(q.normal_maps == (preset == Preset::Gaming), "relief: preset %d (on in Quality, off in Balanced and Performance)", (int)preset);
        q.normal_maps = !q.normal_maps;
        CHECK(detect_preset(q) == Preset::Custom, "relief: toggled in preset %d is custom", (int)preset);
        /* An older file without the key: the saved preset's value. */
        Profile o;
        apply_preset(o, preset, "");
        o.preset = preset_key(preset);
        ProfileFile of, og;
        of.profiles.push_back(o);
        std::string ini = write_ini(of), key = o.normal_maps ? "\nnormal_maps = true\n" : "\nnormal_maps = false\n";
        const size_t at = ini.find(key);
        CHECK(at != std::string::npos, "relief: key written");
        if (at != std::string::npos) ini.erase(at + 1, key.size() - 1);
        CHECK(parse_ini(ini, og, nullptr) && og.profiles.size() == 1 && og.profiles[0].normal_maps == o.normal_maps &&
                  og.profiles[0].preset == preset_key(preset),
              "relief: a %s profile saved before the key keeps its preset", preset_key(preset));
    }
    /* A Custom profile saved before the key stays off. */
    Profile c;
    c.preset = "custom";
    ProfileFile cf, cg;
    cf.profiles.push_back(c);
    std::string cini = write_ini(cf);
    const size_t cat = cini.find("\nnormal_maps = false\n");
    if (cat != std::string::npos) cini.erase(cat + 1, std::string("normal_maps = false\n").size());
    CHECK(parse_ini(cini, cg, nullptr) && cg.profiles.size() == 1 && !cg.profiles[0].normal_maps, "relief: an older custom profile stays off");
    Profile m;
    m.extra_args = "--normal-maps --normal-strength 1.5";
    CHECK(migrate_extra_args(m) == 1 && m.normal_maps && m.extra_args == "--normal-strength 1.5",
          "relief: moved out of extra_args, the strength stays typed (%s)", m.extra_args.c_str());
    m.extra_args = "--no-normal-maps";
    migrate_extra_args(m);
    CHECK(!m.normal_maps && m.extra_args.empty(), "relief: --no-normal-maps moved too");
    CHECK(std::string(tr(S::normal_maps, Lang::En)) == "Relief (normal maps)" && std::string(tr(S::normal_maps, Lang::Es)) == "Relieve (mapas normales)" &&
              std::string(tr(S::normal_maps_desc, Lang::Es)) != "" && std::string(tr(S::relief_pack_missing, Lang::Es)) != "",
          "relief: the toggle's label in Spanish and English");
}

/* Metal and glass highlights (--dry-highlights): saved as dry_highlights, on in Quality only; a
 * profile saved before it existed as Quality loads with it on and keeps its preset; a typed
 * --dry-highlights moves into it. */
static void test_dry_highlights_key() {
    ProfileFile f, g;
    Profile p;
    p.dry_highlights = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    CHECK(parse_ini(write_ini(f), g, &warn) && g.profiles.size() == 1 && g.profiles[0].dry_highlights && warn.empty(), "dry highlights: ini round trip");
    CHECK(write_ini(f).find("dry_highlights = true") != std::string::npos, "dry highlights: saved as dry_highlights");
    CHECK(!Profile().dry_highlights, "dry highlights: off by default");
    CHECK(opts(p) == "--dry-highlights", "dry highlights: the flag (%s)", opts(p).c_str());
    CHECK(opts(Profile()).find("--dry-highlights") == std::string::npos, "dry highlights: left out when off");
    for (Preset preset : {Preset::Gaming, Preset::Balanced, Preset::Laptop}) {
        Profile q;
        apply_preset(q, preset, "");
        CHECK(q.dry_highlights == (preset == Preset::Gaming), "dry highlights: preset %d (on in Quality, off in Balanced and Performance)", (int)preset);
        q.dry_highlights = !q.dry_highlights;
        CHECK(detect_preset(q) == Preset::Custom, "dry highlights: toggled in preset %d is custom", (int)preset);
        Profile o;
        apply_preset(o, preset, "");
        o.preset = preset_key(preset);
        ProfileFile of, og;
        of.profiles.push_back(o);
        std::string ini = write_ini(of), key = o.dry_highlights ? "\ndry_highlights = true\n" : "\ndry_highlights = false\n";
        const size_t at = ini.find(key);
        CHECK(at != std::string::npos, "dry highlights: key written");
        if (at != std::string::npos) ini.erase(at + 1, key.size() - 1);
        CHECK(parse_ini(ini, og, nullptr) && og.profiles.size() == 1 && og.profiles[0].dry_highlights == o.dry_highlights &&
                  og.profiles[0].preset == preset_key(preset),
              "dry highlights: a %s profile saved before the key keeps its preset", preset_key(preset));
    }
    Profile c;
    c.preset = "custom";
    ProfileFile cf, cg;
    cf.profiles.push_back(c);
    std::string cini = write_ini(cf);
    const size_t cat = cini.find("\ndry_highlights = false\n");
    if (cat != std::string::npos) cini.erase(cat + 1, std::string("dry_highlights = false\n").size());
    CHECK(parse_ini(cini, cg, nullptr) && cg.profiles.size() == 1 && !cg.profiles[0].dry_highlights, "dry highlights: an older custom profile stays off");
    Profile m;
    m.extra_args = "--dry-highlights --dry-highlights-strength 1.5";
    CHECK(migrate_extra_args(m) == 1 && m.dry_highlights && m.extra_args == "--dry-highlights-strength 1.5",
          "dry highlights: moved out of extra_args, the strength stays typed (%s)", m.extra_args.c_str());
    m.extra_args = "--no-dry-highlights";
    migrate_extra_args(m);
    CHECK(!m.dry_highlights && m.extra_args.empty(), "dry highlights: --no-dry-highlights moved too");
    CHECK(std::string(tr(S::dry_highlights, Lang::En)) == "Metal and glass highlights" && std::string(tr(S::dry_highlights, Lang::Es)) == "Brillos en metal y vidrio" &&
              std::string(tr(S::dry_highlights_desc, Lang::Es)) != "",
          "dry highlights: the toggle's label in Spanish and English");
}

/* Better hair (--hair): saved as hair, on in Quality only, passed only with character lighting
 * (it shades the lit character draws); a profile saved before it existed as Quality loads with it
 * on and keeps its preset; a typed --hair moves into it. */
static void test_hair_key() {
    ProfileFile f, g;
    Profile p;
    p.char_lighting = p.hair = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    CHECK(parse_ini(write_ini(f), g, &warn) && g.profiles.size() == 1 && g.profiles[0].hair && warn.empty(), "hair: ini round trip");
    CHECK(write_ini(f).find("\nhair = true\n") != std::string::npos, "hair: saved as hair");
    CHECK(!Profile().hair, "hair: off by default");
    CHECK(opts(p) == "--char-lighting|--hair", "hair: the flag after character lighting (%s)", opts(p).c_str());
    Profile nc;
    nc.hair = true;
    CHECK(opts(nc).find("--hair") == std::string::npos, "hair: left out without character lighting (%s)", opts(nc).c_str());
    Profile off;
    off.char_lighting = true;
    CHECK(opts(off) == "--char-lighting", "hair: left out when off (%s)", opts(off).c_str());
    for (Preset preset : {Preset::Gaming, Preset::Balanced, Preset::Laptop}) {
        Profile q;
        apply_preset(q, preset, "");
        CHECK(q.hair == (preset == Preset::Gaming), "hair: preset %d (on in Quality, off in Balanced and Performance)", (int)preset);
        q.hair = !q.hair;
        CHECK(detect_preset(q) == Preset::Custom, "hair: toggled in preset %d is custom", (int)preset);
        Profile o;
        apply_preset(o, preset, "");
        o.preset = preset_key(preset);
        ProfileFile of, og;
        of.profiles.push_back(o);
        std::string ini = write_ini(of), key = o.hair ? "\nhair = true\n" : "\nhair = false\n";
        const size_t at = ini.find(key);
        CHECK(at != std::string::npos, "hair: key written");
        if (at != std::string::npos) ini.erase(at + 1, key.size() - 1);
        CHECK(parse_ini(ini, og, nullptr) && og.profiles.size() == 1 && og.profiles[0].hair == o.hair && og.profiles[0].preset == preset_key(preset),
              "hair: a %s profile saved before the key keeps its preset", preset_key(preset));
    }
    Profile c;
    c.preset = "custom";
    ProfileFile cf, cg;
    cf.profiles.push_back(c);
    std::string cini = write_ini(cf);
    const size_t cat = cini.find("\nhair = false\n");
    if (cat != std::string::npos) cini.erase(cat + 1, std::string("hair = false\n").size());
    CHECK(parse_ini(cini, cg, nullptr) && cg.profiles.size() == 1 && !cg.profiles[0].hair, "hair: an older custom profile stays off");
    Profile m;
    m.extra_args = "--hair --hair-strength 1.5";
    CHECK(migrate_extra_args(m) == 1 && m.hair && m.extra_args == "--hair-strength 1.5", "hair: moved out of extra_args, the strength stays typed (%s)",
          m.extra_args.c_str());
    m.extra_args = "--no-hair";
    migrate_extra_args(m);
    CHECK(!m.hair && m.extra_args.empty(), "hair: --no-hair moved too");
    CHECK(std::string(tr(S::hair, Lang::En)) == "Better hair" && std::string(tr(S::hair, Lang::Es)) == "Cabello mejorado" &&
              std::string(tr(S::hair_desc, Lang::Es)) != "",
          "hair: the toggle's label in Spanish and English");
}

/* Realistic skin (--skin): saved as skin, on in Quality only, passed only with character lighting
 * (it shades the lit character draws); a profile saved before it existed as Quality loads with it
 * on and keeps its preset; a typed --skin moves into it. */
static void test_skin_key() {
    ProfileFile f, g;
    Profile p;
    p.char_lighting = p.skin = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    CHECK(parse_ini(write_ini(f), g, &warn) && g.profiles.size() == 1 && g.profiles[0].skin && warn.empty(), "skin: ini round trip");
    CHECK(write_ini(f).find("\nskin = true\n") != std::string::npos, "skin: saved as skin");
    CHECK(!Profile().skin, "skin: off by default");
    CHECK(opts(p) == "--char-lighting|--skin", "skin: the flag after character lighting (%s)", opts(p).c_str());
    Profile nc;
    nc.skin = true;
    CHECK(opts(nc).find("--skin") == std::string::npos, "skin: left out without character lighting (%s)", opts(nc).c_str());
    Profile off;
    off.char_lighting = true;
    CHECK(opts(off) == "--char-lighting", "skin: left out when off (%s)", opts(off).c_str());
    for (Preset preset : {Preset::Gaming, Preset::Balanced, Preset::Laptop}) {
        Profile q;
        apply_preset(q, preset, "");
        CHECK(q.skin == (preset == Preset::Gaming), "skin: preset %d (on in Quality, off in Balanced and Performance)", (int)preset);
        q.skin = !q.skin;
        CHECK(detect_preset(q) == Preset::Custom, "skin: toggled in preset %d is custom", (int)preset);
        Profile o;
        apply_preset(o, preset, "");
        o.preset = preset_key(preset);
        ProfileFile of, og;
        of.profiles.push_back(o);
        std::string ini = write_ini(of), key = o.skin ? "\nskin = true\n" : "\nskin = false\n";
        const size_t at = ini.find(key);
        CHECK(at != std::string::npos, "skin: key written");
        if (at != std::string::npos) ini.erase(at + 1, key.size() - 1);
        CHECK(parse_ini(ini, og, nullptr) && og.profiles.size() == 1 && og.profiles[0].skin == o.skin && og.profiles[0].preset == preset_key(preset),
              "skin: a %s profile saved before the key keeps its preset", preset_key(preset));
    }
    Profile c;
    c.preset = "custom";
    ProfileFile cf, cg;
    cf.profiles.push_back(c);
    std::string cini = write_ini(cf);
    const size_t cat = cini.find("\nskin = false\n");
    if (cat != std::string::npos) cini.erase(cat + 1, std::string("skin = false\n").size());
    CHECK(parse_ini(cini, cg, nullptr) && cg.profiles.size() == 1 && !cg.profiles[0].skin, "skin: an older custom profile stays off");
    Profile m;
    m.extra_args = "--skin --skin-strength 1.5";
    CHECK(migrate_extra_args(m) == 1 && m.skin && m.extra_args == "--skin-strength 1.5", "skin: moved out of extra_args, the strength stays typed (%s)",
          m.extra_args.c_str());
    m.extra_args = "--no-skin";
    migrate_extra_args(m);
    CHECK(!m.skin && m.extra_args.empty(), "skin: --no-skin moved too");
    CHECK(std::string(tr(S::skin, Lang::En)) == "Realistic skin" && std::string(tr(S::skin, Lang::Es)) == "Piel realista" &&
              std::string(tr(S::skin_desc, Lang::Es)) != "",
          "skin: the toggle's label in Spanish and English");
}

/* Hair and skin together: both after character lighting, hair first. */
static void test_hair_and_skin() {
    Profile p;
    p.char_lighting = p.hair = p.skin = true;
    CHECK(opts(p) == "--char-lighting|--hair|--skin", "hair and skin: both flags (%s)", opts(p).c_str());
}

/* Realistic eyes (--eyes) and clothing materials (--cloth): saved as eyes / cloth, on in Quality
 * only, passed only with character lighting (they shade the lit character draws), after hair and
 * skin; a profile saved before they existed as Quality loads with them on and keeps its preset; a
 * typed --eyes / --cloth moves into them (their strengths stay typed). */
static void test_mat_key(const char *key, bool Profile::*field, S label, S desc, const char *en, const char *es) {
    const std::string k = key, flag = "--" + k;
    ProfileFile f, g;
    Profile p;
    p.char_lighting = true;
    p.*field = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    CHECK(parse_ini(write_ini(f), g, &warn) && g.profiles.size() == 1 && g.profiles[0].*field && warn.empty(), "%s: ini round trip", key);
    CHECK(write_ini(f).find("\n" + k + " = true\n") != std::string::npos, "%s: saved as %s", key, key);
    CHECK(!(Profile().*field), "%s: off by default", key);
    CHECK(opts(p) == "--char-lighting|" + flag, "%s: the flag after character lighting (%s)", key, opts(p).c_str());
    Profile nc;
    nc.*field = true;
    CHECK(opts(nc).find(flag) == std::string::npos, "%s: left out without character lighting (%s)", key, opts(nc).c_str());
    for (Preset preset : {Preset::Gaming, Preset::Balanced, Preset::Laptop}) {
        Profile q;
        apply_preset(q, preset, "");
        CHECK(q.*field == (preset == Preset::Gaming), "%s: preset %d (on in Quality, off in Balanced and Performance)", key, (int)preset);
        q.*field = !(q.*field);
        CHECK(detect_preset(q) == Preset::Custom, "%s: toggled in preset %d is custom", key, (int)preset);
        Profile o;
        apply_preset(o, preset, "");
        o.preset = preset_key(preset);
        ProfileFile of, og;
        of.profiles.push_back(o);
        std::string ini = write_ini(of), line = "\n" + k + (o.*field ? " = true\n" : " = false\n");
        const size_t at = ini.find(line);
        CHECK(at != std::string::npos, "%s: key written", key);
        if (at != std::string::npos) ini.erase(at + 1, line.size() - 1);
        CHECK(parse_ini(ini, og, nullptr) && og.profiles.size() == 1 && og.profiles[0].*field == o.*field && og.profiles[0].preset == preset_key(preset),
              "%s: a %s profile saved before the key keeps its preset", key, preset_key(preset));
    }
    Profile c;
    c.preset = "custom";
    ProfileFile cf, cg;
    cf.profiles.push_back(c);
    std::string cini = write_ini(cf);
    const size_t cat = cini.find("\n" + k + " = false\n");
    if (cat != std::string::npos) cini.erase(cat + 1, k.size() + 9);
    CHECK(parse_ini(cini, cg, nullptr) && cg.profiles.size() == 1 && !(cg.profiles[0].*field), "%s: an older custom profile stays off", key);
    Profile m;
    m.extra_args = flag + " " + flag + "-strength 1.5";
    CHECK(migrate_extra_args(m) == 1 && m.*field && m.extra_args == flag + "-strength 1.5", "%s: moved out of extra_args, the strength stays typed (%s)", key,
          m.extra_args.c_str());
    m.extra_args = "--no-" + k;
    migrate_extra_args(m);
    CHECK(!(m.*field) && m.extra_args.empty(), "%s: --no-%s moved too", key, key);
    CHECK(std::string(tr(label, Lang::En)) == en && std::string(tr(label, Lang::Es)) == es && std::string(tr(desc, Lang::Es)) != "" &&
              std::string(tr(desc, Lang::En)) != "",
          "%s: the toggle's label in Spanish and English", key);
}
static void test_eyes_cloth_keys() {
    test_mat_key("eyes", &Profile::eyes, S::eyes, S::eyes_desc, "Realistic eyes", "Ojos realistas");
    test_mat_key("cloth", &Profile::cloth, S::cloth, S::cloth_desc, "Clothing materials", "Materiales de ropa");
    Profile p;
    p.char_lighting = p.hair = p.skin = p.eyes = p.cloth = true;
    CHECK(opts(p) == "--char-lighting|--hair|--skin|--eyes|--cloth", "hair, skin, eyes and cloth: in order (%s)", opts(p).c_str());
}

/* Parallax relief (--parallax): saved as parallax, on in Quality only; a
 * profile saved before it existed as Quality loads with it on and keeps its preset; a typed
 * --parallax moves into it (its depth stays typed). */
static void test_parallax_key() {
    ProfileFile f, g;
    Profile p;
    p.parallax = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    CHECK(parse_ini(write_ini(f), g, &warn) && g.profiles.size() == 1 && g.profiles[0].parallax && warn.empty(), "parallax: ini round trip");
    CHECK(write_ini(f).find("parallax = true") != std::string::npos, "parallax: saved as parallax");
    CHECK(!Profile().parallax, "parallax: off by default");
    CHECK(opts(p) == "--parallax", "parallax: the flag (%s)", opts(p).c_str());
    CHECK(opts(Profile()).find("--parallax") == std::string::npos, "parallax: left out when off");
    for (Preset preset : {Preset::Gaming, Preset::Balanced, Preset::Laptop}) {
        Profile q;
        apply_preset(q, preset, "");
        CHECK(q.parallax == (preset == Preset::Gaming), "parallax: preset %d (on in Quality, off in Balanced and Performance)", (int)preset);
        q.parallax = !q.parallax;
        CHECK(detect_preset(q) == Preset::Custom, "parallax: toggled in preset %d is custom", (int)preset);
        Profile o;
        apply_preset(o, preset, "");
        o.preset = preset_key(preset);
        ProfileFile of, og;
        of.profiles.push_back(o);
        std::string ini = write_ini(of), key = o.parallax ? "\nparallax = true\n" : "\nparallax = false\n";
        const size_t at = ini.find(key);
        CHECK(at != std::string::npos, "parallax: key written");
        if (at != std::string::npos) ini.erase(at + 1, key.size() - 1);
        CHECK(parse_ini(ini, og, nullptr) && og.profiles.size() == 1 && og.profiles[0].parallax == o.parallax &&
                  og.profiles[0].preset == preset_key(preset),
              "parallax: a %s profile saved before the key keeps its preset", preset_key(preset));
    }
    Profile c;
    c.preset = "custom";
    ProfileFile cf, cg;
    cf.profiles.push_back(c);
    std::string cini = write_ini(cf);
    const size_t cat = cini.find("\nparallax = false\n");
    if (cat != std::string::npos) cini.erase(cat + 1, std::string("parallax = false\n").size());
    CHECK(parse_ini(cini, cg, nullptr) && cg.profiles.size() == 1 && !cg.profiles[0].parallax, "parallax: an older custom profile stays off");
    Profile m;
    m.extra_args = "--parallax --parallax-depth 0.05";
    CHECK(migrate_extra_args(m) == 1 && m.parallax && m.extra_args == "--parallax-depth 0.05",
          "parallax: moved out of extra_args, the depth stays typed (%s)", m.extra_args.c_str());
    m.extra_args = "--no-parallax";
    migrate_extra_args(m);
    CHECK(!m.parallax && m.extra_args.empty(), "parallax: --no-parallax moved too");
    CHECK(std::string(tr(S::parallax, Lang::En)) == "Parallax relief" && std::string(tr(S::parallax, Lang::Es)) == "Relieve con profundidad" &&
              std::string(tr(S::parallax_desc, Lang::Es)) != "",
          "parallax: the toggle's label in Spanish and English");
}

/* The relief-pack folder next to the game: with the toggle on, launch adds it as the last texture
 * pack (after the user's, typed ones included), not twice; without the folder it takes
 * --normal-maps back out (the flag would do nothing); with the toggle off it adds nothing. */
static void test_relief_pack() {
    const std::string pack = "/opt/warriors/relief-pack";
    std::vector<std::string> a = {"xbrun", "d/default.xbe", "d", "--texture-pack", "/opt/warriors/texpack-v3", "--normal-maps", "--texture-pack", "typed"};
    add_relief_pack(a, pack, true);
    CHECK(join(a) == "xbrun|d/default.xbe|d|--texture-pack|/opt/warriors/texpack-v3|--normal-maps|--texture-pack|typed|--texture-pack|" + pack,
          "relief pack: found, added after every other texture pack (%s)", join(a).c_str());
    add_relief_pack(a, pack, true);
    CHECK(join(a) == "xbrun|d/default.xbe|d|--texture-pack|/opt/warriors/texpack-v3|--normal-maps|--texture-pack|typed|--texture-pack|" + pack,
          "relief pack: not added twice");
    for (const char *listed : {"/opt/warriors/relief-pack/", "relief-pack"}) {
        std::vector<std::string> u = {"xbrun", "d/default.xbe", "d", "--normal-maps", "--texture-pack", listed};
        add_relief_pack(u, pack, true);
        CHECK(join(u) == std::string("xbrun|d/default.xbe|d|--normal-maps|--texture-pack|") + listed,
              "relief pack: already listed as %s, not added again (%s)", listed, join(u).c_str());
    }
    std::vector<std::string> n = {"xbrun", "d/default.xbe", "d", "--texture-pack", "/opt/warriors/texpack-v3", "--normal-maps", "--skip-intro"};
    add_relief_pack(n, pack, false);
    CHECK(join(n) == "xbrun|d/default.xbe|d|--texture-pack|/opt/warriors/texpack-v3|--skip-intro",
          "relief pack: missing, --normal-maps taken out and nothing added (%s)", join(n).c_str());
    std::vector<std::string> off = {"xbrun", "d/default.xbe", "d", "--skip-intro"};
    add_relief_pack(off, pack, true);
    CHECK(join(off) == "xbrun|d/default.xbe|d|--skip-intro", "relief pack: toggle off adds nothing");
    Profile q;
    apply_preset(q, Preset::Gaming, "");
    std::vector<std::string> g = build_argv(q);
    add_relief_pack(g, pack, true);
    CHECK(g.size() >= 3 && g[g.size() - 2] == "--texture-pack" && g.back() == pack &&
              std::string(RELIEF_PACK_DIR) == "relief-pack",
          "relief pack: Quality with the folder ends with it (%s)", join(g).c_str());
}

/* Wet light reflections (--wet-light-streaks): saved as wet_light_streaks, passed only with better
 * rain; a profile saved before it existed as Quality loads with it on and keeps its preset. */
static void test_wet_light_streaks_key() {
    ProfileFile f, g;
    Profile p;
    p.rain_hd = p.wet_light_streaks = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    CHECK(parse_ini(write_ini(f), g, &warn) && g.profiles.size() == 1 && g.profiles[0].wet_light_streaks && warn.empty(), "wet streaks: ini round trip");
    CHECK(write_ini(f).find("wet_light_streaks = true") != std::string::npos, "wet streaks: saved as wet_light_streaks");
    CHECK(!Profile().wet_light_streaks, "wet streaks: off by default");
    CHECK(opts(p).find("--rain-hd|--wet-light-streaks") != std::string::npos, "wet streaks: the flag after better rain (%s)", opts(p).c_str());
    p.rain_hd = false;
    CHECK(opts(p).find("--wet-light-streaks") == std::string::npos, "wet streaks: left out without better rain");
    for (Preset preset : {Preset::Gaming, Preset::Balanced, Preset::Laptop}) {
        Profile q;
        apply_preset(q, preset, "");
        CHECK(q.wet_light_streaks == (preset == Preset::Gaming),
              "wet streaks: preset %d (on in Quality, off in Balanced and Performance)", (int)preset);
        q.wet_light_streaks = !q.wet_light_streaks;
        CHECK(detect_preset(q) == Preset::Custom, "wet streaks: toggled in preset %d is custom", (int)preset);
        /* An older file without the key: the saved preset's value. */
        Profile o;
        apply_preset(o, preset, "");
        o.preset = preset_key(preset);
        ProfileFile of, og;
        of.profiles.push_back(o);
        std::string ini = write_ini(of), key = o.wet_light_streaks ? "wet_light_streaks = true\n" : "wet_light_streaks = false\n";
        const size_t at = ini.find(key);
        CHECK(at != std::string::npos, "wet streaks: key written");
        if (at != std::string::npos) ini.erase(at, key.size());
        CHECK(parse_ini(ini, og, nullptr) && og.profiles.size() == 1 && og.profiles[0].wet_light_streaks == o.wet_light_streaks &&
                  og.profiles[0].preset == preset_key(preset),
              "wet streaks: a %s profile saved before the key keeps its preset", preset_key(preset));
    }
    Profile m;
    m.extra_args = "--wet-light-streaks --wet-light-streaks-strength 2";
    migrate_extra_args(m);
    CHECK(m.wet_light_streaks && m.extra_args == "--wet-light-streaks-strength 2", "wet streaks: moved out of extra_args, the strength stays typed (%s)",
          m.extra_args.c_str());
    m.extra_args = "--no-wet-light-streaks";
    migrate_extra_args(m);
    CHECK(!m.wet_light_streaks && m.extra_args.empty(), "wet streaks: --no-wet-light-streaks moved too");
    CHECK(std::string(tr(S::wet_light_streaks, Lang::Es)) == "Reflejos de luces en la calle mojada" &&
              std::string(tr(S::wet_light_streaks, Lang::En)) == "Light reflections on wet streets" &&
              std::string(tr(S::wet_light_streaks_desc, Lang::Es)) != "",
          "wet streaks: the toggle's label in Spanish and English");
}

static void test_lamp_lights_key() {
    ProfileFile f, g;
    Profile p;
    p.lamp_lights = true;
    p.lamp_lights_strength = 1.5f;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].lamp_lights && g.profiles[0].lamp_lights_strength == 1.5f && warn.empty(),
          "lamp lights: ini round trip");
    CHECK(!Profile().lamp_lights && Profile().lamp_lights_strength < 0, "lamp lights: off by default, strength left out");
    Profile m;
    m.extra_args = "--lamp-lights --lamp-lights-strength 2 --lamp-lights-strength 9";
    migrate_extra_args(m);
    CHECK(m.lamp_lights && m.lamp_lights_strength == 2 && m.extra_args == "--lamp-lights-strength 9",
          "lamp lights: moved out of extra_args, a bad value stays typed (%.2f, %s)", m.lamp_lights_strength, m.extra_args.c_str());
    m.extra_args = "--no-lamp-lights";
    migrate_extra_args(m);
    CHECK(!m.lamp_lights && m.extra_args.empty(), "lamp lights: --no-lamp-lights moved too");
    CHECK(std::string(tr(S::lamp_lights, Lang::Es)) == "Luces de faroles" && std::string(tr(S::lamp_lights, Lang::En)) == "Lamp lights",
          "lamp lights: launcher toggle in Spanish and English");
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

/* PLAY stays off during the whole first-run wizard (it used to turn on at step 2 once the copy
 * was extracted, off at step 3 and on again at step 4) and while the game runs. */
static void test_play_rule() {
    CHECK(!play_enabled(true, false, true) && !play_enabled(true, false, false), "play: off on every wizard step, ready or not");
    CHECK(play_enabled(false, false, true), "play: on after the wizard when the copy is ready");
    CHECK(!play_enabled(false, false, false) && !play_enabled(false, true, true), "play: off when not ready or running");
}

/* A typed path's "does not exist" waits until the field loses focus or typing pauses. */
static void test_path_message_debounce() {
    CHECK(!path_message_visible(true, 10.0, 10.0) && !path_message_visible(true, 10.5, 10.0),
          "path message: hidden while typing (0 s and 0.5 s after a keystroke)");
    CHECK(path_message_visible(true, 10.6, 10.0) && path_message_visible(true, 12.0, 10.0),
          "path message: shown after %.1f s without typing", PATH_SETTLE_SECONDS);
    CHECK(path_message_visible(false, 10.0, 10.0), "path message: shown at once when the field loses focus");
    CHECK(path_message_visible(true, 0.0, -1e9), "path message: shown in a focused field nobody typed in");
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

/* The characters-hd folder next to the launcher: .wmesh files in it add --mesh-pack, an empty or
 * missing folder adds nothing, and a --mesh-pack the user typed wins (no second one). */
static void test_characters_hd(const std::string &scratch) {
    std::string dir = path_join(scratch, "characters-hd");
    make_dir(dir);
    for (const char *n : {"a.wmesh", "b.wmesh", "readme.txt"}) remove_file(path_join(dir, n));
    CHECK(count_wmesh(path_join(scratch, "no-such-folder")) == 0, "characters-hd: a missing folder has no models");
    CHECK(count_wmesh(dir) == 0, "characters-hd: an empty folder has no models");
    write_file(path_join(dir, "readme.txt"), "x");
    CHECK(count_wmesh(dir) == 0, "characters-hd: other files are not models");
    std::vector<std::string> a = {"xbrun", "d/default.xbe", "d"};
    add_auto_mesh_pack(a, "characters-hd", count_wmesh(dir));
    CHECK(join(a) == "xbrun|d/default.xbe|d", "characters-hd: empty folder adds nothing");
    write_file(path_join(dir, "a.wmesh"), "x");
    write_file(path_join(dir, "b.wmesh"), "x");
    CHECK(count_wmesh(dir) == 2, "characters-hd: counts the .wmesh files");
    add_auto_mesh_pack(a, "characters-hd", count_wmesh(dir));
    CHECK(join(a) == "xbrun|d/default.xbe|d|--mesh-pack|characters-hd", "characters-hd: .wmesh files add --mesh-pack");
    std::vector<std::string> u = {"xbrun", "d/default.xbe", "d", "--mesh-pack", "mine"};
    add_auto_mesh_pack(u, "characters-hd", 2);
    CHECK(join(u) == "xbrun|d/default.xbe|d|--mesh-pack|mine", "characters-hd: the user's --mesh-pack wins, no duplicate");
}

/* The merged graphics presets in one table: sharp text in every preset, sharp faces and puddles in Balanced and
 * Quality, contact shadows in Quality only, soft particles nowhere (opt-in); turning any of them over is custom. */
static void test_gfx_preset_union() {
    struct Row { Preset preset; bool fonts, faces, puddles, contact; };
    const Row rows[] = {{Preset::Laptop, true, false, false, false},
                        {Preset::Balanced, true, true, true, false},
                        {Preset::Gaming, true, true, true, true}};
    for (const Row &r : rows) {
        Profile q;
        apply_preset(q, r.preset, "");
        CHECK(q.sharp_fonts == r.fonts && q.sharp_faces == r.faces && q.rain_puddles == r.puddles &&
                  q.contact_shadows == r.contact && !q.soft_particles && detect_preset(q) == r.preset,
              "gfx union: %s preset table", preset_key(r.preset));
        bool Profile::*fields[] = {&Profile::sharp_fonts, &Profile::sharp_faces, &Profile::rain_puddles,
                                   &Profile::contact_shadows, &Profile::soft_particles};
        for (bool Profile::*f : fields) {
            Profile e = q;
            e.*f = !(e.*f);
            CHECK(detect_preset(e) == Preset::Custom, "gfx union: %s with one toggle flipped is custom", preset_key(r.preset));
        }
    }
}

/* Character lighting (--char-lighting): saved as char_lighting; a profile saved before it existed
 * as Balanced or Quality loads with it on and keeps its preset, a Custom one keeps it off; typed in
 * extra_args it moves into the option. */
static void test_char_lighting_key() {
    ProfileFile f, g;
    Profile p;
    p.char_lighting = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].char_lighting && warn.empty(), "char lighting: ini round trip");
    CHECK(write_ini(f).find("char_lighting = true") != std::string::npos, "char lighting: saved as char_lighting");
    CHECK(!Profile().char_lighting, "char lighting: off by default");
    for (Preset pr : {Preset::Balanced, Preset::Gaming}) {
        ProfileFile o, h;
        Profile q;
        apply_preset(q, pr, "");
        q.name = "q";
        o.profiles.push_back(q);
        std::string ini = write_ini(o), key = "char_lighting = true\n";
        const size_t at = ini.find(key);
        CHECK(at != std::string::npos, "char lighting: %s writes the key", preset_key(pr));
        if (at != std::string::npos) ini.erase(at, key.size());
        CHECK(parse_ini(ini, h, nullptr) && h.profiles.size() == 1 && h.profiles[0].char_lighting && h.profiles[0].preset == preset_key(pr),
              "char lighting: an older %s profile turns it on and stays %s", preset_key(pr), preset_key(pr));
    }
    ProfileFile o, h;
    Profile c;
    c.name = "c";
    o.profiles.push_back(c);
    parse_ini(write_ini(o), h, nullptr);
    CHECK(h.profiles.size() == 1 && !h.profiles[0].char_lighting, "char lighting: a custom profile keeps it off");
    Profile e = p;
    e.char_lighting = false;
    ProfileFile x, y;
    x.profiles.push_back(e);
    apply_preset(x.profiles[0], Preset::Gaming, "");
    x.profiles[0].char_lighting = false;   /* Quality, turned off by hand: Custom, stays off */
    parse_ini(write_ini(x), y, nullptr);
    CHECK(y.profiles.size() == 1 && !y.profiles[0].char_lighting && y.profiles[0].preset == "custom",
          "char lighting: turned off by hand stays off (%s)", y.profiles.empty() ? "" : y.profiles[0].preset.c_str());
    Profile m;
    m.extra_args = "--char-lighting";
    migrate_extra_args(m);
    CHECK(m.char_lighting && m.extra_args.empty(), "char lighting: moved out of extra_args (%s)", m.extra_args.c_str());
}

/* World lighting (--world-lighting): saved as world_lighting; a profile saved before it existed as
 * Quality loads with it on and keeps its preset, Balanced and Custom ones keep it off; typed in
 * extra_args it moves into the option. */
static void test_world_lighting_key() {
    ProfileFile f, g;
    Profile p;
    p.world_lighting = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].world_lighting && warn.empty(), "world lighting: ini round trip");
    CHECK(write_ini(f).find("world_lighting = true") != std::string::npos, "world lighting: saved as world_lighting");
    CHECK(!Profile().world_lighting, "world lighting: off by default");
    {
        ProfileFile o, h;
        Profile q;
        apply_preset(q, Preset::Gaming, "");
        q.name = "q";
        o.profiles.push_back(q);
        std::string ini = write_ini(o), key = "world_lighting = true\n";
        const size_t at = ini.find(key);
        CHECK(at != std::string::npos, "world lighting: Quality writes the key");
        if (at != std::string::npos) ini.erase(at, key.size());
        CHECK(parse_ini(ini, h, nullptr) && h.profiles.size() == 1 && h.profiles[0].world_lighting && h.profiles[0].preset == "gaming",
              "world lighting: an older Quality profile turns it on and stays Quality");
    }
    {
        ProfileFile o, h;
        Profile q;
        apply_preset(q, Preset::Balanced, "");
        q.name = "b";
        o.profiles.push_back(q);
        std::string ini = write_ini(o), key = "world_lighting = false\n";
        const size_t at = ini.find(key);
        if (at != std::string::npos) ini.erase(at, key.size());
        CHECK(parse_ini(ini, h, nullptr) && h.profiles.size() == 1 && !h.profiles[0].world_lighting && h.profiles[0].preset == "balanced",
              "world lighting: an older Balanced profile keeps it off and stays Balanced");
    }
    ProfileFile x, y;
    x.profiles.push_back(p);
    apply_preset(x.profiles[0], Preset::Gaming, "");
    x.profiles[0].world_lighting = false;   /* Quality, turned off by hand: Custom, stays off */
    parse_ini(write_ini(x), y, nullptr);
    CHECK(y.profiles.size() == 1 && !y.profiles[0].world_lighting && y.profiles[0].preset == "custom",
          "world lighting: turned off by hand stays off (%s)", y.profiles.empty() ? "" : y.profiles[0].preset.c_str());
    Profile m;
    m.extra_args = "--world-lighting";
    migrate_extra_args(m);
    CHECK(m.world_lighting && m.extra_args.empty(), "world lighting: moved out of extra_args (%s)", m.extra_args.c_str());
}

/* Fire light (--fire-light): saved as fire_light; a profile saved before it existed as Quality loads
 * with it on and keeps its preset, others keep it off; typed in extra_args it moves into the option. */
static void test_fire_light_key() {
    ProfileFile f, g;
    Profile p;
    p.fire_light = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].fire_light && warn.empty(), "fire light: ini round trip");
    CHECK(write_ini(f).find("fire_light = true") != std::string::npos, "fire light: saved as fire_light");
    CHECK(!Profile().fire_light, "fire light: off by default");
    for (Preset pr : {Preset::Gaming, Preset::Balanced}) {
        ProfileFile o, h;
        Profile q;
        apply_preset(q, pr, "");
        q.name = "q";
        o.profiles.push_back(q);
        const bool on = pr == Preset::Gaming;
        std::string ini = write_ini(o), key = on ? "fire_light = true\n" : "fire_light = false\n";
        const size_t at = ini.find(key);
        CHECK(at != std::string::npos, "fire light: the preset writes the key");
        if (at != std::string::npos) ini.erase(at, key.size());
        CHECK(parse_ini(ini, h, nullptr) && h.profiles.size() == 1 && h.profiles[0].fire_light == on &&
                  h.profiles[0].preset == (on ? "gaming" : "balanced"),
              "fire light: an older %s profile takes the preset's value and keeps its preset", on ? "Quality" : "Balanced");
    }
    Profile m;
    m.extra_args = "--fire-light";
    migrate_extra_args(m);
    CHECK(m.fire_light && m.extra_args.empty(), "fire light: moved out of extra_args (%s)", m.extra_args.c_str());
    Profile c;
    c.fire_light = true;
    const std::vector<std::string> a = build_argv(c);
    CHECK(std::find(a.begin(), a.end(), "--fire-light") != a.end(), "fire light: passed as --fire-light");
}

/* Lamp shadows (--lamp-shadows): saved as lamp_shadows; a profile saved before it existed as Quality
 * loads with it on and keeps its preset, others keep it off; typed in extra_args it moves into the
 * option. */
static void test_lamp_shadows_key() {
    ProfileFile f, g;
    Profile p;
    p.lamp_shadows = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].lamp_shadows && warn.empty(), "lamp shadows: ini round trip");
    CHECK(write_ini(f).find("lamp_shadows = true") != std::string::npos, "lamp shadows: saved as lamp_shadows");
    CHECK(!Profile().lamp_shadows, "lamp shadows: off by default");
    for (Preset pr : {Preset::Ultra, Preset::Gaming, Preset::Balanced}) {
        ProfileFile o, h;
        Profile q;
        apply_preset(q, pr, "");
        q.name = "q";
        o.profiles.push_back(q);
        const bool on = pr == Preset::Ultra;
        std::string ini = write_ini(o), key = on ? "lamp_shadows = true\n" : "lamp_shadows = false\n";
        const size_t at = ini.find(key);
        CHECK(at != std::string::npos, "lamp shadows: the preset writes the key");
        if (at != std::string::npos) ini.erase(at, key.size());
        CHECK(parse_ini(ini, h, nullptr) && h.profiles.size() == 1 && h.profiles[0].lamp_shadows == on && h.profiles[0].preset == preset_key(pr),
              "lamp shadows: an older %s profile takes the preset's value and keeps its preset", preset_key(pr));
    }
    Profile m;
    m.extra_args = "--lamp-shadows";
    migrate_extra_args(m);
    CHECK(m.lamp_shadows && m.extra_args.empty(), "lamp shadows: moved out of extra_args (%s)", m.extra_args.c_str());
    m.extra_args = "--no-lamp-shadows";
    migrate_extra_args(m);
    CHECK(!m.lamp_shadows && m.extra_args.empty(), "lamp shadows: --no-lamp-shadows moved out of extra_args too");
    Profile c;
    c.lamp_shadows = true;
    const std::vector<std::string> a = build_argv(c);
    CHECK(std::find(a.begin(), a.end(), "--lamp-shadows") != a.end(), "lamp shadows: passed as --lamp-shadows");
    c.lamp_shadows = false;
    const std::vector<std::string> b = build_argv(c);
    CHECK(std::find(b.begin(), b.end(), "--lamp-shadows") == b.end(), "lamp shadows: off passes nothing");
}

/* Real level lights (--level-lights, --level-lights-max 12 when cheap): saved as level_lights and
 * level_lights_cheap; Quality turns it on (no preset sets the cheap cap), the others off; a profile
 * saved before it existed as Quality takes the preset's value and keeps its preset;
 * typed in extra_args both move into the options. */
static void test_level_lights_key() {
    ProfileFile f, g;
    Profile p;
    p.level_lights = p.level_lights_cheap = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].level_lights && g.profiles[0].level_lights_cheap && warn.empty(), "level lights: ini round trip");
    CHECK(write_ini(f).find("level_lights = true") != std::string::npos && write_ini(f).find("level_lights_cheap = true") != std::string::npos,
          "level lights: saved as level_lights and level_lights_cheap");
    CHECK(!Profile().level_lights && !Profile().level_lights_cheap, "level lights: off by default");
    for (Preset pr : {Preset::Gaming, Preset::Balanced, Preset::Laptop}) {
        Profile q;
        apply_preset(q, pr, "");
        const bool on = pr == Preset::Gaming, cheap = false;
        CHECK(q.level_lights == on && q.level_lights_cheap == cheap && detect_preset(q) == pr, "level lights: preset %d sets %d/%d", (int)pr, on, cheap);
        Profile r = q;
        r.level_lights = !on;
        CHECK(detect_preset(r) == Preset::Custom, "level lights: preset %d with it flipped is custom", (int)pr);
        if (on) {
            r = q;
            r.level_lights_cheap = !cheap;
            CHECK(detect_preset(r) == Preset::Custom, "level lights: preset %d with the cap flipped is custom", (int)pr);
        }
        ProfileFile o, h;
        q.name = "q";
        o.profiles.push_back(q);
        std::string ini = write_ini(o);
        for (const char *k : {"level_lights_cheap", "level_lights"}) {
            const std::string key = std::string(k) + " = " + ((std::string(k) == "level_lights" ? on : cheap) ? "true\n" : "false\n");
            const size_t at = ini.find(key);
            CHECK(at != std::string::npos, "level lights: the preset writes %s", k);
            if (at != std::string::npos) ini.erase(at, key.size());
        }
        CHECK(parse_ini(ini, h, nullptr) && h.profiles.size() == 1 && h.profiles[0].level_lights == on && h.profiles[0].level_lights_cheap == cheap &&
                  h.profiles[0].preset == preset_key(pr),
              "level lights: an older profile of preset %d takes the preset's value and keeps its preset (%s)", (int)pr,
              h.profiles.empty() ? "" : h.profiles[0].preset.c_str());
    }
    Profile m;
    m.extra_args = "--level-lights --level-lights-max 12 --level-lights-max 20";
    migrate_extra_args(m);
    CHECK(m.level_lights && m.level_lights_cheap && m.extra_args == "--level-lights-max 20", "level lights: moved out of extra_args, another cap stays (%s)",
          m.extra_args.c_str());
    m.extra_args = "--no-level-lights";
    migrate_extra_args(m);
    CHECK(!m.level_lights && m.extra_args.empty(), "level lights: --no-level-lights moved out of extra_args too");
    Profile c;
    c.level_lights = true;
    std::vector<std::string> a = build_argv(c);
    CHECK(std::find(a.begin(), a.end(), "--level-lights") != a.end() && std::find(a.begin(), a.end(), "--level-lights-max") == a.end(),
          "level lights: passed as --level-lights, no cap");
    c.level_lights_cheap = true;
    a = build_argv(c);
    auto it = std::find(a.begin(), a.end(), "--level-lights-max");
    CHECK(it != a.end() && it + 1 != a.end() && *(it + 1) == "12", "level lights: cheap passes --level-lights-max 12");
    c.level_lights = false;
    a = build_argv(c);
    CHECK(std::find(a.begin(), a.end(), "--level-lights") == a.end() && std::find(a.begin(), a.end(), "--level-lights-max") == a.end(),
          "level lights: off passes nothing (cheap alone too)");
    CHECK(std::string(tr(S::level_lights, Lang::Es)) == "Luces reales del nivel" && std::string(tr(S::level_lights, Lang::En)) == "Real level lights",
          "level lights: Spanish and English names");
}

/* World shadows (--world-shadows, --world-shadows-size 1024 when low): saved as world_shadows and
 * world_shadows_low; a profile saved before they existed as Quality loads with world shadows on (the
 * low-cost map stays off) and keeps its preset; others keep them off; typed in extra_args they move
 * into the options. */
static void test_world_shadows_key() {
    ProfileFile f, g;
    Profile p;
    p.world_shadows = true;
    p.world_shadows_low = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].world_shadows && g.profiles[0].world_shadows_low && warn.empty(), "world shadows: ini round trip");
    CHECK(write_ini(f).find("world_shadows = true") != std::string::npos && write_ini(f).find("world_shadows_low = true") != std::string::npos,
          "world shadows: saved as world_shadows, world_shadows_low");
    CHECK(!Profile().world_shadows && !Profile().world_shadows_low, "world shadows: off by default");
    for (Preset pr : {Preset::Gaming, Preset::Balanced}) {
        ProfileFile o, h;
        Profile q;
        apply_preset(q, pr, "");
        q.name = "q";
        o.profiles.push_back(q);
        const bool on = pr != Preset::Balanced, low = false;
        std::string ini = write_ini(o);
        for (std::string key : {std::string(on ? "world_shadows = true\n" : "world_shadows = false\n"),
                                std::string(low ? "world_shadows_low = true\n" : "world_shadows_low = false\n")}) {
            const size_t at = ini.find(key);
            CHECK(at != std::string::npos, "world shadows: the preset writes %s", key.c_str());
            if (at != std::string::npos) ini.erase(at, key.size());
        }
        CHECK(parse_ini(ini, h, nullptr) && h.profiles.size() == 1 && h.profiles[0].world_shadows == on && h.profiles[0].world_shadows_low == low &&
                  h.profiles[0].preset == preset_key(pr) && detect_preset(h.profiles[0]) == pr,
              "world shadows: an older %s profile takes the preset's values and keeps its preset", preset_key(pr));
    }
    Profile m;
    m.extra_args = "--world-shadows --world-shadows-size 1024";
    migrate_extra_args(m);
    CHECK(m.world_shadows && m.world_shadows_low && m.extra_args.empty(), "world shadows: moved out of extra_args (%s)", m.extra_args.c_str());
    m.extra_args = "--world-shadows-size 2048 --no-world-shadows";
    migrate_extra_args(m);
    CHECK(!m.world_shadows && !m.world_shadows_low && m.extra_args.empty(), "world shadows: --no-world-shadows and size 2048 moved out too");
    m.extra_args = "--world-shadows-size 4096 --world-shadows-strength 0.5";
    migrate_extra_args(m);
    CHECK(m.extra_args == "--world-shadows-size 4096 --world-shadows-strength 0.5", "world shadows: other sizes and the strength stay typed (%s)",
          m.extra_args.c_str());
    Profile c;
    c.world_shadows = true;
    const std::vector<std::string> a = build_argv(c);
    CHECK(std::find(a.begin(), a.end(), "--world-shadows") != a.end() && std::find(a.begin(), a.end(), "--world-shadows-size") == a.end(),
          "world shadows: passed as --world-shadows (the runtime's 2048 map)");
    c.world_shadows_low = true;
    const std::vector<std::string> l = build_argv(c);
    const auto at = std::find(l.begin(), l.end(), "--world-shadows-size");
    CHECK(at != l.end() && at + 1 != l.end() && at[1] == "1024" && at > std::find(l.begin(), l.end(), "--world-shadows"),
          "world shadows: low passes --world-shadows-size 1024 after it");
    c.world_shadows = false;
    const std::vector<std::string> b = build_argv(c);
    CHECK(std::find(b.begin(), b.end(), "--world-shadows") == b.end() && std::find(b.begin(), b.end(), "--world-shadows-size") == b.end(),
          "world shadows: off passes nothing (low ignored)");
}

/* Light shafts (--light-shafts): saved as light_shafts; a profile saved before it existed as Quality
 * loads with it on and keeps its preset, others keep it off; typed in extra_args it moves into the
 * option. light_shafts_cheap passes --light-shafts-cheap, only with the shafts on. */
static void test_light_shafts_key() {
    ProfileFile f, g;
    Profile p;
    p.light_shafts = true;
    p.light_shafts_cheap = true;
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].light_shafts && g.profiles[0].light_shafts_cheap && warn.empty(), "light shafts: ini round trip");
    CHECK(write_ini(f).find("light_shafts = true") != std::string::npos && write_ini(f).find("light_shafts_cheap = true") != std::string::npos,
          "light shafts: saved as light_shafts and light_shafts_cheap");
    CHECK(!Profile().light_shafts && !Profile().light_shafts_cheap, "light shafts: off by default");
    for (Preset pr : {Preset::Gaming, Preset::Balanced}) {
        ProfileFile o, h;
        Profile q;
        apply_preset(q, pr, "");
        q.name = "q";
        o.profiles.push_back(q);
        const bool on = pr == Preset::Gaming;
        std::string ini = write_ini(o), key = on ? "light_shafts = true\n" : "light_shafts = false\n";
        const size_t at = ini.find(key);
        CHECK(at != std::string::npos, "light shafts: the preset writes the key");
        if (at != std::string::npos) ini.erase(at, key.size());
        CHECK(parse_ini(ini, h, nullptr) && h.profiles.size() == 1 && h.profiles[0].light_shafts == on &&
                  h.profiles[0].preset == (on ? "gaming" : "balanced"),
              "light shafts: an older %s profile takes the preset's value and keeps its preset", on ? "Quality" : "Balanced");
    }
    Profile m;
    m.extra_args = "--light-shafts --light-shafts-cheap";
    migrate_extra_args(m);
    CHECK(m.light_shafts && m.light_shafts_cheap && m.extra_args.empty(), "light shafts: moved out of extra_args (%s)", m.extra_args.c_str());
    Profile c;
    c.light_shafts_cheap = true;
    std::vector<std::string> a = build_argv(c);
    CHECK(std::find(a.begin(), a.end(), "--light-shafts") == a.end() && std::find(a.begin(), a.end(), "--light-shafts-cheap") == a.end(),
          "light shafts: off passes neither flag");
    c.light_shafts = true;
    a = build_argv(c);
    CHECK(std::find(a.begin(), a.end(), "--light-shafts") != a.end() && std::find(a.begin(), a.end(), "--light-shafts-cheap") != a.end(),
          "light shafts: passed as --light-shafts --light-shafts-cheap");
    c.light_shafts_cheap = false;
    a = build_argv(c);
    CHECK(std::find(a.begin(), a.end(), "--light-shafts-cheap") == a.end(), "light shafts: the full path passes no cheap flag");
}

/* Volumetric fog (--volfog): saved as volfog and volfog_quality; a profile saved before it existed as
 * Quality loads with it on and keeps its preset, others keep it off; typed in extra_args it moves into
 * the options. On it passes --volfog --volfog-quality (high unless low or medium is chosen). */
static void test_volfog_key() {
    ProfileFile f, g;
    Profile p;
    p.volfog = true;
    p.volfog_quality = "low";
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].volfog && g.profiles[0].volfog_quality == "low" && warn.empty(), "volfog: ini round trip");
    CHECK(write_ini(f).find("volfog = true") != std::string::npos && write_ini(f).find("volfog_quality = low") != std::string::npos,
          "volfog: saved as volfog and volfog_quality");
    CHECK(!Profile().volfog && Profile().volfog_quality.empty(), "volfog: off by default (high when turned on)");
    for (Preset pr : {Preset::Gaming, Preset::Balanced}) {
        ProfileFile o, h;
        Profile q;
        apply_preset(q, pr, "");
        q.name = "q";
        o.profiles.push_back(q);
        const bool on = pr == Preset::Gaming;
        std::string ini = write_ini(o), key = on ? "volfog = true\n" : "volfog = false\n";
        const size_t at = ini.find(key);
        CHECK(at != std::string::npos, "volfog: the preset writes the key");
        if (at != std::string::npos) ini.erase(at, key.size());
        CHECK(parse_ini(ini, h, nullptr) && h.profiles.size() == 1 && h.profiles[0].volfog == on && h.profiles[0].preset == (on ? "gaming" : "balanced"),
              "volfog: an older %s profile takes the preset's value and keeps its preset", on ? "Quality" : "Balanced");
    }
    Profile m;
    m.extra_args = "--volfog --volfog-quality medium";
    migrate_extra_args(m);
    CHECK(m.volfog && m.volfog_quality == "medium" && m.extra_args.empty(), "volfog: moved out of extra_args (%s)", m.extra_args.c_str());
    Profile c;
    c.volfog_quality = "low";
    std::vector<std::string> a = build_argv(c);
    CHECK(std::find(a.begin(), a.end(), "--volfog") == a.end() && std::find(a.begin(), a.end(), "--volfog-quality") == a.end(), "volfog: off passes neither flag");
    c.volfog = true;
    a = build_argv(c);
    auto it = std::find(a.begin(), a.end(), "--volfog-quality");
    CHECK(std::find(a.begin(), a.end(), "--volfog") != a.end() && it != a.end() && it + 1 != a.end() && it[1] == "low", "volfog: passed as --volfog --volfog-quality low");
    c.volfog_quality = "";
    a = build_argv(c);
    it = std::find(a.begin(), a.end(), "--volfog-quality");
    CHECK(it != a.end() && it + 1 != a.end() && it[1] == "high", "volfog: the default quality passes high");
}

/* gpu_vertex_programs: auto (default) leaves the runtime's GPU world vertex programs on, except on a T0
 * card with FSR off (integrated graphics are always T0), where add_auto_gpu_vertex_programs passes
 * --no-gpu-vertex-programs; on / off in the profile, or the flag typed in extra_args, win. */
static void test_gpu_vertex_programs_key() {
    CHECK(Profile().gpu_vertex_programs == "auto", "gpu vertex programs: auto by default");
    Profile p;
    CHECK(opts(p).find("gpu-vertex-programs") == std::string::npos, "gpu vertex programs: auto adds nothing by itself (%s)", opts(p).c_str());
    p.gpu_vertex_programs = "off";
    CHECK(opts(p) == "--no-gpu-vertex-programs", "gpu vertex programs: off (%s)", opts(p).c_str());
    p.gpu_vertex_programs = "on";
    CHECK(opts(p) == "--gpu-vertex-programs", "gpu vertex programs: on (%s)", opts(p).c_str());
    ProfileFile f, g;
    p.gpu_vertex_programs = "off";
    f.profiles.push_back(p);
    std::vector<std::string> warn;
    parse_ini(write_ini(f), g, &warn);
    CHECK(g.profiles.size() == 1 && g.profiles[0].gpu_vertex_programs == "off" && warn.empty(), "gpu vertex programs: ini round trip");
    GpuDetection t0, t2, none;
    t0.tier = "t0";
    t2.tier = "t2";
    none.tier = "unknown";
    Profile a;
    const size_t n0 = build_argv(a).size();
    std::vector<std::string> v = build_argv(a);
    add_auto_gpu_vertex_programs(v, a, t0);
    CHECK(v.size() == n0 + 1 && v.back() == "--no-gpu-vertex-programs", "gpu vertex programs: auto on T0 turns them off (%s)", join(v).c_str());
    for (const GpuDetection *d : {&t2, &none}) {
        v = build_argv(a);
        add_auto_gpu_vertex_programs(v, a, *d);
        CHECK(v.size() == n0, "gpu vertex programs: auto on %s keeps the runtime default (%s)", d->tier.c_str(), join(v).c_str());
    }
    /* With FSR on the card draws fewer pixels, so on T0 auto keeps them on the GPU (measured on an
     * Intel integrated GPU: CPU vertex programs 10-15 ms -> 0.02 ms per frame, 60 fps at FSR quality). */
    Profile fsr;
    fsr.fsr = "quality";
    const size_t nf = build_argv(fsr).size();
    v = build_argv(fsr);
    add_auto_gpu_vertex_programs(v, fsr, t0);
    CHECK(v.size() == nf, "gpu vertex programs: auto on T0 with FSR keeps the runtime default (%s)", join(v).c_str());
    Profile tup;   /* the temporal upscaler draws fewer pixels too */
    tup.taa_upscale = "balanced";
    const size_t nt = build_argv(tup).size();
    v = build_argv(tup);
    add_auto_gpu_vertex_programs(v, tup, t0);
    CHECK(v.size() == nt, "gpu vertex programs: auto on T0 with temporal upscaling keeps the runtime default (%s)", join(v).c_str());
    Profile on;
    on.gpu_vertex_programs = "on";
    v = build_argv(on);
    add_auto_gpu_vertex_programs(v, on, t0);
    CHECK(v.back() == "--gpu-vertex-programs" && join(v).find("--no-gpu-vertex-programs") == std::string::npos,
          "gpu vertex programs: an explicit on wins on T0 (%s)", join(v).c_str());
    Profile off;
    off.gpu_vertex_programs = "off";
    v = build_argv(off);
    add_auto_gpu_vertex_programs(v, off, t2);
    CHECK(v.back() == "--no-gpu-vertex-programs" && v.size() == n0 + 1, "gpu vertex programs: an explicit off stays off on T2, once (%s)", join(v).c_str());
    Profile typed;
    typed.extra_args = "--gpu-vertex-programs";
    v = build_argv(typed);
    add_auto_gpu_vertex_programs(v, typed, t0);
    CHECK(join(v).find("--no-gpu-vertex-programs") == std::string::npos, "gpu vertex programs: a typed flag wins on T0 (%s)", join(v).c_str());
}

/* The video-memory meter (vram.h): the frame targets follow the runtime's allocations exactly,
 * the title's surfaces scale with the render scale and the shadow scale squared, the packs count
 * with the share a game loads up to the texture cache budget, pack folders are measured from their
 * file headers, and two configurations stay within 15% of what the runtime measured. */
static void test_vram(const std::string &scratch) {
    const uint64_t MiB = 1ull << 20;
    Profile base;
    base.resolution = "1920x1080";
    base.widescreen = true;
    VramEnv env;
    VramEstimate e0 = estimate_vram(base, env);
    CHECK(e0.total() == e0[VramCat::Screen] + e0[VramCat::Shadows] + e0[VramCat::Textures] + e0[VramCat::Characters] + e0[VramCat::Other],
          "vram: the total is the sum of the categories (%llu)", (unsigned long long)e0.total());
    CHECK(e0[VramCat::Screen] > 0 && e0[VramCat::Shadows] > 0 && e0[VramCat::Textures] > 0 && e0[VramCat::Other] > 0,
          "vram: 1080p without options: screen %llu, shadows %llu, textures %llu, other %llu MiB",
          (unsigned long long)(e0[VramCat::Screen] / MiB), (unsigned long long)(e0[VramCat::Shadows] / MiB),
          (unsigned long long)(e0[VramCat::Textures] / MiB), (unsigned long long)(e0[VramCat::Other] / MiB));
    CHECK(e0[VramCat::Characters] == 0, "vram: no HD characters, nothing for them");

    /* MSAA 4 at 1920x1080: a 4-sample color target (4 bytes x 4) and the depth target from 1 to 4
     * samples (4 bytes x 3 more). */
    Profile msaa = base;
    msaa.msaa = 4;
    const uint64_t px = 1920ull * 1080;
    uint64_t d = estimate_vram(msaa, env)[VramCat::Screen] - e0[VramCat::Screen];
    CHECK(d == px * 4 * 4 + px * 4 * 3, "vram: MSAA 4 adds its color and depth samples (%llu)", (unsigned long long)d);

    /* Shadow scale 2 and supersampling 2: the title's surfaces x 4 (both axes). */
    Profile sh = base;
    sh.shadow_scale = 2;
    CHECK(estimate_vram(sh, env)[VramCat::Shadows] == 4 * e0[VramCat::Shadows], "vram: shadow scale 2 = 4x the surfaces");
    Profile ss = base;
    ss.supersample = 2;
    CHECK(estimate_vram(ss, env)[VramCat::Shadows] == 4 * e0[VramCat::Shadows], "vram: supersampling 2 = 4x the surfaces");
    CHECK(estimate_vram(ss, env)[VramCat::Screen] > 2 * e0[VramCat::Screen], "vram: supersampling 2 = about 4x the frame targets (the swapchain stays)");

    /* 4:3 at 1920x1080 is pillarboxed to 1440x1080: a smaller frame than 16:9. */
    Profile narrow = base;
    narrow.widescreen = false;
    CHECK(estimate_vram(narrow, env)[VramCat::Screen] < e0[VramCat::Screen], "vram: 4:3 output is smaller than 16:9 at 1080p");

    /* Effects add their targets. */
    Profile fx = base;
    fx.bloom = fx.ssao = fx.lamp_lights = fx.rain_hd = fx.contact_shadows = fx.filmic = true;
    CHECK(estimate_vram(fx, env)[VramCat::Screen] > e0[VramCat::Screen] + px * 8, "vram: effects add their targets");

    /* Packs: the share of a pack a game loads (36%), at most the budget. */
    VramEnv small = env, big = env;
    small.pack = big.pack = true;
    small.pack_bytes = 100 * MiB;
    big.pack_bytes = 6144 * MiB;
    uint64_t ts = estimate_vram(base, small)[VramCat::Textures], tb = estimate_vram(base, big)[VramCat::Textures];
    CHECK(ts == e0[VramCat::Textures] + 36 * MiB, "vram: a 100 MiB pack adds 36%% (%llu MiB)", (unsigned long long)((ts - e0[VramCat::Textures]) / MiB));
    CHECK(tb == VRAM_TEXTURE_BUDGET_PACK, "vram: a 6 GiB pack is capped by the 1 GiB budget (%llu MiB)", (unsigned long long)(tb / MiB));

    VramEnv hd = env;
    hd.hd_models = 40;
    CHECK(estimate_vram(base, hd)[VramCat::Characters] > 0, "vram: HD characters count");

    /* Fullscreen: the swapchain has the desktop's size. */
    Profile full = base;
    full.fullscreen = true;
    VramEnv desk = env;
    desk.desktop_w = 3840;
    desk.desktop_h = 2160;
    CHECK(estimate_vram(full, desk)[VramCat::Screen] > e0[VramCat::Screen], "vram: a 4K desktop swapchain in fullscreen");

    CHECK(vram_level(80, 100) == VramLevel::Ok && vram_level(81, 100) == VramLevel::High && vram_level(100, 100) == VramLevel::High &&
              vram_level(101, 100) == VramLevel::Over && vram_level(5000, 0) == VramLevel::Ok,
          "vram: levels at 80%% and 100%%");
    CHECK(format_vram_gb(1536 * MiB, true) == "1,5 GB" && format_vram_gb(1536 * MiB, false) == "1.5 GB" &&
              format_vram_gb(6144 * MiB, true) == "6,0 GB" && format_vram_gb(0, true) == "0,0 GB",
          "vram: GB with a decimal comma (%s, %s)", format_vram_gb(1536 * MiB, true).c_str(), format_vram_gb(6144 * MiB, true).c_str());

    /* Calibration (RTX 2060, headless runs, so without the swapchain): nvidia-smi's per-process
     * memory in the hub after 5000 frames. A: 1920x1080 16:9, no effects, no packs: 102 MiB.
     * B: the same with MSAA 4, shadow scale 2, bloom, filmic, ssao, lamp lights, contact shadows,
     * better rain with puddles and the HD characters: 440 MiB, of which the texture cache (the
     * v4 pack, one area) was 137 MiB, so 303 MiB without it. */
    const uint64_t swapchain = 4ull * 1920 * 1080 * 3;
    auto within = [](uint64_t est, uint64_t measured) { return est * 100 >= measured * 85 && est * 100 <= measured * 115; };
    Profile ca = base;
    ca.hud_style = "hd-glass";
    ca.sharp_fonts = false;
    uint64_t est_a = estimate_vram(ca, env).total() - swapchain;
    CHECK(within(est_a, 102 * MiB), "vram: calibration A within 15%% (%llu MiB, measured 102)", (unsigned long long)(est_a / MiB));
    Profile cb = ca;
    cb.msaa = 4;
    cb.shadow_scale = 2;
    cb.bloom = cb.filmic = cb.ssao = cb.lamp_lights = cb.contact_shadows = cb.rain_hd = cb.rain_puddles = true;
    VramEstimate eb = estimate_vram(cb, hd);
    uint64_t est_b = eb.total() - eb[VramCat::Textures] - swapchain;
    CHECK(within(est_b, 303 * MiB), "vram: calibration B without textures within 15%% (%llu MiB, measured 303)", (unsigned long long)(est_b / MiB));

    /* A pack folder: a 256x128 PNG (its header), and a hash with both a .dds (128-byte header) and a .png. */
    std::string dir = path_join(scratch, "vram pack");
    make_dir(dir);
    auto png = [](uint32_t w, uint32_t h) {
        std::string s("\x89PNG\r\n\x1a\n\0\0\0\x0dIHDR", 16);
        for (uint32_t v : {w, h})
            for (int i = 3; i >= 0; i--) s += (char)((v >> (8 * i)) & 0xff);
        s += std::string("\x08\x06\0\0\0", 5);
        return s;
    };
    write_file(path_join(dir, "00000000000000aa.png"), png(256, 128));
    write_file(path_join(dir, "00000000000000bb.png"), png(64, 64));
    std::string dds(128, '\0');
    dds.replace(0, 4, "DDS ");
    dds += std::string(1000, '\x55');
    write_file(path_join(dir, "00000000000000bb.dds"), dds);
    write_file(path_join(dir, "readme.txt"), "not a texture");
    uint64_t a = 43691ull * 4, b_png = 5461ull * 4;   /* the texels of their full mip chains, 4 bytes each */
    uint64_t with_bc = pack_device_bytes(dir, true), without = pack_device_bytes(dir, false);
    CHECK(with_bc == a + 1000, "vram: pack with BC: the .dds payload wins over its .png (%llu, want %llu)", (unsigned long long)with_bc,
          (unsigned long long)(a + 1000));
    CHECK(without == a + b_png, "vram: pack without BC: the .png files (%llu, want %llu)", (unsigned long long)without,
          (unsigned long long)(a + b_png));
    CHECK(pack_device_bytes(path_join(scratch, "no such pack"), true) == 0, "vram: a missing pack is 0");
}

/* The launcher's slow startup work (the GPU probe, the pack scans) runs on a worker: start returns
 * at once, ready() never blocks, and the value arrives unchanged; a cancelled scan stops early. */
static void test_async(const std::string &scratch) {
    std::atomic<bool> gate{false};
    Async<int> job;
    CHECK(!job.started() && !job.ready(), "async: a job not started is not ready");
    CHECK(job.start([&gate](const std::atomic<bool> &) {
              while (!gate) thread_yield();
              return 42;
          }),
          "async: the job starts");
    CHECK(!job.ready(), "async: ready() returns at once while the worker is still busy");
    CHECK(!job.start([](const std::atomic<bool> &) { return 1; }), "async: a started job is not started again");
    gate = true;
    job.wait();
    CHECK(job.ready() && job.get() == 42, "async: the worker's value once it is done (%d)", job.get());

    Async<int> cancelled;   /* shutdown: cancel() asks the worker to stop and joins it */
    cancelled.start([](const std::atomic<bool> &stop) {
        while (!stop) thread_yield();
        return 7;
    });
    cancelled.cancel();
    CHECK(cancelled.ready() && cancelled.get() == 7, "async: cancel() stops and joins a running worker");

    std::string dir = path_join(scratch, "async pack");
    make_dir(dir);
    std::string png("\x89PNG\r\n\x1a\n\0\0\0\x0dIHDR\0\0\0\x10\0\0\0\x10", 24);
    write_file(path_join(dir, "00000000000000aa.png"), png);
    const uint64_t sync = pack_device_bytes(dir, false);
    Async<uint64_t> scan;
    scan.start([dir](const std::atomic<bool> &stop) { return pack_device_bytes(dir, false, &stop); });
    scan.wait();
    CHECK(sync > 0 && scan.get() == sync, "async: a pack scan on the worker gives the same bytes (%llu, want %llu)",
          (unsigned long long)scan.get(), (unsigned long long)sync);
    std::atomic<bool> stop{true};
    CHECK(pack_device_bytes(dir, false, &stop) == 0, "async: a cancelled pack scan stops before reading the files");
}

/* Lua mods (docs/MODDING.md): the mods / mods_dir keys round trip, off by default (no --mod), the
 * argv (--mods only when set, and only with a mod enabled), the enabled list kept in the folder's
 * load order, and the folder scan through the runtime's own mod.ini parser. */
static void test_mods(const std::string &scratch) {
    Profile d;
    d.xbrun = "/opt/port/xbrun";
    d.disc = "/g/disc";
    CHECK(d.mods.empty() && d.mods_dir.empty() && join(build_argv(d)).find("--mod") == std::string::npos,
          "mods: off by default, no --mod or --mods");
    Profile p = d;
    p.mods = "extra-hats,trophies";
    std::string a = join(build_argv(p));
    CHECK(a.find("--mod|extra-hats,trophies") != std::string::npos && a.find("--mods|") == std::string::npos,
          "mods: --mod with the enabled ids, default folder left out (%s)", a.c_str());
    p.mods_dir = "C:\\My Mods";
    a = join(build_argv(p));
    CHECK(a.find("--mods|C:\\My Mods|--mod|extra-hats,trophies") != std::string::npos, "mods: --mods DIR before --mod (%s)", a.c_str());
    Profile q = d;
    q.mods_dir = "/m";
    CHECK(join(build_argv(q)).find("--mods") == std::string::npos, "mods: a folder alone passes nothing");

    ProfileFile f;
    f.profiles.push_back(p);
    ProfileFile g;
    std::vector<std::string> warn;
    CHECK(parse_ini(write_ini(f), g, &warn) && g.profiles.size() == 1 && g.profiles[0].mods == p.mods &&
              g.profiles[0].mods_dir == p.mods_dir,
          "mods: round trip of mods and mods_dir");
    ProfileFile e;
    e.profiles.push_back(d);
    CHECK(write_ini(e).find("\nmods = \nmods_dir = \n") != std::string::npos, "mods: empty keys while off");

    CHECK(mods_path(d) == "/opt/port/mods", "mods: default folder next to the game program (%s)", mods_path(d).c_str());
    Profile r = d;
    r.mods_dir = "my-mods";
    CHECK(mods_path(r) == "/opt/port/my-mods", "mods: a relative folder is relative to the program (%s)", mods_path(r).c_str());

    std::string dir = path_join(scratch, "mods folder");
    make_dir(dir);
    make_dir(path_join(dir, "20-trophies"));
    make_dir(path_join(dir, "10-hats"));
    make_dir(path_join(dir, "30-broken"));
    write_file(path_join(dir, "20-trophies/mod.ini"), "id = trophies\nname = Trophies\nname.es = Trofeos\nversion = 1.0\nsaves = no\n");
    write_file(path_join(dir, "10-hats/mod.ini"), "id = extra-hats\nname = More hats\nversion = 0.9\ndescription = Hats fly\n");
    write_file(path_join(dir, "30-broken/mod.ini"), "name = no id\n");
    ModList l = mods_scan(dir);
    CHECK(l.folder_ok && l.mods.size() == 2 && std::string(l.mods[0].id) == "extra-hats" && std::string(l.mods[1].id) == "trophies" &&
              std::string(l.mods[1].name_es) == "Trofeos",
          "mods: scan in folder order with names (%zu)", l.mods.size());
    CHECK(l.problems.size() == 1 && l.problems[0].find("30-broken") != std::string::npos, "mods: the invalid mod.ini is reported");
    CHECK(!mods_scan(path_join(scratch, "no such mods")).folder_ok, "mods: a missing folder");

    Profile t = d;
    mod_set_enabled(t, "trophies", true, l.mods);
    mod_set_enabled(t, "extra-hats", true, l.mods);
    CHECK(t.mods == "extra-hats,trophies", "mods: enabled list in load order (%s)", t.mods.c_str());
    mod_set_enabled(t, "extra-hats", false, l.mods);
    CHECK(t.mods == "trophies" && !mod_enabled(t, "extra-hats") && mod_enabled(t, "trophies"), "mods: disable one (%s)", t.mods.c_str());
    t.mods = "gone,trophies";
    mod_set_enabled(t, "extra-hats", true, l.mods);
    CHECK(t.mods == "extra-hats,trophies,gone", "mods: an id missing from the folder stays enabled (%s)", t.mods.c_str());
    t.mods = "trophies, Bad Id,trophies";
    CHECK(mods_enabled(t).size() == 1, "mods: invalid and repeated ids ignored");
}

/* Mod safety (docs/MODDING.md "Safety"): the runtime's save backups for mods listed and restored,
 * the Mods page's safety checks, the crash-with-mods notice and its crash_seen key. */
static void test_mod_safety(const std::string &scratch) {
    std::string base = path_join(scratch, "mod-safety"), hdd = path_join(base, "hdd");
    remove_tree_test(base);
    make_dirs(base);
    std::string save = save_folder(hdd), root = mods_backup_root(hdd), text;
    CHECK(root == path_join(hdd, "mods-backups"), "mod safety: backups in <hdd>/mods-backups (%s)", root.c_str());
    CHECK(list_backups(root).empty(), "mod safety: no backups yet");
    make_dirs(path_join(save, "0123456789AB"));
    write_file(path_join(path_join(save, "0123456789AB"), "SaveMeta.xbx"), "live");
    /* two backups as runtime/src/modsafe.c writes them (hle_mods_test checks that side) and a cut-short one */
    auto runtime_backup = [&](const std::string &name, const std::string &content) {
        std::string b = path_join(root, name), d = path_join(path_join(b, "save"), "0123456789AB");
        make_dirs(d);
        write_file(path_join(d, "SaveMeta.xbx"), content);
        write_file(path_join(b, "backup.txt"), "kind = mods\nfiles = 1\nbytes = " + std::to_string(content.size()) +
                                                   "\nsignature = 0123456789abcdef\n");
    };
    runtime_backup("20261117-100000", "oldest");
    runtime_backup("20261117-110000", "newer");
    make_dirs(path_join(path_join(root, "20261117-120000.partial"), "save"));
    std::vector<BackupInfo> list = list_backups(root);
    CHECK(list.size() == 2 && list[0].name == "20261117-110000" && list[0].kind == "mods" && list[0].files == 1 &&
              list[0].bytes == 5 && list[1].name == "20261117-100000",
          "mod safety: the runtime's backups are listed newest first, the cut-short one left out (%zu)", list.size());
    std::string undo, err;
    bool ok = restore_backup(save, root, list[1], "20261117-130000", &undo, &err);
    CHECK(ok && read_file(path_join(path_join(save, "0123456789AB"), "SaveMeta.xbx"), text, 64) && text == "oldest",
          "mod safety: a chosen (older) backup is restored (%s)", err.c_str());
    list = list_backups(root);
    CHECK(list.size() == 3 && list[0].kind == "restore" && list[0].path == undo &&
              read_file(path_join(path_join(path_join(undo, "save"), "0123456789AB"), "SaveMeta.xbx"), text, 64) && text == "live",
          "mod safety: the save before the restore is kept with the mods backups, so it can be undone");

    /* the Mods page's checks: a leftover and a too-new min_runtime */
    std::string dir = path_join(base, "mods");
    make_dirs(path_join(path_join(dir, "40-bandana"), "meshes"));
    make_dirs(path_join(path_join(dir, "40-bandana"), "meshes-v2"));
    make_dirs(path_join(dir, "50-future"));
    write_file(path_join(path_join(dir, "40-bandana"), "mod.ini"), "id = bandana\nname = Bandana\nversion = 1.2\nmeshes = meshes-v2\nboot = b.lua\n");
    write_file(path_join(path_join(dir, "40-bandana"), "b.lua"), "x = 1");
    write_file(path_join(path_join(dir, "50-future"), "mod.ini"), "id = future\nname = Future\nversion = 1\nmin_runtime = 99.0\nboot = f.lua\n");
    ModList ml = mods_scan(dir);
    CHECK(ml.mods.size() == 2 && ml.checks.size() == 2 && ml.checks[0].runtime_ok && ml.checks[0].leftovers.size() == 1 &&
              ml.checks[0].leftovers[0] == "meshes" && ml.checks[0].missing.empty() && !ml.checks[1].runtime_ok &&
              ml.checks[1].missing.size() == 1 && ml.checks[1].missing[0] == "f.lua",
          "mod safety: the page's checks (an old meshes folder left over; a too-new min_runtime, a missing Lua file)");

    /* the crash-with-mods notice */
    std::vector<std::string> crash = {"[mods] loaded 2: protection-money 1.0 (changes save data), warriors-mode 1.0 (changes save data)",
                                      "[crash-context] reason: SIGSEGV (last 3 breadcrumbs, oldest first)",
                                      "[breadcrumb] -1.000s frame 10: level load level95",
                                      "[crash-context] level level95, mission 0, frame 10, uptime 3.0 s, guest arena used 1 MiB of 2 MiB, threads 9",
                                      "[crash-context] mods: protection-money 1.0, warriors-mode 1.0\r"};
    std::vector<std::string> got = crash_mods(crash);
    CHECK(join(got) == "protection-money|warriors-mode", "mod safety: the crash dump's mods line (%s)", join(got).c_str());
    std::vector<std::string> clean = {"[mods] loaded 1: look 1.0", "[exit] the game returned 0"};
    std::vector<std::string> nomods = {"[crash-context] reason: SIGSEGV", "[crash-context] level -, mission -, frame 1"};
    CHECK(crash_mods(clean).empty() && crash_mods(nomods).empty() && crash_mods({crash[4]}).empty(),
          "mod safety: no notice for a clean exit, a crash without mods, a mods line without a crash");
    CHECK(crash_notice_due(false, "/l/a.log", "", got) && !crash_notice_due(true, "/l/a.log", "", got) &&
              !crash_notice_due(false, "/l/a.log", "/l/a.log", got) && !crash_notice_due(false, "/l/a.log", "", {}) &&
              crash_notice_due(false, "/l/b.log", "/l/a.log", got),
          "mod safety: the notice shows once per crashed log, never while the game runs");
    ProfileFile f, g;
    f.profiles.push_back(Profile());
    f.profiles[0].name = "p";
    f.settings.crash_seen = "C:\\logs\\p-20261117.log";
    std::vector<std::string> warn;
    CHECK(parse_ini(write_ini(f), g, &warn) && g.settings.crash_seen == f.settings.crash_seen &&
              write_ini(ProfileFile()).find("crash_seen") == std::string::npos,
          "mod safety: crash_seen round trip, left out while empty");
    remove_tree_test(base);
}

/* "Preparing mods" (mods.h): the enabled mods whose assets.ini cache is not current, built with the
 * runtime's engine on a synthetic disc folder (an empty XBoxWad.idx: a [hide] needs no game file). */
static void remove_all(const std::string &path) {   /* a scratch folder and everything in it */
    std::vector<DirEntry> es;
    if (list_dir(path, es))
        for (const DirEntry &e : es) e.dir ? remove_all(path_join(path, e.name)) : (void)remove(path_join(path, e.name).c_str());
    remove_dir(path);
}

static void test_mod_assets(const std::string &scratch) {
    remove_all(path_join(scratch, "mod-assets"));   /* the cache of an earlier run */
    std::string root = path_join(scratch, "mod-assets"), mods = path_join(root, "mods"), disc = path_join(root, "disc"), hdd = path_join(root, "hdd");
    make_dir(root), make_dir(mods), make_dir(disc), make_dir(hdd);
    const char idx[20] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 20, 0, 0, 0, 20, 0, 0, 0};
    write_file(path_join(disc, "XBoxWad.idx"), std::string(idx, 20));
    const char *names[4] = {"10-hats", "20-bad", "30-plain", "40-off"}, *ids[4] = {"hats", "bad", "plain", "off"};
    for (int i = 0; i < 4; i++) {
        std::string d = path_join(mods, names[i]);
        make_dir(d);
        write_file(path_join(d, "mod.ini"), std::string("id = ") + ids[i] + "\nname = " + ids[i] + "\nname.es = es-" + ids[i] + "\nversion = 1\n");
    }
    write_file(path_join(path_join(mods, "10-hats"), "assets.ini"), "format = 1\n[hide worn]\nhost = dyn_dest_ltb\n");
    write_file(path_join(path_join(mods, "20-bad"), "assets.ini"), "format = 1\n[hide worn]\nhost = nothing\n");
    write_file(path_join(path_join(mods, "40-off"), "assets.ini"), "format = 1\n[hide worn]\nhost = dyn_dest_ltb\n");
    Profile p;
    ModList found = mods_scan(mods);
    mod_set_enabled(p, "hats", true, found.mods), mod_set_enabled(p, "bad", true, found.mods), mod_set_enabled(p, "plain", true, found.mods);
    std::vector<ModAssetsJob> due = mods_assets_due(found, mods_enabled(p), mods, disc, hdd);
    CHECK(due.size() == 2 && due[0].id == "hats" && due[1].id == "bad" && due[0].name_es == "es-hats" && due[0].folder == path_join(mods, "10-hats"),
          "mod assets: due for the enabled mods with an assets.ini, in load order (not the disabled one, not one without)");
    ModAssetsResult a = due.size() == 2 ? mods_assets_build(due[0], disc, hdd) : ModAssetsResult();
    CHECK(a.ok && a.built && a.error.empty(), "mod assets: built");
    std::vector<DirEntry> keys;
    const std::string cache = path_join(path_join(hdd, "mods-cache"), "hats");
    CHECK(list_dir(cache, keys) && keys.size() == 1 && is_file(path_join(path_join(cache, keys.empty() ? "" : keys[0].name), "6830d7eb9625cfad.hide")),
          "mod assets: the hide file in <hdd>/mods-cache/<id>/<key>");
    ModAssetsResult b = due.size() == 2 ? mods_assets_build(due[1], disc, hdd) : ModAssetsResult();
    CHECK(!b.ok && b.error.find("assets.ini:3: unknown host object nothing") != std::string::npos, "mod assets: a failure says file:line and why");
    due = mods_assets_due(found, mods_enabled(p), mods, disc, hdd);
    CHECK(due.size() == 1 && due[0].id == "bad", "mod assets: a current cache is skipped; the failed one stays due");
    CHECK(mods_assets_due(found, mods_enabled(p), mods, path_join(root, "no disc"), hdd).size() == 2, "mod assets: without the disc folder every recipe is due (the build reports it)");
    CHECK(mods_assets_build(ModAssetsJob{"hats", "hats", "", path_join(mods, "10-hats"), "1"}, path_join(root, "no disc"), hdd).error.find("XBoxWad.idx") !=
              std::string::npos,
          "mod assets: no disc folder: a clear reason");
    CHECK(std::string(tr(S::mods_preparing_n, Lang::En)) == "Preparing mods… %d of %d", "mod assets: the progress string");
    CHECK(std::string(tr(S::mods_preparing_n, Lang::Es)) == "Preparando mods… %d de %d" && std::string(tr(S::mod_assets_failed, Lang::Es)).find("tus archivos") != std::string::npos,
          "mod assets: Spanish strings (neutral tú)");
}

int main() {
    std::string scratch = path_join(exe_dir(), "test-scratch");
    make_dir(scratch);
    test_async(scratch);
    test_char_lighting_key();
    test_world_lighting_key();
    test_fire_light_key();
    test_lamp_shadows_key();
    test_level_lights_key();
    test_world_shadows_key();
    test_light_shafts_key();
    test_volfog_key();
    test_gpu_vertex_programs_key();
    test_ini_round_trip();
    test_ini_parsing();
    test_argv();
    test_quoting();
    test_xbe(scratch);
    test_child(scratch);
    test_recommended(scratch);
    test_glyph_auto(scratch);
    test_find_game_exe(scratch);
    test_appimage_data_dir(scratch);
    test_argv_v2();
    test_ini_v2();
    test_migration();
    test_presets(scratch);
    test_balanced_preset(scratch);
    test_intel_integrated_recommendation();
    test_integrated_preset();
    test_integrated_detection();
    test_retired_iris_xe_key();
    test_preset_outputs_pinned();
    test_gpu_classifier();
    test_gpu_tiers();
    test_png();
    test_iso(scratch);
    test_titleimage(scratch);
    test_i18n();
    test_keymap(scratch);
    test_pad_remap(scratch);
    test_backup(scratch);
    test_mod_safety(scratch);
    test_mod_assets(scratch);
    test_report(scratch);
    test_sharpen_key();
    test_bloom_key();
    test_rain_key();
    test_ssao_key();
    test_lamp_lights_key();
    test_contact_puddles_keys();
    test_soft_particles_key();
    test_filmic_key();
    test_auto_exposure_key();
    test_cutscene_dof_key();
    test_taa_key();
    test_frame_gen_key();
    test_taa_upscale_key();
    test_taa_upscale_native();
    test_upscaler_key();
    test_upscaler_pills();
    test_taa_sharpness_key();
    test_hdr_saturation_keys();
    test_dynres_key();
    test_vrs_key();
    test_anim_interp_key();
    test_wet_light_streaks_key();
    test_materials_key();
    test_damp_streets_key();
    test_reflections_key();
    test_neon_reflections_key();
    test_normal_maps_key();
    test_dry_highlights_key();
    test_hair_key();
    test_skin_key();
    test_hair_and_skin();
    test_eyes_cloth_keys();
    test_parallax_key();
    test_relief_pack();
    test_sky_hd_key();
    test_sharp_fonts_key();
    test_audio_3d_keys();
    test_sharp_faces_key();
    test_neon_glow_key();
    test_sharp_faces_over_pack_key();
    test_fps_overlay_key();
    test_perf_overlay();
    test_skip_intro_key();
    test_gfx_preset_union();
    test_shadow_range_key();
    test_uiscale();
    test_play_rule();
    test_path_message_debounce();
    test_padmap();
    test_controllers();
    test_characters_hd(scratch);
    test_vram(scratch);
    test_mods(scratch);
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("ALL OK\n");
    return 0;
}
