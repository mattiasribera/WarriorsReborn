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
RELEASE_VERSION = 0.1.9
VERSION := $(patsubst v%,%,$(shell git describe --tags --match 'v[0-9]*' --dirty 2>/dev/null || echo $(RELEASE_VERSION)))
CFLAGS += -DXBRUN_VERSION='"$(VERSION)"'
# Steam Audio's public header (src/steam_audio.c; the library itself is loaded at run time)
SA_INCLUDE = -Ithird_party/steam-audio/include
CFLAGS += $(SA_INCLUDE)
LDFLAGS = -no-pie -Wl,--image-base=0x60000000
B = build
# Per-vertex hot paths (vertex programs, vertex fetch/transform/lighting, the GPU-skinning weld hash), the mixer's resampler and reverb,
# and the renderer call stream (render_wire.c, render_rec.c: per-call encoding and content hashing)
# are built at -O2 so their lane loops are vectorized (clang's -O1 has no loop vectorizer). Same
# results: no -ffast-math, and SSE2 has no fused multiply-add; -fno-math-errno only lets sqrtf
# become the (correctly rounded) SSE instruction instead of a libm call.
HOT_SRCS = src/vsh.c src/geom.c src/resample.c src/reverb.c src/gpu_skin.c src/tri_check.c src/world_light.c src/render_wire.c src/render_rec.c
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

# KTX2 (Basis Universal) texture-pack files: src/ktx2.cpp over the transcoder vendored unmodified in
# third_party/basisu (Apache-2.0; zstd decoder BSD-3-Clause). The vendored code is built with its own
# flags and without warnings (-w): they are upstream's, not ours. Only xbrun / xbrun-core.dll and
# ktx2_test link it (main.c registers ktx2_transcode with texpack_set_ktx2).
BASISU = third_party/basisu
BASISU_DEFS = -DBASISD_SUPPORT_KTX2=1 -DBASISD_SUPPORT_KTX2_ZSTD=1 -DBASISD_SUPPORT_PVRTC1=0 -DBASISD_SUPPORT_PVRTC2=0 \
              -DBASISD_SUPPORT_ATC=0 -DBASISD_SUPPORT_FXT1=0
BASISU_CXXFLAGS = -std=c++17 -O2 -g1 -w -fno-strict-aliasing -fno-exceptions -fno-rtti -mstackrealign $(BASISU_DEFS)
KTX2_OBJS = $(B)/ktx2.o $(B)/basisu_transcoder.o $(B)/zstddeclib.o

all: $(B)/xbrun $(B)/xbeinfo $(B)/hle_audio_test $(B)/audio_test $(B)/audio3d_test $(B)/hle_d3d_test $(B)/yuv_test $(B)/renderer_test $(B)/geom_test $(B)/xtex_test $(B)/texpack_test $(B)/builtin_tex_test $(B)/pngraw $(B)/hle_input_test $(B)/input_map_test $(B)/kfile_test $(B)/vsh_test $(B)/psh_test $(B)/kxc_test $(B)/hle_xapi_test $(B)/fspatch_test $(B)/tee_test $(B)/nvset_test $(B)/hle_spawn_test $(B)/hle_fx_test $(B)/hle_gore_test $(B)/hle_ratefix_test $(B)/hle_skipintro_test $(B)/hle_drawdist_test $(B)/hle_textfix_test $(B)/hle_scriptfix_test $(B)/memmap_test $(B)/pacing_test $(B)/hle_sim60_test $(B)/hud_glass_test $(B)/rateprobe_test $(B)/hle_warp_test $(B)/hle_goto_test $(B)/hle_mods_test $(B)/hle_aidbg_test $(B)/hle_allies_test $(B)/hle_tactics_test $(B)/video_pads_test $(B)/kbm_glyphs_test $(B)/bloom_test $(B)/shadow_range_test $(B)/rain_test $(B)/ssao_test $(B)/meshpack_test $(B)/soft_particles_test $(B)/xact_extract $(B)/xact_extract_test $(B)/sndpack_test $(B)/movie_hd_test $(B)/resample_test $(B)/vp_pool_test $(B)/page_bitmap_test $(B)/hle_sim60_rates_test $(B)/reverb_test $(B)/audio_lead_test $(B)/filmic_test $(B)/auto_exposure_test $(B)/dof_test $(B)/sky_test $(B)/lamp_lights_test $(B)/level_lights_test $(B)/light_shafts_test $(B)/wet_streaks_test $(B)/materials_test $(B)/damp_test $(B)/neon_refl_test $(B)/char_light_test $(B)/hair_test $(B)/world_light_test $(B)/fire_light_test $(B)/lamp_shadow_test $(B)/world_shadow_test $(B)/gpu_skin_test $(B)/sharp_font_test $(B)/sharp_faces_test $(B)/neon_glow_test $(B)/kobj_test $(B)/vertex_sink_test $(B)/fps_overlay_test $(B)/pipe_cache_test $(B)/texload_test $(B)/vram_budget_test $(B)/perf_log_test $(B)/refresh_match_test $(B)/load_prof_test $(B)/mem_test $(B)/tri_check_test $(B)/diaglog_test $(B)/tex_race_test $(B)/gamma_ramp_test $(B)/normal_maps_test $(B)/fsr_test $(B)/cinema_bars_test $(B)/taa_test $(B)/framegen_test $(B)/combat_debug_test $(B)/anim_interp_test $(B)/hats_test $(B)/ktx2_test $(B)/volfog_test $(B)/sssr_test $(B)/steam_audio_test $(B)/hle_memreg_test $(B)/dynres_test $(B)/bench_test $(B)/colpatch_test $(B)/texlayer_test $(B)/hle_scars_test $(B)/hle_radar_test $(B)/hle_huspeed_test $(B)/hle_luagc_test $(B)/hle_diffmenu_test $(B)/vk_caps_test $(B)/disc_test $(B)/modassets_test $(B)/modsound_test $(B)/gamectl_test $(B)/modmenu_test $(B)/hle_modmenu_test $(B)/settings_model_test $(B)/settings_sync_test $(B)/settings_apply_test $(B)/settings_menu_test $(B)/hdr_test $(B)/photo_test

