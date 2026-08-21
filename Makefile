PROJECT := gba-affine-raster-lab
BUILD := build
ASM_OBJECTS := $(BUILD)/r_math_arm.o $(BUILD)/d_frame_arm.o \
	$(BUILD)/r_bsp_arm.o $(BUILD)/r_affine_arm.o $(BUILD)/r_exact_arm.o \
	$(BUILD)/r_beam_arm.o $(BUILD)/r_beam_cpu_arm.o
TESTS := beam_xytest_sc beam_frame beam_frame_emu beam_frame_sc bsp_textured_sc floor_mode7_sc quad_affine_exact_static_sc quad_affine_exact_sc floor_mode7 bsp_textured_shape bsp_textured_nolight bsp_textured_noclip bsp_textured_noback bsp_textured_nopvs bsp_textured_nofrustum bsp_textured_nopoly bsp_textured_nograd bsp_textured_nowalkers bsp_textured_nofetch bsp_textured_norows bsp_textured_nospans bsp_textured_walk bsp_textured_cref baseline pa_pc xy combined32 stream32 window cube cube_affine cube_dynamic cube_wireframe bsp_wireframe bsp_textured bsp_textured_solid bsp_textured_nocoverage cube_software quad_reference quad_affine quad_affine_static quad_affine_staged quad_affine_exact quad_affine_exact_static
DKP_IMAGE ?= devkitpro/devkitarm:latest

DEVKITARM ?= /opt/devkitpro/devkitARM
PREFIX := $(DEVKITARM)/bin/arm-none-eabi-
CC := $(PREFIX)gcc
OBJCOPY := $(PREFIX)objcopy
GBAFIX ?= /opt/devkitpro/tools/bin/gbafix

CFLAGS := -mcpu=arm7tdmi -mthumb -mthumb-interwork -O2 -g -Wall -Wextra \
	-ffreestanding -fno-strict-aliasing -ffunction-sections -fdata-sections
LDFLAGS := -mcpu=arm7tdmi -mthumb -mthumb-interwork -specs=gba.specs \
	-Wl,--gc-sections

.SECONDARY:

.PHONY: all clean native docker run $(TESTS)
all: $(TESTS:%=$(BUILD)/%.gba)
$(TESTS): %: $(BUILD)/%.gba

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: src/main.c | $(BUILD)
	$(CC) $(CFLAGS) -DTEST_NAME=\"$*\" -DTEST_$(shell echo $* | tr a-z A-Z)=1 -c $< -o $@

src/generated/cube_frames.h: scripts/generate_cube.py
	mkdir -p src/generated
	python3 $< $@

src/generated/cube_reference_frames.h: scripts/generate_cube_reference.py scripts/generate_cube.py
	mkdir -p src/generated
	python3 $< $@

$(BUILD)/cube.o: src/cube_reference.c src/generated/cube_reference_frames.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/cube_affine.o: src/cube.c src/generated/cube_frames.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

src/generated/runtime_cube_luts.h: scripts/generate_runtime_luts.py
	mkdir -p src/generated
	python3 $< $@

$(BUILD)/cube_dynamic.o: src/cube_dynamic.c src/quad_stream.h src/gba_hardware.h src/generated/runtime_cube_luts.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -c $< -o $@

src/generated/beam_frame.h: scripts/generate_beam_frame.py src/generated/bsp_wireframe_map.h
	python3 $<

$(BUILD)/beam_frame.o: src/beam_frame.c src/gba_hardware.h src/generated/beam_frame.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -c $< -o $@

$(BUILD)/r_beam_trace_arm.o: src/asm/r_beam_arm.S | $(BUILD)
	$(CC) -mcpu=arm7tdmi -marm -mthumb-interwork -g -DBEAM_CPU_TIMER=1 -DBEAM_TRACE=1 \
	    -D'raster_beam_frame=raster_beam_frame_cpu' -c $< -o $@

