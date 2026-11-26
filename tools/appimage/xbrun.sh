#!/bin/sh
# usr/bin/xbrun inside the AppImage (tools/appimage/build.sh installs it): runs the i386 game runtime
# usr/lib/warriorsreborn/xbrun through the bundled i386 loader with only the bundled i386 libraries
# (usr/lib/i386-linux-gnu: glibc, SDL2 for audio and pads, the PulseAudio client), so the host needs
# no 32-bit libraries.
#
# - XBRUN_PROGRAM_DIR: started through the loader, /proc/self/exe is ld-linux.so.2, so the runtime's
#   "program folder" (mods/, Photos/) is named here: the data folder the game runs in.
# - The window, input and GPU go to the x86_64 render helper (--render-process helper), whose Vulkan
#   and SDL2 come from the host's 64-bit libraries; given explicitly with --render64-helper. Also with
#   --video null (it renders offscreen there): in-process rendering would need the host's i386
#   Vulkan driver. A command line that already picks --render-process is left as is
#   (--render-process off: in-process, with the host's i386 Vulkan driver if it has one).
set -eu
here=$(dirname "$(readlink -f "$0")")
usr=$(dirname "$here")
lib32=$usr/lib/i386-linux-gnu
XBRUN_PROGRAM_DIR=${WARRIORS_DATA_DIR:-$PWD}
export XBRUN_PROGRAM_DIR

helper=1
for a in "$@"; do
    [ "$a" = "--render-process" ] && helper=0
done
if [ "$helper" = 1 ]; then
    set -- "$@" --render-process helper
fi
exec "$lib32/ld-linux.so.2" --library-path "$lib32" --argv0 "$0" "$usr/lib/warriorsreborn/xbrun" "$@" \
    --render64-helper "$here/xbrun-render64"
