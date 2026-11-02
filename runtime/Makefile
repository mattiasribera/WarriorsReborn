# Build with zig as a dynamically linked 32-bit glibc binary so the runtime can dlopen the
# host i386 graphics libraries (SDL2, Vulkan). glibc 2.34+ has pthread and dl in libc.so.6;
# the host provides libc6:i386. The binary is non-PIE, linked at 0x60000000 so the XBE
# image range (0x10000+) stays free. i386 glibc keeps its TLS in %gs, so %fs stays free
# for the Xbox KPCR (see kpcr.c).
CC = zig cc -target x86-linux-gnu.2.34
CFLAGS = -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter -fno-omit-frame-pointer -mstackrealign
# The build's version ("[start] xbrun <version>" in the log): git describe of the nearest release
# tag without its "v" (0.1.0, 0.1.0-3-g1234abc, -dirty with local edits), or RELEASE_VERSION
# without a tag or outside a checkout. Keep RELEASE_VERSION, launcher/Makefile and
# src/thewarriors.rc in step.
RELEASE_VERSION = 0.1.2
VERSION := $(patsubst v%,%,$(shell git describe --tags --match 'v[0-9]*' --dirty 2>/dev/null || echo $(RELEASE_VERSION)))
CFLAGS += -DXBRUN_VERSION='"$(VERSION)"'
LDFLAGS = -no-pie -Wl,--image-base=0x60000000
B = build
# Per-vertex hot paths (vertex programs, vertex fetch/transform/lighting) are built at -O2 so the
# batch interpreter's lane loops are vectorized (clang's -O1 has no loop vectorizer). Same
# results: no -ffast-math, and SSE2 has no fused multiply-add; -fno-math-errno only lets sqrtf
# become the (correctly rounded) SSE instruction instead of a libm call.
HOT_SRCS = src/vsh.c src/geom.c
HOT_OBJS = $(patsubst src/%.c,$(B)/hot_%.o,$(HOT_SRCS))
SRCS = $(filter-out $(HOT_SRCS),$(wildcard src/*.c)) $(wildcard src/*.S)
# Host platform layer (src/platform.h): POSIX here, Win32 in the windows target below.
PLAT = src/platform/posix.c

# Host renderer (src/renderer/, C++ behind the C ABI in src/renderer.h). Vulkan is loaded with
# dlopen at run time, so only the headers are needed to build; libc++ is linked statically by
# zig (-lc++ with zig cc links its bundled libc++ and libc++abi as static archives).
CXX = zig c++ -target x86-linux-gnu.2.34
VULKAN_INCLUDE ?= /home/linuxbrew/.linuxbrew/include
CXXFLAGS = -std=c++17 -O1 -g -Wall -Wextra -fno-exceptions -fno-rtti -fno-omit-frame-pointer -mstackrealign \
           -Wno-missing-field-initializers -isystem $(VULKAN_INCLUDE) -I$(B)
RENDERER_OBJS = $(B)/vk_renderer.o
RENDERER_LIBS = -lc++ -ldl

all: $(B)/xbrun $(B)/xbeinfo $(B)/hle_audio_test $(B)/audio_test $(B)/audio3d_test $(B)/hle_d3d_test $(B)/yuv_test $(B)/renderer_test $(B)/geom_test $(B)/xtex_test $(B)/texpack_test $(B)/pngraw $(B)/hle_input_test $(B)/input_map_test $(B)/kfile_test $(B)/vsh_test $(B)/psh_test $(B)/kxc_test $(B)/hle_xapi_test $(B)/fspatch_test $(B)/tee_test $(B)/nvset_test $(B)/hle_spawn_test $(B)/hle_ratefix_test $(B)/hle_textfix_test $(B)/memmap_test $(B)/pacing_test $(B)/hle_sim60_test $(B)/hud_glass_test $(B)/rateprobe_test $(B)/hle_warp_test $(B)/video_pads_test $(B)/kbm_glyphs_test $(B)/bloom_test $(B)/shadow_range_test $(B)/rain_test $(B)/ssao_test

$(B)/xbrun: $(SRCS) $(PLAT) src/*.h $(HOT_OBJS) $(RENDERER_OBJS) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(PLAT) $(HOT_OBJS) $(RENDERER_OBJS) -lm $(RENDERER_LIBS)

$(B)/hot_%.o: src/%.c src/*.h | $(B)
	$(CC) $(CFLAGS) -O2 -fno-math-errno -c -o $@ $<

GLSLANG ?= /home/linuxbrew/.linuxbrew/bin/glslangValidator
$(B)/vk_renderer.o: src/renderer/vk_renderer.cpp src/renderer.h src/platform.h $(B)/geom_vert.h $(B)/geom_frag.h $(B)/glass_vert.h $(B)/glass_blur_frag.h $(B)/glass_comp_frag.h $(B)/cas_frag.h $(B)/bloom_down_frag.h $(B)/bloom_up_frag.h $(B)/bloom_comp_frag.h $(B)/rain_vert.h $(B)/rain_frag.h $(B)/ssao_ao_frag.h $(B)/ssao_ao_ms_frag.h $(B)/ssao_blur_frag.h $(B)/ssao_comp_frag.h $(B)/ssao_comp_ms_frag.h | $(B)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# GLSL -> SPIR-V as a C array (uint32_t <name>[]) included by the renderer.
$(B)/geom_vert.h: src/renderer/shaders/geom.vert | $(B)
	$(GLSLANG) -V --quiet --vn geom_vert -o $@ $<
$(B)/geom_frag.h: src/renderer/shaders/geom.frag | $(B)
	$(GLSLANG) -V --quiet --vn geom_frag -o $@ $<
$(B)/glass_vert.h: src/renderer/shaders/glass.vert | $(B)
	$(GLSLANG) -V --quiet --vn glass_vert -o $@ $<
$(B)/glass_blur_frag.h: src/renderer/shaders/glass_blur.frag | $(B)
	$(GLSLANG) -V --quiet --vn glass_blur_frag -o $@ $<
$(B)/glass_comp_frag.h: src/renderer/shaders/glass_comp.frag | $(B)
	$(GLSLANG) -V --quiet --vn glass_comp_frag -o $@ $<
$(B)/cas_frag.h: src/renderer/shaders/cas.frag | $(B)
	$(GLSLANG) -V --quiet --vn cas_frag -o $@ $<
$(B)/bloom_down_frag.h: src/renderer/shaders/bloom_down.frag | $(B)
	$(GLSLANG) -V --quiet --vn bloom_down_frag -o $@ $<
$(B)/bloom_up_frag.h: src/renderer/shaders/bloom_up.frag | $(B)
	$(GLSLANG) -V --quiet --vn bloom_up_frag -o $@ $<
$(B)/bloom_comp_frag.h: src/renderer/shaders/bloom_comp.frag | $(B)
	$(GLSLANG) -V --quiet --vn bloom_comp_frag -o $@ $<
$(B)/rain_vert.h: src/renderer/shaders/rain.vert | $(B)
	$(GLSLANG) -V --quiet --vn rain_vert -o $@ $<
$(B)/rain_frag.h: src/renderer/shaders/rain.frag | $(B)
	$(GLSLANG) -V --quiet --vn rain_frag -o $@ $<
SSAO_GLSL = src/renderer/shaders/ssao_common.glsl
$(B)/ssao_ao_frag.h: src/renderer/shaders/ssao_ao.frag $(SSAO_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn ssao_ao_frag -o $@ $<
$(B)/ssao_ao_ms_frag.h: src/renderer/shaders/ssao_ao.frag $(SSAO_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DMSAA --vn ssao_ao_ms_frag -o $@ $<
$(B)/ssao_blur_frag.h: src/renderer/shaders/ssao_blur.frag $(SSAO_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn ssao_blur_frag -o $@ $<
$(B)/ssao_comp_frag.h: src/renderer/shaders/ssao_comp.frag $(SSAO_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn ssao_comp_frag -o $@ $<
$(B)/ssao_comp_ms_frag.h: src/renderer/shaders/ssao_comp.frag $(SSAO_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DMSAA --vn ssao_comp_ms_frag -o $@ $<

# Game-independent geometry helpers test (layouts, element decoding, transforms, primitives).
$(B)/geom_test: tools/geom_test.c src/geom.c src/geom.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/geom_test.c src/geom.c -lm

# Game-independent Vulkan clear/readback test; exits 77 (skipped) without a Vulkan device.
$(B)/renderer_test: tools/renderer_test.c src/renderer.h $(RENDERER_OBJS) $(PLAT) | $(B)
	$(CC) $(CFLAGS) -o $@ tools/renderer_test.c $(RENDERER_OBJS) $(PLAT) $(RENDERER_LIBS)

# Game-independent HUD bar recognizer and glass rect math test (src/hud_glass.c).
$(B)/hud_glass_test: tools/hud_glass_test.c src/hud_glass.c src/hud_glass.h src/renderer.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/hud_glass_test.c src/hud_glass.c -lm

# Game-independent --bloom options and scene/HUD split rule test (src/bloom.c).
$(B)/bloom_test: tools/bloom_test.c src/bloom.c src/bloom.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/bloom_test.c src/bloom.c

# Game-independent --rain-hd options, rain layer rule, scroll, depth fit and slices test (src/rain.c).
$(B)/rain_test: tools/rain_test.c src/rain.c src/rain.h src/renderer.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/rain_test.c src/rain.c -lm

# Game-independent --ssao options, opaque-world split rule and projection scale test (src/ssao.c).
$(B)/ssao_test: tools/ssao_test.c src/ssao.c src/ssao.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/ssao_test.c src/ssao.c -lm

# Game-independent --shadow-range options and light projection fit test (src/shadow_range.c).
$(B)/shadow_range_test: tools/shadow_range_test.c src/shadow_range.c src/shadow_range.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/shadow_range_test.c src/shadow_range.c -lm

# Game-independent NV2A vertex program interpreter test (hand-encoded microcode).
$(B)/vsh_test: tools/vsh_test.c $(B)/hot_vsh.o src/vsh.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/vsh_test.c $(B)/hot_vsh.o -lm

# Game-independent pixel shader definition conversion test (constant resolution, offsets).
$(B)/psh_test: tools/psh_test.c src/psh.c src/psh.h src/renderer.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/psh_test.c src/psh.c -lm

$(B)/xbeinfo: tools/xbeinfo.c src/xbe.c src/xbe.h src/fspatch.c src/fspatch.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) -o $@ tools/xbeinfo.c src/xbe.c src/fspatch.c $(PLAT)

# Console + log output layer of the Windows build (src/platform/outlog.c), checked on Linux:
# every line must reach stdout/stderr and the log exactly once.
$(B)/tee_test: tools/tee_test.c src/platform/outlog.c src/platform/outlog.h | $(B)
	$(CC) $(CFLAGS) -include src/platform/outlog.h -o $@ tools/tee_test.c src/platform/outlog.c -lpthread

# Game-independent guest fs: patching test (hand-encoded instructions, fake KPCR).
$(B)/fspatch_test: tools/fspatch_test.c src/fspatch.c src/fspatch.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) -o $@ tools/fspatch_test.c src/fspatch.c $(PLAT)

# Game-independent HLE audio test (maps the audio library range, needs no game data).
AUDIO_SRCS = src/audio.c src/xact.c $(PLAT)
AUDIO_HDRS = src/audio.h src/xact.h src/platform.h
$(B)/hle_audio_test: tools/hle_audio_test.c src/hle_audio.c src/hle_thunk.S src/hle_audio.h $(AUDIO_SRCS) $(AUDIO_HDRS) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tools/hle_audio_test.c src/hle_audio.c src/hle_thunk.S $(AUDIO_SRCS) -lm

# Game-independent XACT 3D positioning test (source in tests/: distance/pan math and the HLE
# listener/source setters, mixed without the mixer thread).
$(B)/audio3d_test: ../tests/audio3d_test.c tools/audio_synth.h src/hle_audio.c src/hle_thunk.S src/hle_audio.h $(AUDIO_SRCS) $(AUDIO_HDRS) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ ../tests/audio3d_test.c src/hle_audio.c src/hle_thunk.S $(AUDIO_SRCS) -lm

# Game-independent audio test: Xbox ADPCM decoding, synthetic wave/sound banks, mixer, WAV dump.
$(B)/audio_test: tools/audio_test.c $(AUDIO_SRCS) $(AUDIO_HDRS) | $(B)
	$(CC) $(CFLAGS) -o $@ tools/audio_test.c $(AUDIO_SRCS) -lm

# Game-independent HLE input test: src/hle_input.c built with its own test main (maps the XPP range).
$(B)/hle_input_test: src/hle_input.c src/hle_input.h src/hle_audio.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -DHLE_INPUT_TEST -o $@ src/hle_input.c $(PLAT) -lm

# Game-independent SDL controller -> port test: src/video.c (included by the test) on SDL's dummy
# video driver with SDL virtual game controllers; prints SKIP when SDL2 cannot provide them.
$(B)/video_pads_test: tools/video_pads_test.c src/video.c src/video.h src/input_map.c src/input_map.h src/hle_input.c src/hle_input.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tools/video_pads_test.c src/input_map.c src/hle_input.c $(PLAT) -lm

# Game-independent host input layer test: src/input_map.c built with its own test main (key
# state, auto-repeat filtering, bindings) over src/hle_input.c's pad state.
$(B)/input_map_test: src/input_map.c src/input_map.h src/hle_input.c src/hle_input.h src/hle_audio.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -DINPUT_MAP_TEST -o $@ src/input_map.c src/hle_input.c $(PLAT) -lm

# Game-independent kernel file I/O test: src/kfile.c built with its own test main and kernel
# stubs, over a temporary host directory (directory enumeration, path mapping).
$(B)/kfile_test: src/kfile.c src/files.c src/files.h src/kernel.h src/kobj.h src/hle_textfix.c src/hle_textfix.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) -DKFILE_TEST -o $@ src/kfile.c src/files.c src/hle_textfix.c $(PLAT)

# Game-independent kernel crypto test: src/kxc.c built with its own test main (SHA-1 and
# HMAC-SHA1 against the RFC 3174 / RFC 2202 vectors).
$(B)/kxc_test: src/kxc.c src/kxc.h | $(B)
	$(CC) $(CFLAGS) -DKXC_TEST -o $@ src/kxc.c

# Game-independent guest memory placement test (no guest pages inside other code's reservations).
$(B)/memmap_test: tools/memmap_test.c src/platform.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) -o $@ tools/memmap_test.c $(PLAT)

# Game-independent XAPI timing test: maps the XAPI range, plants QueryPerformanceCounter's
# entry bytes, installs the patch and checks the counter rate (Xbox CPU clock, 733333333 Hz).
$(B)/hle_xapi_test: tools/hle_xapi_test.c src/hle_xapi.c src/hle_xapi.h src/pacing.c src/pacing.h | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tools/hle_xapi_test.c src/hle_xapi.c src/pacing.c

# Game-independent rate-fix test: a host buffer stands in for guest memory (signature match,
# mismatch left untouched, --no-rate-fixes, every title table entry on a planted copy).
# Game-independent frame pacing test (cadence statistics, frame-locked clock with a fake host clock).
$(B)/pacing_test: tools/pacing_test.c src/pacing.c src/pacing.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/pacing_test.c src/pacing.c

$(B)/hle_ratefix_test: tools/hle_ratefix_test.c src/hle_ratefix.c src/hle_ratefix.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/hle_ratefix_test.c src/hle_ratefix.c

# Game-independent --sim60 patch test (whole-set application, mismatch writes nothing, every
# title table entry on a planted image).
$(B)/hle_sim60_test: tools/hle_sim60_test.c src/hle_sim60.c src/hle_sim60.h | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tools/hle_sim60_test.c src/hle_sim60.c

# Game-independent rate probe test (options, game-time schedule, snapshot file round trip).
$(B)/rateprobe_test: tools/rateprobe_test.c src/rateprobe.c src/rateprobe.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/rateprobe_test.c src/rateprobe.c

# Game-independent --warp test (parsing, mission table, signature check, both stubs executed in
# host executable memory against fake title functions).
$(B)/hle_warp_test: tools/hle_warp_test.c src/hle_warp.c src/hle_warp.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tools/hle_warp_test.c src/hle_warp.c $(PLAT)

# Text-fix test: game-independent; with ../extracted/disc/Main.wad0 as argument also the real file.
$(B)/hle_textfix_test: tools/hle_textfix_test.c src/hle_textfix.c src/hle_textfix.h src/kbm_glyphs.c src/kbm_glyphs.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/hle_textfix_test.c src/hle_textfix.c src/kbm_glyphs.c -lm

# Game-independent spawn scaling test: src/hle_spawn.c with its own test main (scaling and
# clamping, option parsing, the patched stores run on planted copies at their guest addresses).
$(B)/hle_spawn_test: src/hle_spawn.c src/hle_spawn.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -DHLE_SPAWN_TEST -o $@ src/hle_spawn.c $(PLAT)

# Game-independent HLE D3D test (maps the D3D8 library range, null video backend).
HLE_D3D_SRCS = tools/hle_d3d_test.c src/hle_d3d.c src/hle_d3d_thunk.S src/video.c src/hle_input.c src/input_map.c src/mem.c src/yuv.c src/geom.c src/xtex.c src/texpack.c src/texpack_stb.c src/vsh.c src/psh.c src/hle_spawn.c src/hle_sim60.c src/pacing.c src/hle_xapi.c src/hud_glass.c src/rateprobe.c src/kbm_glyphs.c src/bloom.c src/shadow_range.c src/rain.c src/ssao.c $(PLAT)
$(B)/hle_d3d_test: $(HLE_D3D_SRCS) src/hle_d3d.h src/hle_spawn.h src/hle_input.h src/input_map.h src/video.h src/mem.h src/yuv.h src/geom.h src/xtex.h src/texpack.h src/vsh.h src/psh.h src/renderer.h src/kbm_glyphs.h $(RENDERER_OBJS) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(HLE_D3D_SRCS) $(RENDERER_OBJS) -lm $(RENDERER_LIBS)

# Game-independent YUV quad helpers test (unswizzle, shader coefficients, scaled quad).
$(B)/yuv_test: tools/yuv_test.c src/yuv.c src/yuv.h src/xtex.c src/xtex.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/yuv_test.c src/yuv.c src/xtex.c -lm

# Game-independent non-volatile settings test (language, regions, the widescreen video flag).
$(B)/nvset_test: tools/nvset_test.c src/nvset.c src/nvset.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/nvset_test.c src/nvset.c

# Game-independent Xbox texture helpers test (formats, unswizzle, DXT decoding, cache key).
$(B)/xtex_test: tools/xtex_test.c src/xtex.c src/xtex.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/xtex_test.c src/xtex.c

# Game-independent texture dump / replacement pack test (hash, PNG, mips, pack index, manifest).
TEXPACK_SRCS = src/texpack.c src/texpack_stb.c src/xtex.c
$(B)/texpack_test: tools/texpack_test.c $(TEXPACK_SRCS) src/texpack.h src/xtex.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/texpack_test.c $(TEXPACK_SRCS) -lm

# Game-independent keyboard/mouse button icon test (rect table, labels, atlas painter).
$(B)/kbm_glyphs_test: tools/kbm_glyphs_test.c src/kbm_glyphs.c src/kbm_glyphs.h src/input_map.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/kbm_glyphs_test.c src/kbm_glyphs.c -lm

# PNG -> raw RGBA8 decoder used by tools/texture_upscale.py (host tool, no game data).
$(B)/pngraw: tools/pngraw.c $(TEXPACK_SRCS) src/texpack.h src/xtex.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/pngraw.c $(TEXPACK_SRCS) -lm

$(B):
	mkdir -p $(B)

# Game-derived symbol file (not committed): make symbols XBE=../extracted/default.xbe
XBSYMDB ?= $(HOME)/tools/ghidra_12.1.4_PUBLIC/Ghidra/Extensions/ghidra-xbe/os/linux_x86_64/XbSymbolDatabaseCLI
XBE ?= ../extracted/default.xbe
symbols: | $(B)
	$(XBSYMDB) $(XBE) > $(B)/default.syms

# ---- Windows (cross-compiled; 32-bit, runs under WOW64 on 64-bit Windows) ----
# TheWarriors.exe: CRT-free loader fixed at 0x10000 whose image reserves the XBE range, then
# loads xbrun-core.dll (the whole runtime, src/platform/win32.c instead of posix.c). See README.
# (The Linux binary keeps the name xbrun; earlier Windows builds called the loader xbrun.exe.)
WCC = zig cc -target x86-windows-gnu
WCXX = zig c++ -target x86-windows-gnu
WCFLAGS = -std=gnu99 -O1 -Wall -Wextra -Wno-unused-parameter -fno-omit-frame-pointer -mstackrealign \
          -include src/platform/outlog.h -DXBRUN_VERSION='"$(VERSION)"'
WCXXFLAGS = -std=c++17 -O1 -Wall -Wextra -fno-exceptions -fno-rtti -fno-omit-frame-pointer -mstackrealign \
            -Wno-missing-field-initializers -isystem $(VULKAN_INCLUDE) -I$(B)
WB = $(B)/win

windows: $(B)/TheWarriors.exe $(B)/xbrun-core.dll $(B)/fspatch_test.exe $(B)/memmap_test.exe

$(WB):
	mkdir -p $(WB)

$(WB)/vk_renderer.o: src/renderer/vk_renderer.cpp src/renderer.h src/platform.h $(B)/geom_vert.h $(B)/geom_frag.h $(B)/glass_vert.h $(B)/glass_blur_frag.h $(B)/glass_comp_frag.h $(B)/cas_frag.h $(B)/bloom_down_frag.h $(B)/bloom_up_frag.h $(B)/bloom_comp_frag.h $(B)/rain_vert.h $(B)/rain_frag.h $(B)/ssao_ao_frag.h $(B)/ssao_ao_ms_frag.h $(B)/ssao_blur_frag.h $(B)/ssao_comp_frag.h $(B)/ssao_comp_ms_frag.h | $(WB)
	$(WCXX) $(WCXXFLAGS) -c -o $@ $<

# The per-vertex hot paths at -O2, as in the Linux build (HOT_SRCS above).
WHOT_OBJS = $(patsubst src/%.c,$(WB)/hot_%.o,$(HOT_SRCS))
$(WB)/hot_%.o: src/%.c src/*.h src/platform/outlog.h | $(WB)
	$(WCC) $(WCFLAGS) -O2 -fno-math-errno -c -o $@ $<

# main() becomes xbrun_main, called by xbrun_dll_main (win32.c) with the command line.
WPLAT = src/platform/win32.c src/platform/outlog.c
$(B)/xbrun-core.dll: $(SRCS) $(WPLAT) src/platform/win32_entry.c src/platform/outlog.h src/*.h $(WHOT_OBJS) $(WB)/vk_renderer.o | $(WB)
	$(WCC) $(WCFLAGS) -Dmain=xbrun_main -shared -Wl,--image-base=0x60000000 -Wl,--no-dynamicbase -o $@ \
	    $(SRCS) $(WPLAT) src/platform/win32_entry.c $(WHOT_OBJS) $(WB)/vk_renderer.o -lc++ -lshell32

# Game-independent checks to run on Windows (no Wine here): fs patching through the TEB slot.
# Both .exe files embed an asInvoker manifest: without one, UAC installer detection runs
# fspatch_test.exe elevated (its name contains "patch") in a console of its own.
WMANIFEST = src/platform/asinvoker.rc src/platform/asinvoker.manifest
$(B)/fspatch_test.exe: tools/fspatch_test.c src/fspatch.c src/fspatch.h $(WPLAT) src/platform/outlog.h $(WMANIFEST) | $(WB)
	$(WCC) $(WCFLAGS) -o $@ tools/fspatch_test.c src/fspatch.c $(WPLAT) src/platform/asinvoker.rc

$(B)/memmap_test.exe: tools/memmap_test.c src/platform.h $(WPLAT) src/platform/outlog.h $(WMANIFEST) | $(WB)
	$(WCC) $(WCFLAGS) -o $@ tools/memmap_test.c $(WPLAT) src/platform/asinvoker.rc

# Linked under a temporary name and renamed once pe_flags.py has set the header flags, so a
# failed flag step leaves no TheWarriors.exe that make would later take as up to date. Its
# resources: the same manifest, the icon (tools/icon/make_icon.py) and the version info.
WEXE_RC = src/thewarriors.rc src/platform/asinvoker.manifest ../assets/icon/wonder-wheel.ico
$(B)/TheWarriors.exe: src/platform/win32_loader.c tools/pe_flags.py $(WEXE_RC) | $(WB)
	$(WCC) -O1 -Wall -Wextra -c -o $(WB)/win32_loader.o src/platform/win32_loader.c
	$(WCC) -nostdlib -Wl,--image-base=0x10000 -Wl,--no-dynamicbase -Wl,--entry=xbrun_loader_start@0 \
	    -o $(WB)/TheWarriors-unflagged.exe $(WB)/win32_loader.o src/thewarriors.rc -lkernel32
	python3 tools/pe_flags.py $(WB)/TheWarriors-unflagged.exe
	mv -f $(WB)/TheWarriors-unflagged.exe $@

clean:
	rm -rf $(B)

.PHONY: all clean symbols windows