$(BUILD)/beam_frame_trace.gba: $(BUILD)/beam_frame_emu.o $(BUILD)/r_beam_trace_arm.o $(BUILD)/r_math_arm.o $(BUILD)/d_frame_arm.o $(BUILD)/r_bsp_arm.o $(BUILD)/r_affine_arm.o $(BUILD)/r_exact_arm.o $(BUILD)/r_beam_arm.o
	$(CC) $(CFLAGS) -specs=gba.specs -Wl,--gc-sections -Wl,-Map,$(BUILD)/beam_frame_trace.elf.map $(BUILD)/beam_frame_emu.o $(BUILD)/r_beam_trace_arm.o $(BUILD)/r_math_arm.o $(BUILD)/d_frame_arm.o $(BUILD)/r_bsp_arm.o $(BUILD)/r_affine_arm.o $(BUILD)/r_beam_arm.o -o $(BUILD)/beam_frame_trace.elf
	$(OBJCOPY) -O binary $(BUILD)/beam_frame_trace.elf $@
	$(GBAFIX) $@ -t"AFFINE LAB" -cAFLB -m01

$(BUILD)/r_beam_cpu_arm.o: src/asm/r_beam_arm.S | $(BUILD)
	$(CC) -mcpu=arm7tdmi -marm -mthumb-interwork -g -DBEAM_CPU_TIMER=1 \
	    -D'raster_beam_frame=raster_beam_frame_cpu' -c $< -o $@

$(BUILD)/beam_frame_emu.o: src/beam_frame.c src/gba_hardware.h src/generated/beam_frame.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -D'raster_beam_frame=raster_beam_frame_cpu' -c src/beam_frame.c -o $@

$(BUILD)/beam_xytest_sc.o: src/beam_xytest.c src/gba_hardware.h src/generated/beam_frame.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -DGBA_SLOW_ROM=1 -c src/beam_xytest.c -o $@

$(BUILD)/beam_frame_sc.o: src/beam_frame.c src/gba_hardware.h src/generated/beam_frame.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -DGBA_SLOW_ROM=1 -c src/beam_frame.c -o $@

$(BUILD)/r_beam_arm.o: src/asm/r_beam_arm.S | $(BUILD)
	$(CC) -mcpu=arm7tdmi -marm -mthumb-interwork -g -c $< -o $@

src/generated/floor_plan.h: scripts/generate_floor_plan.py src/generated/bsp_wireframe_map.h
	python3 $<

$(BUILD)/floor_mode7.o: src/floor_mode7.c src/gba_hardware.h src/generated/floor_plan.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -c $< -o $@

$(BUILD)/cube_wireframe.o: src/cube_wireframe.c src/gba_hardware.h src/generated/runtime_cube_luts.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -c $< -o $@

BSP_MAP ?= /Users/alim/work/badlogic/softquake/maps/dm1.bsp
BSP_PAK ?= /Users/alim/Downloads/Quake/id1/pak0.pak
# Quake textures are authored for 320x200; this renderer draws 120x80 and
# doubles, so level 1 is already finer than the screen resolves. Level 0 is
# indistinguishable here and costs 132KB of ROM and a 32KB working set.
BSP_MIP ?= 1
# World units per lightmap luxel. dm1 as shipped is baked at 4, which is 16x
# more data than a 120x80 screen can resolve; 16 is what stock Quake used and
# what the extractor box-filters down to.
BSP_LMSCALE ?= 16
# Exposure, in percent. 100 renders the bake as authored.
BSP_LMGAIN ?= 100
# The generated header depends on the settings as much as on the scripts, and
# make cannot see a variable change. Stamp them into a file that is rewritten
# only when they actually differ, so changing BSP_MIP or BSP_LMGAIN on the
# command line rebuilds the map and leaving them alone does not.
# Coplanar merging: `simple` allows the merged polygon to be concave, which
# the crossings-based row sweep can fill. `convex` keeps the old restriction.
BSP_MERGE ?= simple
BSP_MERGE_FLAG := $(if $(filter simple,$(BSP_MERGE)),--concave-merge,)
BSP_SETTINGS := map=$(BSP_MAP) mip=$(BSP_MIP) lmscale=$(BSP_LMSCALE) lmgain=$(BSP_LMGAIN) merge=$(BSP_MERGE)
.PHONY: FORCE
FORCE:
src/generated/.bsp_settings: FORCE
	@mkdir -p src/generated
	@echo '$(BSP_SETTINGS)' | cmp -s - $@ || echo '$(BSP_SETTINGS)' > $@

