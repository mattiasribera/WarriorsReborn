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
boot = record.lua               ; hook: see below
after.config_preload2 = extend.lua
after.level95 = trophies.lua
```

Lua files are plain file names inside the mod's folder (no `/`, `\`, `:` or `..`). They are read once
when the game starts.

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
| `MOD_MESHES` | How many mesh overrides the runtime loaded from the mod's `meshes` folder, groups included (0 without one): a mod that needs its meshes checks it and changes nothing without them. |
| `ModLog(text)` | Writes `[mods] <id>: text` to the game's log (`<id>` = the mod running at that moment, else `log`). |
| `ModMeshSet(group, on)` | Turns a group of mesh overrides on (`on` not `nil` or `0`) or off; `group` is `"<mod id>/<subfolder>"` (see [Mesh overrides](#mesh-overrides)). Logs `[mods] mesh group <group> on\|off (N overrides)` when it changes, `[mods] ModMeshSet: no mesh group ...` for an unknown one. The state is the runtime's, not the Lua state's: a mod sets it again at every level start (`boot`). |
| `ModMeshPoint(group, key, x, y, z)` | Sets (x, y, z numbers) or removes (no coordinates) the world point of a mesh group under `key` (a number or string, e.g. a character's handle): the remembered characters of an [instance group](#instance-groups-per-character), the objects of a [placed group](#placed-groups-per-object). Returns the group's number of points, or `nil` (logged) for an unknown or full group. A new Lua state starts with none. Older runtimes lack it: check `if ModMeshPoint then`. |
| `ModBlood(option, value)` | Turns up the game's own blood (runtime hooks; see [Brutal fights](#brutal-fights-90-gore-id-gore)): `"drops"` 1-4 (x the drop count of a blood effect; a punch has 1), `"pool_hold"` 0 or 10-1000 s (how long a ground pool stays at full size after the game's own 3 s; 0 = the game's fade at once), `"pool_max"` 16-512 (held pools at most, default 128; the oldest fades first), `"char_gain"` 1-4 (blood on fighters shows after a smaller health loss), `"char_keep"` 0/1 (a fighter keeps the blood of his lowest health). Values are clamped to the range; an unknown option or a value that is not a number is logged and ignored. A setting lasts until the game exits; the per-level records (held pools, lowest health) start over in each new Lua state. The first call that leaves an option's game value writes its hook (`[blood] ...` in the log). |
| `ModScar(char_id, level)` | Healed marks on every character of that `CfgChar` id (see [Lasting scars](#lasting-scars-87-scars-id-scars)): `level` 0-1 (drawn in 8 steps; 0 removes them) turns the game's own blood overlay of a fighter into old injuries, plum bruises and red-brown closed cuts, fainter for a lower level, with a plaster on the face from 0.6. Fresh blood (health under 90 %) is drawn over the marks; healing takes only the fresh blood away. `ModScar()` removes every mark: the marks are the runtime's, so a mod calls it at the start of each level. At most 32 ids at once; a call that is not an id and a number is logged and ignored. The first mark writes the hook (`[scars] healed marks at 0008fe10`). |

| `ModTexLayer(layer, on)` | Turns a texture layer on (`on` not `nil` or `0`) or off; `layer` is `"<mod id>/<label>"` (see [Texture layers](#texture-layers)). Returns the number of textures the layer covers, or `nil` (logged) when there is no such layer: a mod whose layers are not built checks it. Logs `[mods] texture layer <layer> on\|off (N textures)` when it changes. Like `ModMeshSet`, the state is the runtime's: set it again at every level start. |
| `ModSpeed(character, scale)` | Caps how fast one character moves (mods version 1.3; see [Visible wounds](#visible-wounds-85-wounds-id-wounds)): `character` is a handle as `HuCreate` returns it (or `player`), `scale` 0.3-1 (clamped) multiplies the speed the game picks for him every world update, so he walks, runs and sprints at that share of it and speeds up and slows down as the game does; the player and AI characters alike (the player's speeds also come from the game's own per-character speed multiplier, which the game lowers itself for a while in one of its recovery states: it is kept at or under `scale` too). `scale` 1 removes the cap: an AI character's speed comes back at once, the player's over a few seconds (the game's own rise of that multiplier, 0.2 % per 30 Hz frame; the same per second with `--sim60`); without `scale` it only reads. Returns the character's cap after the call (1 = none), or `nil` when the handle is not a live character. A cap stays with that character until it is removed or he is gone; another character later created in the same slot starts without one. The first cap below 1 writes its two hooks (`[huspeed] speed caps at 0016b872 (AI target speed) and 0016b02c (the player's speed multiplier)` in the log). |
| `ModSave(key, value)` | Stores `value` (a string or number, kept as a string; `nil` removes the key) under `key` in the mod's own data file, for the save profile the game has loaded. Returns `1`, or `nil` with a `[mods] <id>: ModSave(...)` line in the log. See [Mod data](#mod-data). |
| `ModLoad(key)` | The string stored under `key` for the loaded save profile, or `nil`. |

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
  exact `files` name is logged. A missing meshes folder keeps its own message (it is built by a tool).
- **`min_runtime = X`.** The mods version this game supports is `MOD_RUNTIME_VERSION`
  (`runtime/src/modini.h`, now `1.3`, which added `ModSpeed`; it goes up when mods gain something older games lack). A mod
  whose `min_runtime` is newer does not load:
  `[mods] <id>: needs mod runtime X or newer (this game has 1.3); mod not loaded: update the game`.
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

Useful globals in a level: `player` (the player's character), `ENV` (the current area), `Objects` (the
level's spawned objects by name).

## Lua 4.0 in short

`for k, v in t do ... end` walks a table (there is no `pairs`); `for i = 1, n do`; `getn(t)`,
`tinsert(t, v)`, `sort(t, f)`; `format("%d", n)`, `strfind`, `strsub`; varargs arrive in the table `arg`
(`arg.n`) and `call(f, arg)` calls `f` with them; outer locals inside a function are upvalues `%x`.

## Example mods (bundled, all off by default)

| Folder | Id | What |
| --- | --- | --- |
| `10-extra-hats` | `extra-hats` | Police officers (street, rookie and transit cops) lose their cap in a fight, like gang members lose their hats. Two things kept every cop's cap on: the game sets the caps of `dyn_cops_va` (street and rookie cops), `dyn_cops_vb` (security guards) and `dyn_cops_vc` (transit cops) up as `OBJECT.TYPE_RIOT_HAT` with no physics, the kind of "hat" it also uses for hair and helmets, which never leaves the head; and its fight code never knocks a hat off a police character (`CfgChar` kind 1, ids 245-258, 263, 264), while the same model and cap under a non-police id (266, or the guards 358/359) loses it, and a beaten cop is knocked out (health stays just above 0) and later removed with the cap still on. `boot` (`record.lua`) wraps `CfgObj` and gives the three caps the settings of the game's own flying hats (the pimps': `OBJECT.TYPE_HAT`, `PHYS.OBB` on `AXIS.Z`, `ANIM.LeftHandHatPickUp`), and in every level wraps `HuCreate` and the spawn callback of `GangAddSpawner` to watch each cop the level creates: every 0.1 s it knocks the cap off with `HuDrop(cop, 1, 0, 0)` (the game's own knock-off) when a hit takes the cop under his power class's hat threshold (`CfgPowerClass` argument 10, the rule the game applies to gang members: 20 % on Normal), when one hit takes 10 % of his health or more, and when he is knocked out or dies. The cap falls to the ground, stays there and can be picked up and worn; each knock-off is logged (`[mods] extra-hats: cap knocked off cop ...`). Version 1.3: a cop who gets up picks his cap up again: from 2.5 s after the knock-off, every 0.5 s, while he is alive and conscious, no player is within 3 m of him and the cap lies within 5 m, the mod gives him the game's own "get my hat" goal `GoalGetItem(cop, 4, cap)` (the goal the game's thug brain gives itself for a lost hat); a second try 8 s later, then he leaves it (also when the player took it or it lies farther). Worn again, it can be knocked off again (`[mods] extra-hats: cop 245 goes back for his cap (2.1 m)`, `... picked his cap up`). It also records the game's `CfgHat` calls and, after `config_preload2`, gives models with an identical hat object the poses they lack. Not changed: riot police (`cops_vc1`) keep `dyn_riot_visor`, a visor on a helmet that is part of their model; undercover and internal-affairs cops wear no cap. Cops created by the game itself without a level script (if any) are not watched. |
| `20-hideout-trophies` | `hideout-trophies` | Three more trophies on the hideout's trophy wall (the south wall of the main room, where the game shows its own five trophies: `Objects.Spider`, `King`, `LC`, `Fuckyou` (a "F.U. Huns" T-shirt), `Bigmoe`), unlocked the way the game unlocks its own. The retail trophies are unlocks of type 5 in `global.lua`'s `Unlockables` table (`{level, 0, 0, 5, name, n}`: Spider = level 34, King = 2, LC = 5, Fuckyou = 81, Bigmoe = 31), opened by `UM_Unlock(Level, 0, 0)` when that mission is completed; `flashback.SetupTrophies()` in `level95` destroys the ones still locked. The mod does not add unlock records (that would change the profile's unlock table); it checks the same mission completion with `UM_IsLevelComplete(level)` (the check `level95` itself uses for level 84) when the hideout loads, and places a trophy only when its mission is done: the green dragon statue (`dyn_special_dragon`) after **level 20** (Flashback D, Fox's flashback in Chinatown against the Savage Huns: the game registers the statue there as a stolen item, `CfgInventoryItem(..., 13, ...)`, but the objective that took it was cut), and Ash's vest, in his memory, after **level 86** (mission 9, the Destroyers attack the Coney strip, where Ash dies in scene `L86C3`): a Warriors vest (`dyn_war_vest`, the game's flat vest with the gang's back patch). `boot` (`config.lua`) wraps `CfgObj` so the statue, set up by the game as a floating pick-up, is a plain `simple_object` that stands where it is put, and holds the vest to the same plain registration (the game already registers it that way, like the retail T-shirt: no pick-up, spin axis or special type). `after.level95` (`trophies.lua`) places them among the retail trophies, on real surfaces: the dragon on a rusty barrel (`dyn_barrel_b`, like the barrel under the Spider trophy) beside the Huns T-shirt (`Fuckyou`), between it and the subway map, facing the room; Ash's vest hung flat on the wall between the crown's shelf (`King`) and the panties' frame (`LC`), like the T-shirt, back patch facing the room (the model lies flat with the patch up; the rotation `{0, 0.7071068, 0.7071068, 0}` stands it against the wall upright; the other quarter turn shows it mirrored, from the inside). It gives each the hideout's own walk-up prompt (`AddFlag` + `SetMsgHandlerEx`, as for the Flashback and menu spots, set up once `Cameras.follow` exists): the name as a big title at the top of the screen and one line in the message box at the bottom left, "Dragon Statue / Taken from the Savage Huns" and "Ash's Vest / In his memory" (Spanish when `GetLanguage() == 1`: "Estatua del dragón / Robada a los Savage Huns", "Chaleco de Ash / En su memoria"). The retail trophies have no text of their own; pressing the action button at the prompt does nothing (like the retail locked-flashback spot, `flashback.NoAction`). On a profile that has not completed a mission its trophy and label are not there (`[mods] hideout-trophies: <model>: level N not complete, not shown`). Not used: `dyn_s_vest_b` (no model on the retail disc); `dyn_s_vest` (1.3's Ash's vest, the vest crumpled on the fence in the death scene: a narrow rag that looks the same from every side, so on the wall it seemed to float and turn with the camera). The game has no Chatterbox vest object. Version 1.2's trophy room in the caged room under the main floor was dropped in 1.3: that room had no collision (no floor), so it could not be entered; 1.6 brings it back, walkable (see [The gang trophy room](#example-the-gang-trophy-room-20-hideout-trophies)). Version 1.5 adds **Cleon's necklace** once the story is finished (`UM_IsLevelComplete(84)`, the final mission, the check `level95`'s own `flashback.SetupTrophies()` uses): the silver necklace painted on Cleon's chest, laid on deep red velvet in a wooden shadow-box frame hung on the wall above the crown's shelf (`King`; walk-up spot at the shelf's left end, halfway between the Flashback spots in front of the Spider trophy and the crown; those spots only prompt while their flashback is unlocked and not yet played, so after the story they are silent), with the label "Cleon's Necklace / The one he always wore" ("Collar de Cleon / El que siempre llevó"). Its mesh is built once from your game files (see [Example: Cleon's necklace](#example-cleons-necklace-20-hideout-trophies)); without it the necklace is not shown and the other trophies are. Version 1.6 turns the caged room under the main floor (walkable since its collision patch, `cage.col`) into a lit **gang trophy room**: one trophy per gang the Warriors beat, each shown once its mission is complete, with its own walk-up label (see [The gang trophy room](#example-the-gang-trophy-room-20-hideout-trophies)). |
| `30-train-flyby` | `train-flyby` | `after.level9_chap2_tags`: wraps `ch2.ContinueLevel` (mission 13, All City: the objective "Bomb four more fresh trains" starts) and 1.5 s later plays the unused retail camera shot `l9_flyby1` with `SuperRunScene`, then gives the camera and HUD back. Once per level load (a checkpoint restart reloads the level and plays it again). |
| `40-cleon-bandana` | `cleon-bandana` | Cleon's bandana is a hat the game can knock off; without it he shows his flashback head. Needs its mesh overrides built once from your game files (see [Mesh overrides](#mesh-overrides)). |
| `50-protection-money` | `protection-money` | `after.level95_coney`: wraps the hub's `shops.Setup` / `shops.CleanUp`. In the protection mission (level 86) the Warriors press Talk at each shop owner, ask for the week's money and get $25. On the Coney streets afterwards the game has the butcher (Stefano), Voodoo Motors (Skinny Pete) and the Little Buddha (Twiggy) each open with a 65 % chance, carrying $25 (`HuSetMoney`) only in its three collection rounds, and you can only mug them; Charlie's plumbing store never opens. The mod gives every open owner who has money the mission's Talk prompt (`SetMsgHandlerEx`, with the hub's own `CfgActionDistance(1, 2)` while you are near) and plays the mission's exchange: Cleon's or Swan's ask line, the owner's pay line, the `money_take` / `money_give` hand-over, the reassurance. An owner who owes the game nothing pays again 10 minutes of game time after his last payment, and Charlie opens with the same odds on the pavement in front of his shutters (a fourth `GoalShopkeeper`, in the hub's existing volume box there: `AddVolumeBox` while the streets are up crashes). Payments (talked or mugged) are noticed when the owner's money drops to 0; cooldowns live in the hub's Lua state (a mission or reload starts fresh); the game's collection flags are not touched. `saves = yes`: the money is saved by the game. |
| `70-gym-records` | `gym-records` | Records for the hideout gym, once the story is finished (`UM_IsLevelComplete(84)`, the last story mission; before that the mod logs `story not finished ...` and changes nothing). `after.level95_workout` (`records.lua`, loaded again for every set) wraps the set's `workout.StartHUD`, `workout.DoRep` (chin-ups, push-ups, sit-ups), `workout.Check01` (heavy bag) and `workout.CleanUp` (every end of a set): a set is timed with `GetGameTime()` from the counter appearing to the rep or combo that reached the rank's target (`repTable[rank]`), and the fastest successful time per exercise and rank is kept with `ModSave` (`best.<exercise>.<rank>`, ms, per save profile); timed-out and abandoned sets do not count. 1.8 s after a successful set the hideout's announce slot (`HUDSetAnnounceMsg(5, ...)`, where the game shows "new feature unlocked") shows the exercise, the rank, the time and the record, or "New record!" with the old one (Spanish when `GetLanguage() == 1`: "¡Nuevo récord!"). `after.level95` wraps `worksetup.BuildWorkoutMessage`: each station's walk-up panel gets a purple "Rank N record: 9.53 s" line for the rank you would train next. Every set is logged: `[mods] log: gym-records: chin-ups rank 10: 20/20 in 14.20 s (limit 36 s), done, record 14.20 s (new)`. `saves = no`: the records are in `mods-data/gym-records.json`, not in the game's save. |
| `80-dirty-fights` | `dirty-fights` | "Dirty fights: falling accessories" / "Peleas más sucias: accesorios que caen". Glasses, caps and winter hats fly off in a fight, stay on the ground and can be picked up and worn; per character. Glasses can break (1.1), a character who lost his glasses keeps looking so after leaving the view, and some fighters walk back for their hat once the fight calms down. See [Dirty fights](#dirty-fights-80-dirty-fights-id-dirty-fights). |
| `85-wounds` | `wounds` | "Visible wounds" / "Heridas que se notan": low on health, the player and his crew move slower (walk, run and sprint); healing gives the speed back; enemies keep their speed (an option). `boot` only, through `ModSpeed`. See [Visible wounds](#visible-wounds-85-wounds-id-wounds). |
| `87-scars` | `scars` | "Lasting scars" / "Cicatrices que duran": a Warrior who ended a level badly beaten shows bruises, closed cuts or a plaster in the next missions, fading over the night; remembered per Warrior with `ModSave`, drawn with `ModScar`. See [Lasting scars](#lasting-scars-87-scars-id-scars). |
| `88-dirty-clothes` | `dirty-clothes` | "Dirty clothes" / "Ropa que se ensucia": the Warriors' vests, shirts and trousers get dirty, stained and torn with the story and every fight, per Warrior; "Change clothes" in the hideout makes them clean again. Needs its texture layers built once from your game files. See [Dirty clothes](#dirty-clothes-88-dirty-clothes-id-dirty-clothes). |
| `89-police-memory` | `police-memory` | "The police remember you" / "La policía te recuerda". Trouble you cause in a district is remembered per save profile; on later visits there are more patrols, more responders and more cops react. See [Police memory](#police-memory-89-police-memory-id-police-memory). |
| `90-gore` | `gore` | "Brutal fights" / "Peleas más brutales": more of the game's own blood per hit, ground pools that stay and blood on fighters that stays; `boot` only (the runtime's `ModBlood` settings), it spawns nothing itself. See [Brutal fights](#brutal-fights-90-gore-id-gore). |


**Not in this release: the mesh builder.** Four example mods use meshes or texture layers that are
built on your PC from your own game files: `40-cleon-bandana` (all of it), `20-hideout-trophies`
(Cleon's necklace only), `80-dirty-fights` (the glasses only) and `88-dirty-clothes` (all of it).
The builder the sections below describe (`tools/mod_meshes`) is not in the public repository yet.
Without the built files, each of these mods logs that the files are missing and skips that part.
The rest of each mod, and the other mods, work as described.

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
  indices and the bytes of the vertices they use (`runtime/src/meshover.h`; `tools/mod_meshes/core.py`
  computes it). Run the game with `--mesh-log` to print the hash, size, strides and texture (its
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
the level script creates him. While he wears it nothing changes on screen: Cleon is the game's own
model with the modelled bandana (opening scenes and cutscenes too) and the hat object is hidden.
Every 100 ms the mod asks the game (`HuHasHat`) whether each Cleon still wears the hat it placed;
once one does not (a hard hit knocked it off), it turns `cleon-bandana/knocked` on: Cleon shows his
flashback head and the hat object, falling and then lying on the ground, is drawn as the bandana
until the level reloads (each level starts with the group off).

Limit: the group applies to every draw of Cleon's model: if a level had two Cleons and one lost the
bandana, both would show the flashback head (an instance group, below, would make it per character).

Build the meshes once (Python 3, from the repository; `extracted/wad` = the WAD files unpacked by
`tools/wad_extract.py` from your disc):

```
cd tools
python3 -m mod_meshes cleon-bandana --wad ../extracted/wad --out ../mods/40-cleon-bandana/meshes-v2
```

Then enable the mod (`--mod cleon-bandana`). The log shows `[mods] cleon-bandana: 3 mesh overrides
from meshes-v2`; with fewer (not built, or built by an older version of the tool: build again) the mod
changes nothing.

### Example: Cleon's necklace (`20-hideout-trophies`)

Cleon's necklace (two silver chains up the neck and two rows of silver drops) is not an object of
the game and not part of his model's geometry: it is painted on the bare chest of his skin texture
(model `warr_cl`; his flashback model `dest_cl` wears a tank top and no necklace). The only necklace
object of the game, `dyn_necklace`, is level 34's store loot. So the hideout-trophies mod shows it
the way a jeweller would: laid on deep red velvet in a wooden shadow-box frame hung on the trophy
wall. The tool builds, from the player's game files, one override:

- `7b6f89aa421da125.wmesh` / `.png`: the draw of `dyn_s_paper_list` (a sheet of paper, 8 triangles,
  that no level, sector or script of the game places: only its `CfgObj` registration exists) becomes
  the frame: a velvet board and four wooden bars made by the tool (60 triangles, 0.32 x 0.04 x
  0.44 m, the back at y = 0 against the wall, the bottom at z = 0), drawn with a texture where the
  necklace's texels from Cleon's skin (the silver ones: neutral or bluish grey, unlike the brown skin,
  the red vest and the black background) lie on drawn velvet with a soft shadow, enlarged 3 times,
  and a drawn wood grain.

`trophies.lua` spawns `dyn_s_paper_list` on the wall above the crown's shelf, at (-169.75, 81.74, -192.62)
with no rotation, only when the story is finished
(`UM_IsLevelComplete(84)`, the final mission; `level95`'s own `flashback.SetupTrophies()` uses the
same check) and the meshes are there (`MOD_MESHES >= 1`); without them it logs
`[mods] hideout-trophies: dyn_s_paper_list: its meshes are not built (tools/mod_meshes cleon-necklace), not shown`
and the other trophies are placed as before.

Build the mesh once (Python 3, from the repository; `extracted/wad` = the WAD files unpacked by
`tools/wad_extract.py` from your disc):

```
cd tools
python3 -m mod_meshes cleon-necklace --wad ../extracted/wad --out ../mods/20-hideout-trophies/meshes-v1
```

The log then shows `[mods] hideout-trophies: 1 mesh override from meshes-v1`. The folder name
carries the mesh version: a later version of the mod that needs another mesh names another folder,
so a mesh built by an older tool is never used (the old folder is reported as left over).

## Dirty fights (`80-dirty-fights`, id `dirty-fights`)

"Peleas más sucias: accesorios que caen" / "Dirty fights: falling accessories". Off by default,
`saves = no`.

**Accessories the game already has as separate objects.** The game sets 52 `hat_object`s up as
`OBJECT.TYPE_RIOT_HAT` (no physics, never leave the head): hair pieces, helmets and visors, but also
whole accessories. `boot` (`dirty.lua`) wraps `CfgObj` and gives these eight the settings of the
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

Not changed: riot hats that carry hair in the same mesh (a beanie or cap over hair: `dyn_bm_vb4`,
`dyn_eh_ma4`, `dyn_tw`, `dyn_bn_fa2`, `dyn_samo2`; without them the head would be bald), hair pieces
(`dyn_mohawk`, `dyn_sml_fro`, ...), helmets and visors, police caps (`10-extra-hats`). Each object is
its own hat, so each character loses only his own; worn by the player after a pick-up it sits where the
game puts that hat.

**Sunglasses modelled into a head** (needs the meshes, below). Two characters have sunglasses as a
separate piece of their head mesh in front of a complete face with eyes: Vargas (the Hurricanes,
`hurr_va`, CfgChar 119: level 5, AotN level 62) and Masai (the Riffs, CfgChar 64 `riff_bo_va1`: AotN
levels 60 and 64, level 84). The tool cuts each pair out as the mesh of a spare hat object no character
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
`TYPE_HAT`. Log: `[mods] dirty-fights: dyn_sun knocked off character 279 (health 12.0 %)`.

**Glasses that break** (needs the meshes; version 1.1). 1.5 s after a pair fell (both kinds of
sunglasses above, `dyn_sun` and `dyn_tr_cw`), if it still lies where it fell, it breaks with a chance
that grows with the hit: 50 % for a knock-off by a hit of 10 % of health, 2 % more per % above (less
below), 60 % at least when the wearer is knocked out or killed, never under 30 % or over 85 %. The pair
is removed (`ObjDestroy`) and a prop nothing in the game places (`dyn_s_newspaper_c`, a folded
newspaper; no other bundled mod uses it) is spawned in its place, turned at random about the
vertical; the tool builds, per kind, the broken pair as that object's mesh: lying on the ground, snapped at the bridge (one half turned 28° away), the lens of the
kept half cracked (light lines drawn into a copy of its texture) and the other lens gone, scattered as
nine glass shards around it. Each kind is a placed group (`broken-vargas`, `broken-masai`, `broken-sun`,
`broken-trcw`, see [Placed groups](#placed-groups-per-object)): the mod gives it a point where each
broken pair lies (moved with it when it is kicked), so several kinds can lie in one level and the
prop's own draw is never changed elsewhere. Vargas's and Masai's broken pairs
use their skin texture; the retail pairs are drawn in colours the tool picks (black frame and dark
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
eight above, the spares, and the game's own flying hats) may go back for it: 60 % of them do. From 2.5 s
after the loss, every 0.5 s, once he has been calm for 1.5 s (alive and conscious, `BrHasEnemies` false,
no player within 8 m) and the hat lies within 7 m of him, the mod gives him the game's own "get my hat"
goal, `GoalGetItem(h, 4, hat)` (the goal the game's thug brain gives itself when it wants its hat back,
`BrSetThugWantsHat`): he walks to it, picks it up and puts it on. A second try follows 8 s later if the
first did not end with the hat on his head; then he leaves it. Nobody goes back for a hat that was
picked up, moved, broken or is too far, nor a dead character or the player. Picked up again, the hat can
be knocked off again. Log: `[mods] dirty-fights: character 279 goes back for his dyn_bm_vb9 (4.2 m)`,
`... put his dyn_bm_vb9 back on`.

**Build the meshes** (Python 3, from the repository; `extracted/wad` = the WAD files unpacked by
`tools/wad_extract.py` from your disc):

```
cd tools
python3 -m mod_meshes dirty-fights --wad ../extracted/wad --out ../mods/80-dirty-fights/meshes-v2
```

The log then says `[mods] dirty-fights: 10 mesh overrides from meshes-v2` and turns the six groups on.
Without them the eight objects above still fly (and are fetched) but never break, and the glasses of
Vargas and Masai stay on. The folder name carries the mesh version (1.2: `meshes-v2`, the broken pairs
on `dyn_s_newspaper_c`; 1.1 drew them on the paper sheet the hideout-trophies frame uses): a folder
built by an older tool is reported as left over and not used; delete it and build again.

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
- As with meshes, layers are made from the game's textures: a mod ships the tool that paints them
  on the player's PC, and the folder is not part of the repository (`mods/.gitignore`).

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
the cutscene and civilian clothes) the tool paints three layers (`<tex>@<code>-<stage>.png`)
over the clothing texels only: the UV islands on the legs (trousers) and on the torso (vest,
shirt), not the arms, hands, feet or head, and on the torso no texel of the Warrior's skin colour
(measured on his face) or among such texels, so the chest under the open vest, the necklaces
painted on it, the faces and arms stay as they are. Dirt is made from each texel's 3D bind-pose
position (no seams between UV islands): soot and street grime (more on the lower legs and the
vest's hem), dust on the trousers, sweat and grease stains with a darker rim, and tears that show
shadowed skin through the cloth with frayed lighter threads at their edges. Each stage keeps the
marks of the earlier ones. No blood (that is the gore mod's). The blue ghost skins are left alone.
`clothes.lua` (`boot`) defines the mod and watches the fighters; `start.lua` (`after.level*`, once
per level, not in the main menu: the game's progress is not readable at the very start of the front
end) turns on, per Warrior, the layer of his stage, and a fight that raises it turns on the next.
Skins belong to the Warrior whose `CfgChar` keys name them (`warr_sw`, `_gen`, `_chase`, `_ghost`,
`_cv1`, `_cv2`): a model's own skin list also names other Warriors' skins.

**Change clothes.** `hideout.lua` (`after.level95`) puts a walk-up prompt on the hideout's main
floor beside the couch in the crew's corner, at (-179.3, 86.6, -194.3) (`AddFlag` + `SetMsgHandlerEx`, set up once
the follow camera exists, its strings kept referenced): "Change clothes / Clean clothes for the
whole crew" ("Cambiarte de ropa / Ropa limpia para toda la banda" when `GetLanguage() == 1`). The
action button stores the story missions done as `base`, clears every fight count and shows the
line as help text; every Warrior is at stage 0 at once. Like the hideout's own walk-up spots, the
prompt answers the Y button.

**Build the layers** once (Python 3, from the repository; `extracted/wad` = the WAD files unpacked
by `tools/wad_extract.py` from your disc; a few minutes):

```
cd tools
python3 -m mod_meshes dirty-clothes --wad ../extracted/wad --out ../mods/88-dirty-clothes/layers-v1
```

The log then says `[mods] dirty-clothes: 84 texture layer files from layers-v1` (28 skins, 3 stages each). Without them the
mod logs `its texture layers are not built ...` and the clothes stay clean (the wear is still
counted).

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
| Base settings | | | the game's Hard config, whatever the profile's difficulty (`SetDifficulty(2)`, so the levels' own Hard extras too) |
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

**Where it does not apply.** The menus, Quick Rumble and the Armies of the Night levels 60-64 keep the
game's settings (the log says so); the gang changes are made once at start-up and therefore also apply
there.

**Log** (`--mod warriors-mode`): `[mods] warriors-mode: applied after config_hard (profile difficulty 2,
level 80, ...): 41 enemy power classes damage x1.25 timing x0.80; block chance 70 -> 80; break-in delay 7
-> 5`, then `power c0 0.54>0.675 7>5.6 ...` (class, damage multiplier before > after, first timing value
before > after), `gang N: attackers on the player 2 -> 3, guard-break level 1 -> 2` at start-up, and one
`heal` / `money` line each time a gain is reduced.

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
mission, then 2/3, 1/3, then gone (`<who>: healed`). Levels 100 and up (menus, Quick Rumble) show
nothing. The marks are passed to `ModScar` for every id of that Warrior; healing (flash) in a level
removes the fresh blood, not the marks.

**How they look** (runtime, `runtime/src/hle_scars.c`). The game draws a fighter's blood as a 64 x 64
atlas over his skin: small pieces (a bruise, a black eye, slashes, drips) that each model maps to its own
body parts, the face to the bruise and black-eye pieces. A marked character gets a healed copy of that
atlas, made in memory from the game's own atlas when it is first needed: the lightest one (`d1`) up to
level 0.5, the next one (`d2`) above; every piece recoloured as an old injury (plum bruises with a
yellow-green edge, thin strong pieces a red-brown closed cut, no fresh red; mid tones, so they read on
light and dark skin), its opacity 25-80 % by level; and from level 0.6 a plaster (a beige strip with a
gauze pad) over the face pieces. Fresh blood is
laid over the healed atlas, so a marked Warrior who is hurt again shows both. The same draw is used in
gameplay and in the real-time cutscenes. Nothing of the game is stored or shipped.

**Log** (`--mod scars`): at each level start `[mods] scars: swan ended level 3 at 5 % (1 knock-outs): new
marks 1.00`, `swan: marks 1.00 (severity 1.00, 0 missions)` per marked Warrior, and `level N: watching
W Warriors`; `scars: fox knocked out (1 so far this level)` during a level; `[scars] healed marks at
0008fe10` once. With `--mods-trace`, `[scars] character 5: marks step 8` per call and one `[scars]
texture: ...` line per healed atlas made.

**Limits.** A mark is per character id: two characters of the same id in a level (the game does not do
that with the Warriors) would both show it. Where on a model a mark or the plaster lands is the model's own
mapping of the atlas: the same pieces also cover parts of the arms and chest, so a heavy mark shows a
bruise or a strip of plaster there too, and on some views of a face the plaster is only a narrow strip.
On dark skin the marks are subtler. Characters
created without a level script (none of the Warriors) are not watched. The game's no-blood setting hides
the marks too.

## Visible wounds (`85-wounds`, id `wounds`)

"Heridas que se notan" / "Visible wounds": low on health, you and your crew move slower. Off by default;
`saves = no`. Nothing new is drawn or animated: the game's own walk, run and sprint, slower.

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

**What the mod does** (`wounds.lua`, `boot`; every number is a field of `Wounds.cfg`): every 200 ms of
game time it checks every character the level creates (`HuCreate`, gang spawners) and the player,
and caps his speed by his health (`HuGetHealthPercent`):

| Health | You and your crew (gang type 0) | Everyone else (`others`, off by default) |
| --- | --- | --- |
| 40 % and more | full speed | full speed |
| under 40 % | 80 % | full speed (0.9 when switched on) |
| under 20 % | 65 % | full speed (0.8 when switched on) |

You run at exactly that share of your speed. An AI character (your crew) does not run a little slower:
below his running speed he drops to a slower gait, so a capped crew member moves at about half his
running speed (measured: caps from 0.65 to 0.9 all gave 0.5-0.65 of it, 0.95 nothing). That is why
enemies are left alone by default: hurt enemies at half speed would reach you and get away much more
slowly, and the mod must not make fights easier. Set `others` in `Wounds.cfg.steps` (e.g. 0.9) to slow
them too.

Healing (flash, the game's own health rises) gives the speed back at the next check: a crew member at
once, you over a few seconds (the game's own recovery of your speed).

**Where it does not apply.** Characters in a scene (`HuIsInScene`); characters a level runs in a
scripted chase: the ones it makes tireless (`HuSetTireless(h, 1)`, as the chase missions do with the
player and the crew; full speed again when the level turns it off) and the runners of the chase goals
(`GoalDevilRun`, `GoalRunCarrotRun`, `GoalLeadChase`, for the rest of the level). Missions with a
timer are not exempt: being slower when hurt is the point, and the game's flash heals it. Quick
Rumble and Armies of the Night levels apply it too. Pedestrians the game creates by itself (not
through a level script) are not watched.

**Log** (`--mod wounds`): `[mods] log: wounds: character 5 (player): health 35 % -> speed x0.80` at
every change; the first cap writes the runtime hooks (`[huspeed] speed caps at 0016b872 ...`).

**Measured** (headless, mission 2 street, `--goto m2:4`; scratch probes): the player running straight
on the street at 7.8 m/s runs at 6.2 m/s with the 80 % cap and 5.1 m/s with 65 %, the same with
`--sim60`; an AI fighter running 10 m to the player, 5.8-6.6 m/s, ran 3.3-4.5 m/s with a 65 % cap,
3.3 m/s with 90 %, unchanged with 95 %. With the mod: the player at 15 % health got x0.65 within one
check and x1.00 when healed to 100 %, at 30 Hz and with `--sim60`, no crash.

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
| HUD hint once 8 s of the level have passed (`HUDSetAnnounceMsg(5, ...)`, on two lines) | | "The cops are looking for you around here" / "La policía anda buscándote por acá" | same | same |

**Fair play.** Responders are capped at 5 per crime (with Warriors Mode, whose +1 stops at 4, the
levels' calls end at 4-5); the mod never makes you wanted on arrival, never forces the crime state and
never touches arrests, so a hot district means more and quicker police, not a trap you cannot leave.
Notoriety only grows from what you do there.

**Log** (`--mod police-memory`): `[mods] log: police-memory: level 95 (coney): notoriety 31.8 (was 63.6,
1 level(s) away; peak 63.6, 4 crimes, 1 cops beaten) -> tier 1: responders +1 (at most 5), ...` at every
level start, `patrol spawner BeatCop (BeatCop): 1 at a time -> 2, every 3000 -> 2250 ms` for each
changed spawner, `wanted (incident N here): notoriety X`, `no longer wanted after S s: ...`,
`cop beaten (N here): ...` and `hint shown: ...`. Setting `PoliceMemory.cfg.status_ms` (e.g. with
`--lua-exec`) logs the police gangs' head counts and how many characters their spawners made that often.

**Limits.** The effects are set when the level loads (spawners and responders are the level's); new
trouble counts from the next visit. Notoriety is per district, not per street of the hub. Cops a level
creates without `HuCreate` or a police spawner are not counted when beaten.

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
health rule for everyone, the player too. The no-blood switch of the game (a word it reads before any
blood) is left alone.

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

`--mods DIR`, `--mod ID[,ID...]` (repeatable; `all` = every mod found), `--no-mods`, `--mods-trace`: see
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
