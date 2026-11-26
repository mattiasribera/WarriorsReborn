# Lua mods

The game's own logic is written in Lua 4.0 (the scripts inside `Main.wad0`). A mod is a small set of
Lua files that the runtime runs **in the game's own Lua state**, right after the game has loaded one
of its scripts. A mod can call every function the game's scripts call (`ObjSpawn`, `CfgHat`,
`Teleport`, `ScheduleFunc`, ...) and change the game's Lua tables and functions.

Mods are off unless you switch them on: in the launcher's **Mods** page (per profile), or with
`--mod ID` on the command line. Nothing of the game is shipped with a mod: a mod only refers to the
game's own objects and functions by name.

## Folder layout

```
mods/                          next to the game program (TheWarriors.exe / xbrun), or --mods DIR
  10-extra-hats/
    mod.ini
    record.lua
    extend.lua
  20-hideout-trophies/
    mod.ini
    config.lua
    trophies.lua
    assets.ini                 optional: meshes and textures the game builds from your disc (see assets.ini)
```

- One folder per mod. A folder without `mod.ini` is ignored; one with an invalid `mod.ini`, or with an
  `id` already used by an earlier folder, is skipped with a `[mods]` line in the log.
- **Load order**: folders sorted by name (byte order), so a number prefix (`10-`, `20-`) orders them;
  inside a mod, the hooks run in the order of their lines in `mod.ini`.

## mod.ini

```ini
; comments start with ; or #, [section] lines are ignored
id = extra-hats                 ; required: a-z 0-9 - _ (1-40 characters), unique
name = More hats fly            ; required
name.es = Más sombreros que vuelan
description = What the mod does, in one or two sentences.
description.es = Lo que hace el mod.
version = 1.0                   ; required
author = Your name
saves = no                      ; yes = the mod changes what the game writes to the save
meshes = local                  ; optional: a folder of mesh overrides (see "Mesh overrides")
collision = cage.col            ; optional: a collision patch file (see "Collision patches")
min_runtime = 1.1               ; optional: the oldest mods version the mod needs (see "Safety")
files = notes.txt, data*        ; optional: more names its folder may hold (see "Safety")
apply_to_all = no               ; optional: the mod's own options, read with ModOption (see below)
boot = record.lua               ; hook: see below
after.config_preload2 = extend.lua
after.level95 = trophies.lua
```

Lua files are plain file names inside the mod's folder (no `/`, `\`, `:` or `..`). They are read once
when the game starts.

Any other key whose name is 1-40 characters of `a-z 0-9 _` (no dot or dash) is one of the mod's own
options (mods version 1.4): up to 8 per mod, values up to 63 bytes, the first line of a name wins. The
mod reads them with `ModOption(name)`; a player changes them by editing `mod.ini`.

## Hooks