src/generated/bsp_wireframe_map.h: scripts/extract_bsp_wireframe.py scripts/bsp_lightmap.py scripts/quake_palette.py src/generated/.bsp_settings
	mkdir -p src/generated
	python3 $< $(BSP_MAP) $@ $(BSP_PAK) --mip=$(BSP_MIP) --lmscale=$(BSP_LMSCALE) --lmgain=$(BSP_LMGAIN) $(BSP_MERGE_FLAG)

$(BUILD)/bsp_wireframe.o: src/quake/r_unity.c src/quake/r_state.c src/quake/r_fixed.h src/quake/r_bsp.c src/quake/r_clip.c src/quake/d_draw.c src/quake/d_scan.c src/quake/d_segment_finish.inc src/quake/r_surf.c src/quake/r_main.c src/gba_hardware.h src/generated/runtime_cube_luts.h src/generated/bsp_wireframe_map.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -c $< -o $@

$(BUILD)/bsp_textured_sc.o: src/quake/r_unity.c src/quake/r_state.c src/quake/r_fixed.h src/quake/r_bsp.c src/quake/r_clip.c src/quake/d_draw.c src/quake/d_scan.c src/quake/d_segment_finish.inc src/quake/r_surf.c src/quake/r_main.c src/gba_hardware.h src/generated/runtime_cube_luts.h src/generated/bsp_wireframe_map.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -DBSP_TEXTURED=1 -DBSP_TEXTURED_C_REFERENCE=1 -DGBA_SLOW_ROM=1 -c $< -o $@

$(BUILD)/floor_mode7_sc.o: src/floor_mode7.c src/gba_hardware.h src/generated/floor_plan.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -DGBA_SLOW_ROM=1 -c src/floor_mode7.c -o $@

$(BUILD)/quad_affine_exact_static_sc.o: src/quad_affine.c src/generated/quad_affine_frames.h | $(BUILD)
	$(CC) $(CFLAGS) -DQUAD_EXACT=1 -DQUAD_STATIC=1 -DGBA_SLOW_ROM=1 -c src/quad_affine.c -o $@

$(BUILD)/quad_affine_exact_sc.o: src/quad_affine.c src/generated/quad_affine_frames.h | $(BUILD)
	$(CC) $(CFLAGS) -DQUAD_EXACT=1 -DGBA_SLOW_ROM=1 -c src/quad_affine.c -o $@

$(BUILD)/bsp_textured.o: src/quake/r_unity.c src/quake/r_state.c src/quake/r_fixed.h src/quake/r_bsp.c src/quake/r_clip.c src/quake/d_draw.c src/quake/d_scan.c src/quake/d_segment_finish.inc src/quake/r_surf.c src/quake/r_main.c src/gba_hardware.h src/generated/runtime_cube_luts.h src/generated/bsp_wireframe_map.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -DBSP_TEXTURED=1 -DBSP_TEXTURED_C_REFERENCE=1 -c $< -o $@

$(BUILD)/bsp_textured_walk.o: src/quake/r_unity.c src/quake/r_state.c src/quake/r_fixed.h src/quake/r_bsp.c src/quake/r_clip.c src/quake/d_draw.c src/quake/d_scan.c src/quake/d_segment_finish.inc src/quake/r_surf.c src/quake/r_main.c src/gba_hardware.h src/generated/runtime_cube_luts.h src/generated/bsp_wireframe_map.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -DBSP_TEXTURED=1 -DBSP_TEXTURED_C_REFERENCE=1 -DBSP_AUTO_WALK=1 -DBSP_PROFILE_COUNTS=1 -c $< -o $@

$(BUILD)/bsp_textured_nospans.o: src/quake/r_unity.c src/quake/r_state.c src/quake/r_fixed.h src/quake/r_bsp.c src/quake/r_clip.c src/quake/d_draw.c src/quake/d_scan.c src/quake/d_segment_finish.inc src/quake/r_surf.c src/quake/r_main.c src/gba_hardware.h src/generated/runtime_cube_luts.h src/generated/bsp_wireframe_map.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -DBSP_TEXTURED=1 -DBSP_TEXTURED_C_REFERENCE=1 -DBSP_TEXTURED_NO_SPANS=1 -c $< -o $@

