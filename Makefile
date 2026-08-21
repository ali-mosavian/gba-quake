PROJECT := gba-affine-raster-lab
BUILD := build
ASM_OBJECTS := $(BUILD)/r_math_arm.o $(BUILD)/d_frame_arm.o \
	$(BUILD)/r_bsp_arm.o $(BUILD)/r_affine_arm.o
TESTS := bsp_textured_nowalkers bsp_textured_nofetch bsp_textured_norows bsp_textured_nospans bsp_textured_walk bsp_textured_cref baseline pa_pc xy combined32 stream32 window cube cube_affine cube_dynamic cube_wireframe bsp_wireframe bsp_textured bsp_textured_solid bsp_textured_nocoverage cube_software quad_reference quad_affine quad_affine_static quad_affine_staged
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

$(BUILD)/cube_wireframe.o: src/cube_wireframe.c src/gba_hardware.h src/generated/runtime_cube_luts.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -c $< -o $@

BSP_MAP ?= /Users/alim/work/badlogic/softquake/maps/dm1.bsp
BSP_PAK ?= /Users/alim/Downloads/Quake/id1/pak0.pak
src/generated/bsp_wireframe_map.h: scripts/extract_bsp_wireframe.py
	mkdir -p src/generated
	python3 $< $(BSP_MAP) $@ $(BSP_PAK)

$(BUILD)/bsp_wireframe.o: src/quake/r_unity.c src/quake/r_state.c src/quake/r_fixed.h src/quake/r_bsp.c src/quake/r_clip.c src/quake/d_draw.c src/quake/d_scan.c src/quake/r_surf.c src/quake/r_main.c src/gba_hardware.h src/generated/runtime_cube_luts.h src/generated/bsp_wireframe_map.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -c $< -o $@

$(BUILD)/bsp_textured.o: src/quake/r_unity.c src/quake/r_state.c src/quake/r_fixed.h src/quake/r_bsp.c src/quake/r_clip.c src/quake/d_draw.c src/quake/d_scan.c src/quake/r_surf.c src/quake/r_main.c src/gba_hardware.h src/generated/runtime_cube_luts.h src/generated/bsp_wireframe_map.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -DBSP_TEXTURED=1 -DBSP_TEXTURED_C_REFERENCE=1 -c $< -o $@

$(BUILD)/bsp_textured_walk.o: src/quake/r_unity.c src/quake/r_state.c src/quake/r_fixed.h src/quake/r_bsp.c src/quake/r_clip.c src/quake/d_draw.c src/quake/d_scan.c src/quake/r_surf.c src/quake/r_main.c src/gba_hardware.h src/generated/runtime_cube_luts.h src/generated/bsp_wireframe_map.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -DBSP_TEXTURED=1 -DBSP_TEXTURED_C_REFERENCE=1 -DBSP_AUTO_WALK=1 -DBSP_PROFILE_COUNTS=1 -c $< -o $@

$(BUILD)/bsp_textured_nospans.o: src/quake/r_unity.c src/quake/r_state.c src/quake/r_fixed.h src/quake/r_bsp.c src/quake/r_clip.c src/quake/d_draw.c src/quake/d_scan.c src/quake/r_surf.c src/quake/r_main.c src/gba_hardware.h src/generated/runtime_cube_luts.h src/generated/bsp_wireframe_map.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -DBSP_TEXTURED=1 -DBSP_TEXTURED_C_REFERENCE=1 -DBSP_TEXTURED_NO_SPANS=1 -c $< -o $@

$(BUILD)/bsp_textured_norows.o: src/quake/r_unity.c src/quake/r_state.c src/quake/r_fixed.h src/quake/r_bsp.c src/quake/r_clip.c src/quake/d_draw.c src/quake/d_scan.c src/quake/r_surf.c src/quake/r_main.c src/gba_hardware.h src/generated/runtime_cube_luts.h src/generated/bsp_wireframe_map.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -DBSP_TEXTURED=1 -DBSP_TEXTURED_C_REFERENCE=1 -DBSP_TEXTURED_NO_ROWS=1 -c $< -o $@

$(BUILD)/bsp_textured_nowalkers.o: src/quake/r_unity.c src/quake/r_state.c src/quake/r_fixed.h src/quake/r_bsp.c src/quake/r_clip.c src/quake/d_draw.c src/quake/d_scan.c src/quake/r_surf.c src/quake/r_main.c src/gba_hardware.h src/generated/runtime_cube_luts.h src/generated/bsp_wireframe_map.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -DBSP_TEXTURED=1 -DBSP_TEXTURED_C_REFERENCE=1 -DBSP_TEXTURED_NO_WALKERS=1 -c $< -o $@

$(BUILD)/bsp_textured_nofetch.o: src/quake/r_unity.c src/quake/r_state.c src/quake/r_fixed.h src/quake/r_bsp.c src/quake/r_clip.c src/quake/d_draw.c src/quake/d_scan.c src/quake/r_surf.c src/quake/r_main.c src/gba_hardware.h src/generated/runtime_cube_luts.h src/generated/bsp_wireframe_map.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -DBSP_TEXTURED=1 -DBSP_TEXTURED_C_REFERENCE=1 -DBSP_TEXTURED_NO_FETCH=1 -c $< -o $@

$(BUILD)/bsp_textured_cref.o: src/quake/r_unity.c src/quake/r_state.c src/quake/r_fixed.h src/quake/r_bsp.c src/quake/r_clip.c src/quake/d_draw.c src/quake/d_scan.c src/quake/r_surf.c src/quake/r_main.c src/gba_hardware.h src/generated/runtime_cube_luts.h src/generated/bsp_wireframe_map.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -DBSP_TEXTURED=1 -DBSP_TEXTURED_C_REFERENCE=1 -DBSP_PROFILE_COUNTS=1 -c $< -o $@

$(BUILD)/bsp_textured_solid.o: src/quake/r_unity.c src/quake/r_state.c src/quake/r_fixed.h src/quake/r_bsp.c src/quake/r_clip.c src/quake/d_draw.c src/quake/d_scan.c src/quake/r_surf.c src/quake/r_main.c src/gba_hardware.h src/generated/runtime_cube_luts.h src/generated/bsp_wireframe_map.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -marm -DBSP_TEXTURED=1 -DBSP_TEXTURED_C_REFERENCE=1 -DBSP_TEXTURED_SOLID=1 -c $< -o $@

$(BUILD)/bsp_textured_nocoverage.o: src/quake/r_unity.c src/quake/r_state.c src/quake/r_fixed.h src/quake/r_bsp.c src/quake/r_clip.c src/quake/d_draw.c src/quake/d_scan.c src/quake/r_surf.c src/quake/r_main.c src/gba_hardware.h src/generated/runtime_cube_luts.h src/generated/bsp_wireframe_map.h | $(BUILD)
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

$(BUILD)/d_span_arm.o: src/asm/d_span_arm.S | $(BUILD)
	$(CC) -mcpu=arm7tdmi -marm -mthumb-interwork -g -c $< -o $@

$(BUILD)/%.elf: $(BUILD)/%.o $(ASM_OBJECTS)
	$(CC) $(LDFLAGS) -Wl,-Map,$@.map $^ -o $@

$(BUILD)/%.gba: $(BUILD)/%.elf
	$(OBJCOPY) -O binary $< $@
	$(GBAFIX) $@ -t"AFFINE LAB" -cAFLB -m01

native: all

docker:
	./scripts/build.sh

run: $(BUILD)/$(TEST).gba
	./scripts/run.sh $(TEST)

clean:
	rm -rf $(BUILD)