$(B)/xbrun: $(SRCS) $(PLAT) src/*.h src/renderer/vrs.h src/renderer/vk_caps.h src/fps_font.inc $(HOT_OBJS) $(RENDERER_OBJS) $(KTX2_OBJS) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(PLAT) $(HOT_OBJS) $(RENDERER_OBJS) $(KTX2_OBJS) -lm $(RENDERER_LIBS)

$(B)/ktx2.o: src/ktx2.cpp src/ktx2.h src/texpack.h $(BASISU)/transcoder/basisu_transcoder.h | $(B)
	$(CXX) $(CXXFLAGS) $(BASISU_DEFS) -c -o $@ src/ktx2.cpp
$(B)/basisu_transcoder.o: $(BASISU)/transcoder/*.cpp $(BASISU)/transcoder/*.h $(BASISU)/transcoder/*.inl $(BASISU)/transcoder/*.inc | $(B)
	$(CXX) $(BASISU_CXXFLAGS) -c -o $@ $(BASISU)/transcoder/basisu_transcoder.cpp
$(B)/zstddeclib.o: $(BASISU)/zstd/zstddeclib.c | $(B)
	$(CC) -O2 -g1 -w -mstackrealign -c -o $@ $<

$(B)/hot_%.o: src/%.c src/*.h | $(B)
	$(CC) $(CFLAGS) -O2 -fno-math-errno -c -o $@ $<

GLSLANG ?= /home/linuxbrew/.linuxbrew/bin/glslangValidator
$(B)/vk_renderer.o: src/renderer/vk_renderer.cpp src/renderer.h src/gamma_ramp.h $(B)/gamma_frag.h $(B)/output_frag.h $(B)/output_hdr_frag.h src/hdr.h src/renderer/vertex_sink.h src/ssao.h src/platform.h $(B)/geom_vert.h $(B)/geom_frag.h $(B)/glass_vert.h $(B)/glass_blur_frag.h $(B)/glass_comp_frag.h $(B)/cas_frag.h $(B)/fsr_easu_frag.h $(B)/fsr_rcas_frag.h src/fsr.h $(B)/bloom_down_frag.h $(B)/bloom_up_frag.h $(B)/bloom_comp_frag.h $(B)/filmic_frag.h $(B)/filmic_ae_frag.h $(B)/exposure_frag.h $(B)/ae_meter_frag.h $(B)/ae_adapt_frag.h $(B)/dof_focus_frag.h $(B)/dof_focus_ms_frag.h $(B)/dof_down_frag.h $(B)/dof_down_ms_frag.h $(B)/dof_gather_frag.h $(B)/dof_comp_frag.h $(B)/dof_comp_ms_frag.h $(B)/dof_copy_frag.h $(B)/rain_vert.h $(B)/rain_frag.h $(B)/ssao_ao_frag.h $(B)/ssao_ao_ms_frag.h $(B)/ssao_blur_frag.h $(B)/ssao_comp_frag.h $(B)/ssao_comp_ms_frag.h $(B)/ssao_blur_mask_frag.h $(B)/ssao_comp_ms_mask_frag.h $(B)/gtao_prefilter_comp.h $(B)/gtao_prefilter_ms_comp.h $(B)/gtao_main_comp.h $(B)/gtao_denoise_comp.h $(B)/gtao_denoise_mask_comp.h $(B)/gtao_temporal_comp.h $(B)/wet_frag.h $(B)/wet_ms_frag.h $(B)/sssr_hiz_frag.h $(B)/sssr_hiz_ms_frag.h $(B)/sssr_hiz_down_frag.h $(B)/sssr_surf_vert.h $(B)/sssr_surf_frag.h $(B)/rain_cover_vert.h $(B)/world_shadow_cut_vert.h $(B)/world_shadow_cut_frag.h $(B)/world_shadow_gate_comp.h $(B)/rain_cover_frag.h $(B)/splash_cover_frag.h $(B)/splash_cover_ms_frag.h $(B)/wet_cover_frag.h $(B)/wet_cover_ms_frag.h $(B)/char_mask_vert.h $(B)/splash_frag.h $(B)/splash_ms_frag.h $(B)/geom_soft_frag.h $(B)/geom_soft_ms_frag.h $(B)/geom_lit_vert.h $(B)/geom_lit_frag.h $(B)/geom_emit_frag.h $(B)/geom_lit_emit_frag.h $(B)/geom_lit_relief_frag.h $(B)/geom_lit_hair_frag.h $(B)/geom_lit_relief_hair_frag.h $(B)/geom_lit_skin_frag.h $(B)/geom_lit_relief_skin_frag.h $(B)/geom_lit_hair_skin_frag.h $(B)/geom_lit_relief_hair_skin_frag.h $(B)/geom_lit_mat_frag.h $(B)/geom_lit_relief_mat_frag.h $(B)/geom_lit_hair_mat_frag.h $(B)/geom_lit_relief_hair_mat_frag.h $(B)/vsh_skin_comp.h $(B)/skin_faces_comp.h $(B)/skin_normals_comp.h $(B)/sky_vert.h $(B)/sky_frag.h $(B)/skyline_frag.h src/lamp_lights.h src/volfog.h $(B)/lamp_frag.h $(B)/lamp_ms_frag.h $(B)/shafts_frag.h $(B)/shafts_ms_frag.h $(B)/volfog_polar_comp.h $(B)/volfog_inject_comp.h $(B)/volfog_integrate_comp.h $(B)/volfog_comp_frag.h $(B)/volfog_comp_ms_frag.h $(B)/contact_frag.h $(B)/contact_ms_frag.h $(B)/fps_overlay_frag.h src/fps_overlay.h $(B)/taa_resolve_frag.h $(B)/taa_resolve_ms_frag.h $(B)/taa_out_frag.h $(B)/taa_up_frag.h $(B)/taa_up_ms_frag.h $(B)/taa_out_up_frag.h $(B)/taa_vel_vert.h $(B)/taa_vel_frag.h $(B)/fg_mv_comp.h $(B)/fg_mv_ms_comp.h $(B)/fg_splat_comp.h $(B)/fg_interp_frag.h $(B)/fg_flow_comp.h $(B)/fg_hud_comp.h $(B)/fsr3_prepare_comp.h $(B)/fsr3_prepare_ms_comp.h $(B)/fsr3_pyramid_comp.h $(B)/fsr3_shading_comp.h $(B)/fsr3_reactivity_comp.h $(B)/fsr3_instability_comp.h $(B)/fsr3_accumulate_comp.h $(B)/fsr3_accumulate_ms_comp.h $(B)/fsr3_mask_frag.h $(B)/fsr3_mask_ms_frag.h src/renderer/vrs.h $(B)/vrs_rate_comp.h $(B)/taa_up_lite_frag.h src/renderer/vk_caps.h src/renderer/tex_lru.h | $(B)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# GLSL -> SPIR-V as a C array (uint32_t <name>[]) included by the renderer.
$(B)/geom_vert.h: src/renderer/shaders/geom.vert | $(B)
	$(GLSLANG) -V --quiet --vn geom_vert -o $@ $<
$(B)/geom_frag.h: src/renderer/shaders/geom.frag src/renderer/shaders/light_walls.glsl | $(B)
	$(GLSLANG) -V --quiet --vn geom_frag -o $@ $<
CHAR_LIT_GLSL = src/renderer/shaders/char_lit.glsl src/renderer/shaders/world_lit.glsl
$(B)/geom_lit_vert.h: src/renderer/shaders/geom.vert $(CHAR_LIT_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DCHAR_LIT --vn geom_lit_vert -o $@ $<
$(B)/geom_lit_frag.h: src/renderer/shaders/geom.frag src/renderer/shaders/light_walls.glsl $(CHAR_LIT_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DCHAR_LIT --vn geom_lit_frag -o $@ $<
$(B)/geom_emit_frag.h: src/renderer/shaders/geom.frag src/renderer/shaders/light_walls.glsl | $(B)
	$(GLSLANG) -V --quiet -DEMIT --vn geom_emit_frag -o $@ $<
$(B)/geom_lit_emit_frag.h: src/renderer/shaders/geom.frag src/renderer/shaders/light_walls.glsl $(CHAR_LIT_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DCHAR_LIT -DEMIT --vn geom_lit_emit_frag -o $@ $<
$(B)/geom_lit_relief_frag.h: src/renderer/shaders/geom.frag src/renderer/shaders/light_walls.glsl $(CHAR_LIT_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DCHAR_LIT -DRELIEF --vn geom_lit_relief_frag -o $@ $<
$(B)/geom_lit_hair_frag.h: src/renderer/shaders/geom.frag src/renderer/shaders/light_walls.glsl src/renderer/shaders/hair.glsl $(CHAR_LIT_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DCHAR_LIT -DHAIR --vn geom_lit_hair_frag -o $@ $<
$(B)/geom_lit_relief_hair_frag.h: src/renderer/shaders/geom.frag src/renderer/shaders/light_walls.glsl src/renderer/shaders/hair.glsl $(CHAR_LIT_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DCHAR_LIT -DRELIEF -DHAIR --vn geom_lit_relief_hair_frag -o $@ $<
SKIN_GLSL = src/renderer/shaders/geom.frag src/renderer/shaders/light_walls.glsl src/renderer/shaders/hair.glsl src/renderer/shaders/skin.glsl $(CHAR_LIT_GLSL)
$(B)/geom_lit_skin_frag.h: $(SKIN_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DCHAR_LIT -DSKIN --vn geom_lit_skin_frag -o $@ $<
$(B)/geom_lit_relief_skin_frag.h: $(SKIN_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DCHAR_LIT -DRELIEF -DSKIN --vn geom_lit_relief_skin_frag -o $@ $<
$(B)/geom_lit_hair_skin_frag.h: $(SKIN_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DCHAR_LIT -DHAIR -DSKIN --vn geom_lit_hair_skin_frag -o $@ $<
$(B)/geom_lit_relief_hair_skin_frag.h: $(SKIN_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DCHAR_LIT -DRELIEF -DHAIR -DSKIN --vn geom_lit_relief_hair_skin_frag -o $@ $<
MAT_GLSL = $(SKIN_GLSL) src/renderer/shaders/mat.glsl
$(B)/geom_lit_mat_frag.h: $(MAT_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DCHAR_LIT -DSKIN -DMAT --vn geom_lit_mat_frag -o $@ $<
$(B)/geom_lit_relief_mat_frag.h: $(MAT_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DCHAR_LIT -DRELIEF -DSKIN -DMAT --vn geom_lit_relief_mat_frag -o $@ $<
$(B)/geom_lit_hair_mat_frag.h: $(MAT_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DCHAR_LIT -DHAIR -DSKIN -DMAT --vn geom_lit_hair_mat_frag -o $@ $<
$(B)/geom_lit_relief_hair_mat_frag.h: $(MAT_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DCHAR_LIT -DRELIEF -DHAIR -DSKIN -DMAT --vn geom_lit_relief_hair_mat_frag -o $@ $<
$(B)/geom_soft_frag.h: src/renderer/shaders/geom.frag src/renderer/shaders/light_walls.glsl | $(B)
	$(GLSLANG) -V --quiet -DSOFT --vn geom_soft_frag -o $@ $<
$(B)/geom_soft_ms_frag.h: src/renderer/shaders/geom.frag src/renderer/shaders/light_walls.glsl | $(B)
	$(GLSLANG) -V --quiet -DSOFT -DMSAA --vn geom_soft_ms_frag -o $@ $<
$(B)/glass_vert.h: src/renderer/shaders/glass.vert | $(B)
	$(GLSLANG) -V --quiet --vn glass_vert -o $@ $<
$(B)/glass_blur_frag.h: src/renderer/shaders/glass_blur.frag | $(B)
	$(GLSLANG) -V --quiet --vn glass_blur_frag -o $@ $<
$(B)/glass_comp_frag.h: src/renderer/shaders/glass_comp.frag | $(B)
	$(GLSLANG) -V --quiet --vn glass_comp_frag -o $@ $<
$(B)/cas_frag.h: src/renderer/shaders/cas.frag | $(B)
	$(GLSLANG) -V --quiet --vn cas_frag -o $@ $<
# --fsr: AMD FidelityFX FSR 1 EASU upscale and RCAS sharpening (32-bit path of ffx_fsr1.h).
$(B)/fsr_easu_frag.h: src/renderer/shaders/fsr_easu.frag | $(B)
	$(GLSLANG) -V --quiet --vn fsr_easu_frag -o $@ $<
$(B)/fsr_rcas_frag.h: src/renderer/shaders/fsr_rcas.frag | $(B)
	$(GLSLANG) -V --quiet --vn fsr_rcas_frag -o $@ $<
$(B)/fps_overlay_frag.h: src/renderer/shaders/fps_overlay.frag src/fps_font.inc | $(B)
	$(GLSLANG) -V --quiet --vn fps_overlay_frag -o $@ $<
$(B)/bloom_down_frag.h: src/renderer/shaders/bloom_down.frag | $(B)
	$(GLSLANG) -V --quiet --vn bloom_down_frag -o $@ $<
$(B)/bloom_up_frag.h: src/renderer/shaders/bloom_up.frag | $(B)
	$(GLSLANG) -V --quiet --vn bloom_up_frag -o $@ $<
$(B)/bloom_comp_frag.h: src/renderer/shaders/bloom_comp.frag | $(B)
	$(GLSLANG) -V --quiet --vn bloom_comp_frag -o $@ $<
$(B)/filmic_frag.h: src/renderer/shaders/filmic.frag | $(B)
	$(GLSLANG) -V --quiet --vn filmic_frag -o $@ $<
AE_GLSL = src/renderer/shaders/ae_common.glsl
$(B)/filmic_ae_frag.h: src/renderer/shaders/filmic.frag $(AE_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DAUTO_EXPOSURE --vn filmic_ae_frag -o $@ $<
$(B)/exposure_frag.h: src/renderer/shaders/exposure.frag $(AE_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn exposure_frag -o $@ $<
$(B)/ae_meter_frag.h: src/renderer/shaders/ae_meter.frag | $(B)
	$(GLSLANG) -V --quiet --vn ae_meter_frag -o $@ $<
$(B)/ae_adapt_frag.h: src/renderer/shaders/ae_adapt.frag | $(B)
	$(GLSLANG) -V --quiet --vn ae_adapt_frag -o $@ $<
# --cutscene-dof: focus, prefilter, gather, composite (MSAA variants read the multisampled depth) and copy.
DOF_GLSL = src/renderer/shaders/dof_common.glsl
$(B)/dof_focus_frag.h: src/renderer/shaders/dof_focus.frag $(DOF_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn dof_focus_frag -o $@ $<
$(B)/dof_focus_ms_frag.h: src/renderer/shaders/dof_focus.frag $(DOF_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DMSAA --vn dof_focus_ms_frag -o $@ $<
$(B)/dof_down_frag.h: src/renderer/shaders/dof_down.frag $(DOF_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn dof_down_frag -o $@ $<
$(B)/dof_down_ms_frag.h: src/renderer/shaders/dof_down.frag $(DOF_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DMSAA --vn dof_down_ms_frag -o $@ $<
$(B)/dof_gather_frag.h: src/renderer/shaders/dof_gather.frag $(DOF_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn dof_gather_frag -o $@ $<
$(B)/dof_comp_frag.h: src/renderer/shaders/dof_comp.frag $(DOF_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn dof_comp_frag -o $@ $<
$(B)/dof_comp_ms_frag.h: src/renderer/shaders/dof_comp.frag $(DOF_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DMSAA --vn dof_comp_ms_frag -o $@ $<
$(B)/dof_copy_frag.h: src/renderer/shaders/dof_copy.frag $(DOF_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn dof_copy_frag -o $@ $<
# --taa: the resolve (MSAA variant: the multisampled depth) and the output pass.
$(B)/taa_resolve_frag.h: src/renderer/shaders/taa_resolve.frag | $(B)
	$(GLSLANG) -V --quiet --vn taa_resolve_frag -o $@ $<
$(B)/taa_resolve_ms_frag.h: src/renderer/shaders/taa_resolve.frag | $(B)
	$(GLSLANG) -V --quiet -DMSAA --vn taa_resolve_ms_frag -o $@ $<
$(B)/taa_out_frag.h: src/renderer/shaders/taa_out.frag | $(B)
	$(GLSLANG) -V --quiet --vn taa_out_frag -o $@ $<
# --taa-upscale: the upscaling resolve (MSAA variant) and the output pass that also writes the depth.
$(B)/taa_up_frag.h: src/renderer/shaders/taa_up.frag | $(B)
	$(GLSLANG) -V --quiet --vn taa_up_frag -o $@ $<
$(B)/taa_up_lite_frag.h: src/renderer/shaders/taa_up.frag | $(B)
	$(GLSLANG) -V --quiet -DLITE --vn taa_up_lite_frag -o $@ $<
$(B)/taa_up_ms_frag.h: src/renderer/shaders/taa_up.frag | $(B)
	$(GLSLANG) -V --quiet -DMSAA --vn taa_up_ms_frag -o $@ $<
$(B)/taa_out_up_frag.h: src/renderer/shaders/taa_out.frag | $(B)
	$(GLSLANG) -V --quiet -DUPSCALE --vn taa_out_up_frag -o $@ $<
$(B)/taa_vel_vert.h: src/renderer/shaders/taa_vel.vert | $(B)
	$(GLSLANG) -V --quiet --vn taa_vel_vert -o $@ $<
$(B)/taa_vel_frag.h: src/renderer/shaders/taa_vel.frag | $(B)
	$(GLSLANG) -V --quiet --vn taa_vel_frag -o $@ $<
$(B)/fg_mv_comp.h: src/renderer/shaders/fg_mv.comp | $(B)
	$(GLSLANG) -V --quiet --vn fg_mv_comp -o $@ $<
$(B)/fg_mv_ms_comp.h: src/renderer/shaders/fg_mv.comp | $(B)
	$(GLSLANG) -V --quiet -DMSAA --vn fg_mv_ms_comp -o $@ $<
$(B)/fg_splat_comp.h: src/renderer/shaders/fg_splat.comp | $(B)
	$(GLSLANG) -V --quiet --vn fg_splat_comp -o $@ $<
$(B)/fg_interp_frag.h: src/renderer/shaders/fg_interp.frag | $(B)
	$(GLSLANG) -V --quiet --vn fg_interp_frag -o $@ $<
$(B)/fg_flow_comp.h: src/renderer/shaders/fg_flow.comp | $(B)
	$(GLSLANG) -V --quiet --vn fg_flow_comp -o $@ $<
$(B)/fg_hud_comp.h: src/renderer/shaders/fg_hud.comp | $(B)
	$(GLSLANG) -V --quiet --vn fg_hud_comp -o $@ $<
# --vrs: the shading-rate image of the next frame (src/renderer/vrs.h).
$(B)/vrs_rate_comp.h: src/renderer/shaders/vrs_rate.comp | $(B)
	$(GLSLANG) -V --quiet --vn vrs_rate_comp -o $@ $<

FSR3_GLSL = src/renderer/shaders/fsr3_common.glsl
$(B)/fsr3_prepare_comp.h: src/renderer/shaders/fsr3_prepare.comp $(FSR3_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn fsr3_prepare_comp -o $@ $<
$(B)/fsr3_prepare_ms_comp.h: src/renderer/shaders/fsr3_prepare.comp $(FSR3_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DMSAA --vn fsr3_prepare_ms_comp -o $@ $<
$(B)/fsr3_pyramid_comp.h: src/renderer/shaders/fsr3_pyramid.comp $(FSR3_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn fsr3_pyramid_comp -o $@ $<
$(B)/fsr3_shading_comp.h: src/renderer/shaders/fsr3_shading.comp $(FSR3_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn fsr3_shading_comp -o $@ $<
$(B)/fsr3_reactivity_comp.h: src/renderer/shaders/fsr3_reactivity.comp $(FSR3_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn fsr3_reactivity_comp -o $@ $<
$(B)/fsr3_instability_comp.h: src/renderer/shaders/fsr3_instability.comp $(FSR3_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn fsr3_instability_comp -o $@ $<
$(B)/fsr3_accumulate_comp.h: src/renderer/shaders/fsr3_accumulate.comp $(FSR3_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn fsr3_accumulate_comp -o $@ $<
$(B)/fsr3_accumulate_ms_comp.h: src/renderer/shaders/fsr3_accumulate.comp $(FSR3_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DMSAA --vn fsr3_accumulate_ms_comp -o $@ $<
$(B)/fsr3_mask_frag.h: src/renderer/shaders/fsr3_mask.frag | $(B)
	$(GLSLANG) -V --quiet --vn fsr3_mask_frag -o $@ $<
$(B)/fsr3_mask_ms_frag.h: src/renderer/shaders/fsr3_mask.frag | $(B)
	$(GLSLANG) -V --quiet -DMSAA --vn fsr3_mask_ms_frag -o $@ $<
$(B)/gamma_frag.h: src/renderer/shaders/gamma.frag | $(B)
	$(GLSLANG) -V --quiet --vn gamma_frag -o $@ $<
# --saturation and --hdr (src/hdr.h): the final grade (SDR) and the HDR10 output pass.
$(B)/output_frag.h: src/renderer/shaders/output.frag | $(B)
	$(GLSLANG) -V --quiet --vn output_frag -o $@ $<
$(B)/output_hdr_frag.h: src/renderer/shaders/output.frag | $(B)
	$(GLSLANG) -V --quiet -DHDR --vn output_hdr_frag -o $@ $<
# --sky-hd: the procedural sky (full-target triangle) and the skyline overlay (geom.vert + skyline.frag).
SKY_GLSL = src/renderer/shaders/sky_common.glsl
$(B)/sky_vert.h: src/renderer/shaders/sky.vert | $(B)
	$(GLSLANG) -V --quiet --vn sky_vert -o $@ $<
$(B)/sky_frag.h: src/renderer/shaders/sky.frag $(SKY_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn sky_frag -o $@ $<
$(B)/skyline_frag.h: src/renderer/shaders/skyline.frag $(SKY_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn skyline_frag -o $@ $<
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
# LIGHT_MASK: the light mask in the scene's alpha fades the occlusion (single sample: the last blur
# pass; MSAA: the composite, per pixel from the resolved target).
$(B)/ssao_blur_mask_frag.h: src/renderer/shaders/ssao_blur.frag $(SSAO_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DLIGHT_MASK --vn ssao_blur_mask_frag -o $@ $<
$(B)/ssao_comp_ms_mask_frag.h: src/renderer/shaders/ssao_comp.frag $(SSAO_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DMSAA -DLIGHT_MASK --vn ssao_comp_ms_mask_frag -o $@ $<
# --ssao-mode gtao (XeGTAO): depth MIPs (MSAA: sample 0), main pass, denoise
# (LIGHT_MASK: the last pass fades by the light mask on single-sample targets).
GTAO_GLSL = src/renderer/shaders/gtao_common.glsl
$(B)/gtao_prefilter_comp.h: src/renderer/shaders/gtao_prefilter.comp $(GTAO_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn gtao_prefilter_comp -o $@ $<
$(B)/gtao_prefilter_ms_comp.h: src/renderer/shaders/gtao_prefilter.comp $(GTAO_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DMSAA --vn gtao_prefilter_ms_comp -o $@ $<
$(B)/gtao_main_comp.h: src/renderer/shaders/gtao_main.comp $(GTAO_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn gtao_main_comp -o $@ $<
$(B)/gtao_denoise_comp.h: src/renderer/shaders/gtao_denoise.comp $(GTAO_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn gtao_denoise_comp -o $@ $<
$(B)/gtao_denoise_mask_comp.h: src/renderer/shaders/gtao_denoise.comp $(GTAO_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DLIGHT_MASK --vn gtao_denoise_mask_comp -o $@ $<
$(B)/gtao_temporal_comp.h: src/renderer/shaders/gtao_temporal.comp | $(B)
	$(GLSLANG) -V --quiet --vn gtao_temporal_comp -o $@ $<
# --ssgi: bounce source + MIPs, bitmask gather, temporal, depth-aware denoise,
# composite (MSAA: depth sample 0 and the light mask per pixel). Extra prerequisites of the
# vk_renderer.o rules (kept apart from their long lines; the Windows one after WB below).
SSGI_GLSL = src/renderer/shaders/ssgi_common.glsl
SSGI_HDRS = $(B)/ssgi_prefilter_comp.h $(B)/ssgi_main_comp.h $(B)/ssgi_temporal_comp.h $(B)/ssgi_denoise_comp.h $(B)/ssgi_comp_frag.h $(B)/ssgi_comp_ms_frag.h
$(B)/ssgi_prefilter_comp.h: src/renderer/shaders/ssgi_prefilter.comp $(SSGI_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn ssgi_prefilter_comp -o $@ $<
$(B)/ssgi_main_comp.h: src/renderer/shaders/ssgi_main.comp $(SSGI_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn ssgi_main_comp -o $@ $<
$(B)/ssgi_temporal_comp.h: src/renderer/shaders/ssgi_temporal.comp | $(B)
	$(GLSLANG) -V --quiet --vn ssgi_temporal_comp -o $@ $<
$(B)/ssgi_denoise_comp.h: src/renderer/shaders/ssgi_denoise.comp $(SSGI_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn ssgi_denoise_comp -o $@ $<
$(B)/ssgi_comp_frag.h: src/renderer/shaders/ssgi_comp.frag $(SSAO_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn ssgi_comp_frag -o $@ $<
$(B)/ssgi_comp_ms_frag.h: src/renderer/shaders/ssgi_comp.frag $(SSAO_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DMSAA -DLIGHT_MASK --vn ssgi_comp_ms_frag -o $@ $<
$(B)/vk_renderer.o: $(SSGI_HDRS)
$(B)/wet_frag.h: src/renderer/shaders/wet.frag src/renderer/shaders/light_walls.glsl src/renderer/shaders/sssr.glsl $(SSAO_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn wet_frag -o $@ $<
$(B)/wet_ms_frag.h: src/renderer/shaders/wet.frag src/renderer/shaders/light_walls.glsl src/renderer/shaders/sssr.glsl $(SSAO_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DMSAA --vn wet_ms_frag -o $@ $<
# --reflections sssr: the min-depth hierarchy (level 0 from the depth, single-sample or MSAA; the others from the level above).
$(B)/sssr_hiz_frag.h: src/renderer/shaders/sssr_hiz.frag $(SSAO_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DFROM_DEPTH --vn sssr_hiz_frag -o $@ $<
$(B)/sssr_hiz_ms_frag.h: src/renderer/shaders/sssr_hiz.frag $(SSAO_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DFROM_DEPTH -DMSAA --vn sssr_hiz_ms_frag -o $@ $<
$(B)/sssr_hiz_down_frag.h: src/renderer/shaders/sssr_hiz.frag | $(B)
	$(GLSLANG) -V --quiet --vn sssr_hiz_down_frag -o $@ $<
# --reflections sssr's glass: the surface pass (material draws again: face normal, shine, gloss).
$(B)/sssr_surf_vert.h: src/renderer/shaders/sssr_surf.vert | $(B)
	$(GLSLANG) -V --quiet --vn sssr_surf_vert -o $@ $<
$(B)/sssr_surf_frag.h: src/renderer/shaders/sssr_surf.frag | $(B)
	$(GLSLANG) -V --quiet --vn sssr_surf_frag -o $@ $<
# --contact-shadows: the occlusion mask and its composite (specialization constant MODE).
$(B)/contact_frag.h: src/renderer/shaders/contact.frag $(SSAO_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn contact_frag -o $@ $<
$(B)/contact_ms_frag.h: src/renderer/shaders/contact.frag $(SSAO_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DMSAA --vn contact_ms_frag -o $@ $<
# --volfog: the polar wall maps, the froxel injection and integration (compute) and the composite.
VOLFOG_GLSL = src/renderer/shaders/volfog_common.glsl
$(B)/volfog_polar_comp.h: src/renderer/shaders/volfog_polar.comp $(VOLFOG_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn volfog_polar_comp -o $@ $<
$(B)/volfog_inject_comp.h: src/renderer/shaders/volfog_inject.comp $(VOLFOG_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn volfog_inject_comp -o $@ $<
$(B)/volfog_integrate_comp.h: src/renderer/shaders/volfog_integrate.comp $(VOLFOG_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn volfog_integrate_comp -o $@ $<
$(B)/volfog_comp_frag.h: src/renderer/shaders/volfog_comp.frag $(VOLFOG_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn volfog_comp_frag -o $@ $<
$(B)/volfog_comp_ms_frag.h: src/renderer/shaders/volfog_comp.frag $(VOLFOG_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DMSAA --vn volfog_comp_ms_frag -o $@ $<
# --light-shafts: the scattered light at the working size and its composite (specialization constant MODE).
$(B)/shafts_frag.h: src/renderer/shaders/shafts.frag $(SSAO_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn shafts_frag -o $@ $<
$(B)/shafts_ms_frag.h: src/renderer/shaders/shafts.frag $(SSAO_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DMSAA --vn shafts_ms_frag -o $@ $<
# --lamp-lights: the light at the working size and its composite (specialization constant MODE).
$(B)/lamp_frag.h: src/renderer/shaders/lamp.frag src/renderer/shaders/light_walls.glsl $(SSAO_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn lamp_frag -o $@ $<
$(B)/lamp_ms_frag.h: src/renderer/shaders/lamp.frag src/renderer/shaders/light_walls.glsl $(SSAO_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DMSAA --vn lamp_ms_frag -o $@ $<
# --rain-hd-cover variants (RAIN_COVER: the rain map test of rain_cover.glsl) and the map's vertex shader.
COVER_GLSL = src/renderer/shaders/rain_cover.glsl
$(B)/rain_cover_vert.h: src/renderer/shaders/rain_cover.vert | $(B)
	$(GLSLANG) -V --quiet --vn rain_cover_vert -o $@ $<
# --world-shadows cut-out casters (alpha-tested fences, wire: their holes cast no shadow).
$(B)/world_shadow_cut_vert.h: src/renderer/shaders/world_shadow_cut.vert | $(B)
	$(GLSLANG) -V --quiet --vn world_shadow_cut_vert -o $@ $<
$(B)/world_shadow_cut_frag.h: src/renderer/shaders/world_shadow_cut.frag | $(B)
	$(GLSLANG) -V --quiet --vn world_shadow_cut_frag -o $@ $<
$(B)/world_shadow_gate_comp.h: src/renderer/shaders/world_shadow_gate.comp | $(B)
	$(GLSLANG) -V --quiet --vn world_shadow_gate_comp -o $@ $<
$(B)/rain_cover_frag.h: src/renderer/shaders/rain.frag $(COVER_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DRAIN_COVER --vn rain_cover_frag -o $@ $<
$(B)/splash_cover_frag.h: src/renderer/shaders/ssao_comp.frag $(SSAO_GLSL) $(COVER_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DRAIN_COVER -DCHAR_MASK --vn splash_cover_frag -o $@ $<
$(B)/splash_cover_ms_frag.h: src/renderer/shaders/ssao_comp.frag $(SSAO_GLSL) $(COVER_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DRAIN_COVER -DCHAR_MASK -DMSAA --vn splash_cover_ms_frag -o $@ $<
# The rain's character mask (renderer_rain_chars_on): its vertex shader and the splash mask's
# variant that reads it (CHAR_MASK; wet.frag always reads it).
SKIN_GLSL = src/renderer/shaders/vsh_skin_common.glsl
$(B)/vsh_skin_comp.h: src/renderer/shaders/vsh_skin.comp $(SKIN_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn vsh_skin_comp -o $@ $<
$(B)/skin_faces_comp.h: src/renderer/shaders/skin_faces.comp $(SKIN_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn skin_faces_comp -o $@ $<
$(B)/skin_normals_comp.h: src/renderer/shaders/skin_normals.comp $(SKIN_GLSL) | $(B)
	$(GLSLANG) -V --quiet --vn skin_normals_comp -o $@ $<
$(B)/char_mask_vert.h: src/renderer/shaders/char_mask.vert | $(B)
	$(GLSLANG) -V --quiet --vn char_mask_vert -o $@ $<
$(B)/splash_frag.h: src/renderer/shaders/ssao_comp.frag $(SSAO_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DCHAR_MASK --vn splash_frag -o $@ $<
$(B)/splash_ms_frag.h: src/renderer/shaders/ssao_comp.frag $(SSAO_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DCHAR_MASK -DMSAA --vn splash_ms_frag -o $@ $<
$(B)/wet_cover_frag.h: src/renderer/shaders/wet.frag src/renderer/shaders/light_walls.glsl src/renderer/shaders/sssr.glsl $(SSAO_GLSL) $(COVER_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DRAIN_COVER --vn wet_cover_frag -o $@ $<
$(B)/wet_cover_ms_frag.h: src/renderer/shaders/wet.frag src/renderer/shaders/light_walls.glsl src/renderer/shaders/sssr.glsl $(SSAO_GLSL) $(COVER_GLSL) | $(B)
	$(GLSLANG) -V --quiet -DRAIN_COVER -DMSAA --vn wet_cover_ms_frag -o $@ $<

# Game-independent FPS overlay test: meter, text, layout, font (src/fps_overlay.c).
$(B)/hle_modmenu_test: tests/hle_modmenu_test.c src/hle_modmenu.c src/hle_modmenu.h src/modmenu.c src/modmenu.h src/disc.c src/xtex.c src/kbm_glyphs.c $(PLAT) | $(B)
	$(CC) $(CFLAGS) -o $@ tests/hle_modmenu_test.c src/hle_modmenu.c src/modmenu.c src/disc.c src/xtex.c src/kbm_glyphs.c $(PLAT) -lpthread -lm

$(B)/modmenu_test: tests/modmenu_test.c src/modmenu.c src/modmenu.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/modmenu_test.c src/modmenu.c -lm

# Photo mode's pure part: camera, capture plan, accumulation, look, PNG, state.
# `make test` runs it (its PNG round trip writes under $$TMPDIR and removes the file).
$(B)/photo_test: tests/photo_test.c src/photo.c src/photo.h src/texpack_stb.c | $(B)
	$(CC) $(CFLAGS) -o $@ tests/photo_test.c src/photo.c src/texpack_stb.c -lm
photo-test: $(B)/photo_test
	./$(B)/photo_test > $(B)/photo_test.out
test: photo-test
.PHONY: photo-test

$(B)/fps_overlay_test: tests/fps_overlay_test.c src/fps_overlay.c src/fps_overlay.h src/fps_font.inc | $(B)
	$(CC) $(CFLAGS) -o $@ tests/fps_overlay_test.c src/fps_overlay.c -lm

# Game-independent geometry helpers test (layouts, element decoding, transforms, primitives).
$(B)/geom_test: tools/geom_test.c src/geom.c src/geom.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/geom_test.c src/geom.c -lm

# Game-independent Vulkan clear/readback test; exits 77 (skipped) without a Vulkan device.
$(B)/renderer_test: tools/renderer_test.c src/hdr.c src/hdr.h src/renderer.h src/renderer/vrs.h src/renderer/vk_caps.h src/ssao.h src/gpu_skin.h src/taa.c src/taa.h src/anim_interp.c src/anim_interp.h src/fps_overlay.c src/fps_overlay.h src/diaglog.c src/lamp_shadow.c src/lamp_shadow.h src/materials.c src/materials.h src/fsr.c src/fsr.h src/auto_exposure.c src/auto_exposure.h src/filmic.c src/filmic.h src/dof.c src/dof.h src/volfog.c src/volfog.h $(B)/hot_geom.o $(RENDERER_OBJS) $(PLAT) $(B)/hot_gpu_skin.o $(B)/hot_vsh.o src/disc.c | $(B)
	$(CC) $(CFLAGS) -o $@ tools/renderer_test.c src/hdr.c src/anim_interp.c src/fps_overlay.c src/diaglog.c src/lamp_shadow.c src/materials.c src/fsr.c src/taa.c src/auto_exposure.c src/filmic.c src/dof.c src/volfog.c $(B)/hot_geom.o $(RENDERER_OBJS) $(PLAT) $(B)/hot_gpu_skin.o $(B)/hot_vsh.o src/disc.c -lm $(RENDERER_LIBS)

# Game-independent HUD bar recognizer and glass rect math test (src/hud_glass.c).
$(B)/hud_glass_test: tools/hud_glass_test.c src/hud_glass.c src/hud_glass.h src/renderer.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/hud_glass_test.c src/hud_glass.c -lm

# Game-independent --bloom options and scene/HUD split rule test (src/bloom.c).
$(B)/bloom_test: tools/bloom_test.c src/bloom.c src/bloom.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/bloom_test.c src/bloom.c

# Game-independent --cutscene-dof options, bars gate, cut test, focus and CoC math (src/dof.c).
$(B)/dof_test: tests/dof_test.c src/dof.c src/dof.h src/renderer.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/dof_test.c src/dof.c -lm

# Game-independent --auto-exposure options, adaptation and roll-off test (src/auto_exposure.c).
$(B)/auto_exposure_test: tests/auto_exposure_test.c src/auto_exposure.c src/auto_exposure.h src/filmic.c src/filmic.h src/renderer.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/auto_exposure_test.c src/auto_exposure.c src/filmic.c -lm

# Game-independent --filmic options, tonemap, grade LUT and .cube parser test (src/filmic.c).
$(B)/filmic_test: tests/filmic_test.c src/filmic.c src/filmic.h src/renderer.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/filmic_test.c src/filmic.c -lm
$(B)/hdr_test: tests/hdr_test.c src/hdr.c src/hdr.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/hdr_test.c src/hdr.c -lm

# Game-independent --gamma-ramp options, D3DGAMMARAMP parsing, identity and lookup table test (src/gamma_ramp.c).
$(B)/gamma_ramp_test: tests/gamma_ramp_test.c src/gamma_ramp.c src/gamma_ramp.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/gamma_ramp_test.c src/gamma_ramp.c -lm

# --cinema-bars options, letterbox bar classifier and replay reorder test (src/cinema_bars.c).
$(B)/cinema_bars_test: tests/cinema_bars_test.c src/cinema_bars.c src/cinema_bars.h src/renderer.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/cinema_bars_test.c src/cinema_bars.c -lm
# Game-independent --fsr options, render size rounding, conflict rules and EASU/RCAS constants test (src/fsr.c).
$(B)/fsr_test: tests/fsr_test.c src/fsr.c src/fsr.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/fsr_test.c src/fsr.c -lm
# Game-independent --anim-interp rate tracking, palette blending and snap test (src/anim_interp.c).
$(B)/anim_interp_test: tests/anim_interp_test.c src/anim_interp.c src/anim_interp.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/anim_interp_test.c src/anim_interp.c -lm
# Game-independent --taa options, Halton jitter, reprojection map and camera-cut test (src/taa.c).
$(B)/taa_test: tests/taa_test.c src/taa.c src/taa.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/taa_test.c src/taa.c -lm

$(B)/dynres_test: tests/dynres_test.c src/dynres.c src/dynres.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/dynres_test.c src/dynres.c -lm

$(B)/bench_test: tests/bench_test.c src/bench.c src/bench.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/bench_test.c src/bench.c -lm

$(B)/framegen_test: tests/framegen_test.c src/framegen.c src/framegen.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/framegen_test.c src/framegen.c

# Game-independent --rain-hd options, rain layer rule, scroll, depth fit and slices test (src/rain.c).
$(B)/rain_test: tools/rain_test.c src/rain.c src/rain.h src/renderer.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/rain_test.c src/rain.c -lm

# Game-independent --ssao options, opaque-world split rule and projection scale test (src/ssao.c).
$(B)/ssao_test: tools/ssao_test.c src/ssao.c src/ssao.h src/renderer.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/ssao_test.c src/ssao.c -lm

# Game-independent --mesh-pack hash, .wmesh parsing and memo test (src/meshpack.c).
$(B)/meshpack_test: tools/meshpack_test.c src/meshpack.c src/meshpack.h src/meshover.c src/meshover.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/meshpack_test.c src/meshpack.c src/meshover.c

# Game-independent --lamp-shadows options and renderer input test (src/lamp_shadow.c).
$(B)/lamp_shadow_test: tests/lamp_shadow_test.c src/lamp_shadow.c src/lamp_shadow.h src/renderer.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/lamp_shadow_test.c src/lamp_shadow.c -lm

# Game-independent --fire-light options, flame sprites, per-fire clustering, flicker and lights test (src/fire_light.c).
$(B)/fire_light_test: tests/fire_light_test.c src/fire_light.c src/fire_light.h src/lamp_lights.c src/lamp_lights.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/fire_light_test.c src/fire_light.c src/lamp_lights.c -lm

# Game-independent --wet-light-streaks options and wet-pass light list (src/wet_streaks.c).
$(B)/wet_streaks_test: tests/wet_streaks_test.c src/wet_streaks.c src/wet_streaks.h src/lamp_lights.h src/renderer.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/wet_streaks_test.c src/wet_streaks.c -lm

# Game-independent --volfog options, grid, slices, fog fade, density, light list and reprojection (src/volfog.c).
$(B)/volfog_test: tests/volfog_test.c src/volfog.c src/volfog.h src/geom.c src/geom.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/volfog_test.c src/volfog.c src/geom.c -lm
# Game-independent --light-shafts options, rain easing, density and cone shape (src/light_shafts.c).
$(B)/light_shafts_test: tests/light_shafts_test.c src/light_shafts.c src/light_shafts.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/light_shafts_test.c src/light_shafts.c -lm

# Game-independent --lamp-lights options, light list (dedup, tracking, cap) and falloff test (src/lamp_lights.c).
$(B)/lamp_lights_test: tests/lamp_lights_test.c src/lamp_lights.c src/lamp_lights.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/lamp_lights_test.c src/lamp_lights.c -lm

# Game-independent --damp-streets options, damp ground, night test and fade (src/damp.c).
$(B)/damp_test: tests/damp_test.c src/damp.c src/damp.h src/renderer.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/damp_test.c src/damp.c -lm
# Game-independent --reflections options (src/sssr.c).
$(B)/sssr_test: tests/sssr_test.c src/sssr.c src/sssr.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/sssr_test.c src/sssr.c
# Game-independent --neon-reflections options, sign colour and position, wet-ground lights (src/neon_refl.c).
$(B)/neon_refl_test: tests/neon_refl_test.c src/neon_refl.c src/neon_refl.h src/wet_streaks.c src/wet_streaks.h src/lamp_lights.c src/lamp_lights.h src/renderer.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/neon_refl_test.c src/neon_refl.c src/wet_streaks.c src/lamp_lights.c -lm
# Mod collision patches: patch files and their insertion into a collision world (src/colpatch.c).
# Texture layers of the mods (src/texlayer.h): names, folder loading, combinations, blending.
$(B)/texlayer_test: tests/texlayer_test.c src/texlayer.c src/texlayer.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/texlayer_test.c src/texlayer.c

$(B)/colpatch_test: tests/colpatch_test.c src/colpatch.c src/colpatch.h src/materials.c src/disc.c src/materials.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/colpatch_test.c src/colpatch.c src/materials.c src/disc.c -lm
# The mod asset engine (src/modassets.h): assets.ini parser, cache folder, build steps, Python-compatible
# random numbers, over the disc reader, xtex and stb_image_write's zlib.
MODASSETS_SRCS = src/modassets.c src/modassets_build.c src/modassets_own.c src/modassets_libs.c src/pyrand.c src/texpack.c src/texpack_stb.c
$(B)/modassets_test: tools/modassets_test.c $(MODASSETS_SRCS) src/disc.c src/xtex.c src/meshpack.c src/modassets.h src/pyrand.h src/disc.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/modassets_test.c $(MODASSETS_SRCS) src/disc.c src/xtex.c src/meshpack.c -lm
# A mod's own sounds (src/modsound.h) through fake mixer voices.
$(B)/modsound_test: tools/modsound_test.c src/modsound.c src/modsound.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/modsound_test.c src/modsound.c -lm
# Game-independent disc reader (src/disc.c): XBoxWad.idx lookups, the container walk and scan, skin
# bindings, character and texture chunks, on a synthetic disc folder.
$(B)/disc_test: tools/disc_test.c src/disc.c src/disc.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/disc_test.c src/disc.c
# Game-independent --materials options, collision mesh, top-down material map and WAD lookup (src/materials.c).
$(B)/materials_test: tests/materials_test.c src/materials.c src/disc.c src/materials.h src/renderer.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/materials_test.c src/materials.c src/disc.c -lm

# Game-independent --level-lights options, LightManager reader (fake guest memory) and selection test (src/level_lights.c).
$(B)/level_lights_test: tests/level_lights_test.c src/level_lights.c src/level_lights.h src/lamp_lights.c src/lamp_lights.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/level_lights_test.c src/level_lights.c src/lamp_lights.c -lm
# Game-independent --char-lighting option and smooth character normals test (src/char_light.c).
$(B)/hair_test: tests/hair_test.c src/hair.c src/hair.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/hair_test.c src/hair.c -lm -lpthread
$(B)/char_light_test: tests/char_light_test.c src/char_light.c src/char_light.h src/renderer.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/char_light_test.c src/char_light.c -lm
# Game-independent --hat-fix / --hat-subdiv options, hat detection, smoothed normals and PN subdivision test (src/hats.c).
$(B)/hats_test: tests/hats_test.c src/hats.c src/hats.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/hats_test.c src/hats.c -lm

# Game-independent --world-lighting option, program classifier, view normals and light mask test (src/world_light.c).
$(B)/world_light_test: tests/world_light_test.c src/world_light.c src/world_light.h src/renderer.h $(B)/hot_vsh.o src/geom.c src/ssao.c | $(B)
	$(CC) $(CFLAGS) -o $@ tests/world_light_test.c src/world_light.c src/geom.c src/ssao.c $(B)/hot_vsh.o -lm

# Game-independent renderer vertex array test (src/renderer/vertex_sink.h, direct geometry writes).
$(B)/vertex_sink_test: tests/vertex_sink_test.cpp src/renderer/vertex_sink.h src/renderer.h | $(B)
	$(CXX) $(CXXFLAGS) -o $@ tests/vertex_sink_test.cpp

# Game-independent pipeline cache files test (src/renderer/pipe_cache.h): header check, key list, atomic write.
$(B)/pipe_cache_test: tests/pipe_cache_test.cpp src/renderer/pipe_cache.h | $(B)
	$(CXX) $(CXXFLAGS) -o $@ tests/pipe_cache_test.cpp

# Game-independent --gpu-skinning option, program matcher, packing and adjacency test (src/gpu_skin.c).
$(B)/gpu_skin_test: tests/gpu_skin_test.c src/gpu_skin.h src/vsh.h src/geom.h src/renderer.h $(B)/hot_gpu_skin.o $(B)/hot_vsh.o | $(B)
	$(CC) $(CFLAGS) -o $@ tests/gpu_skin_test.c $(B)/hot_gpu_skin.o $(B)/hot_vsh.o -lm

# Game-independent --soft-particles options, particle draw signature and soft quad rule test (src/soft_particles.c).
$(B)/soft_particles_test: tools/soft_particles_test.c src/soft_particles.c src/soft_particles.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/soft_particles_test.c src/soft_particles.c -lm
# Game-independent --world-shadows options, light fit and interior gate test (src/world_shadow.c).
$(B)/world_shadow_test: tests/world_shadow_test.c src/world_shadow.c src/world_shadow.h src/renderer.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/world_shadow_test.c src/world_shadow.c -lm

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
AUDIO_SRCS = src/audio.c src/steam_audio.c src/xact.c src/sndpack.c src/disc.c src/diaglog.c $(B)/hot_resample.o $(B)/hot_reverb.o $(PLAT)
AUDIO_HDRS = src/diaglog.h src/steam_audio.h src/audio.h src/xact.h src/sndpack.h src/resample.h src/reverb.h src/platform.h
$(B)/hle_audio_test: tools/hle_audio_test.c src/hle_audio.c src/hle_thunk.S src/hle_audio.h $(AUDIO_SRCS) $(AUDIO_HDRS) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tools/hle_audio_test.c src/hle_audio.c src/hle_thunk.S $(AUDIO_SRCS) -lm

# Game-independent XACT 3D positioning test (source in tests/: distance/pan math and the HLE
# listener/source setters, mixed without the mixer thread).
$(B)/audio3d_test: ../tests/audio3d_test.c tools/audio_synth.h src/hle_audio.c src/hle_thunk.S src/hle_audio.h $(AUDIO_SRCS) $(AUDIO_HDRS) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ ../tests/audio3d_test.c src/hle_audio.c src/hle_thunk.S $(AUDIO_SRCS) -lm

# Game-independent audio test: Xbox ADPCM decoding, synthetic wave/sound banks, mixer, WAV dump.
$(B)/sndpack_test: tools/sndpack_test.c tools/audio_synth.h $(AUDIO_SRCS) $(AUDIO_HDRS) | $(B)
	$(CC) $(CFLAGS) -o $@ tools/sndpack_test.c $(AUDIO_SRCS) -lm

# Audio extractor helper (tools/audio_extract): cue listing and WAV decoding with the runtime's
# own XACT parser and wave reader.
$(B)/xact_extract: tools/xact_extract.c $(AUDIO_SRCS) $(AUDIO_HDRS) | $(B)
	$(CC) $(CFLAGS) -o $@ tools/xact_extract.c $(AUDIO_SRCS) -lm
$(B)/xact_extract_test: tools/xact_extract_test.c tools/xact_extract.c tools/audio_synth.h $(AUDIO_SRCS) $(AUDIO_HDRS) | $(B)
	$(CC) $(CFLAGS) -o $@ tools/xact_extract_test.c $(AUDIO_SRCS) -lm

$(B)/audio_test: tools/audio_test.c $(AUDIO_SRCS) $(AUDIO_HDRS) | $(B)
	$(CC) $(CFLAGS) -o $@ tools/audio_test.c $(AUDIO_SRCS) -lm

# Game-independent resampler test (source in runtime/tests/): image level, passband flatness and
# aliasing of the selected resampler (`resample_test linear` fails them by design), voice start/end,
# looping, stereo and rate changes through the mixer FIFO, and the CPU cost per voice.
$(B)/resample_test: tests/resample_test.c $(AUDIO_SRCS) $(AUDIO_HDRS) | $(B)
	$(CC) $(CFLAGS) -o $@ tests/resample_test.c $(AUDIO_SRCS) -lm

# Game-independent I3DL2 reverb test (source in runtime/tests/): impulse-response decay time and
# HF ratio against the I3DL2 parameters, mB levels, silence by default, stability, CPU cost.
# Game-independent output lead test (source in runtime/tests/): the device queue target the
# mixer keeps (--audio-lead-ms), stalled and drifting devices, and the held-back DirectSound cursor.
$(B)/audio_lead_test: tests/audio_lead_test.c $(AUDIO_SRCS) $(AUDIO_HDRS) | $(B)
	$(CC) $(CFLAGS) -o $@ tests/audio_lead_test.c $(AUDIO_SRCS) -lm

# Game-independent Steam Audio test (source in tests/): a missing library,
# and with the library built by `make steam-audio` the HRTF render (right louder right) and its cost;
# the acoustics (materials, occlusion behind a synthetic wall, room reverb).
$(B)/steam_audio_test: ../tests/steam_audio_test.c src/steam_audio.c src/steam_audio.h src/acoustics.c src/acoustics.h src/materials.c src/disc.c src/materials.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) -o $@ ../tests/steam_audio_test.c src/steam_audio.c src/acoustics.c src/materials.c src/disc.c $(PLAT) -lm

# Steam Audio v4.8.1 built from source for both targets (third_party/steam-audio/README.md):
# build/steam-audio/linux-x86/libphonon.so and build/steam-audio/windows-x86/phonon.dll. Not part of
# `all` (a network fetch on the first run and a few minutes of compiling); the runtime runs without it.
steam-audio:
	third_party/steam-audio/build.sh all $(B)/steam-audio

$(B)/reverb_test: tests/reverb_test.c $(B)/hot_reverb.o src/reverb.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/reverb_test.c $(B)/hot_reverb.o -lm

# Game-independent HLE input test: src/hle_input.c built with its own test main (maps the XPP range).
$(B)/hle_input_test: src/hle_input.c src/input_ports.c src/hle_input.h src/hle_audio.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -DHLE_INPUT_TEST -o $@ src/hle_input.c src/input_ports.c $(PLAT) -lm

# Game-independent SDL controller -> port test: src/video.c (included by the test) on SDL's dummy
# video driver with SDL virtual game controllers; prints SKIP when SDL2 cannot provide them.
$(B)/video_pads_test: tools/video_pads_test.c src/video.c src/video.h src/refresh_match.c src/refresh_match.h src/input_map.c src/input_map.h src/hle_input.c src/input_ports.c src/hle_input.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tools/video_pads_test.c src/refresh_match.c src/input_map.c src/hle_input.c src/input_ports.c $(PLAT) -lm

# Game-independent host input layer test: src/input_map.c built with its own test main (key
# state, auto-repeat filtering, bindings) over src/hle_input.c's pad state.
$(B)/input_map_test: src/input_map.c src/input_map.h src/hle_input.c src/input_ports.c src/hle_input.h src/hle_audio.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -DINPUT_MAP_TEST -o $@ src/input_map.c src/hle_input.c src/input_ports.c $(PLAT) -lm

# Game-independent kernel file I/O test: src/kfile.c built with its own test main and kernel
# stubs, over a temporary host directory (directory enumeration, path mapping).
$(B)/kfile_test: src/kfile.c src/files.c src/files.h src/kernel.h src/kobj.h src/hle_textfix.c src/hle_textfix.h src/movie_hd.c src/movie_hd.h src/diaglog.c src/diaglog.h src/load_prof.c src/load_prof.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) -DKFILE_TEST -o $@ src/kfile.c src/files.c src/hle_textfix.c src/movie_hd.c src/diaglog.c src/load_prof.c $(PLAT)

# Game-independent kernel object test: src/kobj.c built with its own test main and kernel
# stubs (waits, wakeups, mutant/semaphore/critical-section hand-offs, timers).
$(B)/kobj_test: src/kobj.c src/kobj.h src/kernel.h src/kpcr.h src/diaglog.c src/diaglog.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) -DKOBJ_TEST -o $@ src/kobj.c src/diaglog.c $(PLAT) -lpthread

# Game-independent kernel crypto test: src/kxc.c built with its own test main (SHA-1 and
# HMAC-SHA1 against the RFC 3174 / RFC 2202 vectors).
$(B)/kxc_test: src/kxc.c src/kxc.h | $(B)
	$(CC) $(CFLAGS) -DKXC_TEST -o $@ src/kxc.c

# Game-independent guest memory placement test (no guest pages inside other code's reservations).
$(B)/memmap_test: tools/memmap_test.c src/platform.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) -o $@ tools/memmap_test.c $(PLAT)
# Game-independent plat_program_dir test: the program's folder and the XBRUN_PROGRAM_DIR override
# the AppImage sets (tools/appimage). `make test` runs it.
$(B)/program_dir_test: tests/program_dir_test.c src/platform.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) -o $@ tests/program_dir_test.c $(PLAT)
program-dir-test: $(B)/program_dir_test
	./$(B)/program_dir_test
test: program-dir-test
.PHONY: program-dir-test
# Game-independent plat_is_mapped test (source in tests/): the Linux page bitmap of guest pages
# (map, partial unmap, remap), the mincore fallback for other mappings, and its cost.
$(B)/page_bitmap_test: tests/page_bitmap_test.c src/platform.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) -o $@ tests/page_bitmap_test.c $(PLAT)

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

# Game-independent --skip-intro test: a host buffer stands in for the title's main (options, the
# three boot movie calls NOPed and nothing else, all-or-none when a site differs).
$(B)/hle_skipintro_test: tools/hle_skipintro_test.c src/hle_skipintro.c src/hle_skipintro.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/hle_skipintro_test.c src/hle_skipintro.c

# Game-independent memory-registry test: a host buffer stands in for the memory manager set-up
# (registry node count raised, nothing else written, all-or-none; used/capacity read-back).
$(B)/hle_memreg_test: tools/hle_memreg_test.c src/hle_memreg.c src/hle_memreg.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/hle_memreg_test.c src/hle_memreg.c

# Game-independent --sharp-faces test (source in tests/): options, the character-skin rule and the
# Game-independent texture replay race detector test (src/tex_race.c): record-key recompute,
# change detection and the [tex-race] log rate limit.
$(B)/tex_race_test: tests/tex_race_test.c src/tex_race.c src/tex_race.h src/xtex.c src/xtex.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/tex_race_test.c src/tex_race.c src/xtex.c

# Game-independent --tri-check detector test (src/tri_check.c).
$(B)/tri_check_test: tests/tri_check_test.c src/tri_check.c src/tri_check.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/tri_check_test.c src/tri_check.c -lm

# Game-independent robustness diagnostics test (src/diaglog.c): breadcrumb ring wraparound and
# format, crash context and dump, once-per-name [hle-missing] dedupe, rate limiting, exit summary.
$(B)/diaglog_test: tests/diaglog_test.c src/diaglog.c src/diaglog.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/diaglog_test.c src/diaglog.c -lpthread

# 2x skin filter (src/sharp_faces.c).
$(B)/sharp_faces_test: tests/sharp_faces_test.c src/sharp_faces.c src/sharp_faces.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/sharp_faces_test.c src/sharp_faces.c -lm

# Game-independent --neon-glow test (source in tests/): options, mask names, the mask index over
# temporary packs, PNG / BC4 .emit.dds loading into an L8 mip chain and the flicker (src/neon_glow.c).
# Texture pack load queue: dedupe, demand before prefetch, staged cap, reclaim, upload budget (src/texload.c).
$(B)/texload_test: tests/texload_test.c src/texload.c src/texload.h src/renderer/tex_lru.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/texload_test.c src/texload.c
# Always-on performance log: hitch threshold and rate limit, line formats, level summary math (src/perf_log.c).
$(B)/perf_log_test: tests/perf_log_test.c src/perf_log.c src/perf_log.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/perf_log_test.c src/perf_log.c

# --load-profile: read classification, counters, the load window and its [load] line (src/load_prof.c).
$(B)/load_prof_test: tests/load_prof_test.c src/load_prof.c src/load_prof.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/load_prof_test.c src/load_prof.c

# The startup line on the Vulkan device's support: versions, device type, feature list (src/renderer/vk_caps.h).
$(B)/vk_caps_test: tests/vk_caps_test.c src/renderer/vk_caps.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/vk_caps_test.c

# --refresh-match: the multiple-of-60 tolerance, the target mode choice and the [video] lines (src/refresh_match.c).
$(B)/refresh_match_test: tests/refresh_match_test.c src/refresh_match.c src/refresh_match.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/refresh_match_test.c src/refresh_match.c

# Adaptive texture-pack cache cap from the driver's memory budget: target, floor, hysteresis (src/vram_budget.c).
$(B)/mem_test: tests/mem_test.c src/mem.c src/mem.h src/platform.h src/diaglog.c src/diaglog.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) -o $@ tests/mem_test.c src/mem.c src/diaglog.c $(PLAT)

$(B)/vram_budget_test: tests/vram_budget_test.c src/vram_budget.c src/vram_budget.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/vram_budget_test.c src/vram_budget.c

# Game-independent --normal-maps test (source in tests/): options, companion names (<hash>_n / _s),
# the companion index over temporary packs and the shader's relief flags (src/normal_maps.c).
$(B)/normal_maps_test: tests/normal_maps_test.c src/normal_maps.c src/normal_maps.h src/texpack.c src/texpack_stb.c src/xtex.c src/texpack.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/normal_maps_test.c src/normal_maps.c $(TEXPACK_SRCS) -lm

$(B)/neon_glow_test: tests/neon_glow_test.c src/neon_glow.c src/neon_glow.h src/texpack.c src/texpack_stb.c src/xtex.c src/texpack.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/neon_glow_test.c src/neon_glow.c src/texpack.c src/texpack_stb.c src/xtex.c -lm

# Game-independent --sky-hd test (source in tests/): options, the sky-stage classifier, the dome
# palette and the sky / skyline parameters (src/sky.c).
$(B)/sky_test: tests/sky_test.c src/sky.c src/sky.h src/renderer.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/sky_test.c src/sky.c -lm

# Game-independent draw-distance test: a host buffer with the shipped bytes planted at each patch
# site (parsing, defaults, every patch written, any mismatch writes nothing of that patch).
$(B)/hle_drawdist_test: tests/hle_drawdist_test.c src/hle_drawdist.c src/hle_drawdist.h src/hle_ratefix.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/hle_drawdist_test.c src/hle_drawdist.c -lm

# Game-independent --sim60 patch test (whole-set application, mismatch writes nothing, every
# title table entry on a planted image).
$(B)/hle_sim60_test: tools/hle_sim60_test.c src/hle_sim60.c src/hle_sim60.h | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tools/hle_sim60_test.c src/hle_sim60.c

# Game-independent check of the --sim60 entries for update-counted litter, fail-safe and combat
# timers (source in tests/): the planted table holds the doubled durations.
$(B)/hle_sim60_rates_test: tests/hle_sim60_rates_test.c src/hle_sim60.c src/hle_sim60.h | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/hle_sim60_rates_test.c src/hle_sim60.c

# Game-independent rate probe test (options, game-time schedule, snapshot file round trip).
$(B)/rateprobe_test: tools/rateprobe_test.c src/rateprobe.c src/rateprobe.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/rateprobe_test.c src/rateprobe.c

# Game-independent --warp test (parsing, mission table, signature check, both stubs executed in
# host executable memory against fake title functions).
$(B)/hle_warp_test: tools/hle_warp_test.c src/hle_warp.c src/hle_warp.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tools/hle_warp_test.c src/hle_warp.c $(PLAT)

# Live game control (src/gamectl_proto.h): protocol, JSON replies, options, request queue, captures.
$(B)/gamectl_test: tools/gamectl_test.c src/gamectl_proto.c src/gamectl_proto.h | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tools/gamectl_test.c src/gamectl_proto.c -lpthread -lm

# The in-game settings model: the launcher builds settings_model.c too.
$(B)/settings_model_test: tools/settings_model_test.c src/settings_model.c src/settings_model.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/settings_model_test.c src/settings_model.c
$(B)/settings_sync_test: tools/settings_sync_test.c src/settings_sync.c src/settings_sync.h src/settings_model.c src/settings_model.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tools/settings_sync_test.c src/settings_sync.c src/settings_model.c $(PLAT)
$(B)/settings_apply_test: tools/settings_apply_test.c src/settings_apply.c src/settings_apply.h src/settings_model.c src/settings_model.h | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tools/settings_apply_test.c src/settings_apply.c src/settings_model.c -lpthread
$(B)/settings_menu_test: tools/settings_menu_test.c src/settings_menu.c src/settings_menu.h src/settings_model.c src/settings_model.h src/modmenu.c src/modmenu.h ../launcher/src/i18n_strings.h | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tools/settings_menu_test.c src/settings_menu.c src/settings_model.c src/modmenu.c -lm
$(B)/hle_goto_test: tools/hle_goto_test.c src/hle_goto.c src/hle_goto.h src/hle_devlua.c src/hle_devlua.h src/hle_warp.c src/hle_warp.h src/bench.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tools/hle_goto_test.c src/hle_goto.c src/hle_devlua.c src/hle_warp.c $(PLAT)
# Game-independent --smart-allies test (rules, parser, both patched call sites on a fake guest).
$(B)/hle_allies_test: tools/hle_allies_test.c src/hle_allies.c src/hle_allies.h src/hle_aidbg.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tools/hle_allies_test.c src/hle_allies.c $(PLAT)
# Game-independent --ai-debug core test (fake guest memory through the read callback).
# Game-independent --enemy-tactics test (rules, parser, the patched call sites on a fake guest).
$(B)/hle_tactics_test: tools/hle_tactics_test.c src/hle_tactics.c src/hle_tactics.h src/hle_allies.c src/hle_allies.h src/hle_aidbg.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tools/hle_tactics_test.c src/hle_tactics.c src/hle_allies.c $(PLAT) -lm

$(B)/hle_mods_test: tools/hle_mods_test.c src/hle_mods.c src/hle_mods.h src/hle_scriptfix.c src/hle_scriptfix.h src/hle_gore.c src/hle_gore.h src/modini.c src/modini.h src/modcheck.c src/modcheck.h src/modsafe.c src/modsafe.h src/diaglog.c src/diaglog.h src/meshover.c src/meshover.h src/texlayer.c src/texlayer.h src/meshpack.c src/hle_devlua.c src/hle_devlua.h src/hle_warp.c src/hle_warp.h $(PLAT) | $(B) src/colpatch.c src/colpatch.h src/materials.c src/disc.c src/hle_scars.c src/hle_scars.h src/hle_radar.c src/hle_radar.h src/mem.c src/xtex.c src/hle_huspeed.c src/hle_huspeed.h src/hle_luagc.c src/hle_luagc.h src/hle_diffmenu.c src/hle_diffmenu.h $(MODASSETS_SRCS) src/modassets.h src/pyrand.h src/disc.h src/hle_modmenu.c src/hle_modmenu.h src/modmenu.c src/modmenu.h src/kbm_glyphs.c src/modsound.c src/modsound.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tools/hle_mods_test.c src/hle_mods.c src/hle_scriptfix.c src/hle_gore.c src/modini.c src/modcheck.c src/modsafe.c src/diaglog.c src/meshover.c src/texlayer.c src/meshpack.c src/hle_devlua.c src/hle_warp.c $(PLAT) -lpthread src/colpatch.c src/materials.c src/disc.c -lm src/hle_scars.c src/hle_radar.c src/mem.c src/xtex.c src/hle_huspeed.c src/hle_luagc.c src/hle_diffmenu.c $(MODASSETS_SRCS) src/hle_modmenu.c src/modmenu.c src/kbm_glyphs.c src/modsound.c src/cinema_bars.c

$(B)/hle_aidbg_test: tools/hle_aidbg_test.c src/hle_aidbg.c src/hle_aidbg.h | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -DHLE_AIDBG_TEST -o $@ tools/hle_aidbg_test.c src/hle_aidbg.c -lm

# Text-fix test: game-independent; with ../extracted/disc/Main.wad0 as argument also the real file.
$(B)/hle_scriptfix_test: tools/hle_scriptfix_test.c src/hle_scriptfix.c src/hle_scriptfix.h src/hle_devlua.c src/hle_devlua.h src/hle_warp.c src/hle_warp.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tools/hle_scriptfix_test.c src/hle_scriptfix.c src/hle_devlua.c src/hle_warp.c $(PLAT)
$(B)/hle_textfix_test: tools/hle_textfix_test.c src/hle_textfix.c src/hle_textfix.h src/kbm_glyphs.c src/disc.c src/kbm_glyphs.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/hle_textfix_test.c src/hle_textfix.c src/kbm_glyphs.c src/disc.c -lm

# Game-independent spawn scaling test: src/hle_spawn.c with its own test main (scaling and
# clamping, option parsing, the patched stores run on planted copies at their guest addresses).
$(B)/hle_spawn_test: src/hle_spawn.c src/hle_spawn.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -DHLE_SPAWN_TEST -o $@ src/hle_spawn.c $(PLAT)

# Game-independent blood test (ModBlood): src/hle_gore.c with its own test main (options, the hooks on
# planted copies of their sites, run with the guest's registers; other bytes refused).
$(B)/hle_gore_test: src/hle_gore.c src/hle_gore.h src/cinema_bars.c src/cinema_bars.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -DHLE_GORE_TEST -o $@ src/hle_gore.c src/cinema_bars.c $(PLAT) -lm

# Game-independent healed-marks test (ModScar): the table of marks, the healed atlas and the plaster.
$(B)/hle_scars_test: src/hle_scars.c src/hle_scars.h | $(B)
	$(CC) $(CFLAGS) -DHLE_SCARS_TEST -o $@ src/hle_scars.c -lm

# Game-independent radar dot colour test (ModRadarColor): the colour table, the parser and the pick of a
# dot's colour by kind and layer. `make test` runs it.
$(B)/hle_radar_test: src/hle_radar.c src/hle_radar.h | $(B)
	$(CC) $(CFLAGS) -DHLE_RADAR_TEST -o $@ src/hle_radar.c
radar-test: $(B)/hle_radar_test
	./$(B)/hle_radar_test > $(B)/hle_radar_test.out
test: radar-test
.PHONY: radar-test

# Game-independent Lua collector guard test (hle_luagc.h): planted clamps, checkGC and a self-nesting
# collectgarbage; refused on other bytes, no nesting and the larger cap once patched.
# Game-independent difficulty-list decisions (hle_diffmenu.h): src/hle_diffmenu.c with its own test main.
$(B)/hle_diffmenu_test: src/hle_diffmenu.c src/hle_diffmenu.h | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -DHLE_DIFFMENU_TEST -o $@ src/hle_diffmenu.c

$(B)/hle_luagc_test: tools/hle_luagc_test.c src/hle_luagc.c src/hle_luagc.h src/mem.c src/mem.h src/diaglog.c src/diaglog.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tools/hle_luagc_test.c src/hle_luagc.c src/mem.c src/diaglog.c $(PLAT) -lm

# Game-independent speed-cap test (ModSpeed): src/hle_huspeed.c with its own test main (the target-speed hook on a
# planted copy of its site, caps per character, other bytes refused).
$(B)/hle_huspeed_test: src/hle_huspeed.c src/hle_huspeed.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -DHLE_HUSPEED_TEST -o $@ src/hle_huspeed.c $(PLAT) -lm

# Game-independent effect-pool test: src/hle_fx.c with its own test main (--stats pool reader and
# sprite flush counter, --fx-scale arithmetic and parser, every patch site planted at its guest
# address, mismatching bytes refused, trampolines run with the guest's registers).
$(B)/hle_fx_test: src/hle_fx.c src/hle_fx.h src/mem.c src/mem.h src/diaglog.c src/diaglog.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -DHLE_FX_TEST -o $@ src/hle_fx.c src/mem.c src/diaglog.c $(PLAT)

# Game-independent HLE D3D test (maps the D3D8 library range, null video backend).
HLE_D3D_SRCS = tools/hle_d3d_test.c src/hle_d3d.c src/render_rec.c src/render_wire.c src/mem_map.c src/hle_d3d_thunk.S src/video.c src/refresh_match.c src/hle_input.c src/input_ports.c src/input_map.c src/mem.c src/yuv.c src/geom.c src/xtex.c src/texpack.c src/texpack_stb.c src/builtin_tex.c src/vsh.c src/psh.c src/hle_spawn.c src/hle_fx.c src/hle_sim60.c src/pacing.c src/hle_xapi.c src/hud_glass.c src/rateprobe.c src/kbm_glyphs.c src/bloom.c src/shadow_range.c src/rain.c src/ssao.c src/meshpack.c src/meshover.c src/texlayer.c src/movie_hd.c $(PLAT) src/soft_particles.c src/filmic.c src/auto_exposure.c src/dof.c src/sky.c src/lamp_lights.c src/level_lights.c src/light_shafts.c src/volfog.c src/wet_streaks.c src/materials.c src/colpatch.c src/damp.c src/neon_refl.c src/sssr.c src/fire_light.c src/lamp_shadow.c src/world_shadow.c src/char_light.c src/hair.c src/hats.c src/world_light.c src/gpu_skin.c src/tri_check.c src/sharp_font.c src/sharp_faces.c src/neon_glow.c src/normal_maps.c src/fps_overlay.c src/texload.c src/vram_budget.c src/perf_log.c src/load_prof.c src/diaglog.c src/tex_race.c src/gamma_ramp.c src/fsr.c src/cinema_bars.c src/taa.c src/dynres.c src/bench.c src/framegen.c src/anim_interp.c src/combat_debug.c src/combat_data.c src/hle_warp.c src/disc.c src/hle_modmenu.c src/modmenu.c src/hle_devlua.c src/render_ipc.c
$(B)/hle_d3d_test: $(HLE_D3D_SRCS) src/hle_d3d.h src/hle_spawn.h src/hle_fx.h src/hle_input.h src/input_map.h src/video.h src/mem.h src/yuv.h src/geom.h src/xtex.h src/texpack.h src/meshpack.h src/vsh.h src/psh.h src/renderer.h src/renderer/vk_caps.h src/kbm_glyphs.h src/movie_hd.h src/gpu_skin.h $(RENDERER_OBJS) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(HLE_D3D_SRCS) $(RENDERER_OBJS) -lm $(RENDERER_LIBS)

# Game-independent vertex-program pool test (source in tests/): byte-identical draw output for
# every --vp-threads size against the synchronous run, single-batch draws on the recording thread,
# the default pool size. Links the HLE D3D test's sources without its main.
VP_POOL_SRCS = tests/vp_pool_test.c $(filter-out tools/hle_d3d_test.c,$(HLE_D3D_SRCS))
$(B)/vp_pool_test: $(VP_POOL_SRCS) src/hle_d3d.h src/vsh.h src/renderer.h src/platform.h $(RENDERER_OBJS) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(VP_POOL_SRCS) $(RENDERER_OBJS) -lm $(RENDERER_LIBS)

# Game-independent YUV quad helpers test (unswizzle, shader coefficients, scaled quad).
# Game-independent --movie-hd test (source in tests/): the _hd file-name rule, the Bink header
# check, the 16:9 fit rectangle, the GPU texel coordinates and the bilinear YUV quad.
$(B)/movie_hd_test: tests/movie_hd_test.c src/movie_hd.c src/movie_hd.h src/yuv.c src/yuv.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/movie_hd_test.c src/movie_hd.c src/yuv.c -lm

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
$(B)/ktx2_test: tests/ktx2_test.c $(TEXPACK_SRCS) src/texpack.h src/ktx2.h $(KTX2_OBJS) | $(B)
	$(CC) $(CFLAGS) -o $@ tests/ktx2_test.c $(TEXPACK_SRCS) $(KTX2_OBJS) -lm -lpthread -lc++
$(B)/texpack_test: tools/texpack_test.c $(TEXPACK_SRCS) src/texpack.h src/xtex.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/texpack_test.c $(TEXPACK_SRCS) -lm

# Built-in replacement textures (the joint): lookup, pre-filter and priority over a texture pack.
$(B)/builtin_tex_test: ../tests/builtin_tex_test.c src/builtin_tex.c src/builtin_tex.h src/builtin_joint.h $(TEXPACK_SRCS) src/texpack.h src/xtex.h | $(B)
	$(CC) $(CFLAGS) -o $@ ../tests/builtin_tex_test.c src/builtin_tex.c $(TEXPACK_SRCS) -lm

# Game-independent keyboard/mouse button icon test (rect table, labels, atlas painter).
# Game-independent sharp font upscale test (src/sharp_font.c).
$(B)/sharp_font_test: tools/sharp_font_test.c src/sharp_font.c src/sharp_font.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/sharp_font_test.c src/sharp_font.c -lm

$(B)/kbm_glyphs_test: tools/kbm_glyphs_test.c src/kbm_glyphs.c src/disc.c src/kbm_glyphs.h src/input_map.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/kbm_glyphs_test.c src/kbm_glyphs.c src/disc.c -lm

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

# ---- render64: the renderer call stream's replay and the IPC benchmark ----
# The renderer is also built for x86_64 (build64/): render64_replay plays a --record-renderer
# recording through it (and through the i386 build, build/render64_replay); ipc_bench measures a
# shared-memory ring between an i386 producer and an x86_64 consumer. Not part of `all`:
# `make render64` (Linux) and `make render64-windows` (the benchmark's .exe pair).
B64 = build64
CC64 = zig cc -target x86_64-linux-gnu.2.34
CXX64 = zig c++ -target x86_64-linux-gnu.2.34
REPLAY_SRCS = tools/render64/render64_replay.c src/render_wire.c src/render_rec.c src/render_ipc.c src/mem_map.c src/anim_interp.c src/fps_overlay.c src/diaglog.c src/lamp_shadow.c \
              src/materials.c src/fsr.c src/taa.c src/auto_exposure.c src/filmic.c src/dof.c src/volfog.c
REPLAY_HOT = geom gpu_skin vsh
render64: render64-wire-check $(B)/render64_replay $(B64)/render64_replay $(B)/ipc_bench32 $(B64)/ipc_bench64 $(B)/render_wire_test $(B64)/render_wire_test
# The wire must cover renderer.h: gen_wire.py --check fails when render_wire_gen.h is stale or a
# renderer.h function has no wire rule (a pointer parameter without PAYLOAD/CODEC, a new type).
render64-wire-check:
	python3 tools/render64/gen_wire.py --check
# `make test`: the render64 wire checks (coverage, round trip in both ABIs and across them).
test: render64-wire-check $(B)/render_wire_test $(B64)/render_wire_test
	$(B)/render_wire_test > $(B)/render_wire_test.out && $(B64)/render_wire_test > $(B64)/render_wire_test.out
	$(B)/render_wire_test --write $(B)/rw32.bin > /dev/null && $(B64)/render_wire_test --read $(B)/rw32.bin > $(B64)/rw_across.out
	$(B64)/render_wire_test --write $(B64)/rw64.bin > /dev/null && $(B)/render_wire_test --read $(B64)/rw64.bin > $(B)/rw_across.out
	@echo "render64 wire: coverage current, round trip i386 / x86_64 / across OK"
$(B64):
	mkdir -p $(B64)
$(B64)/vk_renderer.o: src/renderer/vk_renderer.cpp src/renderer.h src/gamma_ramp.h src/renderer/vertex_sink.h $(B)/vk_renderer.o | $(B64)
	$(CXX64) $(CXXFLAGS) -c -o $@ $<
$(B64)/hot_%.o: src/%.c src/*.h | $(B64)
	$(CC64) $(CFLAGS) -O2 -fno-math-errno -c -o $@ $<
$(B)/render64_replay: $(REPLAY_SRCS) src/render_wire.h src/render_wire_gen.h src/*.h $(RENDERER_OBJS) $(patsubst %,$(B)/hot_%.o,$(REPLAY_HOT)) | $(B)
	$(CC) $(CFLAGS) -o $@ $(REPLAY_SRCS) $(patsubst %,$(B)/hot_%.o,$(REPLAY_HOT)) $(RENDERER_OBJS) $(PLAT) -lm $(RENDERER_LIBS)
$(B64)/render64_replay: $(REPLAY_SRCS) src/render_wire.h src/render_wire_gen.h src/*.h $(B64)/vk_renderer.o $(patsubst %,$(B64)/hot_%.o,$(REPLAY_HOT)) | $(B64)
	$(CC64) $(CFLAGS) -o $@ $(REPLAY_SRCS) $(patsubst %,$(B64)/hot_%.o,$(REPLAY_HOT)) $(B64)/vk_renderer.o tools/render64/plat_dl.c -lm $(RENDERER_LIBS)
$(B)/render_wire_test: tests/render_wire_test.c src/render_wire.c src/render_wire.h src/render_wire_gen.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/render_wire_test.c src/render_wire.c
$(B64)/render_wire_test: tests/render_wire_test.c src/render_wire.c src/render_wire.h src/render_wire_gen.h | $(B64)
	$(CC64) $(CFLAGS) -o $@ tests/render_wire_test.c src/render_wire.c
$(B)/ipc_bench32: tools/render64/ipc_bench.c | $(B)
	$(CC) -std=gnu99 -O2 -Wall -Wextra -o $@ $<
$(B64)/ipc_bench64: tools/render64/ipc_bench.c | $(B64)
	$(CC64) -std=gnu99 -O2 -Wall -Wextra -o $@ $<

# ---- Windows (cross-compiled; 32-bit, runs under WOW64 on 64-bit Windows) ----
# TheWarriors.exe: CRT-free loader fixed at 0x10000 whose image reserves the XBE range, then
# loads xbrun-core.dll (the whole runtime, src/platform/win32.c instead of posix.c). See README.
# (The Linux binary keeps the name xbrun; earlier Windows builds called the loader xbrun.exe.)
WCC = zig cc -target x86-windows-gnu
WCXX = zig c++ -target x86-windows-gnu
WCFLAGS = -std=gnu99 -O1 -Wall -Wextra -Wno-unused-parameter -fno-omit-frame-pointer -mstackrealign \
          -include src/platform/outlog.h -DXBRUN_VERSION='"$(VERSION)"' $(SA_INCLUDE)
WCXXFLAGS = -std=c++17 -O1 -Wall -Wextra -fno-exceptions -fno-rtti -fno-omit-frame-pointer -mstackrealign \
            -Wno-missing-field-initializers -isystem $(VULKAN_INCLUDE) -I$(B)
WB = $(B)/win

windows: $(B)/TheWarriors.exe $(B)/xbrun-core.dll $(B)/fspatch_test.exe $(B)/memmap_test.exe $(B)/pngraw.exe

$(WB):
	mkdir -p $(WB)

$(WB)/vk_renderer.o: src/renderer/vk_renderer.cpp src/renderer.h src/gamma_ramp.h $(B)/gamma_frag.h $(B)/output_frag.h $(B)/output_hdr_frag.h src/hdr.h src/renderer/vertex_sink.h src/ssao.h src/platform.h $(B)/geom_vert.h $(B)/geom_frag.h $(B)/glass_vert.h $(B)/glass_blur_frag.h $(B)/glass_comp_frag.h $(B)/cas_frag.h $(B)/fsr_easu_frag.h $(B)/fsr_rcas_frag.h src/fsr.h $(B)/bloom_down_frag.h $(B)/bloom_up_frag.h $(B)/bloom_comp_frag.h $(B)/filmic_frag.h $(B)/filmic_ae_frag.h $(B)/exposure_frag.h $(B)/ae_meter_frag.h $(B)/ae_adapt_frag.h $(B)/dof_focus_frag.h $(B)/dof_focus_ms_frag.h $(B)/dof_down_frag.h $(B)/dof_down_ms_frag.h $(B)/dof_gather_frag.h $(B)/dof_comp_frag.h $(B)/dof_comp_ms_frag.h $(B)/dof_copy_frag.h $(B)/rain_vert.h $(B)/rain_frag.h $(B)/ssao_ao_frag.h $(B)/ssao_ao_ms_frag.h $(B)/ssao_blur_frag.h $(B)/ssao_comp_frag.h $(B)/ssao_comp_ms_frag.h $(B)/ssao_blur_mask_frag.h $(B)/ssao_comp_ms_mask_frag.h $(B)/gtao_prefilter_comp.h $(B)/gtao_prefilter_ms_comp.h $(B)/gtao_main_comp.h $(B)/gtao_denoise_comp.h $(B)/gtao_denoise_mask_comp.h $(B)/gtao_temporal_comp.h $(B)/wet_frag.h $(B)/wet_ms_frag.h $(B)/sssr_hiz_frag.h $(B)/sssr_hiz_ms_frag.h $(B)/sssr_hiz_down_frag.h $(B)/sssr_surf_vert.h $(B)/sssr_surf_frag.h $(B)/rain_cover_vert.h $(B)/world_shadow_cut_vert.h $(B)/world_shadow_cut_frag.h $(B)/world_shadow_gate_comp.h $(B)/rain_cover_frag.h $(B)/splash_cover_frag.h $(B)/splash_cover_ms_frag.h $(B)/wet_cover_frag.h $(B)/wet_cover_ms_frag.h $(B)/char_mask_vert.h $(B)/splash_frag.h $(B)/splash_ms_frag.h $(B)/geom_soft_frag.h $(B)/geom_soft_ms_frag.h $(B)/geom_lit_vert.h $(B)/geom_lit_frag.h $(B)/geom_emit_frag.h $(B)/geom_lit_emit_frag.h $(B)/geom_lit_relief_frag.h $(B)/geom_lit_hair_frag.h $(B)/geom_lit_relief_hair_frag.h $(B)/geom_lit_skin_frag.h $(B)/geom_lit_relief_skin_frag.h $(B)/geom_lit_hair_skin_frag.h $(B)/geom_lit_relief_hair_skin_frag.h $(B)/geom_lit_mat_frag.h $(B)/geom_lit_relief_mat_frag.h $(B)/geom_lit_hair_mat_frag.h $(B)/geom_lit_relief_hair_mat_frag.h $(B)/vsh_skin_comp.h $(B)/skin_faces_comp.h $(B)/skin_normals_comp.h $(B)/sky_vert.h $(B)/sky_frag.h $(B)/skyline_frag.h src/lamp_lights.h src/volfog.h $(B)/lamp_frag.h $(B)/lamp_ms_frag.h $(B)/shafts_frag.h $(B)/shafts_ms_frag.h $(B)/volfog_polar_comp.h $(B)/volfog_inject_comp.h $(B)/volfog_integrate_comp.h $(B)/volfog_comp_frag.h $(B)/volfog_comp_ms_frag.h $(B)/contact_frag.h $(B)/contact_ms_frag.h $(B)/fps_overlay_frag.h src/fps_overlay.h $(B)/taa_resolve_frag.h $(B)/taa_resolve_ms_frag.h $(B)/taa_out_frag.h $(B)/taa_up_frag.h $(B)/taa_up_ms_frag.h $(B)/taa_out_up_frag.h $(B)/taa_vel_vert.h $(B)/taa_vel_frag.h $(B)/fg_mv_comp.h $(B)/fg_mv_ms_comp.h $(B)/fg_splat_comp.h $(B)/fg_interp_frag.h $(B)/fg_flow_comp.h $(B)/fg_hud_comp.h $(B)/fsr3_prepare_comp.h $(B)/fsr3_prepare_ms_comp.h $(B)/fsr3_pyramid_comp.h $(B)/fsr3_shading_comp.h $(B)/fsr3_reactivity_comp.h $(B)/fsr3_instability_comp.h $(B)/fsr3_accumulate_comp.h $(B)/fsr3_accumulate_ms_comp.h $(B)/fsr3_mask_frag.h $(B)/fsr3_mask_ms_frag.h src/renderer/vrs.h $(B)/vrs_rate_comp.h $(B)/taa_up_lite_frag.h src/renderer/vk_caps.h src/renderer/tex_lru.h | $(WB)
	$(WCXX) $(WCXXFLAGS) -c -o $@ $<
$(WB)/vk_renderer.o: $(SSGI_HDRS)   # --ssgi's shaders (SSGI_HDRS above)

# The per-vertex hot paths at -O2, as in the Linux build (HOT_SRCS above).
WHOT_OBJS = $(patsubst src/%.c,$(WB)/hot_%.o,$(HOT_SRCS))
$(WB)/hot_%.o: src/%.c src/*.h src/platform/outlog.h | $(WB)
	$(WCC) $(WCFLAGS) -O2 -fno-math-errno -c -o $@ $<

# main() becomes xbrun_main, called by xbrun_dll_main (win32.c) with the command line.
WPLAT = src/platform/win32.c src/platform/outlog.c
WKTX2_OBJS = $(WB)/ktx2.o $(WB)/basisu_transcoder.o $(WB)/zstddeclib.o
$(WB)/ktx2.o: src/ktx2.cpp src/ktx2.h src/texpack.h $(BASISU)/transcoder/basisu_transcoder.h | $(WB)
	$(WCXX) $(WCXXFLAGS) $(BASISU_DEFS) -c -o $@ src/ktx2.cpp
$(WB)/basisu_transcoder.o: $(BASISU)/transcoder/*.cpp $(BASISU)/transcoder/*.h $(BASISU)/transcoder/*.inl $(BASISU)/transcoder/*.inc | $(WB)
	$(WCXX) $(subst -g1,,$(BASISU_CXXFLAGS)) -c -o $@ $(BASISU)/transcoder/basisu_transcoder.cpp
$(WB)/zstddeclib.o: $(BASISU)/zstd/zstddeclib.c | $(WB)
	$(WCC) -O2 -w -mstackrealign -c -o $@ $<
$(B)/xbrun-core.dll: $(SRCS) $(WPLAT) src/platform/win32_entry.c src/platform/outlog.h src/*.h src/renderer/vrs.h src/renderer/vk_caps.h $(WHOT_OBJS) $(WB)/vk_renderer.o $(WKTX2_OBJS) $(WMANIFEST) | $(WB)
	$(WCC) $(WCFLAGS) -Dmain=xbrun_main -shared -Wl,--image-base=0x60000000 -Wl,--no-dynamicbase -o $@ \
	    $(SRCS) $(WPLAT) src/platform/win32_entry.c src/platform/asinvoker.rc $(WHOT_OBJS) $(WB)/vk_renderer.o $(WKTX2_OBJS) -lc++ -lshell32

# Game-independent checks to run on Windows (no Wine here): fs patching through the TEB slot.
# Both .exe files embed an asInvoker manifest: without one, UAC installer detection runs
# fspatch_test.exe elevated (its name contains "patch") in a console of its own.
WMANIFEST = src/platform/asinvoker.rc src/platform/asinvoker.manifest
$(B)/fspatch_test.exe: tools/fspatch_test.c src/fspatch.c src/fspatch.h $(WPLAT) src/platform/outlog.h $(WMANIFEST) | $(WB)
	$(WCC) $(WCFLAGS) -o $@ tools/fspatch_test.c src/fspatch.c $(WPLAT) src/platform/asinvoker.rc

# The texture tools' PNG decoder for Windows players (tools/texture_upscale.py find_pngraw): a plain
# console program, no platform layer, with the asInvoker manifest like the tests above.
$(B)/pngraw.exe: tools/pngraw.c $(TEXPACK_SRCS) src/texpack.h src/xtex.h $(WMANIFEST) | $(WB)
	$(WCC) -std=gnu99 -O1 -Wall -Wextra -Wno-unused-parameter -o $@ tools/pngraw.c $(TEXPACK_SRCS) src/platform/asinvoker.rc

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
	rm -rf $(B) $(B64)

.PHONY: all clean symbols windows steam-audio render64 render64-windows render64-wire-check test

# Game-independent check of the --combat-debug helpers (swept distances, frame data per move).
$(B)/combat_debug_test: tests/combat_debug_test.c src/combat_data.c src/combat_debug.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/combat_debug_test.c src/combat_data.c -lm

# render64 (see above): the IPC benchmark for Windows, i386 producer and x86_64 consumer.
render64-windows: $(WB)/ipc_bench32.exe $(WB)/ipc_bench64.exe
$(WB)/ipc_bench32.exe: tools/render64/ipc_bench.c | $(WB)
	zig cc -target x86-windows-gnu -std=gnu99 -O2 -Wall -Wextra -o $@ $<
$(WB)/ipc_bench64.exe: tools/render64/ipc_bench.c | $(WB)
	zig cc -target x86_64-windows-gnu -std=gnu99 -O2 -Wall -Wextra -o $@ $<

# ---- render64 W3: the render_ipc channel and the helper skeleton ----
# src/render_ipc.c is also part of xbrun / xbrun-core.dll (SRCS). render_ipc_test is the parent
# (i386 in build/, x86_64 in build64/), xbrun-render64 the x86_64 helper it starts. `make render-ipc`
# builds them, `make test` runs them, `make windows` also builds the Windows pair (run in W5:
# build/win/render_ipc_test.exe and build/win/x64/WarriorsRender64.exe). Own variable names so
# the render64 (W1) block can define B64 / CC64 independently.
R64IPC_CC64 = zig cc -target x86_64-linux-gnu.2.34
R64IPC_B64 = build64
R64IPC_FLAGS = $(CFLAGS) -O2
R64IPC_WFLAGS = -std=gnu99 -O2 -Wall -Wextra -Wno-unused-parameter -DXBRUN_VERSION='"$(VERSION)"'
R64IPC_DEPS = src/render_ipc.c src/render_ipc.h
# The helper: the skeleton's echo protocol, or with a window config the
# SDL window, event pump and controllers (src/video.c compiled in, its input calls written to the ring).
R64HELPER_SRCS = tools/render64/r64_helper.c tools/render64/r64_helper_io.c src/render_ipc.c src/input_ports.c src/refresh_match.c
R64HELPER_DEPS = src/video.c src/video.h src/input_map.h src/hle_input.h src/refresh_match.h src/platform.h
render-ipc: $(B)/render_ipc_test $(R64IPC_B64)/render_ipc_test $(R64IPC_B64)/xbrun-render64
$(B)/render_ipc_test: tests/render_ipc_test.c $(R64IPC_DEPS) | $(B)
	$(CC) $(R64IPC_FLAGS) -o $@ tests/render_ipc_test.c src/render_ipc.c
$(R64IPC_B64)/render_ipc_test: tests/render_ipc_test.c $(R64IPC_DEPS)
	@mkdir -p $(@D)
	$(R64IPC_CC64) $(R64IPC_FLAGS) -o $@ tests/render_ipc_test.c src/render_ipc.c
# --render-process helper: the Linux helper also renders: render_rec.c's decoder
# (render_rec_serve) into the x86_64 vk_renderer.cpp, with what render64_replay links besides (REPLAY_SRCS).
R64HELPER_RENDER_SRCS = src/mem_map.c src/anim_interp.c src/fps_overlay.c src/diaglog.c src/lamp_shadow.c src/materials.c src/fsr.c \
                        src/taa.c src/auto_exposure.c src/filmic.c src/dof.c src/volfog.c
R64HELPER_HOT = geom gpu_skin vsh render_wire render_rec
$(R64IPC_B64)/xbrun-render64: $(R64HELPER_SRCS) $(R64HELPER_DEPS) $(R64IPC_DEPS) $(R64HELPER_RENDER_SRCS) src/render_rec.h src/render_wire.h \
                              src/render_wire_gen.h $(B64)/vk_renderer.o $(patsubst %,$(B64)/hot_%.o,$(R64HELPER_HOT))
	@mkdir -p $(@D)
	$(R64IPC_CC64) $(R64IPC_FLAGS) -DR64_HELPER_RENDER -o $@ $(R64HELPER_SRCS) $(R64HELPER_RENDER_SRCS) \
	    $(patsubst %,$(B64)/hot_%.o,$(R64HELPER_HOT)) $(B64)/vk_renderer.o -ldl -lm -lpthread $(RENDERER_LIBS)
# The i386 parent with the full acceptance run (10k frames per size, crash paths), then the
# x86_64 parent briefly (the library in both builds).
render-ipc-test: render-ipc
	./$(B)/render_ipc_test --helper $(R64IPC_B64)/xbrun-render64
	./$(R64IPC_B64)/render_ipc_test --helper $(R64IPC_B64)/xbrun-render64 --frames 300 --paced 30 --no-kill
test: render-ipc-test

render-ipc-windows: $(WB)/render_ipc_test.exe $(WB)/x64/WarriorsRender64.exe
$(WB)/render_ipc_test.exe: tests/render_ipc_test.c $(R64IPC_DEPS) | $(WB)
	zig cc -target x86-windows-gnu $(R64IPC_WFLAGS) -o $@ tests/render_ipc_test.c src/render_ipc.c
# The Windows helper renders too: the Linux helper's sources and objects built for
# x86_64-windows-gnu (vk_renderer.cpp with the 32-bit build's flags and the same generated shader headers; the Vulkan
# loader is vulkan-1.dll through plat_dlopen, as in xbrun-core.dll), with the exe manifest of TheWarriors.exe
# (asInvoker, PerMonitorV2). `make render64-windows` stages its own x64 SDL2.dll next to it (the pinned 2.32.10 mingw
# development package the launcher fetches: `make -C ../launcher sdl2-devel`): a 64-bit process cannot load the game's
# 32-bit SDL2.dll. The dev layout copies both to build/x64/, where the game (build/TheWarriors.exe) looks for them.
W64B = $(WB)/x64
W64CC = zig cc -target x86_64-windows-gnu
W64CXX = zig c++ -target x86_64-windows-gnu
SDL2_DEVEL64 ?= $(HOME)/.cache/launcher/sdl/SDL2-2.32.10/x86_64-w64-mingw32
# lld-link resolves every symbol before dropping unused code: the WAD reader materials.c names (disc.c, self-contained)
# and diaglog.c's Windows writer (outlog.c: with no log file open it writes to stdout, the game's pipe).
W64HELPER_EXTRA = src/disc.c src/platform/outlog.c
$(W64B)/vk_renderer.o: src/renderer/vk_renderer.cpp $(WB)/vk_renderer.o
	@mkdir -p $(@D)
	$(W64CXX) $(WCXXFLAGS) -c -o $@ $<
$(W64B)/hot_%.o: src/%.c src/*.h
	@mkdir -p $(@D)
	$(W64CC) $(R64IPC_WFLAGS) -fno-math-errno -c -o $@ $<
$(W64B)/WarriorsRender64.exe: $(R64HELPER_SRCS) $(R64HELPER_DEPS) $(R64IPC_DEPS) $(R64HELPER_RENDER_SRCS) $(W64HELPER_EXTRA) src/render_rec.h \
                              src/render_wire.h src/render_wire_gen.h $(W64B)/vk_renderer.o $(patsubst %,$(W64B)/hot_%.o,$(R64HELPER_HOT)) $(WMANIFEST) | $(WB)
	@mkdir -p $(@D)
	$(W64CC) $(R64IPC_WFLAGS) -DR64_HELPER_RENDER -o $@ $(R64HELPER_SRCS) $(R64HELPER_RENDER_SRCS) $(W64HELPER_EXTRA) \
	    $(patsubst %,$(W64B)/hot_%.o,$(R64HELPER_HOT)) $(W64B)/vk_renderer.o src/platform/asinvoker.rc -lc++
$(W64B)/SDL2.dll: $(SDL2_DEVEL64)/bin/SDL2.dll tools/render64/pe_check.py
	@mkdir -p $(@D)
	python3 tools/render64/pe_check.py --machine x64 $< > /dev/null
	cp -f $< $@
$(B)/x64/%: $(W64B)/%
	@mkdir -p $(@D)
	cp -f $< $@
render64-windows: $(W64B)/WarriorsRender64.exe $(W64B)/SDL2.dll $(B)/x64/WarriorsRender64.exe $(B)/x64/SDL2.dll
windows: render-ipc-windows

.PHONY: render-ipc render-ipc-test render-ipc-windows test

# ---- render64 helper-io: the input region and the helper's window ----
# r64_input_ring_test: the ring protocol in both ABIs. r64_input_test: the game side (i386: src/r64_input.c
# behind video.c's helper backend, the real input_map.c / hle_input.c) with the x86_64 helper on SDL's
# dummy video driver injecting events (SKIP without an x86_64 SDL2).
R64IN_GAME_SRCS = src/r64_input.c src/video.c src/refresh_match.c src/input_map.c src/hle_input.c src/input_ports.c src/render_ipc.c
render-input: $(B)/r64_input_ring_test $(R64IPC_B64)/r64_input_ring_test $(B)/r64_input_test $(R64IPC_B64)/xbrun-render64
$(B)/r64_input_ring_test: tests/r64_input_ring_test.c $(R64IPC_DEPS) | $(B)
	$(CC) $(R64IPC_FLAGS) -o $@ tests/r64_input_ring_test.c src/render_ipc.c -lpthread
$(R64IPC_B64)/r64_input_ring_test: tests/r64_input_ring_test.c $(R64IPC_DEPS)
	@mkdir -p $(@D)
	$(R64IPC_CC64) $(R64IPC_FLAGS) -o $@ tests/r64_input_ring_test.c src/render_ipc.c -lpthread
$(B)/r64_input_test: tests/r64_input_test.c $(R64IN_GAME_SRCS) src/r64_input.h src/video.h src/input_map.h src/hle_input.h src/render_ipc.h $(PLAT) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -O2 -o $@ tests/r64_input_test.c $(R64IN_GAME_SRCS) $(PLAT) -lm
render-input-test: render-input
	./$(B)/r64_input_ring_test
	./$(R64IPC_B64)/r64_input_ring_test
	./$(B)/r64_input_test --helper $(R64IPC_B64)/xbrun-render64
test: render-input-test
.PHONY: render-input render-input-test

# ---- render64 W6: --render-process helper ----
# r64_render_test: the game side (i386, src/render_rec.c's wrappers and the i386 renderer for the reference
# frames) with the x86_64 helper rendering on SDL's dummy video driver (exit 77, a skip, without a Vulkan device or
# an x86_64 SDL2).
R64RENDER_TEST_SRCS = tests/r64_render_test.c src/render_wire.c src/render_rec.c src/render_ipc.c src/mem_map.c src/anim_interp.c \
                      src/fps_overlay.c src/diaglog.c src/lamp_shadow.c src/materials.c src/fsr.c src/taa.c src/auto_exposure.c \
                      src/filmic.c src/dof.c src/volfog.c
$(B)/r64_render_test: $(R64RENDER_TEST_SRCS) src/render_rec.h src/render_wire.h src/render_wire_gen.h src/render_ipc.h $(RENDERER_OBJS) \
                      $(patsubst %,$(B)/hot_%.o,$(REPLAY_HOT)) | $(B)
	$(CC) $(CFLAGS) -o $@ $(R64RENDER_TEST_SRCS) $(patsubst %,$(B)/hot_%.o,$(REPLAY_HOT)) $(RENDERER_OBJS) $(PLAT) -lm $(RENDERER_LIBS)
render-helper-test: $(B)/r64_render_test $(R64IPC_B64)/xbrun-render64
	./$(B)/r64_render_test --helper $(R64IPC_B64)/xbrun-render64; r=$$?; [ $$r = 0 ] || [ $$r = 77 ]
test: render-helper-test
.PHONY: render-helper-test

# ---- render64 W7: the Windows helper's shared logic, checked on Linux ----
# r64_selftest_test: --render64-selftest's code (src/r64_selftest.c, linked as the game links it) against the x86_64 helper
# (PASS, or 77 without a Vulkan device / x86_64 SDL2), a missing helper (FAIL with the reason), and the helper-output line
# splitter the Windows game uses for the helper's stdout pipe (r64_lines_*).
R64SELF_TEST_SRCS = tests/r64_selftest_test.c src/r64_selftest.c $(filter-out tests/r64_render_test.c,$(R64RENDER_TEST_SRCS))
$(B)/r64_selftest_test: $(R64SELF_TEST_SRCS) src/r64_selftest.h src/render_rec.h src/render_wire.h src/render_wire_gen.h src/render_ipc.h \
                        $(RENDERER_OBJS) $(patsubst %,$(B)/hot_%.o,$(REPLAY_HOT)) | $(B)
	$(CC) $(CFLAGS) -o $@ $(R64SELF_TEST_SRCS) $(patsubst %,$(B)/hot_%.o,$(REPLAY_HOT)) $(RENDERER_OBJS) $(PLAT) -lm $(RENDERER_LIBS)
render-selftest-test: $(B)/r64_selftest_test $(R64IPC_B64)/xbrun-render64
	./$(B)/r64_selftest_test --helper $(R64IPC_B64)/xbrun-render64
test: render-selftest-test
.PHONY: render-selftest-test
