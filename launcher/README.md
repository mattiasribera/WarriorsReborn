# warriors-launcher

A small GUI (Dear ImGui on SDL2 + SDL_Renderer) that runs the port without typing the
command line, made for players: plain-language settings, presets and a first-run wizard. Each profile holds the game executable (`TheWarriors.exe` on Windows, `xbrun` on Linux), your own game copy (disc and hdd folders) and
a set of runtime options; Play builds the `xbrun <disc>/default.xbe <disc> [options]` command,
starts it and writes its output to a log file.

The launcher never contains or downloads game data. It reads the header of your
`default.xbe` to check that the disc folder holds The Warriors, and its boot splash or title image
for the background (see Background).

## Build

Both builds are 32-bit, like xbrun, and use zig (as `runtime/Makefile` does).

```sh
make -C launcher sdl2-devel   # once: SDL2 2.32.10 headers + import library into ~/.cache/launcher
make -C launcher              # build/warriors-launcher (Linux) and build/launcher_test
make -C launcher windows      # build/warriors-launcher.exe
./launcher/build/launcher_test
```

Linux needs the i386 SDL2 runtime (`sudo apt install libsdl2-2.0-0:i386`). On Windows, put
`warriors-launcher.exe` next to `TheWarriors.exe` (or in a `launcher\` folder next to it); it
uses the same 32-bit `SDL2.dll`. The font (Inter, `third_party/inter`, SIL Open Font License) is
embedded in the binary at build time (`tools/embed_files.py`, needs `python3`), so the launcher
is one file; ship `third_party/inter/LICENSE.txt` with it as `licenses/Inter-LICENSE.txt`
(see Third-party software).

The icon of `warriors-launcher.exe`, `TheWarriors.exe` and the launcher window is the port's own
Wonder Wheel (a geometric Ferris wheel, no game artwork): `assets/icon/wonder-wheel.ico`
(16 to 256 px) and `wonder-wheel.svg`, both made by `python3 tools/icon/make_icon.py`.

## Run

```sh
warriors-launcher [profiles.ini]
warriors-launcher --smoke <ini>                # draw a few frames and exit 0 (SDL_VIDEODRIVER=dummy for CI)
warriors-launcher --print-argv <ini> [profile] # the argv Play starts, one argument per line
warriors-launcher --play <ini> [profile]       # Play without the window; exits with the game's code
warriors-launcher --report <ini> [profile]     # print the bug report of the last log (as "Copy report for a bug")
warriors-launcher --capture <page> <png> [ini] # draw one page off-screen into a PNG (never writes the ini)
warriors-launcher --extract-iso <iso> <dir>    # check and extract your disc image (exit 0 ok, 1 failed,
                                               # 3 not an Xbox image or not The Warriors)
```

`--capture` pages: `home`, `display` (with the HUD customize rows open), `game`, `controls` (with
a made-up controller in the preview), `padmap` and `padmap-save` (its set-up at LT and at Save), `log` (Home with the log viewer of the profiles file's
`last_log`), `options` (with
the advanced options shown), `wizard1` to `wizard4`, and the disc image step with a made-up image
(no file is read): `wizard2-iso` (chosen), `wizard2-extract` (extracting), `setup-iso` (the Game
page's fold-out); `page:HEIGHT` (e.g. `display:1900`) draws a taller window to see a whole page,
a `-es` suffix (`controls-es:2200`) draws it in Spanish, `page:WxH` another window size and
`page@1.25` / `@1.5` the interface size (`options-es@1.5:1320x660`).
The launcher v2 names still work: `play` (Home), `graphics` and `hud` (Display), `gameplay` and
`setup` (Game), `advanced` (Options). It uses SDL's software renderer, so it needs no display.
A capture of a profile whose game copy is installed shows that game's title art: keep such
images to yourself.

### The window

A sidebar on the left lists the pages (Home, Display, Game, Controls, Options); at its bottom are the
profile, the game status in plain words and a big PLAY button, on every page. The pages are
translucent cards of settings rows over the background: each row has a name and a one-line
explanation on the left and its control on the right, segmented buttons for choices and switches
for on/off settings; the demanding values say "Heavy on laptops". Keyboard and gamepad
navigation work (arrows / D-pad, Enter / A, Esc / B). Changes are saved automatically.

The launcher speaks English or Spanish: Options, "Launcher language" (stored as `ui_language`
in the `[launcher]` section). Without that setting it follows the system: Spanish when the
Windows display language (`GetUserDefaultUILanguage`) or `LC_ALL` / `LC_MESSAGES` / `LANG` on
Linux is Spanish, else English. It is independent of the game's language (Game page,
`--language`). The texts live in `src/i18n_strings.h` (one `X(id, English, Spanish)` line each;
`launcher_test` checks that every id has both and that their `%s` / `%d` placeholders match).

2-player needs no setting: connect two controllers (before PLAY or while playing). The first one
and the keyboard play player 1, the second one player 2 (press Start on it in a story mission, or
choose Vs. in Quick Rumble). See "2-player" in `runtime/README.md`.

- **Home**: THE WARRIORS with the status ("Ready to play", or what is missing and a button to
  the page that fixes it), three summary cards (display mode and resolution, frame rate and
  V-Sync, game language) that open their page, the last game session with "View log", "Open last
  log" and "Close the launcher on PLAY". "View log" shows the last 2000 lines of the last game log
  in the launcher (followed while the game writes it), with a filter for warnings and errors and
  **Copy report for a bug** (see below).
- **Display**: the presets **Performance**, **Balanced** and **Quality** (and **Custom** once a
  covered setting differs); Screen: window mode (Windowed, or Fullscreen = desktop fullscreen,
  `--fullscreen`), resolution (720p to 4K, the monitor's own "Screen" when it is not one of them,
  or Original 640x480 with a render scale), widescreen; Performance: frame rate **30** or **60**
  (`--sim60`), V-Sync (off = `--present immediate`), frames in flight; Quality: anti-aliasing
  (MSAA 2x/4x/8x), supersampling, texture filtering (`--aniso`), shadow quality, soft shadows,
  shadow range (`--shadow-range`: character shadows no longer fall on floors far below them),
  sharpness (Off, 25, 50, 75, 100: `--sharpen N`, contrast-adaptive sharpening), bloom
  (`--bloom`, with a strength slider while it is on: `--bloom-strength F`), better rain
  (`--rain-hd`: while it rains, fine streaks at several depths instead of the flat overlay),
  draw distance (Original = as shipped; Far, Farther, Maximum = `--draw-distance 1.5/2/3
  --prop-detail 2/4/8`: fog and scenery reach farther, small props stay visible farther),
  ambient occlusion (`--ssao`: soft contact shading in corners and under people and objects,
  with a strength slider while it is on: `--ssao-strength F`), contact shadows (`--contact-shadows`:
  a short soft shadow on the ground right under people and objects), puddles (under better rain,
  shown while it is on: `--rain-hd-puddles`, puddles with drop ripples on the open wet street; on in Balanced and Quality), lamp lights (`--lamp-lights`: street
  lamps, fire and neon light the world around them, with a strength slider while it is on:
  `--lamp-lights-strength F`), per-pixel scene lighting ("Iluminación del escenario por píxel" in
  Spanish; `--world-lighting`: the lamp lights fall on walls, floors and buildings per pixel, following
  their surfaces, on top of the original baked light; Quality only), fire light ("Luz del fuego (tachos)"
  in Spanish; `--fire-light`: burning barrels add a warm flickering light to the scene and character
  lighting; Quality only), soft particles (`--soft-particles`: smoke, fire and dust fade where
  they meet the ground and walls; opt-in, no preset turns it on), the on-screen counter (its own card, "Contador en pantalla" in Spanish: "Show counter" / "Mostrar contador"
  passes `--fps-overlay`, then any of "FPS", "Milliseconds per frame (16.6 ms)", "CPU usage (%)" and "GPU usage (%)"
  (`--fps-overlay-items`, FPS only when turned on here; CPU 100% = every logical core, GPU = the game's own GPU
  work per frame, not the whole card's utilization) and its corner (`--fps-overlay-corner`); off in every preset and
  not part of the preset match),
  cinematic look ("Look cinematográfico" in Spanish; `--filmic`: the 1979-film look of the 3D
  scene, warm lights with a red glow, teal shadows and deep blacks; the HUD and text unchanged);
  enhanced sky (`--sky-hd`: a procedural night sky in each level's colours with stars, drifting
  clouds, a moon and lit windows on the distant skyline);
  sharp text ("Texto nítido" in Spanish; on in every preset, `--no-sharp-fonts` when off: the
  menu, HUD, mission and subtitle letters rebuilt at 4x from the game's own font atlases, the
  button icons unchanged);
  neon glow ("Brillo de neones"; `--neon-glow`, with its strength: lit and neon signs glow at night
  and through fog and feed the bloom; on in Balanced and Quality; the `neon-masks` folder next to the
  game is passed as a mask-only `--texture-pack` after the texture pack while it is on);
  sharp faces ("Caras nítidas"; `--sharp-faces`: character skins upscaled 2x at runtime from the
  game's own textures; a texture pack's version wins);
  HUD: the health-bar style (Original, HD, HD Glass, Glass) and, for the HD styles, Customize
  (empty-part and fill opacity, frost, height). No higher frame rates and no frame
  interpolation: the runtime runs the game at 30 or 60 updates per second.
- **Game**: the game files (status, "Install from a disc image (.iso)", "Open folder", "Run the
  setup wizard"; the game program, your game folder with the `default.xbe` check and the save
  folder, each with Browse on Windows, or on Linux when `zenity` is installed); the game
  language, the text fixes and the spray can duration; the optional texture pack, sound pack and
  extra arguments.
- **Controls**: Keyboard & mouse: every game action (movement, attacks, grab, pick up, the
  triggers and the Black/White buttons, stick clicks, the car-theft spin, camera, Start/Back and
  the D-pad) with two key slots drawn as key caps or a small mouse with the bound button in red.
  Click a slot (or focus it and press Enter), then press a key or mouse button; Esc cancels,
  right-click clears a slot, Esc and F1 are refused (the game uses them). A key that already had
  another action moves to the new one and the page says which action lost it; actions left
  without a key say "Not bound". "Reset to defaults" restores the built-in PC layout. Every
  change is saved at once to the key map file the game reads: the profile's key map file
  (`input_config`, relative to the game program's folder), else `warriors-input.cfg` next to the
  game program (the runtime's default, as the game runs in that folder). The file is written
  whole (every binding, `none` for default keys left unbound, the mouse and spin settings), so
  older hand-written files load and keep working. Controller: the first SDL game controller's
  name and a live drawing of its buttons, sticks and triggers, a vibration switch (off =
  `--no-rumble`) with a Test button. **Set up this controller** fixes a pad SDL maps wrongly (LT
  read as LB, A and B swapped...) or does not know at all: it asks for each Xbox input in turn (A,
  B, X, Y, LB, RB, LT, RT, Back, Start, Guide, L3, R3, the D-pad, then each stick right and up)
  with the part ringed on the drawing, and reads the raw SDL joystick (buttons, axes, hats), not
  the current mapping. Triggers are taken when let go, from where they rest (-32768, 0, or two on
  one axis); an axis counts only past a quarter of its range, so jitter is ignored; everything must be let go before the
  next step counts, and an input already used for another step is refused. Skip leaves an input
  out (Guide is often missing), Back redoes the previous one, Cancel or Esc stops. Save writes a
  standard SDL mapping line (`GUID,name,a:b0,...,lefttrigger:a2,...,platform:Windows,`) to
  `gamecontrollerdb.txt` next to the game program (replacing this controller's line for this
  platform, other lines and comments kept), and the launcher uses it at once; the runtime reads
  that file at start (see `runtime/README.md`). The controller then shows "Custom mapping" and
  **Reset to default**, which removes the line and goes back to SDL's own mapping. Save and Reset
  restart SDL's controller support (every controller handle closed, SDL's own mappings reloaded,
  the file's lines re-added), so the preview follows at once also on Windows, where adding a line
  to an open pad whose GUID carries the name's CRC does not change it. Gamepad navigation is off during the set-up. In-game button icons: Auto, Xbox, Keyboard & mouse,
  and an optional button-icon pack folder.
- **Options**: profiles (select, New, Duplicate, Rename, Delete); the launcher language, the
  interface size (see below), "Close the launcher on PLAY" and "Show advanced options"; Save backups (see below); with the latter, renderer, present mode, typed resolution, render
  targets, audio and video backends, `--stats`, `--trace`, frame lock, rate fixes, spawn scale,
  warp, key map file and the command with Copy; and About.

### Interface size

Options, "Interface size": Auto (the default), 1x, 1.25x or 1.5x (`ui_scale` in `[launcher]`:
`1`, `1.25`, `1.5`; not written for Auto), applied at once. The pages are laid out in logical
pixels (the 1x layout) and drawn at the scale (`SDL_RenderSetScale`), with the fonts rasterized at
it (`ImFontConfig::RasterizerDensity`), so text stays sharp and every part grows together. Auto
takes the display's DPI (96 = 1x; on Linux, when the X server reports no screen size,
`GDK_SCALE` / `QT_SCALE_FACTOR`) in steps of 0.25 up to 2x, lowered until a 1000x680 logical
window fits the usable screen: a 1366x768 laptop stays at 1x, 150% on a 1440p screen gives 1.5x,
200% on a 4K screen 2x. The window opens at 1120x760 times the scale (inside the screen) and its
minimum is 880x440 times it, so 1.5x fits a 1366x768 screen (1320x660 at least): there the sidebar
tightens (smaller PLAY, no PROFILE caption, page entries sharing the height), the rows' label
column narrows and the profile buttons wrap. On Windows the launcher is per-monitor DPI aware
(`SDL_WINDOWS_DPI_AWARENESS=permonitorv2`): Windows no longer stretches it as a bitmap, Auto
scales it instead. `--capture page@1.5:WxH` draws a page at a scale and size.

### Bug reports

"Copy report for a bug" puts a plain-text report on the clipboard: the launcher's version (`git
describe` of the release tag at build time, else the release number; also in Options, About), the runtime's (`[start] xbrun <version>` in the
log), the OS, the GPU and driver (`[renderer] Vulkan on <device> (driver X.Y.Z)` in the log), the
command Play starts and the last 200 log lines. Before it is copied, the profile's paths (game
copy, save folder, texture pack, sound pack, button-icon pack, key map file, the game program's folder, the
launcher's folder and logs) are replaced by `<game>`, `<save-folder>` and similar labels, the
home folder by `~`, and the user and computer names by `<user>` and `<host>` (paths match with
either slash and any case; names only as whole words, and only from 3 characters). Nothing is
sent anywhere: the player pastes it where they report the bug.

### Save backups

Before each PLAY the launcher copies the game's save, the title's folder on the emulated hard
disk (`<save folder>/partition1/UDATA/5454009b`, the save folder being the profile's `hdd`, else
`hdd` next to the game program), into
`$XDG_DATA_HOME/warriors-launcher/backups/save-<hash of that path>/<YYYYmmdd-HHMMSS>/` (default
`~/.local/share/...`; `%LOCALAPPDATA%\warriors-launcher\backups\...` on Windows): the copied
files under `save/` (with their modification times) and a `backup.txt` (kind, files, bytes, a
signature of the file names, sizes and times). A save whose signature equals the newest backup's
is not copied again, so PLAY costs a folder listing when nothing changed. The newest 5 are kept
(Options, "Backups to keep", 1 to 20; `backup_count`), and "Back up the save before PLAY"
(`backup_saves`) turns it off. A failed backup never keeps the game from starting; the log's
header says what happened (`# save backup: made ... | unchanged | no save yet | failed: ...`).

Options lists the backups (date, size, files) with **Restore**, after a confirmation: the current
save is backed up first (marked "Before a restore"; restoring that one undoes the restore), the
backup is copied next to the save folder and the two folders are swapped, so a failure leaves the
current save in place. "Open folder" opens the backup folder. Only the title's save folder is
read and replaced; other titles' folders and the rest of the hard disk are never touched, and
nothing is deleted outside the backup folder. A copy cut short by closing the launcher is left as
`<name>.partial` and removed at the next backup.

### Background

The background is art from YOUR installed game, read from the profile's game folder when the game
copy checks out, decoded by `src/titleimage.cpp`, blurred and darkened:

1. The boot splash (the gang over Coney Island, with the autosave notice): the `Main.wad1`
   entry with name hash `0xFDF31D9F` in `XBoxWad.idx`, a 512x512 DXT3 texture whose top 512x384
   is the picture. Scaled to 768x576 with a light blur, so the figures and colours stay
   recognizable and the notice text is not readable.
2. Otherwise the title image in the `$$XTIMAGE` section of `default.xbe` (a 128x128 DXT1
   texture in an XPR0 bundle), scaled to 384 pixels with a heavy blur.
3. Otherwise flat dark grey (Options, About says why).

The result is cached as `bg-<look>-<hash>.png` in `$XDG_CACHE_HOME/warriors-launcher` (default
`~/.cache/warriors-launcher`) or `%LOCALAPPDATA%\warriors-launcher` on Windows; nothing is written
next to the game or the launcher. No game image is part of the launcher or this repository.

The first run (no profiles file, or `setup_done = 0`) opens a four-step wizard instead: find the
game program (looked up automatically), your own copy of the game (your disc image, or a folder
you already extracted), the save folder (default `<install>/hdd`, created on Next), and a preset. A profiles file from an older launcher has no `setup_done`; it only
opens the wizard when its profile cannot play. The preset step also asks "Do you want to see an FPS counter on screen?" ("¿Querés ver un
contador de FPS en pantalla?"): Yes shows the four counter choices with FPS picked, No leaves it off. A profiles file
that was there before the counter existed gets the question once as a card on top of the Display page ("New: on-screen
performance counter", "Set up" / "No, thanks"); either answer, or finishing the wizard's last step, saves
`perf_overlay_asked = true` and the card is not shown again (nor while the counter is on).

The presets set resolution 1920x1080, fullscreen, widescreen, texture filtering 4x, 60 FPS mode
and the HD Glass HUD, plus `texpack-v3` next to the game as the texture pack when that folder
exists and none is chosen. **Performance** adds `--frames-in-flight 2` and nothing heavy, and
puts draw distance and effect pools back to as shipped; **Balanced** adds MSAA 2x, soft shadows
(shadow quality Normal), bloom, better rain, and the first step of a farther draw distance
(`--draw-distance 1.5 --prop-detail 2`), more effects (`--fx-scale 2`), the cinematic look, the enhanced sky, sharp faces and puddles,
without ambient occlusion or lamp lights; **Quality** adds MSAA 4x, shadow quality High, soft shadows, shadow range, bloom,
better rain with puddles, ambient occlusion, contact shadows, lamp lights, per-pixel scene lighting, the cinematic look, the enhanced sky and sharp faces. Sharp text is on in every preset (it is
cheap: one upscale on a worker thread when the fonts first show). Supersampling and soft particles stay off in every
preset (opt in on the Display page).

A new profile (first run, or New) takes its preset from the graphics card (`src/gpu.cpp`).
The launcher lists the adapters through Vulkan, the game's own API (`vulkan-1.dll` on Windows,
`libvulkan.so.1` on Linux), with DXGI (Windows) or the PCI IDs and amdgpu memory in
`/sys/class/drm` (Linux) as fallbacks. For each it reads the name, the vendor and device ID, the
driver version (decoded as the vendor shows it, e.g. NVIDIA `566.36`), the device-local memory
and its budget (`VK_EXT_memory_budget`, else the heap size) and BC texture support. It keeps the
card the game will use (a discrete card before an integrated one, then the most memory) and
gives it a tier from the tier table `data/gpu_tiers.txt` (embedded at build time; its format is
described in the file):

| Tier | Cards (roughly) | Preset |
|---|---|---|
| T0 | integrated graphics, GeForce MX / GT, no BC textures | **Performance** |
| T1 | GTX 1050-1650, RX 400/500, RX 5500/6400/6500, Arc A380/A580 | **Balanced** |
| T2 | RTX 2060 class: GTX 1660/1070/1080, RTX 2060-2080, 3050, 3060, 4060, RX 5600-5700, 6600, 7600, Arc A750/A770, B580 | **Quality** |
| T3 | RTX 3070 and up, RTX 4060 Ti and up, RX 6700 and up, RX 7700 and up, RX 9060 XT, RX 9070 | **Quality** (the place for a future Ultra preset) |

Rules match the card's name (or, for a card reported without a name, its PCI device ID); laptop
parts (`Laptop GPU`, `Max-Q`, RX `M`/`S`) go one tier down, not below T1. A card in no rule gets
the memory rule (under 4 GB T1, 4-8 GB T2, 8 GB and up T3); a card with under 3.5 GB is at most
T1; an unidentified card gets **Balanced**. The detection is kept in the profiles file (`gpu_*`
keys, see below) and worked out again only when the card, its driver or the table's `version`
changes, so a launcher update with the same table never moves the recommendation. The wizard's
graphics step and the Display page's preset card say what was found and what it recommends
("Detectamos: NVIDIA GeForce RTX 2060 (6 GB) — recomendado: Calidad"); any preset can still be
picked. Home says which preset a new profile got and why (for example "We picked Balanced for
your NVIDIA GeForce GTX 1050 (2 GB)."). An in-game benchmark (`xbrun --bench`, stepping down
until the frame times fit) is a planned later step, not part of this. Saved profiles are never changed. The new
profile also gets English and `--stats`. On the first run the
game executable is looked up next to the launcher, then one folder up: `TheWarriors.exe` first,
then the `xbrun.exe` of earlier releases (Linux: `xbrun`).

Below the preset card, the **Video memory** meter compares what the chosen settings need with the
card's memory (the device-local heap from the same Vulkan probe, or its `VK_EXT_memory_budget`
budget when the driver reports one): green, yellow above 80%, red with a warning above 100%. It
updates as options change; the tooltip splits the estimate into resolution and effects, shadows,
HD textures, HD characters and other (driver) memory. The estimate (`src/vram.cpp`, pure, tested
in `launcher_test`) follows the runtime's allocations: the frame targets at the internal size
(resolution, supersampling, MSAA), each post effect's targets, the title's offscreen surfaces
(shadow maps) scaled by the render scale and the shadow scale, the swapchain, and the texture cache:
the selected packs measured from their files (`.dds` payloads, `.png` sizes from their headers),
of which a game loads about a third (the v4 pack's in-game union), up to the runtime's 1 GiB
cache budget. Integrated graphics are marked as shared memory; when the
probe finds no card the meter still shows the estimate. The constants were calibrated against
`xbrun --stats`, whose exit summary prints a `[vram]` line with the renderer's device-local memory
per category.

### Disc images

Most players have their game as an Xbox disc image (`.iso`). Wizard step 2 (and the Game page,
"Install from a disc image (.iso)") takes the image directly: the launcher reads it with its own XDVDFS
reader (`src/iso.cpp`, no other tool), checks that its `default.xbe` is The Warriors, shows the
size and the free space on the destination drive (default `<install>/game`, next to the game
program), and extracts every file on a worker thread with a progress bar and Cancel. When it
finishes, the profile's game copy is that folder. The image is opened read-only and never
changed; no game files come with the port.

- Trimmed images (xiso) and full images are both read: the game partition is looked for at
  `0x0`, `0x18300000`, `0x2080000` and `0xFD90000` ("MICROSOFT\*XBOX\*MEDIA" at the start and at
  `0x7EC` of its sector 32).
- Extraction resumes: a file already in the destination with the same size is skipped, and a
  file cut short by Cancel or an error is removed. Closing the launcher cancels it.
- Names with path separators, drive colons or control characters, `.`/`..`, folder tables with
  loops or entries outside the table, folders nested more than 32 deep and data past the end
  of the image are refused with a plain error, before anything is written.

The game runs in its own folder. Relative paths in a profile are relative to that folder, and a
relative game executable path is relative to the launcher's folder. Each Play writes
`logs/<profile>-<YYYYmmdd-HHMMSS>.log` next to the launcher; its first line is the command.

## Profiles file

`warriors-launcher.ini` next to the launcher (or the path given on the command line):

```ini
[launcher]
selected = stable
setup_done = 1
close_on_start = false
show_advanced = false
last_page = play
last_log = C:\Warriors\launcher\logs\stable-20261026-210000.log

[profile stable]
xbrun = C:\Warriors\TheWarriors.exe
disc = C:\Warriors\disc
hdd = C:\Warriors\hdd
texture_pack = C:\warriors-v2\texpack-c
resolution = 1920x1080
widescreen = true
aniso = 16
language = es

[profile test]
; an older install: profiles that point at xbrun.exe keep working
xbrun = C:\Warriors\xbrun.exe
disc = C:\Warriors\disc
hdd = C:\Warriors\hdd
frame_lock = false
```

| Key | Option | Default (left out of the command) |
| --- | --- | --- |
| `xbrun`, `disc`, `hdd` | game executable, `<disc>/default.xbe <disc>`, `--hdd` | `hdd`: empty (the game's `./hdd`) |
| `resolution` / `render_scale` | `--resolution WxH` / `--render-scale N` | empty / `1` |
| `msaa`, `aniso` | `--msaa N`, `--aniso N` | `0` (off) |
| `widescreen`, `fullscreen` | `--widescreen`, `--fullscreen` | `false` |
| `present` | `--present` | `auto` |
| `texture_pack` | `--texture-pack DIR` | empty |
| `sound_pack` | `--sound-pack DIR` | empty |
| `button_icons` | `kbm` / `auto`: `--button-icons kbm` / `auto` | `xbox` (no option) |
| `icon_pack` | `--texture-pack DIR` before `texture_pack` | empty |
| `rumble` | `--no-rumble` when `false` | `true` |
| `language` | `--language en\|es\|fr\|de\|it` | `en` |
| `spawn_scale` | `--spawn-scale N` | `1` |
| `rate_fixes`, `frame_lock` | `--no-rate-fixes`, `--no-frame-lock` when `false` | `true` |
| `input_config` | `--input-config FILE` | empty |
| `audio`, `video`, `renderer` | `--audio`, `--video`, `--renderer` | `sdl`, `sdl`, `vulkan` |
| `render_targets` | `--no-render-targets` when `false` | `true` |
| `stats`, `trace` | `--stats`, `--trace` | `false` |
| `sim60` | `--sim60` (60 FPS mode) | `false` |
| `hud_style` | `hd`: `--hud-hd`, `hd-glass`: `--hud-hd-glass`, `glass`: `--hud-glass` | `original` (no option) |
| `hud_track_opacity`, `hud_fill_opacity`, `hud_frost`, `hud_height` | `--hud-hd-track-opacity`, `--hud-hd-fill-opacity`, `--hud-hd-frost`, `--hud-hd-height` after the style (hd styles only) | `-1` (left out) |
| `supersample` | `--supersample 2\|4` | `1` |
| `shadow_scale`, `shadow_soft` | `--shadow-scale N` (1 Normal, 2 High, 4 Ultra), `--shadow-soft` | `1`, `false` |
| `fx_scale` | `--fx-scale N` (Display > Effects and particles: 1 Normal, 2 More, 4 Maximum) | `1` (left out) |
| `shadow_range` | `--shadow-range` (`--shadow-range-dist F` stays in Extra arguments) | `false` |
| `frames_in_flight` | `--frames-in-flight 1-3` | `0` (left out: the runtime's 2) |
| `sharpen` | `--sharpen 1-100` (contrast-adaptive sharpening) | `0` (off) |
| `bloom`, `bloom_strength` | `--bloom`, then `--bloom-strength 0.1-3` when the strength is set | `false`, `-1` (left out: the runtime's 0.6) |
| `rain_hd` | `--rain-hd` | `false` |
| `rain_puddles` | `--rain-hd-puddles` (only with `--rain-hd`) | `false` |
| `contact_shadows` | `--contact-shadows` (`--contact-shadows-strength F` stays in Extra arguments) | `false` |
| `ssao`, `ssao_strength` | `--ssao`, then `--ssao-strength 0.1-2` when the strength is set (`--ssao-radius F` stays in Extra arguments) | `false`, `-1` (left out: the runtime's 0.8) |
| `lamp_lights`, `lamp_lights_strength` | `--lamp-lights`, then `--lamp-lights-strength 0.1-3` when the strength is set | `false`, `-1` (left out: the runtime's 1) |
| `world_lighting` | `--world-lighting` (`--world-lighting-strength F` stays in Extra arguments) | `false` (a profile saved as Quality without the key loads with it on) |
| `fire_light` | `--fire-light` (`--fire-light-strength F` stays in Extra arguments) | `false` (a profile saved as Quality without the key loads with it on) |
| `neon_glow`, `neon_glow_strength` | `--neon-glow`, then `--neon-glow-strength 0.1-3` when the strength is set | `false`, `-1` (left out: the runtime's 1.25) |
| `neon_masks` | `--texture-pack DIR` after the texture pack, only with `neon_glow` (presets set `<install>/neon-masks` when it exists) | empty |
| `soft_particles_opt_in` | `--soft-particles` (`--soft-particles-distance F` stays in Extra arguments) | `false` |
| `fps_overlay` | `--fps-overlay` | `false` |
| `fps_overlay_items` | `--fps-overlay-items LIST` (`fps,ms,cpu,gpu` subset in that order; passed only when not `fps,ms`; empty = an older profile: FPS and ms) | empty |
| `fps_overlay_corner` | `--fps-overlay-corner C` (`tl`, `tr`, `bl`, `br`; passed only when not `tl`) | empty (`tl`) |
| `sky_hd` | `--sky-hd` | `false` |
| `sharp_fonts` | `--no-sharp-fonts` when `false` (the runtime's default is on) | `true` (a profile saved without the key loads with it on) |
| `sharp_faces` | `--sharp-faces` | `false` |
| `spray_time` | `--spray-time 100-200` | `110` |
| `text_fixes` | `--no-text-fixes` when `false` | `true` |
| `gpu_vertex_programs` | `auto`: nothing, except `--no-gpu-vertex-programs` on a card detected as tier T0 (`gpu_tier = t0`; integrated graphics are always T0), whose GPU already limits the frame rate (an Iris Xe laptop sat at 100% GPU with the pre-pass taking 4-14 ms), so the world's vertex programs stay on the CPU there; `on`: `--gpu-vertex-programs`; `off`: `--no-gpu-vertex-programs`. No control in the window: set the key, or type either flag in Extra arguments (it wins) | `auto` |
| `preset` | written by the launcher: `laptop`, `balanced`, `gaming` or `custom` (the preset the settings match) | |
| `extra_args` | appended as typed (`"..."` groups, backslashes are literal) | empty |

"In-game button icons" (Controls) picks the prompts. **Keyboard & mouse** (`--button-icons kbm`)
has the runtime draw a key cap (with the key's name, shortened when the icon is small: SHF,
SPC) or a small mouse with the bound button in Warriors red into each button cell of the game's
font/icon atlas, from the key map it loaded, so a remap shows in game the next time it starts.
Nothing is shipped: the cell positions are read from your own `paks\global.pak` when the game
starts, and the icons are painted over your own atlas (or your texture pack's). **Auto**
(`--button-icons auto`) shows those icons while you play with keyboard and mouse and the
controller's (Xbox) icons once you press a button or move a stick on a controller, switching in
game without a restart (both atlases stay cached; the switch costs nothing). The optional
**Button-icon pack** is a folder with your own edited copy of the game's font/icon atlas (see
runtime/README.md); the launcher passes it as the first `--texture-pack`, so it wins over the
texture pack for that atlas and every other texture still comes from the texture pack.

These options come after the older ones and before `extra_args`, so a typed option still has
the last word. Loading a profile moves the options a control now covers (`--sim60`,
`--no-sim60`, `--hud-hd-glass`, `--supersample N`, `--shadow-scale N`, `--shadow-soft`,
`--shadow-range`, `--no-shadow-range`,
`--frames-in-flight N`, `--sharpen N`, `--bloom`, `--no-bloom`, `--bloom-strength F`, `--rain-hd`, `--no-rain-hd`, `--ssao`, `--no-ssao`, `--ssao-strength F`,
`--soft-particles`, `--no-soft-particles`, `--fps-overlay`, `--no-fps-overlay`, `--fps-overlay-items LIST`, `--fps-overlay-corner C`, `--sky-hd`, `--no-sky-hd`, `--sharp-fonts`, `--no-sharp-fonts`, `--sharp-faces`, `--no-sharp-faces`, `--neon-glow`, `--no-neon-glow`, `--neon-glow-strength F`, `--fx-scale N`, `--spray-time N`, `--text-fixes`, `--no-text-fixes`) out of
`extra_args` into their keys, and the command line stays the same (tested with the validated
Windows command). The `soft_particles` key saved before v0.1.4 (when the Quality preset turned
soft particles on) is ignored and not written back, so such a profile loads with soft particles off
and keeps its preset; the opt-in is saved as `soft_particles_opt_in`. A value the runtime would reject stays typed, and so does `--hud-hd-glass`
after another `--hud` option, whose order matters.

`[launcher]` keeps `selected` (the profile), `setup_done` (`0` opens the wizard), the Play
page's `close_on_start`, `show_advanced`, the `last_page` shown, the `last_log` written, and
`backup_saves` (`true`), `backup_count` (`5`, 1-20), `ui_scale` (`auto`) and `perf_overlay_asked` (`false`), written only when
changed. Once a graphics card was detected it also keeps the detection: `gpu_id`
(`vendor:device:driver`, e.g. `10de:1f08:566.36`), `gpu_name`, `gpu_driver`, `gpu_tier` (`t0`-`t3`
or `unknown`), `gpu_recommended` (a preset key), `gpu_vram_mb`, `gpu_budget_mb`, `gpu_bc` (`1`, `0`,
`-1` unknown) and `gpu_table` (the tier table version). Delete them to have the card detected
again.

Booleans accept `true/false`, `yes/no`, `on/off`, `1/0`. Unknown keys stay with their profile and
unknown sections are kept when the launcher saves; comments are not kept.

## Third-party software

No game files are included with the launcher or the port: each player supplies their own copy
of the game. What `warriors-launcher.exe` ships or compiles in:

| Component | Where | License | License text |
| --- | --- | --- | --- |
| SDL 2.32.10 | `SDL2.dll`, shared with the game (imported) | zlib | `LICENSE.txt` of the SDL2 2.32.10 development package (`make sdl2-devel`), shipped as `licenses/SDL2-LICENSE.txt` |
| Dear ImGui v1.91.5 | compiled in, vendored unmodified in `third_party/imgui` | MIT | `third_party/imgui/LICENSE.txt` (`licenses/imgui-LICENSE.txt`) |
| Inter 4.1 (Regular, SemiBold) | embedded font, vendored unmodified in `third_party/inter` | SIL Open Font License 1.1 | `third_party/inter/LICENSE.txt` (`licenses/Inter-LICENSE.txt`) |
| LLVM libc++, libc++abi, libunwind (zig 0.17.0) | linked statically | Apache-2.0 with LLVM exception | zig `lib/libcxx`, `lib/libcxxabi`, `lib/libunwind` `LICENSE.TXT` |
| mingw-w64 runtime support code (zig 0.17.0) | startup and support code | ZPL 2.1 (some parts public domain or BSD) | zig `lib/libc/mingw/COPYING` |

The disc image reader (`src/iso.cpp`) and the PNG writer are the launcher's own code. Not
shipped: the Universal C Runtime (`api-ms-win-crt-*`) comes with Windows 10/11; the Linux build
uses the system's i386 SDL2. The game's own components are listed in `runtime/README.md`
(Third-party software).