$(BUILD)/bsp_textured_norows.o: src/quake/r_unity.c src/quake/r_state.c src/quake/r_fixed.h src/quake/r_bsp.c src/quake/r_clip.c src/quake/d_draw.c src/quake/d_scan.c src/quake/d_segment_finish.inc src/quake/r_surf.c src/quake/r_main.c src/gba_hardware.h src/generated/runtime_cube_luts.h src/generated/bsp_wireframe_map.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -DBSP_TEXTURED=1 -DBSP_TEXTURED_C_REFERENCE=1 -DBSP_TEXTURED_NO_ROWS=1 -c $< -o $@

$(BUILD)/bsp_textured_nofrustum.o: src/quake/r_unity.c src/quake/r_state.c src/quake/r_fixed.h src/quake/r_bsp.c src/quake/r_clip.c src/quake/p_move.c src/quake/d_draw.c src/quake/d_scan.c src/quake/d_segment_finish.inc src/quake/r_surf.c src/quake/r_main.c src/gba_hardware.h src/generated/runtime_cube_luts.h src/generated/bsp_wireframe_map.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -DBSP_TEXTURED=1 -DBSP_TEXTURED_C_REFERENCE=1 -DBSP_PROFILE_COUNTS=1 -DBSP_NO_FRUSTUM_CULL=1 -c $< -o $@

$(BUILD)/bsp_textured_nopvs.o: src/quake/r_unity.c src/quake/r_state.c src/quake/r_fixed.h src/quake/r_bsp.c src/quake/r_clip.c src/quake/p_move.c src/quake/d_draw.c src/quake/d_scan.c src/quake/d_segment_finish.inc src/quake/r_surf.c src/quake/r_main.c src/gba_hardware.h src/generated/runtime_cube_luts.h src/generated/bsp_wireframe_map.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -DBSP_TEXTURED=1 -DBSP_TEXTURED_C_REFERENCE=1 -DBSP_PROFILE_COUNTS=1 -DBSP_NO_PVS=1 -c $< -o $@

$(BUILD)/bsp_textured_noback.o: src/quake/r_unity.c src/quake/r_state.c src/quake/r_fixed.h src/quake/r_bsp.c src/quake/r_clip.c src/quake/p_move.c src/quake/d_draw.c src/quake/d_scan.c src/quake/d_segment_finish.inc src/quake/r_surf.c src/quake/r_main.c src/gba_hardware.h src/generated/runtime_cube_luts.h src/generated/bsp_wireframe_map.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -DBSP_TEXTURED=1 -DBSP_TEXTURED_C_REFERENCE=1 -DBSP_PROFILE_COUNTS=1 -DBSP_NO_BACKFACE_CULL=1 -c $< -o $@

$(BUILD)/bsp_textured_noclip.o: src/quake/r_unity.c src/quake/r_state.c src/quake/r_fixed.h src/quake/r_bsp.c src/quake/r_clip.c src/quake/p_move.c src/quake/d_draw.c src/quake/d_scan.c src/quake/d_segment_finish.inc src/quake/r_surf.c src/quake/r_main.c src/gba_hardware.h src/generated/runtime_cube_luts.h src/generated/bsp_wireframe_map.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -DBSP_TEXTURED=1 -DBSP_TEXTURED_C_REFERENCE=1 -DBSP_PROFILE_COUNTS=1 -DBSP_NO_CLIP=1 -c $< -o $@

$(BUILD)/bsp_textured_nopoly.o: src/quake/r_unity.c src/quake/r_state.c src/quake/r_fixed.h src/quake/r_bsp.c src/quake/r_clip.c src/quake/d_draw.c src/quake/d_scan.c src/quake/d_segment_finish.inc src/quake/r_surf.c src/quake/r_main.c src/gba_hardware.h src/generated/runtime_cube_luts.h src/generated/bsp_wireframe_map.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -DBSP_TEXTURED=1 -DBSP_TEXTURED_C_REFERENCE=1 -DBSP_TEXTURED_NO_POLYGON=1 -c $< -o $@

