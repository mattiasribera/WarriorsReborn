# WarriorsReborn on Linux (AppImage) and Steam Deck

`WarriorsReborn-<version>-x86_64.AppImage` is one file that holds the launcher, the game runtime
and everything they need. It contains no game data: you supply your own copy of the game (an ISO
image of your disc, or a folder with its extracted files), exactly as on Windows.

## What the host needs

- An x86_64 Linux desktop with X11 or Wayland (Xwayland), and a Vulkan driver for your GPU (the
  64-bit one; every current Mesa, NVIDIA and AMD driver package ships it).
- **No 32-bit (i386) libraries.** The game runtime is a 32-bit program, but the AppImage brings
  its own 32-bit C library, SDL2 and PulseAudio client and starts it with them; the window, input
  and GPU work is done by a 64-bit helper that uses your system's normal libraries.
- SDL2 (64-bit): used from your system when installed (`libsdl2-2.0-0` on Debian and Ubuntu,
  `SDL2` or `sdl2-compat` on Fedora and Arch, present on SteamOS). When it is missing, the
  AppImage uses its own copy, which needs glibc 2.38 or newer (Ubuntu 24.04, Fedora 39, Debian 13
  and later); on older systems install your distribution's SDL2 package.
- Sound goes to PulseAudio or PipeWire (through `pipewire-pulse`, the default on current desktops
  and on SteamOS).
- FUSE to mount the AppImage: the `fusermount3` or `fusermount` program (package `fuse3` or
  `fuse`, installed on most desktops and on SteamOS). Without it, start the AppImage with
  `--appimage-extract-and-run`, or set `APPIMAGE_EXTRACT_AND_RUN=1`.

## Desktop Linux

1. Put the AppImage in a folder of its own, for example `~/Games/WarriorsReborn/`.
2. Make it executable: `chmod +x WarriorsReborn-*-x86_64.AppImage` (or Properties > Permissions
   > "Allow executing file as program" in your file manager).
3. Start it. The launcher opens; its setup asks for your game (the ISO or the extracted folder)
   and extracts what it needs.

### Where your files go

Profiles (`warriors-launcher.ini`), saves (`hdd/`), mods (`mods/`), logs (`logs/`), photos and
the extracted game go to one data folder:

1. `WarriorsReborn-data/` next to the AppImage file, when that folder is writable;
2. otherwise `$XDG_DATA_HOME/WarriorsReborn` (usually `~/.local/share/WarriorsReborn`).

Set `WARRIORS_DATA_DIR` to use another folder. `./WarriorsReborn-*.AppImage --data-dir` prints the
folder in use. The example mods are copied into `mods/` on the first start, so you can turn them on
and off in the launcher; later versions do not overwrite that folder (delete it to get the new
examples).

### Without the launcher

`./WarriorsReborn-*.AppImage --xbrun <game>/default.xbe <game> [options]` runs the game directly,
with the options of [ARGUMENTS.md](ARGUMENTS.md), in the data folder.

## Steam Deck

The Deck runs the same AppImage. SteamOS already has everything listed above.

### Install (Desktop Mode)

1. Hold the power button and pick **Switch to Desktop**.
2. Download the AppImage into a folder such as `~/Games/WarriorsReborn/` (a microSD card works
   too, as long as the folder is writable).
3. Right-click it > Properties > Permissions > **Is executable**.
4. Copy your game's ISO to the Deck (or a USB stick), start the AppImage once and finish the
   launcher's setup with the mouse or touchscreen.

### Add it to Steam (Game Mode)

1. Still in Desktop Mode, open Steam > **Games** > **Add a Non-Steam Game to My Library...** >
   **Browse...**, change the file type to "All files", pick the AppImage and click **Add Selected
   Programs**.
2. Optional: in the game's **Properties** rename the shortcut to "WarriorsReborn".
3. Use **Return to Gaming Mode** on the desktop. The game is in your Library under
   **Non-Steam**.

Steam starts the launcher; **Play** starts the game. To skip the launcher, set the shortcut's
launch options to `--xbrun <path to the game>/default.xbe <path to the game> [options]`.

### Controller

The built-in controls appear to the game as an Xbox-style controller (Steam Input's default
gamepad layout for non-Steam games), so no setup is needed: the left stick moves, the face buttons
match the Xbox ones. The launcher's controller pages work with the Deck's buttons too. If you change
the Steam Input layout for the shortcut, keep it a gamepad layout (not "Keyboard (WASD) and
Mouse").

### Tips for the Deck

- In the launcher pick the **Laptop** preset, or a 1280x800 resolution, for a steady frame rate.
- The Steam overlay and Steam's performance settings (frame limit, TDP) apply as for any game.

## Troubleshooting

- Each run writes a log to `logs/` in the data folder; attach it to bug reports.
- "error while loading shared libraries: libSDL2-2.0.so.0" or a "GLIBC_2.38 not found" message:
  install your distribution's SDL2 package (see above).
- A black window or "Vulkan unavailable" in the log: install your GPU's 64-bit Vulkan driver
  (`mesa-vulkan-drivers` on Debian and Ubuntu, `vulkan-radeon`/`vulkan-intel` on Arch, the NVIDIA
  driver's Vulkan part).
- The AppImage always runs the game with the 64-bit render helper (`--render-process helper`).
  `--render-process off` in a profile's extra arguments renders in the 32-bit game process
  instead, which needs your system's 32-bit Vulkan driver.

## Building the AppImage

`make appimage` at the top of the repository (or `tools/appimage/build.sh -o <folder>`) builds the
programs and writes `WarriorsReborn-<version>-x86_64.AppImage`. Every download is pinned by
version and SHA-256: appimagetool, the AppImage runtime, and the Ubuntu packages of the bundled
libraries (`tools/appimage/i386-libs.lock`, `tools/appimage/amd64-libs.lock`, written by
`tools/appimage/lock_libs.py`). Their licences are in `usr/share/licenses/` inside the AppImage
(`./WarriorsReborn-*.AppImage --appimage-extract` unpacks it).
