# warriors-launcher: a Dear ImGui (SDL2 + SDL_Renderer) front end that builds and runs the xbrun
# command line from profiles. 32-bit on both hosts like the runtime, so the Windows build shares
# the 32-bit SDL2.dll shipped next to TheWarriors.exe. Built with zig as in runtime/Makefile.
#
# The GUI needs the SDL2 headers and, for Windows, the import library: both come from the
# SDL2 mingw development package (make sdl2-devel fetches it into ~/.cache/launcher). On Linux
# the launcher links against the host's i386 libSDL2-2.0.so.0 (libsdl2-2.0-0:i386).
CXX = zig c++ -target x86-linux-gnu.2.34
WCXX = zig c++ -target x86-windows-gnu
WARN = -Wall -Wextra
CXXFLAGS = -std=c++17 -O1 -g $(WARN) -fno-exceptions -fno-rtti -mstackrealign
WCXXFLAGS = -std=c++17 -O1 $(WARN) -fno-exceptions -fno-rtti -mstackrealign
B = build
WB = $(B)/win

SDL2_VERSION = 2.32.10
SDL2_CACHE ?= $(HOME)/.cache/launcher
SDL2_DEVEL ?= $(SDL2_CACHE)/sdl/SDL2-$(SDL2_VERSION)/i686-w64-mingw32
SDL2_INCLUDE = $(SDL2_DEVEL)/include/SDL2
SDL2_LINUX_LIB ?= /usr/lib/i386-linux-gnu/libSDL2-2.0.so.0
SDL2_WIN_LIB = $(SDL2_DEVEL)/lib/libSDL2.dll.a

CORE_SRCS = src/profile.cpp src/command.cpp src/xbe_check.cpp src/iso.cpp src/platform.cpp src/png.cpp src/titleimage.cpp src/i18n.cpp src/keymap.cpp src/backup.cpp src/report.cpp src/uiscale.cpp src/padmap.cpp
CORE_HDRS = src/profile.h src/command.h src/xbe_check.h src/iso.h src/platform.h src/png.h src/titleimage.h src/i18n.h src/i18n_strings.h src/keymap.h src/backup.h src/report.h src/uiscale.h src/padmap.h

IMGUI = third_party/imgui
IMGUI_SRCS = $(IMGUI)/imgui.cpp $(IMGUI)/imgui_draw.cpp $(IMGUI)/imgui_tables.cpp $(IMGUI)/imgui_widgets.cpp \
             $(IMGUI)/imgui_impl_sdl2.cpp $(IMGUI)/imgui_impl_sdlrenderer2.cpp $(IMGUI)/imgui_stdlib.cpp
GUI_SRCS = src/main.cpp src/gui.cpp src/theme.cpp
GUI_HDRS = src/gui.h src/theme.h
# The fonts, embedded so the launcher stays one self-contained file (third_party/inter, OFL), and
# the port's icon for the window (tools/icon/make_icon.py).
FONTS = third_party/inter/Inter-Regular.ttf third_party/inter/Inter-SemiBold.ttf
ICON = ../assets/icon/wonder-wheel.ico
# The launcher's version in About and the bug report: git describe of the nearest release tag
# without its "v", or RELEASE_VERSION without a tag or outside a checkout (see runtime/Makefile).
RELEASE_VERSION = 0.1.0
VERSION := $(patsubst v%,%,$(shell git describe --tags --match 'v[0-9]*' --dirty 2>/dev/null || echo $(RELEASE_VERSION)))
GUI_FLAGS = -isystem $(IMGUI) -isystem $(SDL2_INCLUDE) -DSDL_MAIN_HANDLED -DLAUNCHER_VERSION='"$(VERSION)"'
# Vendored and unmodified: v1.91.5's imgui_widgets.cpp trips clang's -Wuninitialized-const-pointer.
IMGUI_FLAGS = $(GUI_FLAGS) -Wno-uninitialized-const-pointer

SDL2_CHECK = @test -f $(SDL2_INCLUDE)/SDL.h || { echo "SDL2 headers not found in $(SDL2_INCLUDE):" \
             "run 'make -C launcher sdl2-devel' or set SDL2_DEVEL" >&2; false; }