$(BUILD)/bsp_textured_nograd.o: src/quake/r_unity.c src/quake/r_state.c src/quake/r_fixed.h src/quake/r_bsp.c src/quake/r_clip.c src/quake/d_draw.c src/quake/d_scan.c src/quake/d_segment_finish.inc src/quake/r_surf.c src/quake/r_main.c src/gba_hardware.h src/generated/runtime_cube_luts.h src/generated/bsp_wireframe_map.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -DBSP_TEXTURED=1 -DBSP_TEXTURED_C_REFERENCE=1 -DBSP_TEXTURED_NO_GRADIENTS=1 -c $< -o $@

$(BUILD)/bsp_textured_nowalkers.o: src/quake/r_unity.c src/quake/r_state.c src/quake/r_fixed.h src/quake/r_bsp.c src/quake/r_clip.c src/quake/d_draw.c src/quake/d_scan.c src/quake/d_segment_finish.inc src/quake/r_surf.c src/quake/r_main.c src/gba_hardware.h src/generated/runtime_cube_luts.h src/generated/bsp_wireframe_map.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -DBSP_TEXTURED=1 -DBSP_TEXTURED_C_REFERENCE=1 -DBSP_TEXTURED_NO_WALKERS=1 -c $< -o $@

$(BUILD)/bsp_textured_nofetch.o: src/quake/r_unity.c src/quake/r_state.c src/quake/r_fixed.h src/quake/r_bsp.c src/quake/r_clip.c src/quake/d_draw.c src/quake/d_scan.c src/quake/d_segment_finish.inc src/quake/r_surf.c src/quake/r_main.c src/gba_hardware.h src/generated/runtime_cube_luts.h src/generated/bsp_wireframe_map.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -DBSP_TEXTURED=1 -DBSP_TEXTURED_C_REFERENCE=1 -DBSP_TEXTURED_NO_FETCH=1 -c $< -o $@

$(BUILD)/bsp_textured_shape.o: src/quake/r_unity.c src/quake/r_state.c src/quake/r_fixed.h src/quake/r_bsp.c src/quake/r_clip.c src/quake/d_draw.c src/quake/d_scan.c src/quake/d_segment_finish.inc src/quake/r_surf.c src/quake/r_main.c src/gba_hardware.h src/generated/runtime_cube_luts.h src/generated/bsp_wireframe_map.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -DBSP_TEXTURED=1 -DBSP_TEXTURED_C_REFERENCE=1 -DBSP_PROFILE_COUNTS=1 -DBSP_PROFILE_SPAN_SHAPE=1 -c $< -o $@

$(BUILD)/bsp_textured_nolight.o: src/quake/r_unity.c src/quake/r_state.c src/quake/r_fixed.h src/quake/r_bsp.c src/quake/r_clip.c src/quake/d_draw.c src/quake/d_scan.c src/quake/d_segment_finish.inc src/quake/r_surf.c src/quake/r_main.c src/gba_hardware.h src/generated/runtime_cube_luts.h src/generated/bsp_wireframe_map.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -DBSP_TEXTURED=1 -DBSP_TEXTURED_C_REFERENCE=1 -DBSP_TEXTURED_NO_LIGHT=1 -c $< -o $@

$(BUILD)/bsp_textured_cref.o: src/quake/r_unity.c src/quake/r_state.c src/quake/r_fixed.h src/quake/r_bsp.c src/quake/r_clip.c src/quake/d_draw.c src/quake/d_scan.c src/quake/d_segment_finish.inc src/quake/r_surf.c src/quake/r_main.c src/gba_hardware.h src/generated/runtime_cube_luts.h src/generated/bsp_wireframe_map.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -DBSP_TEXTURED=1 -DBSP_TEXTURED_C_REFERENCE=1 -DBSP_PROFILE_COUNTS=1 -c $< -o $@

$(BUILD)/bsp_textured_solid.o: src/quake/r_unity.c src/quake/r_state.c src/quake/r_fixed.h src/quake/r_bsp.c src/quake/r_clip.c src/quake/d_draw.c src/quake/d_scan.c src/quake/d_segment_finish.inc src/quake/r_surf.c src/quake/r_main.c src/gba_hardware.h src/generated/runtime_cube_luts.h src/generated/bsp_wireframe_map.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -DBSP_TEXTURED=1 -DBSP_TEXTURED_C_REFERENCE=1 -DBSP_TEXTURED_SOLID=1 -c $< -o $@