| Key | Runs |
| --- | --- |
| `boot = FILE` | Once per Lua state, right after the first script the game loads in it. The game makes a new Lua state for the front end and for each level, so `boot` runs once at start-up (before the game's config scripts) and once at the start of every level. |
| `after.NAME = FILE` | Right after the game has loaded and run its script `NAME` (without `.lua`). Every function and table that script defines exists then, and its top-level code (for a level script: the objects it spawns) has run. |
| `after.PREFIX* = FILE` | The same for every script whose name starts with `PREFIX` (`after.level* = ...` runs after every `level...` script). |

A mod's hook runs every time its script loads: each time the player enters that level. To do
something once per run, keep a flag in a global (`if not MyMod_done then ... MyMod_done = 1 end`);
globals live as long as the Lua state, i.e. until the next level.

Scripts the game loads (from `--mods-trace`, 100 % save, start-up to the hideout):

- start-up (front end state): `enum_preload`, `config_preload`, `config_strings_en`, `config_preload2`
  (hats, characters), `config_preload3` (objects), `xbox_shaders`, `config_normal`, `global`, `level100`
  (main menu);
- every level: `config_normal`, `global`, `levelNN`, then the level's chapter and helper scripts
  (hideout: `level95`, `level95_chapter12`, `tag5`).

Run the game with `--mods-trace` (and any `--mod`) to see every script name as it loads:
`[mods] script level95.lua (Lua state 107a2690)`.

### What a hook may do

- Call game functions and set globals. The config functions (`CfgObj`, `CfgHat`, `CfgChar`, ...) only
  matter at start-up: hook `boot` (before) or `after.config_preload2` / `after.config_preload3`.
- Wrap a game function to run code before or after it: save the original in a global, then replace it
  (see the hats example). In Lua 4.0 a function sees outer locals only as upvalues (`%name`), so keep
  originals in a global table.
- Schedule work for later: `ScheduleFunc("MyMod.Later", 3000)` calls the global function `MyMod.Later`
  after 3000 ms of game time.

## What the runtime adds to the Lua state

| Name | |
| --- | --- |
| `MOD_ID` | The id of the mod whose file is running. |
| `MOD_SCRIPT` | The script the hook runs after (`"level95"`), or for `boot` the first script of the state. |
| `MOD_MESHES` | How many mesh overrides the runtime loaded from the mod's `meshes` folder and from what its [assets.ini](#assetsini-assets-built-from-your-disc) built, groups included (0 without any): a mod that needs its meshes checks it and changes nothing without them. |
| `MOD_ASSETS` | How many output sections (`[mesh]`, `[hide]`, `[layer]`, `[sound]`, `[collision]`) of the mod's `assets.ini` are built and loaded (0 without one, or when its build failed). |
| `ModLog(text)` | Writes `[mods] <id>: text` to the game's log (`<id>` = the mod running at that moment, else `log`). |
| `ModMeshSet(group, on)` | Turns a group of mesh overrides on (`on` not `nil` or `0`) or off; `group` is `"<mod id>/<subfolder>"` (see [Mesh overrides](#mesh-overrides)). Logs `[mods] mesh group <group> on\|off (N overrides)` when it changes, `[mods] ModMeshSet: no mesh group ...` for an unknown one. The state is the runtime's, not the Lua state's: a mod sets it again at every level start (`boot`). |
| `ModMeshPoint(group, key, x, y, z)` | Sets (x, y, z numbers) or removes (no coordinates) the world point of a mesh group under `key` (a number or string, e.g. a character's handle): the remembered characters of an [instance group](#instance-groups-per-character), the objects of a [placed group](#placed-groups-per-object). Returns the group's number of points, or `nil` (logged) for an unknown or full group. A new Lua state starts with none. Older runtimes lack it: check `if ModMeshPoint then`. |
| `ModBlood(option, value)` | Turns up the game's own blood (runtime hooks; see [Brutal fights](#brutal-fights-90-gore-id-gore)): `"drops"` 1-4 (x the drop count of a blood effect; a punch has 1), `"pool_hold"` 0 or 10-1000 s (how long a ground pool stays at full size after the game's own 3 s; 0 = the game's fade at once), `"pool_max"` 16-512 (held pools at most, default 128; the oldest fades first), `"char_gain"` 1-4 (blood on fighters shows after a smaller health loss), `"char_keep"` 0/1 (a fighter keeps the blood of his lowest health; a new character in reused memory, e.g. a cutscene actor, starts clean). While the in-engine cutscene letterbox bars show, fighters wear the game's own blood (no gain, nothing kept or recorded). Values are clamped to the range; an unknown option or a value that is not a number is logged and ignored. A setting lasts until the game exits; the per-level records (held pools, lowest health) start over in each new Lua state. The first call that leaves an option's game value writes its hook (`[blood] ...` in the log). |
| `ModScar(char_id, level)` | Healed marks on every character of that `CfgChar` id (see [Lasting scars](#lasting-scars-87-scars-id-scars)): `level` 0-1 (drawn in 8 steps; 0 removes them) turns the game's own blood overlay of a fighter into old injuries, dark plum bruises and dark red closed cuts that only ever darken the skin (light or dark), fainter for a lower level, with a small tan plaster on the cheekbone from 0.6. Fresh blood (health under 90 %) is drawn over the marks; healing takes only the fresh blood away. `ModScar()` removes every mark: the marks are the runtime's, so a mod calls it at the start of each level. At most 32 ids at once; a call that is not an id and a number is logged and ignored. The first mark writes the hook (`[scars] healed marks at 0008fe10`). |

| `ModTexLayer(layer, on)` | Turns a texture layer on (`on` not `nil` or `0`) or off; `layer` is `"<mod id>/<label>"` (see [Texture layers](#texture-layers)). Returns the number of textures the layer covers, or `nil` (logged) when there is no such layer: a mod whose layers are not built checks it. Logs `[mods] texture layer <layer> on\|off (N textures)` when it changes. Like `ModMeshSet`, the state is the runtime's: set it again at every level start. |
| `ModSpeed(character, scale)` | Caps how fast one character moves (mods version 1.3; see [Visible wounds](#visible-wounds-85-wounds-id-wounds)): `character` is a handle as `HuCreate` returns it (or `player`), `scale` 0.3-1 (clamped) multiplies the speed the game picks for him every world update, so he walks, runs and sprints at that share of it and speeds up and slows down as the game does; the player and AI characters alike (the player's speeds also come from the game's own per-character speed multiplier, which the game lowers itself for a while in one of its recovery states: it is kept at or under `scale` too). On the player it reads as slow motion: the game animates a character from his speed, so a capped player plays a slower, shorter run cycle (x0.85: the stride period 5.5 % longer); `ModSprint` limits him without that. `scale` 1 removes the cap: an AI character's speed comes back at once, the player's over a few seconds (the game's own rise of that multiplier, 0.2 % per 30 Hz frame; the same per second with `--sim60`); without `scale` it only reads. Returns the character's cap after the call (1 = none), or `nil` when the handle is not a live character. A cap stays with that character until it is removed or he is gone; another character later created in the same slot starts without one. The first cap below 1 writes its two hooks (`[huspeed] speed caps at 0016b872 (AI target speed) and 0016b02c (the player's speed multiplier)` in the log). |
| `ModSprint(character, allowed)` | Stops (`allowed` `0`) or allows again (`1`) one player-controlled character's sprint (mods version 1.5; see [Visible wounds](#visible-wounds-85-wounds-id-wounds)): holding the sprint control he runs at his normal run, which the game plays at its own pace; his walk and run are untouched. Without `allowed` it only reads. Returns `1` when he may sprint, `0` when blocked, `nil` when the handle is not a live character. AI characters do not sprint through this test, so it does not change them. The block stays with that character like a `ModSpeed` cap (another character later created in the same slot starts without one). The first block writes its hook (`[huspeed] sprint block at 001657ff (the player's sprint test)` in the log). |
| `ModSave(key, value)` | Stores `value` (a string or number, kept as a string; `nil` removes the key) under `key` in the mod's own data file, for the save profile the game has loaded. Returns `1`, or `nil` with a `[mods] <id>: ModSave(...)` line in the log. See [Mod data](#mod-data). |
| `ModLoad(key)` | The string stored under `key` for the loaded save profile, or `nil`. |
| `ModOption(name)` | The value (a string) of the mod's own option `name` in its `mod.ini`, or `nil` (mods version 1.4). Bound to the mod like `ModSave`. |
| `ModKeep(key, value)` / `ModKept(key)` | Values kept in memory for the rest of the run, in every Lua state (the front end's and each level's), never written anywhere (mods version 1.4): a mod passes what it learnt at start-up to the levels. Same key and value rules as `ModSave` (`nil` removes); at most 256 values in all. `ModKeep` returns `1`, or `nil` with a `[mods] <id>: ModKeep(...)` line. Bound to the mod like `ModSave`. |
| `ModDifficultyOption(label, question[, yes, no])` | Adds one choice after the game's own in the new-game difficulty list (mods version 1.4; the list is the game's C++ profile manager, not Lua). Choosing it rebuilds the page as a question: `question` as the title, `yes` and `no` (default: the game's own YES / NO) as the two answers, the second selected; the first sets the new profile's difficulty to Hard (2) and goes on to the brightness page, the second or back return to the list. `label` `nil` removes the choice. Texts are bytes in the game font (Latin-1 escapes such as `\237` for í). The title is one line (newlines are ignored) and a title much wider than about 40 characters is pushed off the screen, so longer explanations go in the answers; the label is cut to 48 bytes, the other texts to 200. Returns `1`, or `nil` when the list's code is not the shipped one (`[diffmenu] not available: ...`). The setting lasts until the game exits; set it at `boot`. |
| `ModDifficultyChosen()` | What the last new profile chose since the previous call: `1` the mod's choice (YES), `0` one of the game's own, `nil` no new profile. Call it from a level's hook (the new profile is the loaded one there) and store the answer with `ModSave`. |
| `ModMenuOpen(title, items, footer, sel, cover)` | Opens a list menu over the game (mods version 1.6; see [Menus and music](#menus-and-music)): `items` one line per item (`"\n"`), a line `"Label\tvags/music/<id>"` previews that game sound while the item is selected; `sel` the item selected first (1-based); `cover` `nil` or a texture of the player's disc (`"disc:<file>:<chunk>[:x,y,w,h]"`). The game gets a neutral pad while it is open. Returns the item count or `nil`. |
| `ModMenuState()` | `"open"`; `"chosen", i` (A) or `"closed", i` (B, Escape, `ModMenuClose`) once after the menu closed; else `nil` (mods version 1.6). A value menu (`ModMenuValues`) stays open and returns `"chosen", i, dir` once per choice (mods version 1.11). |
| `ModMenuClose()` | Closes the menu as B would (mods version 1.6). |
| `ModMenuValues(values[, notes])` | Makes the open menu a value menu, the look of the in-game settings menu (mods version 1.11; see [Menus and music](#menus-and-music)): `values` one per row (`"\n"`; a missing or empty line shows none) in a value column, `notes` one per row, the selected row's note shown under the list. A, left and right report `"chosen", i, dir` through `ModMenuState` (`dir` 0 for A, -1 left, 1 right) and the menu stays open; call it again to change the values (the notes stay unless given). Returns the row count, or `nil` without an open menu. |
| `ModMenuKey()` | `1` once when the player asked for a mod's menu in the last second, else `nil` (mods version 1.11): **Back + LB** held for half a second on the pad, or **F8** on the keyboard (unless F8 is bound to a control), while no menu is open. A mod checks it every few hundred ms (`ScheduleFunc`) and opens its menu; the first mod that checks gets it. |
| `ModMusicPlay(name, loop)` | Plays a game sound (`"vags/music/in_the_city"`) as the mod's song, looping unless `loop` is `0`, on the game's radio (the place the game last played a muted song), crossfaded (0.3 s); `nil` fades it out and gives the game's own music back. Returns `1` or `nil` (mods version 1.6). |
| `ModMusicPlaying()` | The mod's song while it plays, else `nil` (mods version 1.6). |
| `ModSound(name [, x, y, z] [, volume [, loop]])` | Plays one of the mod's own sounds (a `[sound NAME]` of its `assets.ini`; mods version 1.7; see [Your own assets](#your-own-assets)): without coordinates as a 2D sound, with them at that world point (metres, z up: louder and panned by where the listener is, full level within 2 m, silent beyond 40 m, followed every audio frame); `volume` 0-1 (default 1), `loop` `1` repeats it until stopped. Returns a handle or `nil` (no such sound, 32 already playing for the mod; one log line). Bound to the mod like `ModSave`: keep the copy your file sees (`MyMod.Sound = ModSound`) for calls from scheduled functions. A level load stops every mod sound. |
| `ModSoundMove(handle, x, y, z)` / `ModSoundStop(handle)` | Moves a playing sound (it becomes positioned) / stops it. `1`, or `nil` for a handle that is not this mod's or has ended (mods version 1.7; bound like `ModSound`). |
| `ModRumbleGangs(gang1, gang2)` | Sets the two gangs of the game's Rumble menu (mods version 1.8; see [Sparring ring](#sparring-ring-75-sparring-id-sparring)): CfgRumbleGang ids 1-64, the numbers of the `rumble_full_gang_<id>` paks (the Warriors are 1, the Boppers 13). An arena's script reads its setup from `GetRumbleModeData`, but the native intro (`ShowRumbleModeIntro`) has the DJ name the gangs the menu holds (`dj_gang_<id>`), so a mod that starts a rumble with its own setup calls this before the intro. Only the two gang values change; the menu sets them again when the player uses it. Returns `1`, or `nil` (ids out of range, or the game's rumble code is not the one this runtime knows: nothing written); every call is logged (`[mods] <id>: rumble gangs set: 1 vs 13 (the DJ says dj_gang_1, dj_gang_13)`). |
| `ModRadarColor(char_id, colour)` | The radar dot colour of every character of that `CfgChar` id (mods version 1.9; see [Gang colours on the radar](#gang-colours-on-the-radar-92-radar-colors-id-radar-colors)): `colour` `"RRGGBB"` (hex, a leading `#` allowed) is drawn opaque as that character's dot (gang, police and Warriors dots, on both radars, also when the game tints the dot again later); the dot is drawn with the game's plain round dot frame so the colour shows as it is. `ModRadarColor(char_id)` gives one id the game's dot back, `ModRadarColor()` every id. Objectives and other markers keep the game's look. Returns `1`, or `nil` (logged: not an id 1-65535 and an `"RRGGBB"` text, 512 ids already set, or the radar code is not the one this runtime knows). The table is the runtime's and lasts until the game exits, so a mod sets it while the game configures its characters (start-up). The first colour writes the hooks (`[radar] dot colours at 000f2b60 (add), 000f12a0 (colour), 000f1190 and 000f08f0 (frame)`). |
| `ModComboSlowMo(allowed)` | `0` removes the slow motion the game plays in single player during some fight combos (mods version 1.10; see [No combo slow motion](#no-combo-slow-motion-97-no-combo-slowmo-id-no-combo-slowmo)), as the game does with two players; `1` gives it back; without `allowed` it only reads. Returns `1` (it may start), `0` (removed), or `nil` when the game's combo code is not the one this runtime knows (nothing written). Lasts until the game exits. The first call with `0` or `1` writes its hook (`[huspeed] combo slow-motion hook at 000146ae (single player)`), and every start is then logged: `[huspeed] combo slow motion (player 0): skipped` / `: as the title`. Slow motion the level scripts set with `HuSetSlowMo` is not changed. |
| `ModMusicMute(names)` | The game's own sounds (`"\n"` separated names) kept silent while a mod's song or a menu preview plays; they fade back after. Returns how many names were found (mods version 1.6). |

Nothing else is added: no other file, OS or network access. The game opens only Lua 4.0's base,
string and math libraries.

### Mod data

`ModSave` / `ModLoad` keep a mod's own small values (records, counters, settings) between runs without
touching the game's save:

- One file per mod: `mods-data/<mod id>.json` in the hdd folder (`--hdd`, beside `partition1`; the
  game never sees it). Inside, one object per save profile, keyed by the profile's save folder name
  (`"1A2B3C4D5E6F"`; `"none"` before the player loads a profile), with string values:
  `{"1A2B3C4D5E6F": {"best.2.3": "8.25"}}`. Loading another profile shows its own values.
- Keys: 1-64 characters of `a-z A-Z 0-9 . _ -`. Values: up to 1024 bytes. A file holds at most 64 KiB;
  a save that would make it larger is refused (`nil`, logged).
- Every `ModSave` rewrites the file: written to `<id>.json.tmp`, then renamed over the old one, so a
  crash leaves the old file or the new one. A file that is not valid mod data (edited by hand) is left
  as it is: `ModSave` and `ModLoad` refuse and log why.
- The two functions are bound to the mod whose file is running (the runtime sets them before each of
  its hook files). Code that runs later (a wrapper the game calls, a `ScheduleFunc`, a message
  handler) uses a copy kept while the file runs: `MyMod.Save = ModSave MyMod.Load = ModLoad`. The
  globals called outside a mod's file return `nil` and log
  `[mods] ModSave / ModLoad called outside a mod's file ...`.
- Values are strings: `tonumber(MyMod.Load("best")) or 0`.

## Safety

- **A failing mod never stops the game.** Every hook runs inside a protected call with its own error
  handler. A Lua error (syntax error, calling `nil`, `error(...)`) writes
  `[mods] <id>: error in <file> (after <script>): <message>; mod turned off for this run` and turns that
  mod off until the game restarts; the other mods keep running, and the Lua stack and the game's own
  error handler are restored.
- Errors in code a mod schedules for later (`ScheduleFunc`, message handlers) are handled by the game's
  own error handler, like the game's own script errors.
- A mod runs inside the game: a game function called with wrong arguments (e.g. a model name that does
  not exist) can still crash the game the way a bug in its own scripts would. Test a mod with `--mod ID`
  and read the log.
- `--no-mods` runs no mod even when some are enabled.
- **Lua memory.** The game's own Lua caps each Lua state's garbage collector at 668 KiB, a budget
  only its own scripts were made for. Mods add code, tables and strings to the same state, and past
  that cap the game ran one collection inside another and freed tables still in use (random crashes
  at the hideout or on the way out to Coney with many mods on). With mods loaded the runtime makes
  collections never nest and raises the cap to 4 MiB; the log says
  `[luagc] Lua collections at 00234b60 no longer nest; collection cap 668 -> 4096 KiB (8 sites); Lua heap 2048 -> 8192 KiB at 00245cc1`.
  Each Lua state also lives in a heap of its own that the game caps at 2 MiB; when it is full the game
  stops on purpose with a write to address 0 (`EIP 00236a46` in the crash log). The bundled mods
  together already pass it on the front end (about 1.95 MiB of Lua memory right after
  `config_preload2`: the boot crash of More hats fly with Dirty Fights 1.4 and later), so with mods the
  runtime also raises that heap to 8 MiB (the end of the same line). Repro check, headless, every
  bundled mod on; it must reach the last frame with no `=== FAULT`:
  `tools/xbrun-slot -- runtime/build/xbrun <default.xbe> <disc> --hdd <copy of a save HDD> --mods <the build's mods folder>
  --mod all --big-heap --skip-intro --video null --audio null --frames 5000`.
  Nothing changes without mods. To watch the headroom, each level's Lua state logs its peak when the
  next one starts: `[luagc] level 95: peak Lua memory 1234 KiB (cap 4096)` (the largest size seen
  when a collection starts or a script loads; level 100 = the menus; the last level of a run is logged
  when the game exits, with `(at exit)`).
- Strings handed to a game function that keeps the pointer (`SetMsgHandlerEx` texts,
  `HUDSetAnnounceMsg`, callback names built with `..`) must stay referenced by the mod (a field of
  its table), not only by a local or the call's arguments.

### Save backups before save-changing mods

When at least one enabled mod has `saves = yes`, the game copies its save folder
(`<hdd>/partition1/UDATA/5454009b`: the saves and profiles, a few KB) to
`<hdd>/mods-backups/<YYYYmmdd-HHMMSS>/` before the title starts and logs
`[mods] save backed up to <folder> (before <ids>, which change save data)`. The newest 5 backups are
kept. A save that did not change since the newest backup is not copied again
(`[mods] save unchanged since the backup ...`), so starting the game a few times without playing does
not push the last good copy out. A copy is written as `<stamp>.partial` and renamed when complete; a
failed backup is logged and the game still starts. Each backup holds `save/` (the copy) and
`backup.txt` (`kind = mods`, files, bytes, signature), the launcher's own backup layout.

The launcher's Mods page shows the latest backup and **Restore save backup** ("Restaurar copia de la
partida"): pick a backup, confirm in the page, and it replaces the save while the game is not running.
The current save is copied into `mods-backups` first (shown as "Before a restore"), so a restore can be
undone. Only the game's save folder is copied; data a mod keeps elsewhere is not.

### Mod checks at load

Every enabled mod's folder is checked against its `mod.ini` before it loads:

- **Leftovers.** The folder may hold `mod.ini`, the Lua files its hooks name, its `meshes` folder and
  the names in its optional `files` key (`files = notes.txt, data*`; a trailing `*` matches a prefix).
  Hidden names (a leading dot), `Thumbs.db` and `desktop.ini` are ignored. Anything else, usually what
  an older version had when a new version was copied over it (an old meshes folder, a renamed Lua
  file), is logged as `[mods] <id>: "<name>" in its folder is not part of version <v> (left from an
  older version?); delete it or reinstall the mod. The mod still loads`. Install a new version by
  deleting the old folder first.
- **Missing files.** A Lua file a hook names that is not there keeps the mod from loading
  (`[mods] <id>: missing file <f> (named in its mod.ini); mod not loaded: reinstall it`); a missing
  exact `files` name is logged. A missing meshes folder is not an error: the log says `[mods] <id>: optional meshes not installed (folder <f>); the mod runs without them`.
- **`min_runtime = X`.** The mods version this game supports is `MOD_RUNTIME_VERSION`
  (`runtime/src/modini.h`, now `1.10`: 1.3 added `ModSpeed`, 1.4 `ModOption`, `ModKeep` / `ModKept` and the
  difficulty choice, 1.5 `ModSprint`, 1.6 the menu and music functions, 1.7 own assets and `ModSound`,
  1.8 `ModRumbleGangs`, 1.9 `ModRadarColor`, 1.10 `ModComboSlowMo`; it goes up when mods gain something older games lack). A mod
  whose `min_runtime` is newer does not load:
  `[mods] <id>: needs mod runtime X or newer (this game has 1.10); mod not loaded: update the game`.
  Runtimes older than 1.1 ignore the key.

The launcher's Mods page runs the same checks (`runtime/src/modcheck.c`) and shows each problem under
the mod in warning colour.

### After a crash with mods on

The crash dump (`[crash-context] ...` at the end of the log) gets the mods that ran:
`[crash-context] mods: <id> <version>, ...`. When the last game log ends in such a dump, the launcher's
Home page says "The game closed with mods on: <names>" ("El juego se cerró con mods activos: ...") and
offers **Play once without mods** (that start only gets `--no-mods`; the profile keeps its mods) or
**Dismiss**. Either answer hides the notice for that log.

## Saves

The game writes its save from its own state (progress, unlocks, money, stats). A mod that changes those
(e.g. `UM_Unlock*`, `GiveMoney`, `HuSetMoney`) changes what the next save writes, and the change stays
when the mod is switched off. Such a mod must say `saves = yes` in its `mod.ini`: the launcher marks it
"Changes save data" and the log line `[mods] loaded ...` says `(changes save data)`. A mod that only
changes what you see or how characters react (`saves = no`) leaves the save as it would be without it.
The game backs up the save before each start with such a mod (see "Save backups before save-changing
mods"); the launcher's own backups (Options page, Backups) are a second copy.

## Game functions

The game has about 1100 Lua functions; these are the ones the examples use or that mods most often
need (arguments as the game's own scripts pass them):

| Function | Use |
| --- | --- |
| `ObjSpawn(model, {x, y, z}, {qx, qy, qz, qw}, -1, flags, 0, 4294967295, nil)` | Place an object by its model name; returns its handle. |
| `ObjDestroy(handle)` | Remove a spawned object. |
| `Teleport(handle, {x, y, z}, heading)` | Move a character or object. |
| `TeleportToFlag(handle, flag, -1)` | Move to one of the level's named spots. |
| `CfgObj(model, kind, ...)` | Object setup (start-up only); `"hat_object"` entries are hats: argument 19 `OBJECT.TYPE_HAT` flies off on hard hits and death, `OBJECT.TYPE_RIOT_HAT` never leaves the head. |
| `CfgHat(animA, animB, model, offset, rotation)` | Where a model's hat leaves the head in one knock-down animation (start-up only). |
| `HuDrop(h, hat, a, b)` | With `hat` = 1, knocks the character's hat off with the game's own code (a hat of type `OBJECT.TYPE_RIOT_HAT` or held by `HuSetKeepHat` stays). The other two flags run other drop code of the game (not documented here; pass 0). Not used by the game's scripts. |
| `ScheduleFunc(name, ms)` | Call the global function `name` later. |
| `GetGameTime()` | Game time in ms. |
| `HuGetMoney(human)`, `HuSetMoney(human, n)`, `GiveMoney`, `TakeMoney` | Money (changes the save). |
| `UM_IsLevelComplete(level)`, `UM_IsDataUnlocked(type, n)`, `UM_Unlock(...)` | Progress and unlocks: `UM_IsLevelComplete(86)` is true once that mission is completed (level numbers: `levelNNN` scripts; `UM_Unlock(Level, 0, 0)` is what the game calls at mission complete, and changes the save). |
| `AddFlag(name, {x, y, z}, heading, 0, 0)`, `SetMsgHandlerEx(flag, 0, "Fn", title, line)` | A spot on the floor with the game's walk-up prompt (title at the top, line in the message box at the bottom left, as at the hideout's menu and Flashback spots; needs the level's follow camera); the action button there calls the global function `Fn(flag, who)`. The game keeps pointers to the two strings, not copies: keep them referenced (in a global table), or the prompt later shows whatever text reused that memory. |
| `HUDSetTutorialText(text, seconds)` | A line of help text on screen. |
| `HUDSetAnnounceMsg(seconds, text)` | A big announcement under the top title (as the hideout's "New Feature Unlocked"; markup `<CENTER><SIZE 2.0><BIGFONT><DISPLAYTIME ms>...</BIGFONT>`). Shows while a walk-up prompt holds the message box, where a tutorial text does not. |

Useful globals in a level: `player` (the player's character), `ENV` (the current area), `Objects` (the
level's spawned objects by name).

## Lua 4.0 in short

`for k, v in t do ... end` walks a table (there is no `pairs`); `for i = 1, n do`; `getn(t)`,
`tinsert(t, v)`, `sort(t, f)`; `format("%d", n)`, `strfind`, `strsub`; varargs arrive in the table `arg`
(`arg.n`) and `call(f, arg)` calls `f` with them; outer locals inside a function are upvalues `%x`.

## Example mods (bundled, all off by default)

There are 20, in the repository and in the release zip.

| Folder | Id | What |
| --- | --- | --- |
| `10-extra-hats` | `extra-hats` | Police officers (street, rookie and transit cops) lose their cap in a fight, like gang members lose their hats. Two things kept every cop's cap on: the game sets the caps of `dyn_cops_va` (street and rookie cops), `dyn_cops_vb` (security guards) and `dyn_cops_vc` (transit cops) up as `OBJECT.TYPE_RIOT_HAT` with no physics, the kind of "hat" it also uses for hair and helmets, which never leaves the head; and its fight code never knocks a hat off a police character (`CfgChar` kind 1, ids 245-258, 263, 264), while the same model and cap under a non-police id (266, or the guards 358/359) loses it, and a beaten cop is knocked out (health stays just above 0) and later removed with the cap still on. `boot` (`record.lua`) wraps `CfgObj` and gives the three caps the settings of the game's own flying hats (the pimps': `OBJECT.TYPE_HAT`, `PHYS.OBB` on `AXIS.Z`, `ANIM.LeftHandHatPickUp`), and in every level wraps `HuCreate` and the spawn callback of `GangAddSpawner` to watch each cop the level creates: every 0.1 s it knocks the cap off with `HuDrop(cop, 1, 0, 0)` (the game's own knock-off) when a hit takes the cop under his power class's hat threshold (`CfgPowerClass` argument 10, the rule the game applies to gang members: 20 % on Normal), when one hit takes 10 % of his health or more, and when he is knocked out or dies. The cap falls to the ground, stays there and can be picked up and worn; each knock-off is logged (`[mods] extra-hats: cap knocked off cop ...`). Version 1.3: a cop who gets up picks his cap up again: from 2.5 s after the knock-off, every 0.5 s, while he is alive and conscious, no player is within 3 m of him and the cap lies within 5 m, the mod gives him the game's own "get my hat" goal `GoalGetItem(cop, 4, cap)` (the goal the game's thug brain gives itself for a lost hat); a second try 8 s later, then he leaves it (also when the player took it or it lies farther). Worn again, it can be knocked off again (`[mods] extra-hats: cop 245 goes back for his cap (2.1 m)`, `... picked his cap up`). It also records the game's `CfgHat` calls and, after `config_preload2`, gives models with an identical hat object the poses they lack. Not changed: riot police (`cops_vc1`) keep `dyn_riot_visor`, a visor on a helmet that is part of their model; undercover and internal-affairs cops wear no cap. Cops created by the game itself without a level script (if any) are not watched. |
| `20-hideout-trophies` | `hideout-trophies` | Three more trophies on the hideout's trophy wall (the south wall of the main room, where the game shows its own five trophies: `Objects.Spider`, `King`, `LC`, `Fuckyou` (a "F.U. Huns" T-shirt), `Bigmoe`), unlocked the way the game unlocks its own. The retail trophies are unlocks of type 5 in `global.lua`'s `Unlockables` table (`{level, 0, 0, 5, name, n}`: Spider = level 34, King = 2, LC = 5, Fuckyou = 81, Bigmoe = 31), opened by `UM_Unlock(Level, 0, 0)` when that mission is completed; `flashback.SetupTrophies()` in `level95` destroys the ones still locked. The mod does not add unlock records (that would change the profile's unlock table); it checks the same mission completion with `UM_IsLevelComplete(level)` (the check `level95` itself uses for level 84) when the hideout loads, and places a trophy only when its mission is done: the green dragon statue (`dyn_special_dragon`) after **level 20** (Flashback D, Fox's flashback in Chinatown against the Savage Huns: the game registers the statue there as a stolen item, `CfgInventoryItem(..., 13, ...)`, but the objective that took it was cut), and Ash's vest, in his memory, after **level 86** (mission 9, the Destroyers attack the Coney strip, where Ash dies in scene `L86C3`): a Warriors vest (`dyn_war_vest`, the game's flat vest with the gang's back patch). `boot` (`config.lua`) wraps `CfgObj` so the statue, set up by the game as a floating pick-up, is a plain `simple_object` that stands where it is put, and holds the vest to the same plain registration (the game already registers it that way, like the retail T-shirt: no pick-up, spin axis or special type). `after.level95` (`trophies.lua`) places them among the retail trophies, on real surfaces: the dragon on a rusty barrel (`dyn_barrel_b`, like the barrel under the Spider trophy) beside the Huns T-shirt (`Fuckyou`), between it and the subway map, facing the room; Ash's vest hung flat on the wall between the crown's shelf (`King`) and the panties' frame (`LC`), like the T-shirt, back patch facing the room (the model lies flat with the patch up; the rotation `{0, 0.7071068, 0.7071068, 0}` stands it against the wall upright; the other quarter turn shows it mirrored, from the inside). It gives each the hideout's own walk-up prompt (`AddFlag` + `SetMsgHandlerEx`, as for the Flashback and menu spots, set up once `Cameras.follow` exists): the name as a big title at the top of the screen and one line in the message box at the bottom left, "Dragon Statue / Taken from the Savage Huns" and "Ash's Vest / In his memory" (Spanish when `GetLanguage() == 1`: "Estatua del dragón / Robada a los Savage Huns", "Chaleco de Ash / En su memoria"). The retail trophies have no text of their own; pressing the action button at the prompt does nothing (like the retail locked-flashback spot, `flashback.NoAction`). On a profile that has not completed a mission its trophy and label are not there (`[mods] hideout-trophies: <model>: level N not complete, not shown`). Not used: `dyn_s_vest_b` (no model on the retail disc); `dyn_s_vest` (1.3's Ash's vest, the vest crumpled on the fence in the death scene: a narrow rag that looks the same from every side, so on the wall it seemed to float and turn with the camera). The game has no Chatterbox vest object. Version 1.2's trophy room in the caged room under the main floor was dropped in 1.3: that room had no collision (no floor), so it could not be entered; 1.6 brings it back, walkable (see [The gang trophy room](#example-the-gang-trophy-room-20-hideout-trophies)). Version 1.5 adds **Cleon's necklace** once the story is finished (`UM_IsLevelComplete(84)`, the final mission, the check `level95`'s own `flashback.SetupTrophies()` uses): the silver necklace painted on Cleon's chest, laid on deep red velvet in a wooden shadow-box frame hung on the wall above the crown's shelf (`King`; walk-up spot at the shelf's left end, halfway between the Flashback spots in front of the Spider trophy and the crown; those spots only prompt while their flashback is unlocked and not yet played, so after the story they are silent), with the label "Cleon's Necklace / The one he always wore" ("Collar de Cleon / El que siempre llevó"). Its mesh is built once from your game files (see [Example: Cleon's necklace](#example-cleons-necklace-20-hideout-trophies)); without it the necklace is not shown and the other trophies are. Version 1.6 turns the caged room under the main floor (walkable since its collision patch, `cage.col`) into a lit **gang trophy room**: one trophy per gang the Warriors beat, each shown once its mission is complete, with its own walk-up label (see [The gang trophy room](#example-the-gang-trophy-room-20-hideout-trophies)). |
| `30-train-flyby` | `train-flyby` | `after.level9_chap2_tags`: wraps `ch2.ContinueLevel` (mission 13, All City: the objective "Bomb four more fresh trains" starts) and 1.5 s later plays the unused retail camera shot `l9_flyby1` with `SuperRunScene`, then gives the camera and HUD back. Once per level load (a checkpoint restart reloads the level and plays it again). |
| `40-cleon-bandana` | `cleon-bandana` | Cleon's bandana is a hat the game can knock off; without it he shows his flashback head. Needs its mesh overrides built once from your game files (see [Mesh overrides](#mesh-overrides)). |
| `50-protection-money` | `protection-money` | `after.level95_coney`: wraps the hub's `shops.Setup` / `shops.CleanUp`. In the protection mission (level 86) the Warriors press Talk at each shop owner, ask for the week's money and get $25. On the Coney streets afterwards the game has the butcher (Stefano), Voodoo Motors (Skinny Pete) and the Little Buddha (Twiggy) each open with a 65 % chance, carrying $25 (`HuSetMoney`) only in its three collection rounds, and you can only mug them; Charlie's plumbing store never opens. The mod gives every open owner who has money the mission's Talk prompt (`SetMsgHandlerEx`, with the hub's own `CfgActionDistance(1, 2)` while you are near) and plays the mission's exchange: Cleon's or Swan's ask line, the owner's pay line, the `money_take` / `money_give` hand-over, the reassurance. An owner who owes the game nothing pays again 10 minutes of game time after his last payment, and three payers the game never opens there roll the same odds from one table (`ProtectionMoney.EXTRA`, opened by `OpenExtra`): Charlie on the pavement in front of his plumbing shutters (a fourth `GoalShopkeeper`, in the hub's existing volume box there: `AddVolumeBox` while the streets are up crashes), Hot Dog Jimmy (character 315) at his cart by Voodoo Motors, where the protection mission has him (`GoalShopkeeper` in the ambient-sound box `vbAmbSound[4]`), and the Gypsy (character 312, configured by the game but placed by no level) on the hub's unused storekeeper flag `fShopkeep[4]`, standing (no box covers it). They have no pay lines: Swan asks with the name-free `l86_t13_004`, Cleon and Swan reassure with the butcher's lines; Charlie and Jimmy say one of their own mission lines when first hit. Not added: Apephibian (the hub spawns him as a hostile in the carny minigame, a second copy would meet him there) and the jewellery store (a burglary target: `roam.ResetJeweleryStore` keeps its door closed and pickable and refills its loot). Payments (talked or mugged) are noticed when the owner's money drops to 0; cooldowns live in the hub's Lua state (a mission or reload starts fresh); the game's collection flags are not touched. `saves = yes`: the money is saved by the game. |
| `60-warriors-mode` | `warriors-mode` | "Warriors Mode (truly hard)" / "Modo Warriors (difícil de verdad)": adds WARRIORS to the new-game difficulty list (`ModDifficultyOption`); profiles that chose it play from the game's Hard config and further (enemies hit harder, block more and attack three at a time, half healing, 60% money, more police). `mod.ini` `apply_to_all = yes`: every profile. See [Warriors Mode](#warriors-mode-60-warriors-mode-id-warriors-mode). |
| `70-gym-records` | `gym-records` | Records for the hideout gym, once the story is finished (`UM_IsLevelComplete(84)`, the last story mission; before that the mod logs `story not finished ...` and changes nothing). `after.level95_workout` (`records.lua`, loaded again for every set) wraps the set's `workout.StartHUD`, `workout.DoRep` (chin-ups, push-ups, sit-ups), `workout.Check01` (heavy bag) and `workout.CleanUp` (every end of a set): a set is timed with `GetGameTime()` from the counter appearing to the rep or combo that reached the rank's target (`repTable[rank]`), and the fastest successful time per exercise and rank is kept with `ModSave` (`best.<exercise>.<rank>`, ms, per save profile); timed-out and abandoned sets do not count. 1.8 s after a successful set the hideout's announce slot (`HUDSetAnnounceMsg(5, ...)`, where the game shows "new feature unlocked") shows the exercise, the rank, the time and the record, or "New record!" with the old one (Spanish when `GetLanguage() == 1`: "¡Nuevo récord!"). `after.level95` wraps `worksetup.BuildWorkoutMessage`: each station's walk-up panel gets a purple "Rank N record: 9.53 s" line for the rank you would train next. Every set is logged: `[mods] log: gym-records: chin-ups rank 10: 20/20 in 14.20 s (limit 36 s), done, record 14.20 s (new)`. `saves = no`: the records are in `mods-data/gym-records.json`, not in the game's save. |
| `75-sparring` | `sparring` | "Sparring ring" / "Ring de entrenamiento": after the story, a walk-up spot in the hideout's cage room opens a menu (gang you beat, enemies, your Warrior and crew, venue) and starts the fight in one of the game's rumble arenas, the unused Warrior Turf and Coney Drag included; the fastest win per gang and size and the longest survival are kept with `ModSave` and shown after the fight, then the game goes back to the hideout. See [Sparring ring](#sparring-ring-75-sparring-id-sparring). |
| `80-dirty-fights` | `dirty-fights` | "Dirty fights: falling accessories" / "Peleas más sucias: accesorios que caen". Glasses, caps, winter hats, a paper bag and a few gang hats fly off in a fight, stay on the ground and can be picked up and worn; per character. Glasses can break (1.1), a character who lost his glasses keeps looking so after leaving the view, and some fighters walk back for their hat once the fight calms down. See [Dirty fights](#dirty-fights-80-dirty-fights-id-dirty-fights). |
| `85-wounds` | `wounds` | "Visible wounds" / "Heridas que se notan": low on health the player cannot sprint and his crew moves slower; healing gives it back; enemies keep their speed (an option). `boot` only, through `ModSprint` and `ModSpeed`. See [Visible wounds](#visible-wounds-85-wounds-id-wounds). |
| `87-scars` | `scars` | "Lasting scars" / "Cicatrices que duran": a Warrior who ended a story mission badly beaten shows bruises, closed cuts or a plaster in the hideout and the next missions, fading over the night (replays start clean); remembered per Warrior with `ModSave`, drawn with `ModScar`. See [Lasting scars](#lasting-scars-87-scars-id-scars). |
| `88-dirty-clothes` | `dirty-clothes` | "Dirty clothes" / "Ropa que se ensucia": the Warriors' vests, shirts and trousers get dirty, stained and torn with the story and every fight, per Warrior; "Change clothes" in the hideout makes them clean again. The game paints its texture layers once from your game files (`assets.ini`). See [Dirty clothes](#dirty-clothes-88-dirty-clothes-id-dirty-clothes). |
| `89-police-memory` | `police-memory` | "The police remember you" / "La policía te recuerda". Trouble you cause in a district is remembered per save profile; on later visits there are more patrols, more responders and more cops react. See [Police memory](#police-memory-89-police-memory-id-police-memory). |
| `90-gore` | `gore` | "Brutal fights" / "Peleas más brutales": more of the game's own blood per hit, ground pools that stay and blood on fighters that stays; `boot` only (the runtime's `ModBlood` settings), it spawns nothing itself. See [Brutal fights](#brutal-fights-90-gore-id-gore). |
| `91-own-crate` | `own-crate` | "A crate of our own" / "Una caja propia": the example of a mod's [own assets](#your-own-assets). A 0.6 m wooden crate stands in the hideout beside where the hub starts (x -184.2, y 95.0), solid to walk into, and knocks when placed and when the player walks into it. Its mesh is `own/crate.glb` (made by `tools/mod_examples/make_crate_glb.py`, Python standard library, deterministic: the same bytes on every run), drawn instead of a sheet of paper (`dyn_s_paper_list`) at the one point `crate.lua` gives its placed group; its wood texture is painted by `assets.ini` (`canvas` + two `place = wood` fills), its collision is the .glb's `collision` box (`[collision]`, level95) and the knock is synthesised (`[sound knock]`, two `synth` lines). Nothing in it comes from the game. Needs mods version 1.7. |
| `92-radar-colors` | `radar-colors` | "Gang colours on the radar" / "Colores de pandilla en el radar": enemy dots take their gang's colour, transit cops are bright green and riot cops dark grey, through `ModRadarColor`. See [Gang colours on the radar](#gang-colours-on-the-radar-92-radar-colors-id-radar-colors). |
| `95-jukebox` | `jukebox` | "Jukebox" / "Radio": a walk-up prompt a few steps south of the hideout's boom box opens an album-style list of the game's radio songs with the cover art read from your own game; each song plays as soon as the selection rests on it, A keeps it on the radio while you stay in the hideout (remembered with `ModSave`), "Shuffle" plays them all at random, "The game's radio" gives the game's own back. See [Jukebox](#jukebox-95-jukebox-id-jukebox). |
| `96-cowboy-hat` | `cowboy-hat` | "Cowboy keeps his hat" / "Cowboy no pierde su sombrero": Cowboy's hat stays on his head in every fight (in the original game it flies off when he takes a beating). `boot` only, through the game's own `HuSetKeepHat`. |
| `97-no-combo-slowmo` | `no-combo-slowmo` | "No combo slow motion" / "Sin cámara lenta en combos": in single player the fight combos that slow the whole fight down (X+A, A, A and others, most of all with Rage on) play at full speed, as in co-op. `boot` only, through `ModComboSlowMo` (min_runtime 1.10). |
| `98-cheats` | `cheats` | "Cheats menu" / "Menú de trucos": the game's own cheats from a menu (Back + LB held, or F8, in a mission) instead of button codes; unlimited health, rage and sprint switch on and off, the rest work once, and the cheats that change the save say so and ask twice. `saves = yes`. See [Cheats menu](#cheats-menu-98-cheats-id-cheats). |


**Built by the game.** Four example mods use meshes or texture layers made on your PC from your own
game files: `40-cleon-bandana`, `88-dirty-clothes`, `20-hideout-trophies` (Cleon's necklace) and
`80-dirty-fights` (the glasses that break). Each has an `assets.ini` recipe the game runs the first
time the mod loads (see [assets.ini](#assetsini-assets-built-from-your-disc)); nothing to install
by hand. When a mod names a `meshes` folder that is not there, the log says
`[mods] <id>: optional meshes not installed (folder <f>); the mod runs without them` and the mod
skips that part; the rest of each mod, and the other mods, work as described.

## Mesh overrides

A mod can bring replacement meshes for anything the game draws as an indexed triangle list
(characters, hats, props): `meshes = FOLDER` in `mod.ini` names a folder inside the mod, and while
the mod is enabled the runtime draws each replacement instead of the original. The game's
animation still drives it: a replacement keeps the original draw's vertex layout (and, for a
character, the bone palette), so the game's own vertex program skins or places it.

Files in the folder (`<mesh>` and `<tex>` are 16 lower-case hex digits):

| File | |
| --- | --- |
| `<mesh>.wmesh` | Replaces every draw whose content hash is `<mesh>`. |
| `<mesh>-<tex>.wmesh` | Only the draws of `<mesh>` whose first texture has the texture-pack hash `<tex>` (one skin of a model). Wins over `<mesh>.wmesh`. |
| same name `.png` | Optional: the texture the replacement is drawn with (else the draw's own). |
| `<mesh>.hide`, `<mesh>-<tex>.hide` | Those draws are not drawn at all (the file may be empty). |

A **subfolder** of the meshes folder (a-z 0-9 - _) holds the same kinds of files as a group named
`<mod id>/<subfolder>`. A group is off at start-up; the mod's Lua turns it on and off with
`ModMeshSet` (for example from the game's own state of a character), and while it is on its
overrides win over the meshes folder's own for the same draws. The state applies to every draw of
those meshes, unless the group is an instance group (below).

### Instance groups (per character)

Many characters share one model, so a group that changes a model's draw changes every character of
that model at once. A group folder that also holds a file `<hat>.worn` (`<hat>` = the content hash of
an object draw, usually the hat that carries a cut-out accessory; the file may be empty) is an
**instance group**: while it is on, the runtime decides per character and per frame:

- a **character draw** the group overrides uses the group's override only while that character's
  head carries no draw of `<hat>`; a character wearing it is drawn by the rest of the folder (the
  meshes folder's own override, or the game's own mesh);
- a **draw of `<hat>`** uses the group's override only while it is not on the head of such a
  character (one whose model the group overrides): on such a head the folder's own `<hat>.hide` (or
  the game's draw) applies; fallen to the ground or worn by anyone else, the group's mesh is drawn.

"On a head" is the runtime's own test: the hat draw's origin lies within 0.2 m (view space) of the
head of a character drawn in the same frame (the centre of the vertices the head joint carries, posed
by the bone palette). A character draw comes before its hat, so a character is judged by the previous
frame: its head there (the nearest within 0.3 m in view space) and what it wore. A character keeps
wearing the hat while no draw of it lies within 1 m of his head off any head (the hat is not drawn:
outside the view, some opening scenes), and loses it in the frame such a draw does (the hat flying
off); a character not seen in the previous frame wears it (he is drawn as the game draws him). One
`.worn` file per group; a `.worn` file outside a group folder is ignored (logged). With `--mesh-log`
the runtime prints the distance from each marker draw to the nearest head and whether it counts as
worn (the first 40 of each), and each change of the instance groups a character is drawn with.

A character who lost the hat, left the view and comes back is a new head to the runtime, drawn
wearing it, unless the mod remembers him: `ModMeshPoint(group, key, x, y, z)` gives the group a world
point (feet, as `GetPosition` returns them) under a key, usually the character's handle, and a head
that lies within 0.6 m (view space) of the segment from such a point up 1.8 m does not wear the marker,
whatever the previous frames said. The mod moves the point with the character (every 100 ms is enough)
and removes it (`ModMeshPoint(group, key)`) when he wears the hat again. Limit: another character of
the same model whose head comes that close to the body of one who lost it (nearly touching) is drawn
without it while there. Points are the runtime's: a new Lua state (the next level) starts with none.

### Placed groups (per object)

A group folder that also holds a file named `placed` (empty) is a **placed group**: while it is on,
its overrides apply only to object draws whose origin lies within 0.3 m (view space) of one of the
group's points (`ModMeshPoint`), so a mod can show one object differently from every other object of
the same mesh: spawn a spare object somewhere, give the group a point there, and only that one is drawn
as the group's mesh. A point stays until it is removed or the level reloads. Character draws are never
judged by a placed group. With `--mesh-log` the runtime prints the distance from such draws to the
nearest point (the first 40).

Points are world positions; the runtime takes them to view space with the frame's 3D camera (the
first world draw of the frame into the back buffer while any group has a point; the previous frame's
before it). `ModMeshPoint` returns the group's number of points, or `nil` (logged) for an unknown group
or a full one (64 points per group).

- The content hash is the `--mesh-pack` hash: FNV-1a 64 over the index count and strides, the
  indices and the bytes of the vertices they use (`runtime/src/meshover.h`).
  Run the game with `--mesh-log` to print the hash, size, strides and texture (its
  texture-pack hash, the `<tex>` of a texture-limited override) of each new draw, and the hash of
  every object drawn on a character's head (`[meshover] hat mesh ...`, with `--char-lighting`).
- `.wmesh` is the mesh pack's format (`WMP1`, vertex and index counts, the two strides, the vertex
  bytes, `u16` triangle-list indices) with the original draw's strides; a one-stream draw has
  stride 1 = 0. A replacement with other strides is not used (`[meshover]` line in the log).
- The log says `[mods] <id>: N mesh overrides from FOLDER` at start-up and
  `[meshover] mod mesh <hash> drawn` (or `hidden`) the first time each is used.
- With `--taa` (also `--taa-upscale`, `--frame-gen`) a replacement of an object draw gets its own
  motion like a character (its previous position, not only the camera's): a hat falling off stays
  sharp in the temporal history and in generated frames.
- **Nothing of the game may be shipped in a mod**, and meshes or textures made from the game's
  files are the game's. A mod that needs them ships a tool that builds them on the player's PC from
  their own game files, and its folder is not part of the repository (`mods/.gitignore`).

### Example: Cleon loses his bandana (`40-cleon-bandana`)

Cleon's bandana is modelled into his head (model `warr_cl`), so the game cannot knock it off like
a hat. His flashback model (`dest_cl`, the Destroyer before the Warriors) has the same head without
it. The mod's tool builds, from the player's game files:

- `<hat>.hide` (always on): the draw of `dyn_dest_ltb`, a Destroyers hat object no character of the
  game wears, is not drawn.
- the group `knocked/`:
  - Cleon (his own skin only: the ghost skins of the same model are left alone) with the flashback
    head: every part of the head above the neck comes from `dest_cl` (the neck seam closes
    exactly), drawn with Cleon's texture where the flashback head's texels are pasted in (both use
    the same layout).
  - The bandana, cut out of `warr_cl` as a rigid mesh in the head joint's frame, replacing the draw
    of `dyn_dest_ltb`, drawn with Cleon's original texture.

`bandana.lua` (boot) gives `dyn_dest_ltb` the head joint's frame (attach offset and knock-off poses
made identity in `CfgObj` / `CfgHat`) and puts it on Cleon's head with `HuPlaceHatOnHead` whenever
the level script creates him (100 ms after `HuCreate`; when the game gives him no hat yet, again every
250 ms for up to 60 s: in a rumble arena the sides are created as the intro starts and the game
attaches no hat to them for a while). The log says what happened to each Cleon:
`[mods] log: cleon-bandana: bandana placed on Cleon (try N) (character H, level L)`, or
`... not placed: Cleon already wears a hat ...` / `... NOT placed on Cleon: HuPlaceHatOnHead gave him no
hat in 240 tries ...`, and `[mods] log: cleon-bandana: bandana knocked off (level L)` when it falls.
While he wears it nothing changes on screen: Cleon is the game's own model with the modelled bandana
(opening scenes and cutscenes too) and the hat object is hidden.
Every 100 ms the mod asks the game (`HuHasHat`) whether each Cleon still wears the hat it placed;
once one does not (a hard hit knocked it off), it turns `cleon-bandana/knocked` on: Cleon shows his
flashback head and the hat object, falling and then lying on the ground, is drawn as the bandana
until the level reloads (each level starts with the group off).

Limit: the group applies to every draw of Cleon's model: if a level had two Cleons and one lost the
bandana, both would show the flashback head (an instance group, below, would make it per character).

Nothing to build by hand: the mod's `assets.ini` (see [assets.ini](#assetsini-assets-built-from-your-disc))
declares the three files and the game builds them from your own disc the first time the mod loads
(a fraction of a second). Enable the mod (`--mod cleon-bandana`); the log shows
`[modassets] cleon-bandana: built 3 assets (5 files) in N ms into <hdd>/mods-cache/cleon-bandana/<key>`
(later runs: `3 assets from the cache`) and `[modassets] cleon-bandana: 3 mesh overrides, ...`; when they
cannot be built (`[modassets] cleon-bandana: assets.ini:LINE: reason`) the mod changes nothing.

### Example: Cleon's necklace (`20-hideout-trophies`)

Cleon's necklace (two silver chains up the neck and two rows of silver drops) is not an object of
the game and not part of his model's geometry: it is painted on the bare chest of his skin texture
(model `warr_cl`; his flashback model `dest_cl` wears a tank top and no necklace). The only necklace
object of the game, `dyn_necklace`, is level 34's store loot. So the hideout-trophies mod shows it
the way a jeweller would: laid on deep red velvet in a wooden shadow-box frame hung on the trophy
wall. Its `assets.ini` builds, from the player's game files, one override:

- `7b6f89aa421da125.wmesh` / `.png`: the draw of `dyn_s_paper_list` (a sheet of paper, 8 triangles,
  that no level, sector or script of the game places: only its `CfgObj` registration exists) becomes
  the frame: a velvet board and four wooden bars made by the recipe (60 triangles, 0.32 x 0.04 x
  0.44 m, the back at y = 0 against the wall, the bottom at z = 0), drawn with a texture where the
  necklace's texels from Cleon's skin (the silver ones: neutral or bluish grey, unlike the brown skin,
  the red vest and the black background) lie on drawn velvet with a soft shadow, enlarged 3 times,
  and a drawn wood grain.

`trophies.lua` spawns `dyn_s_paper_list` on the wall above the crown's shelf, at (-169.75, 81.74, -192.62)
with no rotation, only when the story is finished
(`UM_IsLevelComplete(84)`, the final mission; `level95`'s own `flashback.SetupTrophies()` uses the
same check) and the meshes are there (`MOD_MESHES >= 1`); without them it logs
`[mods] hideout-trophies: dyn_s_paper_list: its meshes are not built (see the [modassets] lines of the log), not shown`
and the other trophies are placed as before.

The game builds it from the mod's `assets.ini` at the mod's first load; the log then shows
`[modassets] hideout-trophies: built 1 asset (2 files) ...` and `1 mesh override ... from the cache`.
A `meshes-v1` folder an older version of the mod had you build with a tool is no longer used (it is
reported as left over: delete it).

## assets.ini (assets built from your disc)

A mod may need meshes or textures made from the game's own: a character with another character's
head, a hat cut out of a model, a texture painted on a skin. Those are game data, so no mod can ship
them. Instead the mod ships a recipe, `assets.ini`, and the game builds the files from the player's
own disc (the disc folder it runs from) the first time the mod loads, before the title starts. No
tool, no Python, nothing to install: the build is plain C in the runtime.

- **Where the results go**: `<hdd>/mods-cache/<mod id>/<key>/` (`<hdd>` = the `--hdd` folder), in
  the [mesh override](#mesh-overrides) and [texture layer](#texture-layers) formats, with
  `build.json` and an empty `done`. They are loaded like a meshes folder: its subfolders are the
  mod's groups (`ModMeshSet("<id>/<group>", on)`), and they count in `MOD_MESHES`. Nothing derived
  from the disc is ever written into the mod's folder.
- **When it builds**: when the cache has no folder for the current key. The key (16 hex digits) is a
  hash of the engine version, the bytes of `assets.ini`, the mod's `version` and the disc (its
  `XBoxWad.idx`), so editing the recipe, a new mod version, a new engine or another disc builds
  again; the other keys of that mod are deleted after a successful build. `--rebuild-mod-assets`
  builds every enabled mod's assets again, `--rebuild-mod-assets ID` only that mod's.
- **The launcher builds them first**: PLAY in the launcher builds the assets of every enabled mod
  whose cache is not current with the same engine before it starts the game ("Preparing mods… 1 of 2"
  / "Preparando mods… 1 de 2" where PLAY was, the mod's name above it), so the game starts on a warm
  cache; a mod whose build failed shows the reason under it on the Mods page (in warning colour) and
  the game still starts, that mod without its assets. Mods whose cache is current are skipped at
  once (a read of `assets.ini` and `XBoxWad.idx`).
- **The disc's index**: the first build reads the header of every container on the disc (a few
  seconds on a cold disk) and keeps the list in `<hdd>/mods-cache/disc-<fingerprint>.idx` (about
  3 MB); later builds, in the launcher or the game, read that file instead. Safe to delete.
- **When it fails** (a model not on the disc, a seam that does not close, a typo): one line
  `[modassets] <id>: assets.ini:LINE: reason; the mod loads without its assets`, nothing is cached
  (a build writes into `<key>.partial` and renames it only when complete) and the mod runs with
  `MOD_ASSETS` 0, so its Lua changes nothing that needs them.
- **Budgets** per mod: 30 s of build time, 64 MiB and 256 files of output, textures at most 2048
  texels a side, meshes at most 65535 vertices. Over a budget the build stops as a failure.

The format: lines `key = value`, `;` starts a comment (a whole line or after a value), sections
`[kind name]` (a name is 1-63 of a-z A-Z 0-9 - _ .). The first line that is not a comment is
`format = 1`. Sections run in order; a value names only earlier sections.

| Section | Keys | |
| --- | --- | --- |
| `[source NAME]` | `char = MODEL` or `prop = 0xHASH` | A character by the model key the game's scripts use in `CfgChar` (`warr_cl`), or by its model chunk's hash (`0x2c7976bd`: a model `CfgChar` names only by a skin record of another name). Gives `NAME.skinN` (its N-th skin texture, 0 = its own) and its UV islands. `prop =`: a static prop by its chunk hash (`0x226dffca`, the sunglasses `dyn_sun`), taken whole (`take = all`). |
| `[texture NAME]` | steps, in order | An image (below). |
| `[mesh NAME]` | `from`, one of `head_swap` / `take` / `drop` / `shape`, `target` or `host`, `texture`, `group` | One mesh override file and its `.png`. |
| `[hide NAME]` | `host = OBJECT`, `group` | A `.hide` file: that object's draw is not drawn. |
| `[set NAME]` | lines `CODE = MODEL[, MODEL...]` | The entries a `[layer]` runs for: a code (a-z A-Z 0-9 - _) and up to 4 `CfgChar` model keys (`sw = warr_sw, warr_sw_cv`). |
| `[layer LABEL]` | `for`, steps below | [Texture layers](#texture-layers) painted over the clothes of every skin texture of the set's models: files `<tex>@<label>.png` at the top of the cache, label = LABEL with `{code}` (the entry's code) and `{stage}` (the stage) filled in. |

Selectors (sets of a source's triangles): `SRC.islands(above Z)` are the UV islands lying wholly
above height Z (metres, game z up), `SRC.islands(below Z)` the others; `headgear(OTHER, above Z,
tolerance D)` the islands above Z whose vertices are farther than D (mean distance) from OTHER's
head: a cap or a bandana that OTHER's head of the same base does not have; `largest_apart(min_tris N,
min_width W, top T)` the largest piece apart from the body (welded: vertices at the same position are
one) with at least N triangles, at least W metres across and lying within T metres of the model's top:
glasses modelled as their own piece in front of a face. Inside a `[mesh]`, a selector without `SRC.`
means the `from` source.

`[mesh]` keys:

| Key | |
| --- | --- |
| `from = SRC` | The source character. |
| `head_swap = OTHER, above Z[, closed_seam required]` | SRC with every island above Z replaced by OTHER's (vertex bytes copied unchanged, so the game skins it as SRC). With `closed_seam required` the build fails when OTHER's head does not close the neck seam exactly. |
| `take = SELECTOR` with `rigid = joint BIP_NAME` | The selected triangles as a rigid object in that rig joint's frame (`bip_sw_head`): for a hat. `take = all` for a `prop =` source (its vertices as they are). |
| `drop = SELECTOR` | SRC's draw without the selected triangles (same vertices): a face without its glasses. |
| `paint = frame R G B, lens R G B, size N` | (glasses) Draw the mesh with a texture made here, N x N: the lens triangles flat on the left half in the lens colour, the rest on the frame colour. |
| `break = turn DEG, gap M, lens DEG, shards N, seed S` | (glasses) Lay the pair on the ground, snap it at the bridge (the half at x > 0 turned DEG and moved M away), leave that half's lens out (triangles within `lens` DEG of the facing axis) and scatter N shards of it around. |
| `crack = lines N, seed S` | With `break`: N light crack lines drawn into a copy of the texture over the kept lens. |
| `shape = shadow_box, board W H texels M, bar B, depth D, board_depth E` with `uv = board TEX.PLACE, wood TEX.PLACE` | A box frame (a W x H texel board at M metres a texel, four bars B wide, D deep): the board shows the `board` place of the texture, the bars the `wood` place. |
| `target = SRC skin N` / `target = SRC` | The file replaces SRC's draw with its skin N only (`<mesh>-<tex>.wmesh`), or with every skin (`<mesh>.wmesh`, no `texture`); for a `head_swap` or a `drop`. |
| `host = OBJECT` | The file replaces an object nobody uses (`dyn_dest_ltb`, a Destroyers hat; `dyn_s_paper_list`, a sheet of paper; `dyn_civl_hl_ho` and `dyn_pl_dr`, hats; `dyn_s_newspaper_c`, a folded newspaper), in its vertex layout; `colour = AARRGGBB` fills its diffuse colour. Its draw hash is computed from the object's own prop chunk on the disc (the hash `--mesh-log` shows). |
| `worn = yes` / `placed = yes` | With a `group`: the group's `<host>.worn` instance marker ([Instance groups](#instance-groups-per-character)) / its `placed` marker ([Placed groups](#placed-groups-per-object)). |
| `texture = TEX` | The `.png` it is drawn with: `SRC.skinN` or a `[texture]`. |
| `group = NAME` | Into the group folder `NAME` (off at start, `ModMeshSet`). |

`[texture]` steps:

| Step | |
| --- | --- |
| `base = SRC.skinN` / `canvas = W H, colour R G B` | Start from a skin, or a plain image. |
| `require = same_size TEX` / `require = size W H` | Fail unless the sizes match (another disc). |
| `paste = TEX, uv_of SELECTOR, dilate N, keep SELECTOR` | Copy TEX's texels under the first selector's triangles (grown by N texels), except under the `keep` selector's. |
| `crop = X Y W H` | A rectangle (texel y * width + x: past the right edge it continues on the next row). |
| `mask = colour(r-b <= A, sum >= B)[, parts >= N][, inside][, at_least M]` | Texels of that colour in 8-connected parts of N or more, not touching the border with `inside`; fails with fewer than M. |
| `polish = keep A, grey B, gain G, add D` | Masked texels become (A * colour + B * grey) * G + D. |
| `fill = velvet R G B, noise N, seed S[, shadow]` | Unmasked texels become velvet (a seeded noise of N), darker below and right of the mask with `shadow`. |
| `scale = N bilinear` | N times larger. |
| `place = TEX, at X Y[, as NAME]` / `place = wood R G B seed S, at X Y W H[, as NAME]` | Copy a texture, or draw wood grain, onto the canvas; the place's name (default: TEX, or `wood`) gives a `[mesh]`'s `uv`. |

`[layer]` steps (every value but `for` and one `effect` is optional; the defaults are the bundled
dirty clothes', recipe 1.1). Per-stage values `A B C` are those of stages 1, 2, 3; each stage
keeps what the earlier ones painted and adds to it.

| Step | |
| --- | --- |
| `for = SET` | Runs once per entry of the set (in its order: `{index}` 0, 1, ...), for every model of the entry. |
| `skins = claimed [SUFFIX...]` / `skins = all` | `claimed`: a skin texture belongs to the first entry whose model keys, or the keys plus a suffix (`_gen`, `_cv1`), name it in the game's skin table; a model's own skin list also names other models' skins, and those are left to their owner. `all`: every skin a model lists. A skin is painted once, for the first entry that reaches it. |
| `skip = ghost, painted < S` | Skins left alone: `ghost` (blue clothes: mean blue more than 15 above mean red), `painted < S` (fewer than S of the clothing texels are painted, R+G+B > 40: a skin of another UV layout). |
| `seed = A + B * {index}` / `seed = N` | The seed of the entry's noise and marks. |
| `stages = 1, 2, 3` | The stages written (1 to 3). |
| `region = cloth(mean_z A..B, abs_x <= X, legs below Z, skin_island S, head above H)` | The clothing texels: the model's UV islands whose vertices' mean height (metres, bind pose) lies in A..B and mean \|x\| is at most X; below Z they are trousers, above it torso; a torso island with S or more texels of the face's colour is skin and left out. The face is the largest island lying wholly above H. |
| `protect = face_colour near N share S` | Torso texels of the face's colour are left alone, and so is a torso texel whose (2N+1)^2 window is more than S skin. |
| `vest = red R, patch P, edge E, patch_keep K` | Hue protection: on red texels the grime and stains only darken. A torso R or more red is a vest; its back patch (texels P times brighter than its median red, or orange-yellow, where they cluster) gets K of the grime and no stains or tears. The garment's edge: texels within E texels (at 512 wide) of a texel that is not cloth. |
| `effect = grime tint R G B, scale F, legs_bias Z0 K, torso_bias Z0 K, spread A B C, dark A B C, dark_cloth A B C, desat A B C` | Grime patches (3D noise of frequency F per metre over the bind-pose position, more where (Z0 - z) * K is high), spreading by `spread`; they darken the red by `dark` and other cloth by `dark_cloth` (in sRGB terms, one factor for all channels in linear light) and tint other cloth by `tint` / its largest channel; the red loses at most `desat` of its saturation. Stage 1: only at the garment's edge. |
| `effect = dust colour R G B, scale F, edge A B C, legs L, below Z` | A light grey film at the edge, and on the trousers below Z a film of `colour` (L times the grime's spread). |
| `effect = scuff A B C` | Small lighter, duller rubbed spots. |
| `effect = stains A B C` / `effect = tears A B C` | How many sweat and grease stains (darker inside, a darker rim; at most 16) and tears (shadowed skin through the cloth, frayed threads at their edges; at most 7: knees, the lower legs, the vest's hem, a strap) each stage shows. |
| `effect = damp stage N, above Z, amount A` | Sweat-dark torso cloth above Z at stage N. |
| `effect = wear A B C` | Worn-out cloth: a little darker all over, greyer off the red. |
| `grow = N` | The painted texels grown N texels into the unpainted ones (filtering at the island edges). |

The shapes, noise octaves and mark placement are those of recipe 1.1 (the per-texel formulas of
[Dirty clothes](#dirty-clothes-88-dirty-clothes-id-dirty-clothes), "Recipe 1.1 parameters"). A
layer whose set reaches no skin (another disc) fails like any step.

Randomness is seeded (Python's MT19937 numbers; the layers' own xorshift), so a recipe gives the
same files on every PC with the same disc. The bundled recipes: `mods/40-cleon-bandana/assets.ini` (head swap, headgear, hide),
`mods/20-hideout-trophies/assets.ini` (mask, velvet, atlas, frame),
`mods/80-dirty-fights/assets.ini` (pieces apart, drop, instance and placed groups, broken glasses)
and `mods/88-dirty-clothes/assets.ini` (a set and its layers). A mod's own meshes, textures and
sounds: see [Your own assets](#your-own-assets).

## Your own assets

A mod can bring its own creations: meshes made in Blender (or any tool that writes glTF), textures
and sounds. They go in the mod's `own/` folder and are named by its `assets.ini`, which converts
them into the game's formats in the same cache as the assets built from the disc (so the
launcher's "Preparing mods" and the game build them the same way; nothing to install). Your files
are yours: only ship what you made or may share. The bundled example is
[`91-own-crate`](#example-mods-bundled-all-off-by-default) (mods version 1.7; set
`min_runtime = 1.7`).

| File in `own/` | Read as |
| --- | --- |
| `NAME.glb` | glTF 2.0 binary: meshes (static or skinned), and the base colour texture packed inside |
| `NAME.png` | an 8-bit image (any colour type; alpha kept) |
| `NAME.dds` | BC1 (DXT1) or BC3 (DXT5) with its full mip chain (BC7: use a PNG) |
| `NAME.wav` | PCM 8/16/24/32-bit or 32-bit float, mono or stereo, 8-96 kHz |
| `NAME.ogg` | Ogg Vorbis, mono or stereo (not Opus or FLAC) |

`own/` may hold one level of subfolders; paths in `assets.ini` are always `own/...` and never
leave it. Every file of `own/` is part of the cache key, so changing any of them builds again.

### In assets.ini

| Section / key | What |
| --- | --- |
| `[source NAME]` `file = own/x.glb` | Your mesh. Every mesh node of the scene, all its triangle primitives merged; a node or mesh whose name starts with `collision` is not drawn but kept for `[collision]`. |
| `[mesh NAME]` `from = SRC`, `take = all`, `host = OBJECT` | Your mesh drawn instead of a host object (the [hosts](#assetsini-assets-built-from-your-disc): `dyn_dest_ltb`, `dyn_s_paper_list`, `dyn_civl_hl_ho`, `dyn_pl_dr`, `dyn_s_newspaper_c`), in the object's own frame: model it standing on its origin. `group`, `placed = yes` and `worn = yes` work as for the game's meshes, so a prop is usually a placed group at one point (below). |
| `rigid = joint J, of CHAR` | (with `host`) Moves the mesh from CHAR's rest pose into joint J's frame, the frame hats are drawn in: model a hat on the character's head, in place, and give `rigid = joint bip_sw_head, of cleon` with `host = dyn_dest_ltb`; the game's own hat code then knocks it off like any hat (the `40-cleon-bandana` pattern: `[hide]` the host while it is worn). |
| `target = CHAR [skin N]` | A part of a character: your mesh replaces that character's draw (every skin, or only skin N with `texture =`), skinned with the game's animation. A skinned .glb keeps its weights (joints by name, below); a static one needs `joint = J` (every vertex rigid on that joint). |
| `append = yes` | (with `target`) Your triangles drawn after the character's own, in the same draw: a pouch, a watch, a bigger collar. The draw has one texture, so the part's UVs must use free space of the character's skin (or `texture =` your own copy of the skin with the part painted in). |
| `texture = own/x.png` | Any texture slot (`[mesh] texture =`, `[texture] base =`) also takes an own file: a PNG, a DDS, or `own/x.glb` for the texture packed inside that .glb. At most 2048 texels a side, any size (not only powers of two). |
| `[collision NAME]` `from = SRC`, `level = levelNN`, `at = X Y Z`, `heading = DEG`, `material = N`, `use = collision\|mesh` | Solid triangles for one level (a [collision patch](#collision-patches)): the .glb's `collision` meshes (else the visible mesh), turned `heading` degrees about the up axis (counter-clockwise seen from above) and moved to the world point `at` (the point your Lua places the prop at). At most 2048 triangles, two-sided. |
| `[sound NAME]` `file = own/x.wav` / `own/x.ogg` | A sound for `ModSound("NAME", ...)`, decoded once at the build (at most 120 s each, 16 MiB of samples per mod). |
| `[sound NAME]` `synth = sweep F0 F1 \| tone F, seconds S, attack A, decay D, noise N, seed K, volume V` | A sound made from numbers: a sine gliding from F0 to F1 Hz (20-12000) plus low-passed noise (N 0-1), a linear attack of A s and an exponential decay with time constant D s (0 = none), at most 10 s, 22050 Hz mono. Up to 8 `synth` lines are mixed (a knock and its ring, a chord). |

A placed prop, as `91-own-crate` does it:

```ini
[source crate-glb]
file = own/crate.glb

[mesh crate]
from    = crate-glb
take    = all
host    = dyn_s_paper_list
colour  = ff000000
texture = own/crate.glb          ; or a [texture] painted in assets.ini
group   = crate
placed  = yes

[collision crate-col]
from  = crate-glb
level = level95
at    = -184.2 95.0 -194.38
```
```lua
-- after.level95: spawn the host where the collision stands, and give the placed group its point
local h = ObjSpawn("dyn_s_paper_list", {-184.2, 95.0, -194.38}, {0, 0, 0, -1}, -1, 1, 0, 4294967295, nil)
ObjEnablePhysics(h, 0)
ModMeshPoint("own-crate/crate", "crate", -184.2, 95.0, -194.38)
ModMeshSet("own-crate/crate", 1)
```

The host's own size decides when the game culls it at the screen's edges, so a prop much larger
than the host may vanish a little early at the border; a placed group only replaces the host at its
points, so other mods' uses of the same host elsewhere are not touched.

### Limits and errors

Each problem stops the mod's build with one line, in the log (`[modassets] <id>: assets.ini:LINE:
own/x.glb: reason; the mod loads without its assets`) and under the mod on the launcher's Mods page:

| Message (part) | Fix |
| --- | --- |
| `not a glTF binary (.glb) file` | Export as **glTF Binary (.glb)**, not glTF Separate or Embedded. |
| `its data is in another file` | The same: everything must be inside the .glb. |
| `no UVs` / `no normals` | Unwrap the mesh; export with Normals (and UVs) on. |
| `only triangles` | Points and lines are not drawn; Blender writes triangles. |
| `N vertices: too many (at most 65535 ...)` | Fewer vertices: every UV seam and hard edge splits vertices. |
| `the mesh is N m across: it looks exported in centimetres` | Over 10 m: set Unit Scale 1.0 and Apply > Scale. |
| `unknown joint X (closest: bip_...)` | A vertex group / bone that is not one of the game's joints (list below). |
| `joint bip_sw carries no vertices` | `bip_sw_ground` and `bip_sw` move the whole character; weight to `bip_sw_pelvis` or below. |
| `vertex N has no weight on any joint` | Weight Paint the part fully. |
| `too many bones: the skin has N joints` | More than 64 joints in the armature: export only the deforming bones. |
| `joint X does not move in CHAR's draw (its joints: ...)` | That draw is animated with the listed joints only; move the weight to one of them. |
| `has no skin: ... or attach it rigidly with joint = NAME` | A static mesh as a character part needs `joint =`. |
| `bad texture size WxH` | 1 to 2048 texels a side. |
| `a BC7 DDS` / `the DDS cannot be used: ...` | Use BC1/BC3 with all mips, or a PNG. |
| `a path outside the mod's own folder` | Paths are `own/NAME` or `own/SUB/NAME` (a-z A-Z 0-9 - _ .). |
| `N s long` / `more than 16 MiB decoded` | Shorter or fewer sounds (mono halves the size). |

Warnings do not stop the build (`[modassets] <id>: warning: ...`): more than 3 joints on a vertex
(the 3 strongest are kept and renormalised), a skinned object that was moved, turned or scaled
(the rest pose is used).

### The game's joints

A skinned part's vertex groups (glTF joint node names) must be these names; palette slot = joint
index - 2, and joints 0 and 1 carry no vertices. These are identifiers only (the names the game's
rig uses); the rig itself, its rest pose and the characters come from your own disc.

`bip_sw_ground` (0), `bip_sw` (1), `bip_sw_pelvis`, `bip_sw_spine`, `bip_sw_spine1`, `bip_sw_neck`,
`bip_sw_head`, `bip_brow_left`, `bip_jaw`, `bip_lip_lower`, `bip_mouth_lft`, `bip_mouth_lft1`,
`bip_mouth_rt`, `bip_mouth_rgt1`, `bip_eye_lid`, `bip_lip_upper`, `bip_sw_l_clavicle`,
`bip_sw_l_upperarm`, `bip_sw_l_forearm`, `bip_sw_l_hand`, `bip_sw_l_finger1`, `bip_sw_l_finger2`,
`bip_sw_r_clavicle`, `bip_sw_r_upperarm`, `bip_sw_r_forearm`, `bip_sw_r_hand`, `bip_sw_r_finger1`,
`bip_sw_r_finger2`, `bip_sw_l_thigh`, `bip_sw_l_calf`, `bip_sw_l_foot`, `bip_sw_r_thigh`,
`bip_sw_r_calf`, `bip_sw_r_foot`.

Each character's draw moves with only some of them (a face draw has no legs): the build names
them when a part uses another.

### Exporting from Blender

1. **Units and axes**: metres (Scene > Units: Metric, Unit Scale 1.0). Blender's Z up is the game's
   Z up: the exporter writes glTF's Y-up and the game turns it back, so a prop modelled standing on
   Blender's origin stands on the point where the mod places it. Apply rotation and scale
   (Ctrl+A > All Transforms) before exporting a static mesh; its object transform is applied, so
   leave it at the origin unless you want it offset.
2. **A prop or a hat**: model it around the origin (a hat: around the head of a character in its
   rest pose and give `rigid = joint bip_sw_head, of CHAR`). Add a simple closed mesh named
   `collision` for a prop (a box is best: few triangles, the player slides along it).
3. **A part of a character**: parent it to an armature whose bones carry the game's names (above)
   in the game's rest pose, weight it (Weight Paint; at most 3 bones per vertex count), keep the
   object unmoved relative to the armature, and work in the rest pose.
4. **UVs and texture**: one UV map (the first, `UVMap`); one image per mesh. A texture in the
   material's Base Color is packed into the .glb (Image Texture node into Base Color), or give it as
   its own PNG.
5. **File > Export > glTF 2.0**: Format **glTF Binary (.glb)**; Include: Selected Objects (your
   mesh, its `collision` mesh and, for a part, the armature); Transform: **+Y Up** on; Data: Mesh
   with UVs and Normals, Apply Modifiers on; Skinning on for a part; Animation off. Save it into the
   mod's `own/` folder.
6. Start the game with the mod (or press PLAY in the launcher): the log's `[modassets]` lines say
   what was built, or the exact problem.

## Dirty fights (`80-dirty-fights`, id `dirty-fights`)

"Peleas más sucias: accesorios que caen" / "Dirty fights: falling accessories". Off by default,
`saves = no`.

**Accessories the game already has as separate objects.** The game sets 52 `hat_object`s up as
`OBJECT.TYPE_RIOT_HAT` (no physics, never leave the head): hair pieces, helmets and visors, but also
whole accessories. `boot` (`dirty.lua`) wraps `CfgObj` and gives these twelve the settings of the
game's own flying hats (the pimps': `OBJECT.TYPE_HAT`, `PHYS.OBB` on `AXIS.Z`,
`ANIM.LeftHandHatPickUp`), as `10-extra-hats` does for the police caps:

| Object | What | Worn by (CfgChar) |
| --- | --- | --- |
| `dyn_sun` | sunglasses | `civl_ct_eb` |
| `dyn_tr_cw` | gold-rimmed glasses | `civl_tr_cw`, `civl_ct_ma1` |
| `dyn_scps` | black cap with sunglasses | `civl_scps` |
| `dyn_we_ma2` | red cap with sunglasses | `civl_we_ma2` |
| `dyn_beansie` | winter hat | `dest_lt_be` (Destroyers lieutenant) |
| `dyn_bm_vb8` | winter hat | `civl_bm_vb8` |
| `dyn_bm_vb9` | red and white cap | `civl_bm_vb9` |
| `dyn_samo1` | do-rag | `samo_so_va1` (Satan's Mothers) |
| `dyn_bm_va1` | paper bag (1.5) | `civl_bm_va1` (Old Crazy, CfgChar 270, 285) |
| `dyn_turn_so` | sunglasses (1.5) | `turn_so4`, `turn_so5` (Axel and Skid, the Turnbull ACs) |
| `dyn_pl_ma1` | sunglasses (1.5) | `civl_pl_ma1` (Cameron, Pelham) |
| `dyn_huns_lt2` | rag tied on the back of the head (1.5) | `sava_lt2` (Lingo, Savage Huns) |

Three hats the game already throws (`TYPE_HAT`, with their own knock-off poses) are in the same list
(1.5) so this mod's rule (below) knocks them off too; the game's own threshold for their classes
seldom does: `dyn_sata_bo` (Spider's helmet, `samo_bo`), `dyn_panz_lt` (Isaac's cap, `panz_lt`) and
`dyn_panz_so` (the beret of Keenan and Theo, `panz_so_vb1`, `panz_so_vb2`). `CfgObj` gives them the
settings they already have; `poses.lua` adds donor poses only for the pairs they lack.

**Knock-off poses** (version 1.4). The game registers, per hat, where it sits on the head in each
knock-down animation (`CfgHat`); a hat without a pose for the animation is put in the head joint's own
frame, inside the head and turned the wrong way (1.3: Lamar's red cap, `dyn_we_ma2`, and Scopes'
black cap, `dyn_scps`). None of the riot hats has a pose of its own, so `after.config_preload2`
(`poses.lua`) gives each the police cap's poses (`dyn_cops_va`, 22) and, for the pairs the cap lacks,
the pimp's hat's (`dyn_pimp_va`), as a change of the worn transform: the donor's pose relative to its
attach transform (`CfgObj` arguments 21, 22), applied to the target's own attach transform, so each
keeps its place and facing on the head (the winter hats and glasses are modelled turned half round
from the caps). Log: `[mods] dirty-fights: N knock-off poses added to M headgear models: ...`.

Not changed: riot hats that carry hair in the same mesh (a beanie, cap, bandana or glasses over hair;
without them the head would be bald): `dyn_bm_vb4` (Rudy, Dante), `dyn_bm_vb1` (Ron Weed),
`dyn_eh_ma3` (Carloz: cap, hair and glasses), `dyn_eh_ma4` (Chico, Chuck), `dyn_tw` (Twiggy),
`dyn_bn_fa1`, `dyn_bn_fa2` (Julie, Karen: hair and glasses), `dyn_ct_fa3` (Jane: hair, flower and
sunglasses), `dyn_samo2` (Ted: bandana, hair and glasses), `dyn_huns_lt1` (Jimmy Lo); hair pieces
(`dyn_mohawk`, `dyn_sml_fro`, ...), helmets and visors, police caps (`10-extra-hats`). Headgear
modelled into the head mesh as one shell (no separate piece) cannot fly: the Baseball Furies' caps and
Cobb's helmet, Tuttle's cap, Rosie's hat, the Savage Huns soldiers' rags, Spider's goggles, the
glasses of Beansie, Sid and Maria (painted or part of the face). Virgil's comb is a separate piece of
his head mesh (26 triangles), not an object: it could be cut out like Vargas's glasses, but that
needs a spare hat object no character wears in the engine's 36-byte vertex layout, and none is left
(`dyn_turn_so_vb` has 28-byte vertices). Each object is
its own hat, so each character loses only his own; worn by the player after a pick-up it sits where the
game puts that hat.

**Sunglasses modelled into a head** (needs the meshes, below). Two characters have sunglasses as a
separate piece of their head mesh in front of a complete face with eyes: Vargas (the Hurricanes,
`hurr_va`, CfgChar 119: level 5, AotN level 62) and Masai (the Riffs, CfgChar 64 `riff_bo_va1`: AotN
levels 60 and 64, level 84). The game cuts each pair out as the mesh of a spare hat object no character
wears (Vargas: `dyn_civl_hl_ho`, Masai: `dyn_pl_dr`), in the head joint's frame, and writes the face
without it; each pair is an instance group (`glasses-vargas`, `glasses-masai`, see [Instance
groups](#instance-groups-per-character)), turned on at every level start. `dirty.lua` makes the spares
flying hats in the head joint's frame (attach offset and knock-off poses identity, like the bandana of
`40-cleon-bandana`) and puts the spare on the wearer with `HuPlaceHatOnHead` 100 ms after the level
script creates him. On his head the hat is not drawn and he looks as the game draws him; once it is
off, he is drawn without the glasses and the hat object, falling and lying on the ground, is drawn as
his glasses. Another character of the same model keeps his. Picked up and worn by the player, the
glasses sit on the player's head joint where they sat on the wearer's (the player's face is a little
different: they can sit slightly off). Not cut: the Panzers lieutenant's sunglasses (`panz_lt` already
wears a hat, and a character carries one), goggles and headbands modelled into the head shell (no
separate piece, no head without them), and accessories painted into a skin texture (most pedestrians').

**When they fall** (both kinds; the rule of the police caps): every 100 ms each watched character
(created by the level script or a gang spawner, wearing one of these hats) loses it with the game's own
`HuDrop(h, 1, 0, 0)` when a hit takes him under 20 % of his health, when one hit takes 10 % or more,
and when he is knocked out or dies; the game also applies its own hat rule to them now that they are
`TYPE_HAT`. The checks the mod schedules (placing the glasses 100 ms after a character is created, the
lost-hat watch) are scheduled again when the game dropped a pending one, e.g. on a level change (1.5:
before, characters created after that were never watched). Log: `[mods] dirty-fights: dyn_sun knocked off character 279 (health 12.0 %)`.

**Glasses that break** (needs the meshes; version 1.1). 1.5 s after a pair fell (both kinds of
sunglasses above, `dyn_sun`, `dyn_tr_cw` and, 1.5, `dyn_turn_so`, `dyn_pl_ma1`), if it still lies where it fell, it breaks with a chance
that grows with the hit: 50 % for a knock-off by a hit of 10 % of health, 2 % more per % above (less
below), 60 % at least when the wearer is knocked out or killed, never under 30 % or over 85 %. The pair
is removed (`ObjDestroy`) and a prop nothing in the game places (`dyn_s_newspaper_c`, a folded
newspaper; no other bundled mod uses it) is spawned in its place, turned at random about the
vertical; the game builds, per kind, the broken pair as that object's mesh: lying on the ground, snapped at the bridge (one half turned 28° away), the lens of the
kept half cracked (light lines drawn into a copy of its texture) and the other lens gone, scattered as
nine glass shards around it. Each kind is a placed group (`broken-vargas`, `broken-masai`, `broken-sun`,
`broken-trcw`, `broken-turnso`, `broken-plma1`, see [Placed groups](#placed-groups-per-object)): the mod gives it a point where each
broken pair lies (moved with it when it is kicked), so several kinds can lie in one level and the
prop's own draw is never changed elsewhere. Vargas's and Masai's broken pairs
use their skin texture; the retail pairs are drawn in colours the recipe picks (black frame and dark
lenses, gold frame and light lenses), not their own texture. A broken pair cannot be picked up (the
prop is a plain object: the game offers no pick-up), on purpose: a broken pair is of no use, and worn
it would be drawn as the intact one. Intact pairs stay pickable and wearable. Log: `[mods] dirty-fights: dyn_sun broke (character 279, hit
14.0 %) at x y z`.

**Lost stays lost.** While a character has lost the glasses of an instance group, the mod gives the
group his position every 100 ms (`ModMeshPoint(group, handle, x, y, z)`, removed when he wears them
again or is gone), so he is drawn without them after leaving the view and coming back (the runtime
judges every head by these points; see [Instance groups](#instance-groups-per-character)), until the
level reloads (a new Lua state drops the points).

**Going back for the hat.** Every character the mod watches (created by the level script or a gang
spawner, wearing any hat, police caps excepted: `10-extra-hats`) who loses a hat the game throws (the
headgear above, the spares, and the game's own flying hats) may go back for it: 60 % of them do. From 2.5 s
after the loss, every 0.5 s, once he has been calm for 1.5 s (alive and conscious, `BrHasEnemies` false,
no player within 8 m) and the hat lies within 7 m of him, the mod gives him the game's own "get my hat"
goal, `GoalGetItem(h, 4, hat)` (the goal the game's thug brain gives itself when it wants its hat back,
`BrSetThugWantsHat`): he walks to it, picks it up and puts it on. A second try follows 8 s later if the
first did not end with the hat on his head; then he leaves it. Nobody goes back for a hat that was
picked up, moved, broken or is too far, nor a dead character or the player. Picked up again, the hat can
be knocked off again. Log: `[mods] dirty-fights: character 279 goes back for his dyn_bm_vb9 (4.2 m)`,
`... put his dyn_bm_vb9 back on`.

**The meshes** are built by the game from your own disc the first time the mod loads
(`mods/80-dirty-fights/assets.ini`, see [assets.ini](#assetsini-assets-built-from-your-disc)): the log
says `[modassets] dirty-fights: built 12 assets (28 files) in N ms into ...` (later runs: `12 assets
from the cache`), then `[modassets] dirty-fights: 12 mesh overrides, 0 texture layer files from the
cache`, and the mod turns the eight groups on. If they cannot be
built (another disc, a model missing) the headgear above still flies (and is fetched) but no pair ever
breaks, and the glasses of Vargas and Masai stay on. A `meshes-v2` folder left in the mod by the
Python tool of version 1.2 is no longer read; delete it.

## Texture layers

A mod can paint over part of a character's skin texture without replacing the whole texture, and
change it per character and at any time: files `<tex>@<label>.png` in the mod's `meshes` folder
(the folder itself, not its subfolders). A game without texture layers ignores the files and has
no `ModTexLayer`: a mod checks `if ModTexLayer then ... end`.

- `<tex>`: the texture-pack hash of a texture (16 lower-case hex digits, as `--dump-textures`
  names them); `<label>`: 1-40
  of `a-z 0-9 - _`. All files of one label are the layer `<mod id>/<label>`, off at start-up;
  `ModTexLayer(layer, on)` turns it on and off.
- While a layer is on, every **character draw** whose stage-0 texture is `<tex>` is drawn with that
  texture with the layer's PNG blended over it by the PNG's alpha (colour only: the texture's own
  alpha is kept). A layer of another size is sampled at the nearest texel. When a mesh override
  with its own PNG replaces the draw, the layer is blended over that PNG (same UV layout).
- Several layers of one texture stack: mods in their load order, inside a mod by file name. So a
  layer that paints the clothes and another mod's layer that paints the face both show; each mod
  should only paint (alpha > 0) the part of the texture it changes.
- Per character: most characters with their own skin texture (the Warriors, bosses) are changed
  one by one by giving each texture its own label. Characters that share one texture share its
  layers.
- The composed texture is made once per combination of texture and layers on (B8G8R8A8 with mips,
  up to 1024 combinations per run) and cached; turning layers on and off costs nothing more after
  the first time. Not applied over a `--texture-pack` replacement or `--sharp-faces` upscale of the
  same texture: a layered draw uses the game's texture (or the mesh override's PNG) as its base.
- The log says `[mods] <id>: N texture layer files from FOLDER` at start-up; an unreadable PNG logs
  `[texlayer] cannot read the layer ...` once and the draw keeps its texture without it.
- As with meshes, layers are made from the game's textures, so no mod ships them: a mod's
  `assets.ini` (`[layer]`, see [assets.ini](#assetsini-assets-built-from-your-disc)) has the game
  paint them on the player's PC into `<hdd>/mods-cache/`, loaded like a `meshes` folder (the log:
  `[modassets] <id>: ..., N texture layer files from the cache`).

Source: `runtime/src/texlayer.c` (files, layers, combinations, the blend), `hle_d3d.c` (the draw's
texture hash, compose and upload).

## Dirty clothes (`88-dirty-clothes`, id `dirty-clothes`)

"Ropa que se ensucia" / "Dirty clothes". Off by default, `saves = no`. The Warriors' vests, shirts
and trousers collect dirt, stains and tears as the story goes on and with every fight, each Warrior
on his own; in the hideout, "Change clothes" makes the whole crew clean again. Like the film: they
end the night wrecked.

**Wear.** Each Warrior has wear points: 2 for every story mission completed since the crew last
changed clothes (the missions of `global.lua`'s `findLastMission`, Armies of the Night left out),
plus his own fight points: 1 for every 25 % of health he loses (healing gives nothing back), 2 more
when he is knocked out (0.5 % health or less). Stage 0 is clean, stage 1 from 4 points (worn),
stage 2 from 9 (dirty: stains and the first tears), stage 3 from 15 (wrecked: grime all over, more
stains and tears, sweat-dark cloth). Kept with `ModSave` per save profile (`base` = story missions
done at the last change, `fight.<code>` = fight points; codes `cl sw aj co cb fo ve re sn`). A
profile that never changed clothes counts the whole story so far. Fighters are the Warriors the
level creates (`HuCreate`) and the player; every 250 ms their health is read. Every number is a
field of `DirtyClothes.cfg` in `clothes.lua`. Log: `[mods] dirty-clothes: stages (level95): cl 0
(2) sw 3 (17) ...` at every level start, `sw: +1 fight point (hits), wear 10` for each point.

**Look.** For every skin texture of a Warrior (his gameplay model and his `_cv` model's outfits,
the cutscene and civilian clothes) the game paints three layers (`<tex>@<code>-<stage>.png`)
over the clothing texels only: the UV islands on the legs (trousers) and on the torso (vest,
shirt), not the arms, hands, feet or head, and on the torso no texel of the Warrior's skin colour
(measured on his face) or among such texels, so the chest under the open vest, the necklaces
painted on it, the faces and arms stay as they are. Dirt is made from each texel's 3D bind-pose
position (no seams between UV islands). The vests keep their red at every stage: on red texels the
grime and the stains only darken (one factor for all three colour channels in linear light, so the
hue stays) and take away little of the saturation (none at stage 1, at most 10 % at stage 2 and 18 %
at stage 3), and the back patch (the WARRIORS letters and the emblem: the cream and orange-yellow
texels of a red vest where they cluster) gets about a tenth of the grime and no stains or tears.
Other cloth (trousers, shirts, the cutscene tanks) takes a warm soot tint. Stage 1 (worn) is only
wear at the garment's edges: a little dust and dark grime along the hems, armholes and collar and a
few rubbed, lighter spots, no tint over the cloth and no stains. Stage 2 (dirty) adds grime patches
(more on the lower legs and the vest's hem), dust on the trousers, sweat and grease stains with a
darker rim and the first two tears, which show shadowed skin through the cloth with frayed lighter
threads at their edges. Stage 3 (wrecked) adds more grime, stains and tears and sweat-dark cloth at
the collar and armpits; the red and the back patch stay readable. Each stage keeps the marks of the
earlier ones. No blood (that is the gore mod's). The blue ghost skins are left alone.

*Recipe 1.1 parameters* (`mods/88-dirty-clothes/assets.ini`; all
colour math per texel, results rounded to 0..255): hue protection `red_weight` = smoothstep(0.10 ->
0.06) of the hue's distance from red (in turns) x smoothstep(0.10 -> 0.22) of the HSV saturation x
smoothstep(0.06 -> 0.14) of the value, torso texels only; a torso with >= 40 % texels of weight >
0.5 is a vest; its patch = texels 1.45x brighter (R+G+B) than the median red texel with weight <
0.5, or hue 0.06-0.19 with saturation > 0.45 and value > 0.4, kept where >= 40 % of the
(2r+1)^2 window (r = 2 texels at 512) agrees, grown by r; the edge weight is 1 - d/(e+1) for
d <= e = 6 texels at 512 from a texel that is not cloth. Grime darkens by `(1 - k)^2.2` in linear
light with k = grime x (`DARK` 0.25/0.42/0.55 on the red, `DARK_CLOTH` 0.30/0.45/0.55 elsewhere)
and, off the red, the soot tint (0.48, 0.43, 0.36) / 0.48 per channel; the red loses at most
`RED_DESAT` 0/0.10/0.18 of its saturation (a move toward its own luminance); dust at the edges
`DUST_EDGE` 0.16/0.22/0.28, scuffs `SCUFF` 0.30/0.40/0.50, overall wear `WEAR` 0/0.03/0.07, the
patch gets `PATCH_KEEP` 0.12 of the grime, stains 0/3/6 and tears 0/2/7 per stage. The noise,
seeds (1000 + 37 x the Warrior's index in code order) and the mark placement are those of 1.0.
`clothes.lua` (`boot`) defines the mod and watches the fighters; `start.lua` (`after.level*`, once
per level, not in the main menu: the game's progress is not readable at the very start of the front
end) turns on, per Warrior, the layer of his stage, and a fight that raises it turns on the next.
Skins belong to the Warrior whose `CfgChar` keys name them (`warr_sw`, `_gen`, `_chase`, `_ghost`,
`_cv1`, `_cv2`): a model's own skin list also names other Warriors' skins.

**Change clothes.** `hideout.lua` (`after.level95`) puts a walk-up prompt on the hideout's upper
floor, the one you arrive on: you appear just inside the double doors of the brick wall; turn
right (towards the tall round bar table with its stools) and take two steps, between the doors and
the table. It is at (-185.6, 92.6, -194.3), about 2.4 units from the arrival point (it does not
pop up as the hideout opens) and away from the doors' own "Leave" spot (`AddFlag` +
`SetMsgHandlerEx`, set up once the follow camera exists, its strings kept referenced). Like the
game's own hideout prompts, the title is at the top and the message box shows the action hint with
the button icon, written as `level95` writes its hints (`"press " .. "<MONEYFONT><ST></MONEYFONT>"
.. " to ..."`): "Change clothes / press (Y) to change clothes" ("Cambiarte de ropa / presionar (Y)
para cambiarte de ropa" when `GetLanguage() == 1`). The action button (Y on the pad, E on the
keyboard's default map) stores the story missions done as `base`, clears every fight count and
announces "Clean clothes for the whole crew" ("Ropa limpia para toda la banda") under the title
with `HUDSetAnnounceMsg` (a tutorial text would not show: the message box keeps the prompt's line
while you stand there); every Warrior is at stage 0 at once. Version 1.2 had the spot in the dark
south-west corner of the same floor, by the black leather couch, with no button icon and no
visible answer, so it was hard to find and seemed to do nothing.

**The layers** are painted by the game itself from `assets.ini` the first time the mod loads (or
by the launcher's PLAY, "Preparando mods…"), from your own game files: 84 files (28 skins, 3
stages each), about 8 s on a desktop PC, into `<hdd>/mods-cache/dirty-clothes/`. The log says
`[modassets] dirty-clothes: built 1 asset (84 files) in ... ms`, later `1 asset from the cache`,
and `84 texture layer files from the cache`. A `layers-v1` or `layers-v2` folder left in the mod's
folder by the Python tool of 1.0/1.1 is reported as left over and not used: delete it. When the
build fails (another disc), the reason is in the `[modassets]` line, the mod logs `its texture
layers were not built ...` and the clothes stay clean (the wear is still counted).

**Limits.** Not over a texture pack's or `--sharp-faces`' version of a Warrior's skin (see
[Texture layers](#texture-layers)): with those on, a Warrior with dirty clothes is drawn from the
game's own texture. The other members of the gang created without a level script (if any) count
no fight points. Stage changes during a cutscene show from the next draw of the skin.

### Example: the gang trophy room (`20-hideout-trophies`)

The caged room under the hideout's main floor (x -174.84..-165.16, y 81.70..98.80, floor -197.58; walk in
through the door frame in its north fence, from the corridor behind the main floor) holds one trophy per gang
the Warriors beat. Each is placed by `trophies.lua` only when `UM_IsLevelComplete(level)` says its mission is
done, and gets a walk-up label (`AddFlag` + `SetMsgHandlerEx`): the trophy's name and "Beaten in Mission N"
("Vencidos en la misión N" when `GetLanguage() == 1`). Only retail objects are used: each gang's photo card
(`dyn_<gang>`, the picture the game shows when the gang is introduced; upright, one-sided, its centre at the
origin) and, where the gang has one, a hat or a bat from the disc.

| Gang | Beaten in | Level | Trophy |
| --- | --- | --- | --- |
| Orphans | Mission 5 | 2 | photo card `dyn_orphans` (west wall) |
| Hurricanes | Mission 7 | 5 | `dyn_hurr_lt` hat on the counter, card `dyn_hurricanes` above |
| Hi-Hats | Mission 8 | 81 | white top hat `dyn_hiha_lt` on the counter, card `dyn_hihats` above |
| Destroyers | Mission 10 | 93 | card `dyn_destroyers` (west wall) |
| Turnbull ACs | Mission 11 | 31 | sunglasses `dyn_turn_so` on the counter |
| Jones Street Boys | Mission 12 | 14 | card `dyn_jsbs` (south wall) |
| Moonrunners | Mission 13 | 9 | card `dyn_moonrunners` (east fence) |
| Baseball Furies | Mission 16 | 54 | the boss's black bat `dyn_lt_bat_blk_a` on the counter, card `dyn_furies` above |
| Lizzies, Punks | Mission 17 | 55 | cards `dyn_lizzies`, `dyn_punks` (east fence) |
| Rogues | Mission 18 | 84 | cap `dyn_rogu_lt` on the counter |
| Satan's Mothers | Flashback A | 82 | card `dyn_samo` (east fence) |
| Savage Huns | Flashback D | 20 | card `dyn_huns` (east fence) |
| Boppers | Flashback E | 11 | card `dyn_bopp` (east fence) |

Mission numbers are the game's own (`levelNames` in `config_preload3.lua`, field 5; flashback letters in the
order of `level95.lua`'s `FBMission`). Not in the room: the Riffs (allies), and the Electric Eliminators,
Panzers and Saracens, who only appear in passing. The retail trophies upstairs stay as they are.

- **Furniture**: a low counter along the west wall, two rows of five `dyn_desk_ab` (turned a quarter: 1.28 m
  along the wall, 0.62 m deep, the top 0.46 m up, the origin in the middle of the footprint; its `CfgObj` box,
  1.28 x 1.2 x 0.87, is not its shape), with two table lamps (`dyn_bar_tbllamp`, whose shades draw lit) on the
  back row; `cage.col` gives the counter a solid box 1 m high, so nobody walks through or climbs onto it.
- **Out of reach**: the game's action button (Y) picks up a hat or a bat lying near the player (seen at 1.0 m, not at 1.6 m), even
  one whose physics is off, so the hats and the bat lie at the back of the counter (about 1.5 m from the
  nearest point the player can reach, tested: Y there picks nothing up) with `ObjEnablePhysics(h, 0)` (they stay exactly where they are put instead of dropping).
  Standing at a label, Y runs the label's action (nothing) instead of a pick-up.
- **Light**: four warm `SetLight` lights (`{1, 0.929, 0.843}`, the hideout's lamp colour, radius 6-7), made in
  the `after.level95` hook while the level loads (a light made later is listed but stays off). They light the
  room with the runtime's lighting options (`--level-lights`, in the Quality preset); without them the game lights
  no world surface with a Lua light, so the room stays dim and the lamp shades and photo cards carry it.
- The furniture and the lights are there whatever is unlocked; a profile that has beaten no gang sees an empty
  counter.

## Collision patches

A mod can make new places walkable: `collision = FILE` in `mod.ini` names a text file inside the mod's
folder (mods version 1.2). While the mod is enabled, the runtime adds its shapes to the collision of the
level the file names as soon as the game has loaded that level's collision, and can make some of the
level's own triangles non-solid (to open a gap in a fence). The player, the other characters and the
camera then stand on, walk into and stop at the new shapes like the level's own. Without the mod nothing
changes. AI navigation is not patched: other characters do not plan routes through new places.

```ini
; an example (20-hideout-trophies/cage.col is the full file for the hideout's caged room)
level = level95                          ; required first: the level the patch is for
material = 5                             ; the Lua MATERIAL id of the shapes below (footsteps, impacts)
floor -174.84 81.70 -165.10 98.80 -197.575       ; X0 Y0 X1 Y1 Z: an upward rectangle
wall  -174.84 81.70 -174.84 98.80 -197.60 -194.45 ; X0 Y0 X1 Y1 Z0 Z1: a vertical rectangle
box   -172.45 83.03 -197.60 -172.21 83.27 -194.45  ; X0 Y0 Z0 X1 Y1 Z1: four walls and a top
ceiling -174.84 81.70 -165.16 98.80 -194.45       ; X0 Y0 X1 Y1 Z: a downward rectangle (keeps the camera in)
tri 0 0 0  1 0 0  0 1 0                  ; one triangle, solid on the side its corners turn counter-clockwise
remove -173.40 98.75 -197.60 -167.00 98.86 -194.85   ; the level's triangles inside this box stop being solid
```

- Coordinates are world units (metres) as `GetPosition` returns them, z up. `flags = 0x21` sets the raw
  record flags of the shapes that follow (default 0x21; walls, boxes and ceilings are always two-sided).
- A patch applies only to the level file it was written for: the runtime compares the game's collision
  with `<level>.lev` from the disc and logs `[collision] level95: 1 patch applied: N triangles added
  (A -> B), R made non-solid`. A file with a mistake logs `[collision] <id>: line N: ...` and is not used
  (the rest of the mod still runs).
- Find coordinates with `GetPosition(player)` (walk there, log it with `ModLog`) and test with
  `--goto` runs: a floor 1 cm under the visible floor is invisible in play.
- Up to 4096 triangles per patch; a level holds at most 65535 in all.

## Warriors Mode (`60-warriors-mode`, id `warriors-mode`)

"Modo Warriors (difícil de verdad)" / "Warriors Mode (truly hard)": a harder story mode built from the
game's own difficulty settings. Off by default. It changes no file of the game; it changes the arguments
of the game's own config calls on their way to the engine. `saves = yes`: you earn less money, and the
save keeps what you have.

**Choosing it (version 1.1, mods version 1.4).** With the mod enabled, the difficulty list of a new
Story game (after the profile name) shows **WARRIORS** after the game's own choices. Choosing it asks
"Warriors Mode: truly hard. Are you sure?" with the answers "YES - stronger enemies, less healing and
money" and "NO" ("Modo Warriors: difícil de verdad. ¿Seguro?", "SÍ - enemigos más fuertes, menos
curación y dinero", "NO" with the game in Spanish), on the game's own page and list; YES gives the
profile the game's Hard difficulty and
the mod stores `difficulty = warriors` for that profile in `mods-data/warriors-mode.json` (logged
`this new profile chose WARRIORS: stored for it` at the first level); NO returns to the list. Only
those profiles play the mode: any other profile, new or old, plays the game's own difficulty with the
mod enabled (`level N: not a Warriors profile (profile difficulty D); mode off` in the log). The
page's title holds one line of about 40 characters, so the consequences are on the YES answer, and
the game's font has no long dash (" - ").

`apply_to_all = yes` in the mod's `mod.ini` brings back the behaviour of version 1.0: every profile
plays the mode, and the list has no WARRIORS choice. To give an existing profile the mode, use that,
or add `"difficulty": "warriors"` under the profile's folder name in `mods-data/warriors-mode.json`.

**Where the difficulty shows.** Nowhere else: the game keeps the profile's difficulty in one byte of
the profile (`+0x423`, written and read with the save, read by `GetProfileDifficulty` and a Hard-only
check) and never prints it after the new-game list (the profile list shows names only). A Warriors
profile is a Hard profile to the game.

**How the game's difficulty works.** At every level load `global.lua` runs one of `config_easy`,
`config_normal`, `config_hard` (the profile's difficulty; Quick Rumble levels use `config_rumble`, and
the game itself plays levels 60-64 on Normal). Each defines and runs `CfgDifficulty()`, which calls
`SetDifficulty(n)` (read back by the level scripts' `GetDifficulty()`, e.g. for extra enemies on Hard),
`CfgWarriorClass` (the Warriors: health percent, damage percent, rage), `CfgPowerClass` (one per fighting
class: damage multiplier, ...), `CfgChar` (each character's class, health, damage and attack tables),
`CfgBreakAndEnterDelay` and sets `BaseChanceToBlock`, which `global.lua` then passes to
`CfgBaseChanceToBlock`. The gangs are set once at start-up by `config_preload` with
`CfgGang(gang, attackers on others, attackers on the player, ..., guard-break level, ...)`. Retail Hard
against Normal: enemy damage multipliers x1.2, the Warriors' health 45% instead of 85% of their base and
their damage lower, block chance 70 instead of 60, break-in delay 7 instead of 10, two per-class timing
values about x0.7-0.85 (their exact meaning is not decoded), some attack damage tables higher.

**What the mode does** (every number is a field of `WarriorsMode.cfg` in `warriors_mode.lua`):

| | Normal | Hard | Warriors Mode |
| --- | --- | --- | --- |
| Base settings | | | the game's Hard config (`SetDifficulty(2)`, so the levels' own Hard extras too; a WARRIORS profile is Hard anyway, with `apply_to_all = yes` any profile) |
| Enemy damage (`CfgPowerClass` arg 9, every class except the Warriors' 7, 8 and 58-66) | x1 | x1.2 | Hard x1.25 = Normal x1.5 |
| Enemy timing values (`CfgPowerClass` args 16, 17, same classes) | 100% | ~70-85% | Hard x0.8 |
| Base chance to block (`BaseChanceToBlock`) | 60 | 70 | 80 |
| Break-and-enter delay (`CfgBreakAndEnterDelay`) | 10 | 7 | 5 |
| Attackers on the player at once (`CfgGang` arg 3, every gang but the Warriors) | 1-4 by gang (mostly 2) | same | +1, at most 4 (mostly 3) |
| Guard-break level (`CfgGang` arg 7: an enemy breaks a long block once its count passes (7 - level) x 6; level 0-1 never does) | 0-4 by gang | same | +1, at least 2, at most 4 |
| Police responders per crime (`CfgCrimeResponders(crime, n)`, n >= 1; also the levels' own calls) | level's | level's | n + 1, at most 4 |
| Healing (any rise of the player's health percent, e.g. flash) | full | full | half |
| Money (any rise of `InvGetMoney`, mugging, robbing, rewards) | full | full | 60% |
| The player and the crew (`CfgWarriorClass`, power classes 58-66) | | Hard | Hard, unchanged |
| Mission timers | | | unchanged (scaling them compounds with the scripts' own time bonuses) |

Healing and money: every 250 ms of game time while the player is alive, a rise counts only half (health)
or 60% (money) of itself; damage and spending are untouched, and values the scripts set themselves
(`HuSetHealth*`, `HuSetMaxHealth`, `InvSetMoney`) are taken as they are. Missions that ask for money
(e.g. $75 to pay a debt) need more mugging, never less.

**With the runtime's AI options.** The mode changes data the AI reads; it does not move enemies.
`--enemy-tactics` ("Enemigos más tácticos": surround, take turns, punish a passive player) and
`--smart-allies` work on top: with both, the turn-taking rotates among three attackers instead of two, and
the guard break the tactics enable for every gang starts from the mode's higher levels. Both are
recommended with the mode. `--sim60` is unaffected (the mode only uses the game's own scheduler).

**The gangs.** The game sets them once per run at start-up (`config_preload2`), before a profile is
loaded. The mod keeps each gang's numbers then (`ModKeep`: arguments 1-5 and 7-9 and the 7 numbers of
the strategy table) and, at each level load, calls `CfgGang` again with its values for a Warriors
profile or the game's for any other, only when the set changes (`gangs: 25 set to the Warriors Mode
values` / `... the game's values`). With `apply_to_all = yes` they are changed at start-up as before.

**Where it does not apply.** The menus, Quick Rumble and the Armies of the Night levels 60-64 keep the
game's settings (the log says so); a Warriors profile keeps the mode's gang values in levels 60-64, and
the menus and Quick Rumble get the game's (with `apply_to_all = yes` the mode's everywhere).

**Log** (`--mod warriors-mode`): `[mods] warriors-mode: difficulty list: WARRIORS added (new game)` at
start-up, `[diffmenu] "WARRIORS" confirmed: profile difficulty 2 (Hard)` on YES, then per level
`[mods] warriors-mode: applied after config_hard (Warriors profile, profile difficulty 2, level 80, ...): 41
enemy power classes damage x1.25 timing x0.80; block chance 70 -> 80; break-in delay 7 -> 5`, then `power
c0 0.54>0.675 7>5.6 ...` (class, damage multiplier before > after, first timing value before > after),
`gang N: attackers on the player 2 -> 3, guard-break level 1 -> 2` at start-up with `apply_to_all = yes`,
and one `heal` / `money` line each time a gain is reduced.

## Lasting scars (`87-scars`, id `scars`)

"Cicatrices que duran" / "Lasting scars": a fight that left a Warrior badly beaten leaves marks on him
in the next missions, and they fade over the night. Off by default; `saves = no` (its records are in
`mods-data/scars.json`, per save profile). Works with or without `90-gore`.

**What it remembers** (`scars.lua`, hooks `boot` and `after.level*`; every number is a field of
`Scars.cfg`). `boot` wraps `HuCreate` and watches every Warrior a level script creates: the named crew by
their `CfgChar` ids (Cleon 1, 2, 4; Swan 5, 6, 7, 9; Ajax 11-13; Cochise 15-17; Cowboy 18-20; Fox 21-24;
Vermin 26-29; Rembrandt 30-32; Snow 33-36; Ash 38, 40; Rhino 52 and the other crew ids 41, 43, 45, 49-54),
one person for all the outfits of a Warrior; the flashback "ghost" models (3, 8, 14, 25) are the Warriors
of the past and are left out. Every 250 ms it reads `HuGetHealthPercent` and keeps, per Warrior and level,
his lowest health and his knock-outs (health 0), written with `ModSave` as `r.<who>` = `"<level> <lowest
%> <knock-outs>"` each time the lowest health crosses a 5 % step or a knock-out happens, so the record is
on disk whenever the level ends. A checkpoint reload of the same level continues its record.

**How it becomes marks.** When the next level's own script (`levelNN`, NN < 100) has run, each record from
another level becomes a severity: `(80 - lowest %) / 60`, plus 0.25 per knock-out, at most 1 (ending at
20 % or knocked out: the worst marks; above 80 %: none; under 0.15 is dropped). It replaces the Warrior's
older marks if it is worse than what is left of them, and is kept as `s.<who>` = `"<severity> <missions>"`.
Every mission entered (a level other than the hideout, 95, and not the one entered last) counts one
mission; what shows is the severity x (1 - (missions - 1) / 3): whole in the hideout and in the next
mission, then 2/3, 1/3, then gone (`<who>: healed`). So a Warrior knocked out at the end of a mission
starts the next one, and the hideout before it, with his worst marks: that is the fight he just came
from. Levels 100 and up (menus, Quick Rumble) show nothing. The marks are passed to `ModScar` for every
id of that Warrior; healing (flash) in a level removes the fresh blood, not the marks.

**Only the story carries marks** (1.1). A record becomes marks only once its mission is completed
(`UM_IsLevelComplete`, the profile's own progress): a mission left before the end (quit to the hideout)
leaves no marks (`<who>: level N was not completed: no marks`); its record waits in the hideout and goes
on if that mission is played again. A level outside the story starts clean and records nothing: a
mission the profile has already completed, which is how the game replays one from the hideout's level
menu, and the Armies of the Night levels 60-64 (`level N: a replay (a mission already completed): no
marks, nothing recorded`, or `... Armies of the Night, outside the story ...`). The story's marks on disk
are not touched by such a level, so the hideout and the next story mission show them as before. A
`--goto`/`--warp` to a mission the profile has not completed counts as the story.

**How they look** (runtime, `runtime/src/hle_scars.c`). The game draws a fighter's blood as a 64 x 64
atlas over his skin: small pieces (a bruise, a black eye, slashes, drips) that each model maps to its own
body parts, the face to the bruise and black-eye pieces. A marked character gets a healed copy of that
atlas, made in memory from the game's own atlas when it is first needed: the lightest one (`d1`) up to
level 0.5, the next one (`d2`) above; every piece recoloured as an old injury (dark plum bruises with a
dark olive-brown edge, thin strong pieces a dark red closed cut, no fresh red), its opacity 22-65 % by
level. The atlas is laid over the skin by its alpha, so every colour is darker (luma about 0.11) than
the darkest skin of the crew: a mark darkens and tints light and dark skin alike and never shows as a
pale speck (1.0 used mid tones and a large beige plaster, which on Cleon's dark skin read as small white
patches). From level 0.6 a small tan plaster (rounded, with a gauze pad) sits on the cheekbone under the
right eye: the atlas texels x 41-47, y 9-14. Those were probed on Cleon (original and `characters-hd`
meshes): the texels around them map to the eye (x 40-47, y 0-7), the nose, the jaw and parts of the legs
(x 32-39 and 48-63, y 16-31), so a plaster any larger spills over them as specks. Fresh blood is
laid over the healed atlas, so a marked Warrior who is hurt again shows both. The same draw is used in
gameplay and in the real-time cutscenes. Nothing of the game is stored or shipped.

**Log** (`--mod scars`): at each level start `[mods] scars: swan ended level 3 at 5 % (1 knock-outs): new
marks 1.00`, `swan: marks 1.00 (severity 1.00, 0 missions)` per marked Warrior, and `level N: watching
W Warriors`; `level N: a replay (a mission already completed): no marks, nothing recorded` on a level
outside the story, `swan: level 3 was not completed: no marks` for a mission left before its end;
`scars: fox knocked out (1 so far this level)` during a level; `[scars] healed marks at
0008fe10` once. With `--mods-trace`, `[scars] character 5: marks step 8` per call and one `[scars]
texture: ...` line per healed atlas made.

**Limits.** A mark is per character id: two characters of the same id in a level (the game does not do
that with the Warriors) would both show it. Where on a model a mark lands is the model's own mapping of
the atlas: the same pieces also cover parts of the arms, chest and legs, so a heavy mark shows a bruise
there too; the plaster's texels were chosen on Cleon, and another model may place them elsewhere on its
face. On dark skin the marks are subtler (a darker, plum-tinted patch rather than a colour). The mod
cannot tell a `--goto` jump from the story; a mission already completed always counts as a replay. Characters
created without a level script (none of the Warriors) are not watched. The game's no-blood setting hides
the marks too.

## Visible wounds (`85-wounds`, id `wounds`)

"Heridas que se notan" / "Visible wounds": low on health you cannot sprint and your crew limps: a
little slower. Off by default; `saves = no`; mods version 1.5. Nothing new is drawn or animated: the
game's own walk and run at their own pace.

**What the game has** (research for this mod):

- No limp or injured walk. The game's 564 named animation files hold idles and scene clips only
  (`gen_hurt_lean`, `fidget_tired_enter/idle/exit`, `combat_fidget_tired`, `bum_stagger`; scenes
  `gen_injured_001`, `l5_leg_hurt`); a character's walk, run and sprint come from his animation bank,
  which no Lua function switches.
- No per-character speed in Lua: `CfgSpeedClass` fills a global table the characters on foot do not
  read; `CfgChar`, `CfgWarriorClass` and `CfgPowerClass` set no speed; `HuSetSlowMo` slows the whole
  world; `HuSetScale` sets the model's size; `HuSetWounded` sends an AI character into the game's own
  "wounded" behaviour (the one beaten enemies get), `HuSetTireless` marks chasers in scripted chases.
- Native code: every world update a character on foot picks a target speed (the stick for the
  player, the move and fight goals for the AI) and speeds up or slows down toward it; the player's
  stick speeds are also multiplied by a per-character float the game itself lowers for a while in one
  of its recovery states. The runtime's `ModSpeed` caps both (see the table above).
- The game animates a character on foot from his actual speed, as a blend position between his walk,
  run, jog and sprint speeds (each times that multiplier). A player under any speed cap plays a
  slower, shorter run: at x0.85 he ran 6.6 m/s instead of 7.8 with a stride period of 38 frames
  instead of 36 (60 Hz), and the user saw slow motion even at x0.85-0.90 (versions 1.0 and 1.1).
  Scaling his velocity after the game's locomotion instead does not work either: the next update
  accelerates from the scaled speed (4.5 m/s, not 6.6) and the blend changes all the same.
- The player's gaits (mission 2 street): walk 1.6 m/s (right trigger held), run 7.8 m/s (the stick),
  sprint 10.2 m/s (left trigger held, while his stamina lasts).

**What the mod does** (`wounds.lua`, `boot`; every number is a field of `Wounds.cfg`): every 200 ms of
game time it checks every character the level creates (`HuCreate`, gang spawners) and the player
(`player`, `player2`) by his health (`HuGetHealthPercent`):

| Health | You (`player`, `player2`) | Your crew (gang type 0) | Everyone else (`others`, off by default) |
| --- | --- | --- | --- |
| 40 % and more | as usual | full speed | full speed |
| under 40 % (`no_sprint_below`) | no sprint: holding sprint you run | 90 % | full speed (0.9 when switched on) |
| under 20 % | no sprint | 85 % | full speed (0.85 when switched on) |

Your own speed is never capped (`ModSprint`, not `ModSpeed`): you walk and run as fast as ever and the
game plays your cycles at their own pace; only the sprint is gone, and with it your top speed (10.2
m/s down to 7.8). Version 1.1 slowed you to 90 % and 85 %, which the game showed as slow motion (see
above); a cap a 1.1 state left on you is released. The crew's caps never go under `Wounds.cfg.floor`
(0.85). The cap never jumps: it moves toward its new value by
`ramp_per_s` (0.05 per second of game time: full speed to 90 % in 2 s, 85 % back to full in 3 s). An AI character (your crew) does not run a little slower:
below his running speed he drops to a slower gait, so a capped crew member moves at about half his
running speed (measured: caps from 0.65 to 0.9 all gave 0.5-0.65 of it, 0.95 nothing). That is why
enemies are left alone by default: hurt enemies at half speed would reach you and get away much more
slowly, and the mod must not make fights easier. Set `others` in `Wounds.cfg.steps` (e.g. 0.9) to slow
them too.

Healing (flash, the game's own health rises) gives the speed back the same smooth way (and your sprint at
the next check), and so does a new level: a cap the runtime still holds for a character the new level's script sees again ramps back from
where it was.

**Where it does not apply.** Characters in a scene (`HuIsInScene`); characters a level runs in a
scripted chase: the ones it makes tireless (`HuSetTireless(h, 1)`, as the chase missions do with the
player and the crew, whose sprint and speed come back; full speed again when the level turns it off) and the runners of the chase goals
(`GoalDevilRun`, `GoalRunCarrotRun`, `GoalLeadChase`, for the rest of the level). Missions with a
timer are not exempt: being slower when hurt is the point, and the game's flash heals it. Quick
Rumble and Armies of the Night levels apply it too. Pedestrians the game creates by itself (not
through a level script) are not watched.

**Log** (`--mod wounds`): `[mods] log: wounds: character 5 (player): health 35 % -> no sprint` (and
`-> sprint again`) once per change; `wounds: character 12: health 15 % -> speed x0.85 (from x1.00, 0.05
per s)` once per new crew target (not per ramp step). The first block or cap writes its runtime hook
(`[huspeed] sprint block at 001657ff ...`, `[huspeed] speed caps at 0016b872 ...`).

**Measured** (headless, mission 2 street, `--goto m2:4`, god mode, health forced; scratch probes): the
player running straight at 100 %, 36 % and 15 % health ran 7.8 m/s with a stride period of 35.8, 36.0
and 36.0 frames (60 Hz): the same run, the same pace. Holding sprint he reached 10.2 m/s at 100 % and
7.8 m/s at 36 % and 15 % (`no sprint` logged, no speed hook written). Version 1.1's x0.85 had given
6.6 m/s and 38 frames. The crew's caps (AI): an AI fighter running 10 m to the player, 5.8-6.6 m/s,
ran 3.3-4.5 m/s with a 65 % cap, 3.3 m/s with 90 %, unchanged with 95 % (version 1.0 measurements).

## Police memory (`89-police-memory`, id `police-memory`)

"La policía te recuerda" / "The police remember you". Off by default; `saves = no` (the notoriety is
kept in `mods-data/police-memory.json`, per save profile). One file, `boot` (`police_memory.lua`).

**How the game's police work** (native code; `default.xbe`):

- Crime types (`CfgCrimeResponders(type, n)`, `CrimeIsHappening(pos, type, ...)`): 0 Assault,
  1 BreakAndEnter, 2 CopAssault, 3 CopKill, 4 Custom, 5 DisorderlyConduct, 6 GangFight, 7 GangCurfew,
  8 Mugging, 9 PrecinctAttack, 10 Tagging, 11 Theft, 12 Trespassing, 13 Vandalism.
- A crime the police register makes the Warriors **wanted for 10 s**, and every new crime starts the
  10 s again (`GangIsWanted(GangWarriors, true)` reads it; `ForceCrimeLevel(true)` keeps it on). That
  is the whole "heat": there is no meter in between and no wanted level that lasts.
- A witnessed crime sends `n` responders from the nearest responder spawner. The engine resets `n` at
  every level load: 2 per type, BreakAndEnter and CopAssault 3, PrecinctAttack 4. `config_easy` lowers
  some; Normal and Hard keep them; some levels set their own (0 switches a crime off).
- Patrols and responders are the level scripts' gang spawners: on the Coney streets `AddBeatCop`
  (`GangAddSpawner(GangBeatCop, "BeatCop", 0, CharBeatCopSpawner, "cops_va1", pos, 0, -1, 3000, 1, 7, ...)`:
  argument 9 = spawn interval in ms, 10 = count, 11 = kind: 7 a patrol walking the flag net, 4 a
  responder) and `AddRespondCop` (`..., 1000, 2, 4, ...`); missions use the same calls. Police gangs
  are `GangCreate(1, ...)`.
- How many members of a gang react to a disturbance: `GangSetRespondPercentage(gang, pct)` (default
  30: members reacting, as a share of the gang, stay under it).
- Not used by the mod: a second "crime level" 0-3 (`SetCrimeLevel(level, seconds)`, never called by
  the game's scripts and reset to 0 at every level load) that only feeds `SetCopStation` tables no
  level sets up; cops have no sight range a script can change (`CfgSetWarriorSpotting` is a level's
  on/off switch).

**What the mod does** (every number is a field of `PoliceMemory.cfg`):

- **Area**: every level belongs to a district, from the game's own mission place names: the hub and
  the Coney missions (levels 95, 99, 80, 81, 86, 84, 92) are Coney; East Coney 87, 93, 82, 83;
  Riverside 34, 54; Tremont 2, 52; Soho 3; East Harlem 5; Pelham 31; Bensonhurst 14; Trainyard 9;
  Gunhill 51; Bowery 55; Chinatown 20; Harlem 11. Any other level is its own area. The menus, Quick
  Rumble and the Armies of the Night levels 60-64 are left alone.
- **Notoriety** (0-100) in the district you are in: +8 each time the Warriors become wanted (an
  incident), +0.5 per second wanted, +6 per cop knocked out or killed after being hurt (cops the level
  creates with `HuCreate` or a police spawner; a cop the game removes at full health does not count). Kept per district with `ModSave` (`d.<district>` = score, peak,
  incidents, cops beaten; `n` / `last` count the district changes).
- **Decay**: -1 per 30 s of game time in the district without being wanted, and x0.5 for every level
  played in another district (checkpoint reloads of the same level do not count).
- **Next visit** (from the notoriety at the start of the level; nothing changes under 20):

| | Retail | Tier 1 (20-44) | Tier 2 (45-69) | Tier 3 (70+) |
| --- | --- | --- | --- | --- |
| Responders per crime (`CfgCrimeResponders`, the engine's defaults and the levels' own calls; 0 stays 0) | 2-4 | +1 | +1 | +2 |
| Police patrol and responder spawners (`GangAddSpawner` kinds 7 and 4 of police gangs): count | level's | +1 | +1 | +2 |
| Their spawn interval | level's | x0.75 | x0.6 | x0.5 (at least 0.5 s) |
| Share of a police gang that reacts (`GangSetRespondPercentage`; a level's own call wins) | 30 % | 45 % | 60 % | 75 % |
| The hub's beat-cop patrol in free roam (`coney.CopsOnNow`, `AddBeatCop`; retail only at two story checkpoints) | off | off | on | on |
| HUD hint on the street (the game's tip line, `HUDSetTutorialText`, gone after 6 s) | | "The cops are looking for you here" / "La policía te busca por acá" | same | same |

**Where.** Everything waits for the street: inside the hideout (the hub's `InsideClubhouse`), in a scene
(`HuIsInScene(player)`) or in one of the hub's mini games there is no hint and no patrol. The hint shows
once per level, after 8 s of the level and 2 s on the street, and goes after 6 s or as soon as you walk
back in (a tip the game showed meanwhile is left alone). It is the game's small tip line under the
player (bottom centre): the game's HUD has no text at the top right (its other messages,
`HUDSetAnnounceMsg` types 0, 2 and 3, show fixed texts such as "Objective complete" at the bottom
left, and type 5, used by version 1.0, is a large centred banner that stays). In free roam the hub keeps its patrol off except
at two story checkpoints; from tier 2 the mod switches it on the hub's own way (`coney.CopsOnNow = true`
and `AddBeatCop()`), so the spawner change above gives it its extra cops and the hub's own
`TurnOffCops` / `TurnOnCops` still take it away for a mini game and bring it back.

**Fair play.** Responders are capped at 5 per crime (with Warriors Mode, whose +1 stops at 4, the
levels' calls end at 4-5); the mod never makes you wanted on arrival, never forces the crime state and
never touches arrests, so a hot district means more and quicker police, not a trap you cannot leave.
Notoriety only grows from what you do there.

**Log** (`--mod police-memory`): `[mods] log: police-memory: level 95 (coney): notoriety 31.8 (was 63.6,
1 level(s) away; peak 63.6, 4 crimes, 1 cops beaten) -> tier 1: responders +1 (at most 5), ...` at every
level start, `patrol spawner BeatCop (BeatCop): 1 at a time -> 2, every 3000 -> 2250 ms` for each
changed spawner, `wanted (incident N here): notoriety X`, `no longer wanted after S s: ...`,
`cop beaten (N here): ...`, `free roam: the hub's beat-cop patrol is on (tier 2)`, `hint shown: ...` and
`hint gone`. Setting `PoliceMemory.cfg.status_ms` (e.g. with
`--lua-exec`) logs the police gangs' head counts and how many characters their spawners made that often.

**Limits.** The effects are set when the level loads (spawners and responders are the level's); new
trouble counts from the next visit. Notoriety is per district, not per street of the hub. Cops a level
creates without `HuCreate` or a police spawner are not counted when beaten.

## Gang colours on the radar (`92-radar-colors`, id `radar-colors`)

"Gang colours on the radar" / "Colores de pandilla en el radar". Off by default; `saves = no`. One file,
`radar_colors.lua`, run at `boot` (it wraps `CfgChar`) and `after.config_preload2` (one log line).

**How the game draws the radar** (native code, `GUI\RadarHUD.cpp` in `default.xbe`): when a character
is created (0x14bb80), the radar type of his `CfgChar` (its 2nd argument) picks his dot: 1 (police) a
dot of kind 8 with frame 0x164 (blue square) through 0xdc750, 3 (Warriors) kind 7 with frame 0x16d (a
plain white dot) through 0xd88f0, 2 (gangs) kind 6 with frames 0x160 and 0x167 (two layers) through
0xd84a0. `RadarHUD::Add` (0xf2b60) creates the item with a tint, `SetFrame` (0xf1190) and
`SetItemFrame` (0xf08f0) pick its frame, `SetColour` (0xf12a0) changes the tint later (the game tints
its green objective markers that way). The frames carry their own colour, so a tint only shows as it
is on the white dot. The HUD holds two radars and every dot goes on both. The game's Lua only picks a
frame (`HUDSetRadarItemTexture`), never a colour, so the colours need the runtime's `ModRadarColor`:
for a character id with a colour it replaces the tint of his dot and draws it with the white dot frame
0x16d (a cop's square becomes a round dot too). Alert marks the game puts on a dot's second layer
(frames 0x20, 0x21) keep their own frame and colour.

**What the mod does**: while the game configures its characters at start-up, every `CfgChar` whose
model name (the 10th argument) starts with one of the prefixes of `RadarColors.gangs` gets that gang's
colour. One solid colour per gang (the first colour the player request named for it):

| Who | Models | Colour |
| --- | --- | --- |
| Transit cops | `cops_ty_*` | bright green `39FF3C` |
| Riot cops | `cops_vc*` | black, drawn as dark grey `5A5A6E` |
| Destroyers, Hurricanes | `dest*`, `hurr*` | dark blue, drawn as `2F5BFF` |
| Moonrunners, Electric Eliminators, Punks | `moon*`, `elec*`, `punk*` | yellow `FFE21F` |
| Orphans, Savage Huns, Panzers | `orph*`, `sava*`, `panz*` | olive green `8C9A2B` |
| Baseball Furies, Rogues | `fury*`, `rogu*` | grey `A6A6A6` |
| Hi-Hats, Turnbull ACs, Satan's Mothers | `hiha*`, `turn*`, `samo*` | red `E8261E` |
| Jones Street Boys, Saracens | `jsbs*`, `sara*` | black, drawn as dark grey `5A5A6E` |
| Lizzies | `lizz*` | pink `FF5FC8` |
| Boppers | `bopp*` | purple `A23CF0` |
| Riffs | `riff*` | orange `FF8A1E` |

Readability: the radar shows a dark map with light building blocks, so pure black and a deep navy
would vanish on the dark parts. Black is a dark grey and dark blue a brighter mid blue: they still read
as "black" and "blue" next to the other dots. The 8 Balls and the Van Cortlandt Rangers have no
character models of their own in this version of the game, so they have no entry. Street cops, the
Warriors, civilians and objective markers keep the game's look. A gang member the game dresses in a
civilian model (a few extras use `civl_*`) keeps the game's dot. Change a colour by editing
`RadarColors.colours` at the top of `radar_colors.lua`.

**Log** (`--mod radar-colors`): `[mods] log: radar-colors: gang colours: N character ids coloured`
after the start-up configuration, and `[radar] dot colours at 000f2b60 (add), 000f12a0 (colour),
000f1190 and 000f08f0 (frame)` once. With `--mods-trace` every character dot the radar adds is logged
(`[radar] dot of <handle> (character <id>, kind 6, layer 0): ffffffff -> ff8a1eff`).

## Brutal fights (`90-gore`, id `gore`)

"Peleas más brutales" / "Brutal fights": fights get dirtier with the game's own blood, more of it and for
longer. Off by default; `saves = no`. Nothing new is drawn: the drops, the ground pools and the blood on
fighters are the game's own effects and textures.

**How the game's blood works** (native code; none of it is in the Lua scripts, and it does not depend on
the difficulty):

- A hit's animation has a clip event that starts the particle system `sub_blood_effect` with a drop
  count (1 for every punch seen in a street fight: one drop, no mist). With more than one drop it throws a `blood_mist` and count + 1 drops (`blood_splatter`,
  `blood_drop`); each drop that lands leaves a pool on the ground (`blood_splat_ground`). Blood only
  starts within 25 m of the camera and while more than 512 of the 1400 particle tasks are free
  (`--fx-scale` raises both). `sub_blood_gush` is a short stream from a character; some moves throw
  `part_blood_spray`.
- A ground pool grows for 1/6 s, stays 3 s while it spreads, then fades 3 s; when the particle tasks run
  short (512 or fewer free) a pool ends at once at its next step.
- A fighter wears one of three blood textures over his skin by health: under 90 %, 60 % and 30 %. Healing
  (flash) takes it away again.
- These systems cannot be started from Lua: `SpawnParticle("sub_blood_gush", ...)` (any `sub_blood_*`)
  crashes the game, because they read arguments that `SpawnParticle` never gives them.

**What the mod does** (`gore.lua`, `boot`: every number is a field of `Gore.cfg`, passed to the runtime's
`ModBlood` at the start of every level):

| | Game | Brutal fights |
| --- | --- | --- |
| Drops per blood effect (`drops`) | 1 per punch | x 2.5: 3, so each bloody hit throws the mist and 4 drops |
| Ground pools (`pool_hold`, `pool_max`) | 3 s, then 3 s fade | the game's 3 s, then 900 s more at full size, then the 3 s fade; at most 160 held, the oldest fades first; none is held while the particle tasks run short |
| Blood on fighters (`char_gain`) | texture by health: < 90 / 60 / 30 % | by 100 - (100 - health) x 2: < 95 / 80 / 65 % |
| Blood on fighters after healing (`char_keep`) | washed off | stays: the lowest health a fighter had since he appeared (or the level started) |
| Blood on fighters in cutscenes | by health | the game's own: by health, no gain, none kept |

A held pool goes when the level or a checkpoint reloads, when its 900 s are up, or when 160 newer pools
push it out. With `--fx-scale` 2 or 4 (Display > Effects and particles) more pools and drops fit at once.

**Log** (`--mod gore`): `[mods] gore: blood up in config_normal: drops x2.5, pools 900 s (at most 160),
fighters gain 2.0, blood stays` per level, `[blood] drop count at 002c2751`, `[blood] pool hold at
002bf4ea` and `[blood] character blood stage at 0008fd9c` once, and at each level change `[blood] level
done: N blood effects (D drops, E after "drops"), H pools held, F faded early (over pool_max)`. With
`--mods-trace`, one `[blood] effect: N drops` line per blood effect (the first 200; N is the game's count)
and `[blood] pools: H held so far, N held now, P pushed out` every 10 held pools.

**Limits.** The extra drops share the game's particle budget: in a crowded fight without `--fx-scale` the
game stops starting new blood sooner (it needs 512 free tasks). The blood on a fighter follows the same
health rule for everyone, the player too. The game builds the next character of a slot in the same
memory (cutscene actors too): the kept blood belongs to the character's handle, so a new character starts
clean, and a knocked-out fighter whose handle is already gone keeps his. A cutscene is told by its
letterbox bars in the frame drawn before (`[blood] cutscene starts` / `ends` with `--mods-trace`): its
first frame can still show the kept blood. The no-blood switch of the game (a word it reads before any
blood) is left alone.

## Sparring ring (`75-sparring`, id `sparring`)

"Sparring ring" / "Ring de entrenamiento". Once the story is finished (`UM_IsLevelComplete(84)`, the
gym-records rule), a walk-up spot in the middle of the hideout's caged room under the main floor (walk in
through the north door from the corridor behind the main floor; the mod's own `ring.col` makes the room
walkable, the same room as `20-hideout-trophies`' `cage.col`, so it works with or without that mod) says
"Sparring ring / press (Y) to spar". The action button opens the menu in the game's help-text box:

| Line | Values |
| --- | --- |
| Mode | Fight (beat the other side) or Survival (the game's endless wave of one gang, alone) |
| Gang | the gangs beaten on this profile, in story order (the trophy room's list: Orphans, Hurricanes, Hi-Hats, Destroyers, Turnbull ACs, Jones Street Boys, Moonrunners, Baseball Furies, Lizzies, Punks, Rogues, Satan's Mothers, Savage Huns, Boppers), each once its mission is complete |
| Enemies | 1-9 (fight; 9 a side is the game's own largest rumble) |
| You | the Warrior you play: Cleon, Swan, Ajax, Cochise, Snow, Fox, Cowboy, Vermin, Rembrandt |
| Crew | 1-9 (fight): you and the next Warriors of that list |
| Venue | fight: Fight Pen (102), Red Devil (105), Tremont (134), Warrior Turf (103), Coney Drag (112); survival: The Park (127), Tremont (134) |
| Record | the record of the current choice |

The D-pad's up/down picks a line, left/right changes it, A fights, B goes back (the player stands still
while the menu is open). The last choice is remembered until the game exits.

**How the fight starts.** It is the flow of the game's unused dev script `combatselect` (pick a Warrior,
both sides' sizes, then a bar, court or rooftop fight), which cannot run itself: its `combatselect_init` and
`combatselect_objects` files are not on the disc. The mod uses the game's Rumble mode instead. The hideout's
Rumble menu ends in `menu.BeginRumbleMode(level)`: `SetCheckPoint(1)`, `MenuLoadLevel("levelNNN")`; the
arena's script then reads the menu's setup with `GetRumbleModeData(RM_LuaData)` (1 game mode, 2 game type,
3 gang size, 4/5 the gangs' character paks minus 1, 6-14 and 15-23 the sides' character ids) and
`GetRumbleModeGangName(1/2)`, and `doFile`s `level<N>_<type>_init` (the spawn flags) and the mode's script
(`brawl.lua`, `survival.lua`). The spot keeps the choice for the next Lua state (`ModKeep("fight", ...)`)
and loads the arena the same way (without the menu's autosave); `boot` (`sparring.lua`) wraps those three
functions so the first `GetRumbleModeData` of that arena gets the choice (the Warriors' rumble gang, pak 1,
the chosen Warrior first; the enemy gang's nine rumble characters, its warchief first) and uses it up:
every other rumble gets the game's own data. The intro's DJ does not read that table: the native
`ShowRumbleModeIntro` plays `dj_gang_<id>` for each side from the Rumble menu's own gang values (the
same ones `GetRumbleModeData` copies into `[4]`/`[5]`, ids minus 1), which a game that never opened the
menu leaves at the Warriors for both sides. So the wrapper also calls `ModRumbleGangs(1, gang)` (the
mod needs mods version 1.8) and the DJ names the Warriors and the chosen gang
(`[mods] log: rumble gangs set: 1 vs 13 (the DJ says dj_gang_1, dj_gang_13)`). `after.level1*` (`fight.lua`) then wraps the arena's
`AddRumbleGang1` / `AddRumbleGang2` (each side its own size: the game gives both the same), `StartIt` (the
clock starts when the intro ends), `RumbleOver` (the result) and `GoToMenu` (instead of the Replay / Quit
screen, the result in the announce slot for 6.5 s, then `ExitToClubhouse`, the game's own way back to the
hideout, which puts the hub checkpoint back).

**Unused arenas.** Two levels have a full rumble script, a map, lighting and collision on the disc but are not
in the game's arena list (`CfgRumbleArenaData`): level103 "Warrior Turf" (a sandbagged street on Coney, with
its own brawl spawn file) and level112 "Coney Drag" (the Coney strip; only the spawn files of two other
modes, Have Mercy and Wheels of Steel, so its brawl uses `level112_mercy_init`: ten spawn flags a side at
both ends of the street). Both load, light and fight cleanly. Not used: Fight Pen and the unused arenas
have no `rumble_<N>_brawl.pak` (the game queues it and goes on, as in its own Fight Pen); the rooftop arenas
(131-133) only have Battle Royal.

**Records** (`ModSave`, per save profile, `mods-data/sparring.json`): `win.<gang>.<enemies>.<crew>` the
fastest win in ms (losses and draws do not count), `surv.<gang>` the longest survival in ms and
`survko.<gang>` its knock-outs (the game's own count of enemies the player put down). After a fight the
screen shows "Won: 41.20 s" / "Beaten" / "Survived: 2:31  7 KOs" and the record, or "New record!" with the
old one (Spanish when `GetLanguage() == 1`: "Ganaste: 41,20 s", "¡Nuevo récord!"). Every fight is logged:
`[mods] log: fight Hurricanes 3 vs 2 at Coney Drag: won in 16.72 s, record 16.72 s (new)`. `saves = no`:
the game's save is not changed (rumble mode itself awards nothing).


## Menus and music

Mods version 1.6 adds a small list menu drawn by the runtime over the game, and a music channel
(`src/hle_modmenu.h`, `src/modmenu.h`).

- **Look.** The game is dimmed; a dark panel with a red top line shows the title (the game's own
  font, read from your `paks\global.pak`), an optional square cover on the left, a scrolling list
  of 9 rows on the right (numbered, the selection on a red bar, a thin scroll bar when there are more)
  and a footer line. Texts are bytes in the game font: Latin-1 escapes (`m\250sica`) or UTF-8
  letters both work.
- **Cover.** `"disc:<file>:<chunk>[:x,y,w,h]"`: the XBoxWad.idx name hash of a disc file (hex), the
  hash of a texture chunk (class `0x2a`) in it and an optional texel rectangle. It is decoded from
  the player's disc when the menu first opens and kept for the run; nothing is written to disk. A
  wrong spec logs `[menu] cover ...` and the menu opens without it.
- **Input.** Pad: d-pad or left stick up / down, left / right a page; A, Y or Start choose; B or
  Back close. Keyboard (default bindings): arrows, Enter or E choose, Backspace, F or Escape close
  (Escape closes the menu instead of releasing the mouse). The buttons that opened the menu must be
  released first. While the menu is open, and until every button is released after it closed, the
  game sees a neutral pad: the player stands still and the camera does not move.
- **Preview.** An item `"Label\tvags/music/<id>"` plays that sound once the selection rested on it
  for 150 ms (scrolling fast plays nothing), crossfading in 0.3 s (equal power) from what played;
  the game's sounds given to `ModMusicMute` fade out meanwhile. Choosing (A) such an item keeps the
  previewed song playing as the mod's song (looping, no restart); B fades the preview out and brings
  back what played before (the mod's song, or the game's own music).
- **Where it plays.** On the sound source the game last played a muted song on (for the hideout:
  the boom box, at the level the game plays its radio), so it is heard from there; with no such
  source yet, as a 2D sound. It follows `--music-volume`.
- **Level loads.** A new Lua state (a level load) closes the menu and stops the mod's song; the
  mod starts it again from its hook if it wants (as `95-jukebox` does).
- **Log.** `[menu] open "<title>": N items, cover`, `[menu] preview <sound>`, `[menu] chosen item
  N, playing <sound>`, `[menu] closed item N`; with `--audio-log`, `[xact] mod music N: cue index
  ... on the radio's source` and `... stopped`.

- **Value menus** (mods version 1.11). `ModMenuValues` after `ModMenuOpen` gives the menu a value
  column and a note under the list (the settings menu's look; up to 10 rows at once). A, left and
  right report `"chosen", i, dir` (0 A, -1 left, 1 right) and keep the menu open, so the mod changes that row and calls
  `ModMenuValues` again with the new values; B closes it as usual (`"closed", i`).
- **Opening from anywhere** (mods version 1.11). `ModMenuKey()` is `1` once after the player held
  Back + LB for 0.5 s (pad) or pressed F8 (keyboard, unless bound); a request nobody read within
  1 s, or made while a menu is open, is dropped, and a level load drops it too. The game keeps
  running behind a mod's menu (only the player's pad is neutral).

A minimal use, from a walk-up prompt's handler:

```lua
function MyMod.Open(flag, who)
  if ModMenuOpen("Music", "Quiet\nIn the City\tvags/music/in_the_city", "A: choose  B: back", 1, nil) then
    ScheduleFunc("MyMod.Poll", 100)
  end
  return true
end
function MyMod.Poll()
  local state, i = ModMenuState()
  if state == "open" then ScheduleFunc("MyMod.Poll", 100)
  elseif state == "chosen" and i == 1 then ModMusicPlay(nil) end
end
```

## Jukebox (`95-jukebox`, id `jukebox`)

"Jukebox" / "Radio". In the hideout, the game's boom box (`dyn_blaster`, `level95` spawns it at
(-165.5, 96.3); `StartLevel` hands it to the engine's `SetupRadio`, which plays the radio's songs
and DJ links on it, `vags/music/<id>` cues of the streamed bank `xbox003`) carries level95's own
"Start Next Level" spot (`fMenu_5`). The jukebox's walk-up prompt stands a few steps south of it,
at (-166.3, 93.6), in front of the crates: "Jukebox / press (Y) to choose music" ("Radio /
presionar (Y) para elegir música").

- **Menu.** "The game's radio" (the game's own songs again), "Shuffle (all songs)" (a random song,
  then the next when it ends) and the 16 full-length radio songs of the game, each previewing its
  song. The cover is the game's "Stay Tuned" loading screen (the boom box mini-game), cropped to
  its picture, read from your own disc (`disc:f0529bb6:c9b3c1ce:0,100,262,262`).
- **While in the hideout** the chosen song loops on the boom box and the game's radio songs and DJ
  links stay silent (`ModMusicMute`). The choice is kept with `ModSave` (`song`: an id or
  `shuffle`, per save profile) and comes back 3 s after the hideout loads; outside the hideout the
  game's music is untouched.
- Files: `jukebox.lua` (`boot`: the song list, texts, menu and shuffle), `hideout.lua`
  (`after.level95`: the mute list, the prompt, the resume). Works with or without the other mods.

## Cowboy keeps his hat (`96-cowboy-hat`, id `cowboy-hat`)

"Cowboy keeps his hat" / "Cowboy no pierde su sombrero" (issue #7). Off by default; `saves = no`. This
changes the original game on purpose: there Cowboy's hat flies off when he takes a beating.

**What the game has**: Cowboy is `CfgChar` 18, 19 and 20 (`warr_cb`, `warr_cb_gen`, `warr_cb_cv`), all
with the hat object `dyn_warr_cb` (`config_preload2.lua`: `OBJECT.TYPE_HAT`, `PHYS.OBB`, picked up with
`ANIM.LeftHandHatPickUp`, and its own `CfgHat` knock-off poses), so the game throws it like any gang
hat. The game's own `HuSetKeepHat(h, 1)` keeps a character's hat on: `level54*.lua` use it on Cowboy,
`level80_chapter2.lua` turns it on and later off again.

**What the mod does** (`cowboyhat.lua`, `boot`): wraps `HuCreate` by global name and calls
`HuSetKeepHat(h, 1)` on every Cowboy the level creates; it also wraps `HuSetKeepHat`, so a level
script turning it off for Cowboy keeps it on (other characters' calls go to the game unchanged). The
hat object and its poses are not changed. Log: `[mods] cowboy-hat: Cowboy keeps his hat`, then
`Cowboy (character N) keeps his hat` for the first three Cowboys of a run.

**With other mods**: mods boot in folder order (`10-extra-hats`, `80-dirty-fights`, then
`96-cowboy-hat`), and each `HuCreate` wrapper calls the one it found, so every wrapper runs once per
character whatever the order. More Hats Fly and Dirty Fights change hat objects and poses in
`config_preload2` (police caps, headgear such as sunglasses and beanies); neither lists
`dyn_warr_cb`, so Dirty Fights never knocks it off with its own `HuDrop` rule, and the game's own
knock-off is the one `HuSetKeepHat` stops. Test: `tests/test_mod_cowboy_hat.py` (Lua 4.0, alone and
booted after the other two).

## No combo slow motion (`97-no-combo-slowmo`, id `no-combo-slowmo`)

"No combo slow motion" / "Sin cámara lenta en combos" (issue #7). Off by default; `saves = no`;
`min_runtime = 1.10`.

**What the game does** (native code; Lua cannot reach it): some of the player's fight animations
carry two events, 0x2e (start) and 0x2f (end): the slow motion players see during combos such as
X+A, A, A, most of all with Rage on. The human's animation-event handler (XBE 0x13d70) runs 0x2e
only for a player-controlled character and only when one player plays (`[[0x5b9f78] + 0x220] == 1`):
0x335ec0 marks that player's slow motion and sets the world time step to the follow camera's
slow-motion share (`CfgFollowCamera`'s last argument, `FOLLOWCAM_SLOWMO_PERCENT` = 0.2 in
`global.lua`) times 1/30 s; 0x2f (0x335ef0) clears the mark and sets the normal step. With two
players the test fails, so co-op never slows down.

Other slow motion in the game, which this mod leaves alone: the level scripts' `HuSetSlowMo` (mission
3's chase when the camera fixes, single player only, `level3_chase.lua`; a ring-out in Rumble,
`royal.lua`, in every mode) and a boss's own native scene (XBE 0x1b9020, CfgChar 79).

**What the mod does** (`noslowmo.lua`, `boot`): `ModComboSlowMo(0)`. The runtime's hook on the call at
0x146ae skips the start in single player, as the two-player test does; the end event then finds no
mark and sets the normal step as it always does. Log: `[mods] no-combo-slowmo: combo slow motion
removed in single player (as in co-op)`, then one `[huspeed] combo slow motion (player 0): skipped`
per combo that would have slowed down. Source: `runtime/src/hle_huspeed.c`.

## Cheats menu (`98-cheats`, id `cheats`)

"Cheats menu" / "Menú de trucos". Off by default; `saves = yes` (some cheats change the save for good,
see below); `min_runtime = 1.11` (`ModMenuValues`, `ModMenuKey`).

**What the game has**: the button codes are matched in native code (XBE 0x7e900: the last six buttons
against a table at 0x458a40), which only calls the Lua function `global.lua` names with
`SetCheatCallback("DbgEnterCheat")`, with the cheat's number. `DbgEnterCheat(n)` (not in the front
end, only with a living player) runs `DoCheat(n, player, 1)`, and when it took plays the cheat's voice
line and shows "Cheat Activated!", then runs `DoCheat(n, player2, 2)` for a second player.

| # | Cheat (menu name) | What `DoCheat` does | Kind | Save |
| --- | --- | --- | --- | --- |
| 1 | Unlimited health | `HuSetGodMode(player, true)` | toggle | no |
| 5 | Unlimited rage | full rage, locked (`HuSetFullRage`, `HuSetLockedRage`); the code again turns it off (global `CheatRage`); not in level 99 | toggle | no |
| 2 | Unlimited sprint | `HuSetTireless(player, true)` | toggle | no |
| 3 | Lose the cops | `GangClearWanted(GangWarriors)` | once | no |
| 6, 7, 8, 9, 11 | Get a knife / bat / lead pipe / machete / unbreakable bat | drops the weapon in hand, puts that one in it; not in boss fights | once | no |
| 26 | 99 credits | `HUDANSetCredit(99)`, Armies of the Night only | once | no |
| 0 | $200, flash and spray paint | 9 flash, 9 spray paint, money **set** to $200; 9 cuff keys and 9 handcuffs once those are unlocked | once | yes (money) |
| 12 | Complete current mission | the mission-complete screen after 1.5 s (Armies of the Night: unlocks the level and loads the next) | once | yes |
| 13 | 100% completion | `UM_Unlock` of every mission, bonus and hub reward, then as 12 | once | yes |
| 14-25 | Brass knuckles, Steel-toed boots, Cuff keys, Handcuffs, Uncuff yourself, More flash capacity, Flashhead, More combat stamina, More sprint stamina, Dealer hints, Hobo allies, Knife dealers | `UM_Unlock(95, 1, N)`: the hub reward (data unlock 6,5-6,16; names from the game's unlock list), 14 and 15 also put the gear on | once | yes |
| 4, 10 | (none) | return false: these codes do nothing | - | - |

The game's rule: rage gives god mode (`SetRageMode`) and its end clears it (`ClrRageMode`), so
**rage ends unlimited health**; with unlimited rage on, every rage still ends that way.

**What the mod does** (`cheats.lua`, `after.global`, every Lua state): every 250 ms of game time it
checks `ModMenuKey()` and, in a level with a living player, opens a value menu (`ModMenuOpen` +
`ModMenuValues`) with the cheats above (not 4 and 10), each with a note under the list.

- A cheat from the menu is `DbgEnterCheat(n)`: the same as typing its code (voice line, "Cheat
  Activated!", a second player too). The game's refusals stay (a weapon in a boss fight, credits
  outside Armies of the Night): the row then shows "Not now".
- Toggles show On / Off; A, left or right switches them. Off uses what the cheat set: unlimited
  rage is the game's own second code, unlimited health `HuSetGodMode(player, 0)` (left to the game
  while a rage runs, whose end clears it), unlimited sprint `HuSetTireless(player, 0)`. The mod wraps
  `DoCheat` (to learn whether a cheat took, and to show a typed code's toggle as On) and
  `SetRageMode` / `ClrRageMode` (when rage ends, unlimited health shows Off, as the game made it).
- Toggles that are on stay on for the rest of the run: kept with `ModKeep` (never written) and set
  again silently (`DoCheat`, no voice line) once the player is up in the next level.
- One-shots work on A only. The cheats that change the save (0, 12, 13, 14-25) show "Changes save",
  their note starts with "Changes your save" and they need A twice ("A again: yes"); choosing another
  row cancels. 12 and 13 close the menu (the mission ends). As a `saves = yes` mod, the runtime
  backs up the save before the game starts (see [Safety](#save-backups-before-save-changing-mods)).
- The game keeps running behind the menu (only your pad is neutral): open it when nobody is
  hitting you.

Log: `[mods] cheats: cheats menu ready: ...` at load; later lines come from scheduled code and so read
`[mods] log: ...` (see `ModLog`): `menu open`, `Unlimited health on` / `off`, `<cheat>
(cheat N)`, `<cheat>: the game refused it here`, `rage ended: unlimited health off (the game's
rule)`, `kept cheats set again`; the runtime's `[menu] Back + LB: a mod's menu requested` (or `F8`)
and `[menu] chosen item N (A)`. Test: `tests/test_mod_cheats.py` (Lua 4.0, stand-in game functions).

## Writing your first mod

1. Make `mods/my-first-mod/mod.ini`:
   ```ini
   id = my-first-mod
   name = My first mod
   version = 0.1
   after.level95 = hello.lua
   ```
2. `mods/my-first-mod/hello.lua`: `ModLog("hideout loaded, player = " .. tostring(player))`
3. Run with `--mod my-first-mod` (or switch it on in the launcher's Mods page) and look for
   `[mods] my-first-mod: hideout loaded ...` in the log.

## Command line

`--mods DIR`, `--mod ID[,ID...]` (repeatable; `all` = every mod found), `--no-mods`, `--mods-trace`,
`--rebuild-mod-assets [ID]`: see
[ARGUMENTS.md](ARGUMENTS.md).

## How it works

The runtime patches the game's `lua_dobuffer` (0x233f80), through which the game runs every script it
loads, with a small stub: the original call runs first, then, when it succeeded, the runtime runs the
matching mod files with the same `lua_dobuffer` and the chunk name `<id>/<file>`. That point is a C
boundary outside the Lua VM (the same place where the game's own C code runs scripts), so the state is
consistent. Every patched site is checked against the expected bytes first; on a mismatch nothing is
written and `[mods] not available: ...` is logged. `ModSave` / `ModLoad` are C closures (made with the
game's `lua_pushcclosure`) whose one upvalue is the mod's id; the save profile is the save folder
whose `profile.dat` the game opened last (logged as `[save] profile <folder>`). Source: `runtime/src/hle_mods.c`,
`runtime/src/modini.c` (also used by the launcher).