IMGUI_OBJS = $(patsubst $(IMGUI)/%.cpp,$(B)/imgui_%.o,$(IMGUI_SRCS))
WIMGUI_OBJS = $(patsubst $(IMGUI)/%.cpp,$(WB)/imgui_%.o,$(IMGUI_SRCS))

all: $(B)/launcher_test $(B)/warriors-launcher

windows: $(B)/warriors-launcher.exe

# Game-independent core test (profiles, argv builder, quoting, synthetic XBE headers, child log).
$(B)/launcher_test: tools/launcher_test.cpp $(CORE_SRCS) $(CORE_HDRS) | $(B)
	$(CXX) $(CXXFLAGS) -o $@ tools/launcher_test.cpp $(CORE_SRCS) -lc++

$(B)/warriors-launcher: $(GUI_SRCS) $(GUI_HDRS) $(CORE_SRCS) $(CORE_HDRS) $(IMGUI_OBJS) $(B)/fonts.o | $(B)
	$(CXX) $(CXXFLAGS) $(GUI_FLAGS) -o $@ $(GUI_SRCS) $(CORE_SRCS) $(IMGUI_OBJS) $(B)/fonts.o $(SDL2_LINUX_LIB) -lc++ \
	    -Wl,-rpath,'$$ORIGIN'

$(B)/fonts.cpp: tools/embed_files.py $(FONTS) $(ICON) | $(B)
	python3 tools/embed_files.py $@ inter_regular=third_party/inter/Inter-Regular.ttf \
	    inter_semibold=third_party/inter/Inter-SemiBold.ttf icon_ico=$(ICON)

$(B)/fonts.o: $(B)/fonts.cpp
	$(CXX) $(CXXFLAGS) -g0 -c -o $@ $<

$(WB)/fonts.o: $(B)/fonts.cpp | $(WB)
	$(WCXX) $(WCXXFLAGS) -c -o $@ $<

$(B)/imgui_%.o: $(IMGUI)/%.cpp $(IMGUI)/*.h | $(B)
	$(SDL2_CHECK)
	$(CXX) $(CXXFLAGS) $(IMGUI_FLAGS) -c -o $@ $<

# GUI subsystem (no console window); --smoke still reports through its exit code.
# src/launcher.rc: the port's icon (assets/icon/wonder-wheel.ico).
WRC = src/launcher.rc $(ICON)
$(B)/warriors-launcher.exe: $(GUI_SRCS) $(GUI_HDRS) $(CORE_SRCS) $(CORE_HDRS) $(WIMGUI_OBJS) $(WB)/fonts.o $(WRC) | $(WB)
	$(WCXX) $(WCXXFLAGS) $(GUI_FLAGS) -Wl,--subsystem,windows -o $@ $(GUI_SRCS) $(CORE_SRCS) $(WIMGUI_OBJS) $(WB)/fonts.o src/launcher.rc \
	    $(SDL2_WIN_LIB) -lc++ -lshell32 -lole32 -luuid

$(WB)/imgui_%.o: $(IMGUI)/%.cpp $(IMGUI)/*.h | $(WB)
	$(SDL2_CHECK)
	$(WCXX) $(WCXXFLAGS) $(IMGUI_FLAGS) -c -o $@ $<

# Fetches the SDL2 mingw development package (headers, i686 import library) into the cache.
SDL2_URL = https://github.com/libsdl-org/SDL/releases/download/release-$(SDL2_VERSION)/SDL2-devel-$(SDL2_VERSION)-mingw.tar.gz
sdl2-devel:
	mkdir -p $(SDL2_CACHE)/sdl
	curl -sSLf -o $(SDL2_CACHE)/SDL2-devel-$(SDL2_VERSION)-mingw.tar.gz $(SDL2_URL)
	tar -xzf $(SDL2_CACHE)/SDL2-devel-$(SDL2_VERSION)-mingw.tar.gz -C $(SDL2_CACHE)/sdl

$(B):
	mkdir -p $(B)

$(WB):
	mkdir -p $(WB)

clean:
	rm -rf $(B)

.PHONY: all windows sdl2-devel clean