$(BUILD)/bsp_textured_nocoverage.o: src/quake/r_unity.c src/quake/r_state.c src/quake/r_fixed.h src/quake/r_bsp.c src/quake/r_clip.c src/quake/d_draw.c src/quake/d_scan.c src/quake/d_segment_finish.inc src/quake/r_surf.c src/quake/r_main.c src/gba_hardware.h src/generated/runtime_cube_luts.h src/generated/bsp_wireframe_map.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -DBSP_TEXTURED=1 -DBSP_TEXTURED_C_REFERENCE=1 -DBSP_TEXTURED_NO_COVERAGE=1 -c $< -o $@

src/generated/software_cube_data.h: scripts/generate_software_cube.py
	mkdir -p src/generated
	python3 $< $@

$(BUILD)/cube_software.o: src/cube_software.c src/generated/software_cube_data.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -c $< -o $@

src/generated/quad_affine_frames.h: scripts/generate_quad.py
	mkdir -p src/generated
	python3 $< affine $@

src/generated/quad_reference_frames.h: scripts/generate_quad.py
	mkdir -p src/generated
	python3 $< reference $@

$(BUILD)/quad_affine.o: src/quad_affine.c src/generated/quad_affine_frames.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/quad_affine_static.o: src/quad_affine.c src/generated/quad_affine_frames.h | $(BUILD)
	$(CC) $(CFLAGS) -DQUAD_STATIC=1 -c $< -o $@

$(BUILD)/quad_affine_staged.o: src/quad_affine.c src/generated/quad_affine_frames.h | $(BUILD)
	$(CC) $(CFLAGS) -DQUAD_STAGED=1 -c $< -o $@

$(BUILD)/quad_affine_exact.o: src/quad_affine.c src/generated/quad_affine_frames.h | $(BUILD)
	$(CC) $(CFLAGS) -DQUAD_EXACT=1 -c $< -o $@

$(BUILD)/quad_affine_exact_static.o: src/quad_affine.c src/generated/quad_affine_frames.h | $(BUILD)
	$(CC) $(CFLAGS) -DQUAD_EXACT=1 -DQUAD_STATIC=1 -c $< -o $@

$(BUILD)/quad_reference.o: src/quad_reference.c src/generated/quad_reference_frames.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/r_math_arm.o: src/asm/r_math_arm.S | $(BUILD)
	$(CC) -mcpu=arm7tdmi -marm -mthumb-interwork -g -c $< -o $@

$(BUILD)/d_frame_arm.o: src/asm/d_frame_arm.S | $(BUILD)
	$(CC) -mcpu=arm7tdmi -marm -mthumb-interwork -g -c $< -o $@

$(BUILD)/r_bsp_arm.o: src/asm/r_bsp_arm.S | $(BUILD)
	$(CC) -mcpu=arm7tdmi -marm -mthumb-interwork -g -c $< -o $@

$(BUILD)/r_affine_arm.o: src/asm/r_affine_arm.S | $(BUILD)
	$(CC) -mcpu=arm7tdmi -marm -mthumb-interwork -g -c $< -o $@

$(BUILD)/r_exact_arm.o: src/asm/r_exact_arm.S | $(BUILD)
	$(CC) -mcpu=arm7tdmi -marm -mthumb-interwork -g -c $< -o $@

$(BUILD)/d_span_arm.o: src/asm/d_span_arm.S | $(BUILD)
	$(CC) -mcpu=arm7tdmi -marm -mthumb-interwork -g -c $< -o $@

$(BUILD)/%.elf: $(BUILD)/%.o $(ASM_OBJECTS)
	$(CC) $(LDFLAGS) -Wl,-Map,$@.map $^ -o $@

$(BUILD)/%.gba: $(BUILD)/%.elf
	$(OBJCOPY) -O binary $< $@
	$(GBAFIX) $@ -t"AFFINE LAB" -cAFLB -m01

native: all

# Read one variable back out, for scripts/build.sh.
print-%:
	@echo '$($*)'

docker:
	./scripts/build.sh

run: $(BUILD)/$(TEST).gba
	./scripts/run.sh $(TEST)

clean:
	rm -rf $(BUILD)
