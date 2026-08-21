# GBA affine raster lab

A deliberately small set of ROMs for testing affine-background raster racing,
plus an exact rotating-cube reference and an explicitly experimental affine path.

## Reading the working experiment

The staged rotating quad is the hardware-validation path. Its code is split by
responsibility:

- `src/quad_affine.c` configures BG2, builds the checker texture, selects an
  immutable animation frame, and calls the raster routine.
- `src/quad_stream.h` documents the generated scanline command format. Its
  compile-time checks protect the layout consumed by assembly.
- `src/asm/r_affine_arm.S`, function `raster_staged_frame`, copies one scanline from ROM
  to IWRAM during HBlank and issues the remaining commands during HDraw.
- `scripts/generate_quad.py` performs the offline projection, pixel-center
  coverage, sub-texel correction, and <=32-pixel subdivision.
- `src/generated/` contains generated data, not hand-maintained source.

The other ROMs are isolated experiments and failed/reference paths retained so
results remain reproducible; they are not all parts of one renderer.
The `quad_affine*` names are intentionally historical: those ROMs validate one
affine textured quad. The interactive renderer target is `cube_dynamic`.

## Environment

- Compiler: devkitARM GBA startup/linker support, with no libgba calls.
- Reproducible build: official `devkitpro/devkitarm:latest` container.
- Emulator: mGBA 0.10.5 or newer.
- Target: ARM7TDMI at 16.78 MHz; one pixel is four CPU cycles, so 32 pixels are
  nominally 128 cycles.

On macOS, build the ROMs and the local command-line mGBA frontend:

```sh
brew install cmake sdl2-compat libzip
open -a Docker
./scripts/setup-mgba-cli.sh
./scripts/build.sh
./scripts/run.sh baseline
```

`setup-mgba-cli.sh` checks out the fixed mGBA 0.10.5 tag under the ignored
`work/` directory and builds its lightweight SDL frontend without Qt or OpenGL.
`run.sh` prefers that binary, so a ROM is passed directly to the emulator:

```sh
./scripts/run.sh quad_affine
# Equivalent raw invocation:
work/mgba-0.10.5/build-sdl/sdl/mgba build/quad_affine.gba
```

Pass a second argument to scale the emulator window, for example:

```sh
./scripts/run.sh quad_affine_staged 3
```

The emulator window closes cleanly with Ctrl-C in the launching terminal.

If native devkitPro is installed at `/opt/devkitpro`, `build.sh` uses it instead.
The supported devkitPro install is its pacman installer plus `gba-dev`.

## ROMs and one-variable tests

| ROM | Change made during visible scanline | Expected if assumption holds |
|---|---|---|
| `baseline` | none | stable 16-pixel checkerboard blocks |
| `pa_pc` | only BG2PA/BG2PC | slope changes affect later pixels of that line |
| `xy` | only BG2X/BG2Y | source point jumps on the current line |
| `combined32` | PA/PC and X/Y from immediate ARM values | independently restarted 32-pixel pieces |
| `stream32` | same commands, timed from EWRAM by IWRAM ARM | stable boundaries, no line-to-line drift |
| `window` | combined test behind WIN0 | updates remain stable; only x=40..199, y=24..135 is visible |
| `cube` | exact precomputed Mode 4 frames | clean correctness reference with page flipping |
| `cube_affine` | variable face-aligned affine commands | unsupported experiment; visible corruption remains |
| `cube_dynamic` | runtime geometry controlled by GBA buttons | double-buffered interactive affine cube experiment |
| `cube_wireframe` | runtime transform/project + Mode 4 lines | stable interactive geometry/input baseline |
| `bsp_wireframe` | extracted Quake 1 BSP face edges | first-person `dm1` neighborhood baseline |
| `cube_software` | Mode 4 fixed-point triangle rasterizer | stable perspective-correct software-rendered cube |
| `quad_reference` | exact precomputed Mode 4 plane | synchronized correctness reference for one rotating surface |
| `quad_affine` | <=32-pixel affine pieces on the same plane | temporal experiment; currently flickers |
| `quad_affine_static` | one frozen affine command frame | separates raster instability from animation updates |
| `quad_affine_staged` | frozen quad with HBlank/IWRAM line staging | isolates visible EWRAM reads and late window setup |

## Isolated rotating textured quad

The two quad ROMs are generated from the same 24 camera-space poses and the
same ray/plane intersection code. They advance at the same rate:

```sh
./scripts/run.sh quad_reference
./scripts/run.sh quad_affine
```

`quad_reference` draws exact perspective-correct Mode 4 frames.
`quad_affine` converts every visible scanline into balanced pieces no longer
than 32 pixels, evaluates perspective-correct UVs at each piece's endpoints,
and sends X/Y plus PA/PC commands through the existing EWRAM-to-IWRAM raster
routine. There is no runtime geometry, division, or reciprocal math in either
ROM, keeping this an experiment in display behavior rather than renderer speed.

Still screenshots initially appeared coherent, but live observation shows
substantial flicker. The screenshots cannot validate temporal stability and the
animated affine test is therefore **not passed**. Captures are
`observations/quad-reference-cli.png`, `quad-affine-cli-1.png`, and
`quad-affine-cli-2.png`.

`quad_affine_static` freezes pose 3 and consumes the same EWRAM command frame on
every display frame. Live observation still showed intermittent flicker. The
fault is therefore in scanline/window scheduling rather than command generation,
animation cadence, or command-buffer publication. The polling-based 32-pixel
technique is not considered viable as a renderer. A cube
scanline can cross face boundaries, where one affine BG plus one rectangular
window cannot independently mask and restart overlapping faces. The next safe
renderer experiment is two non-overlapping faces with explicit per-span masks;
the next practical renderer remains a conventional Mode 4 software rasterizer.

`quad_affine_staged` is the animated follow-up scheduler experiment. During each HBlank it
copies the next 196-byte line record into IWRAM and installs that line's WIN0H
and initial affine command. During HDraw all remaining command reads come from
IWRAM. Animation selects an immutable ROM frame by pointer during VBlank; there
is no full-frame EWRAM copy. This
intermediate test intentionally retains timer polling so its live stability can
isolate staging/window/frame-publication timing before replacing the timer with
a calibrated fixed-cycle schedule.

The quad generator applies explicit sub-pixel and sub-texel correction. Polygon
coverage and perspective rays are evaluated at screen pixel centers
`(x+0.5,y+0.5)`, which also pre-steps UV from the true fractional edge to the
first covered center. Each quantized affine segment is re-anchored at that
sample. Texel conversion uses direction-dependent nearest rounding: add 128 in
24.8 for a non-negative texture gradient, or 127 for a negative gradient. This
implements `floor(C+1/2)` versus `ceil(C-1/2)` and prevents rotation-dependent
tie changes when a texture axis reverses.

## Rotating textured cube

### Interactive runtime-generated cube

Before testing texture mapping, validate the complete runtime geometry path:

```sh
./scripts/run.sh cube_wireframe 3
```

`cube_wireframe` transforms and projects all eight vertices every frame, draws
all twelve edges into the hidden Mode 4 page, and flips pages only at VBlank.
It uses the same D-pad, A/B, Start, and Select controls listed below. The four
near-face edges are yellow; the remaining edges are white, making vertex/edge
topology mistakes obvious. This is the baseline to preserve while textured
rendering is reintroduced one face at a time.

### Quake BSP wireframe

```sh
./scripts/run.sh bsp_wireframe 3
```

This ROM embeds compact rendering and visibility lumps from the local SoftQuake
`dm1.bsp`. `scripts/extract_bsp_wireframe.py` converts Quake BSP version 29 into
ROM arrays containing 3,306 vertices, 5,688 edges, 2,283 faces, 498 leaves,
nodes, planes, surfedges, marksurfaces, and the original compressed PVS. It does
not include textures, lightmaps, entity behavior, or collision hulls.

Controls:

- D-pad left/right: turn.
- D-pad up/down: walk forward/backward.
- A/B: strafe left/right.
- Start: reset to the BSP player spawn.

Each frame traverses nodes to find the camera leaf. The Quake RLE PVS and unique
candidate-face list are rebuilt only when that leaf changes. Per-frame culling
appends surviving surfedges and vertices directly to compact lists, with no
full-map face or edge scan. Vertices are batch-transformed and projected once.
Edges crossing the camera are clipped to an eight-unit near plane and then to a
120x80 logical viewport, which is expanded to 240x160 during the page flip.
Wireframe still shows edges through nearer faces because it has no depth buffer;
that is distinct from PVS, which removes geometry in non-visible leaves.

```sh
./scripts/run.sh cube_dynamic 3
```

`cube_dynamic` does not replay generated poses. At runtime it rotates and
projects eight vertices, culls cube faces, scan-converts their convex spans,
evaluates perspective planes at <=32-pixel piece endpoints, and builds the next
`QuadFrame` in EWRAM. The completed frame is published atomically while the
existing IWRAM routine displays the previous frame at 60 Hz.

Controls:

- D-pad: rotate around X and Y.
- A/B: zoom in/out.
- Start: toggle automatic Y rotation.
- Select: reset the pose and zoom.

Generation is restricted by the actual VCOUNT deadline and stops two scanlines
before VBlank ends. Thus a complex pose may take more than one display frame to
finish, but an incomplete command buffer is never published. Per-face
perspective planes and a reciprocal LUT remove divisions from span endpoint
sampling; only vertex projection, plane setup, and polygon-edge intersections
use integer division.

Face changes expose the remaining hardware limitation: BG2PA, BG2PC, BG2X, and
BG2Y are four separate writes, not one atomic mapping update. Commands are issued
four pixels early so the reference-point writes land near the intended edge,
which greatly reduces colored wedges, but moving shared edges still require live
emulator and real-hardware scrutiny.

Build and run it with:

```sh
./scripts/build.sh
./scripts/run.sh cube
```

`cube` is the correctness baseline. `scripts/generate_cube_reference.py`
ray-casts 24 perspective-correct 240x160 indexed frames. The ROM DMA-copies the
next frame to the hidden Mode 4 page during VBlank and flips pages afterward.
It is stable at about 60 fps in mGBA 0.10.5, with continuous faces and no tearing
or streaks. Evidence is in `observations/cube-reference-mgba.jpg` and
`observations/cube-reference-mgba-2.jpg`.

Run the failed affine experiment separately:

```sh
./scripts/run.sh cube_affine
```

`cube_affine` evaluates perspective-correct UV endpoints, restarts at cube-face
edges, subdivides longer runs to <=32 pixels, copies a 31,360-byte command frame
to EWRAM during VBlank, and consumes it from IWRAM. It still exhibits visible
streaks, unstable scanlines, coarse warping, and occasional face corruption.
The earlier screenshots are retained as `observations/cube-affine-mgba*.jpg`.

The clean Mode 4 result proves that the geometry, visibility, atlas, and UV data
are correct. It does **not** validate the affine cube technique. The remaining
fault is in variable mid-scanline register/window scheduling and in the inability
of one affine BG mapping to represent arbitrary face transitions without tighter
masking. Do not use `cube_affine` as a renderer baseline.

### Stable Mode 4 software experiment

```sh
./scripts/run.sh cube_software
```

`cube_software` removes all visible-period register racing. It renders indexed
pixels into the hidden Mode 4 page and changes the display page only during
VBlank. The first conservative version samples at 120x80 and expands each sample
to a 2x2 block. It uses integer edge functions, a 19.2 KB EWRAM inverse-depth
buffer, perspective-correct `u/z`, `v/z`, and `1/z`, and a generated reciprocal
LUT for the final texture-coordinate divide.

The 48 rotated/projected vertex poses are precomputed to keep this experiment
focused on rasterization. Triangle coverage, depth testing, perspective texture
lookup, framebuffer writes, and page flipping happen on the GBA CPU. The initial
prototype used three compiler divisions per pixel and was unusably slow. The
current inner loop instead incrementally steps generated fixed-point `1/z`,
`u/z`, and `v/z` planes and uses a reciprocal LUT; there is no per-pixel divide.
Sub-pixel edge-on triangles are discarded to keep their plane coefficients in
safe 32-bit ranges.

Observed in SDL mGBA 0.10.5: a coherent rotating textured cube with stable page
flips and none of the affine path's scanline streaks. The visible 2x2 pixels and
coarse checker texture are expected. Evidence is in
`observations/cube-software-cli-2.png` and `cube-software-cli-3.png`.

### Quake BSP wireframe experiment

```sh
./scripts/run.sh bsp_wireframe 3
```

This ROM packages the complete Quake `dm1.bsp`. It caches a compact PVS-derived
candidate-face list and contiguous runtime faces for the current leaf, builds
compact unique edge and vertex lists each frame, and never scans every map face
or edge. Map vertices and edges are copied to EWRAM once at startup. Hot culling,
edge processing, and line code is compiled as ARM code in IWRAM. A hand-written
ARM batch loop fuses vertex transformation, reciprocal projection, and outcode
generation so every active vertex is read once. Normal projection and clipping
contain no compiler software division.

Rendering occurs in a 9,600-byte 120x80 EWRAM framebuffer. Specialized
horizontal, vertical, shallow, and steep line loops write ordinary EWRAM bytes;
an ARM loop expands these to the hidden 240x160 Mode 4 page before the VBlank
flip. This replaces scattered VRAM read-modify-write plotting.

BSP projection retains 32-bit screen coordinates until an edge has been clipped
inside the logical viewport. This is required because near-plane endpoints can
project beyond the signed 16-bit range; narrowing earlier makes those values
wrap to the opposite side of the screen. Variable clipping ratios use an
unsaturated 32-bit Q24 reciprocal table.

Each edge is classified from its endpoint outcodes before clipping. Fully visible
edges go straight to the rasterizer, edges sharing an outside plane are rejected,
and only genuine boundary crossings run the near/screen intersection code. The
edge is drawn immediately, so there is no full clipped-edge queue.

Steady-state spawn profiling in SDL mGBA 0.10.5 after the 30 FPS pass measured:

```text
logical clear                         26,471 cycles
leaf lookup / cached PVS               7,690
face cull / edge+vertex lists        125,611
fused transform/project/outcodes      77,788
edge classify/clip/draw              170,522
paired-pixel 2x expansion             96,568
total                                504,698 cycles
```

This is about 33.2 newly rendered frames per second at the profiled spawn view,
below the 559K-cycle 30 FPS budget and about 10.2x faster than the original
5.16M-cycle implementation. The frame contains 746 candidate faces, 122 accepted
faces, 509 unique edges, 403 unique vertices, and 425 drawn edges. Of the edge
tests, 401 are trivial accepts, 81 are trivial rejects, four cross the near
plane, and 27 require screen clipping. Counts differ slightly from the old
reference because the optimized face test now uses an exact 64-bit plane dot
product instead of the old overflowing 32-bit intermediate.

The ELF uses about 5.2 KB of the 32 KB IWRAM and 221.6 KB of the 256 KB EWRAM.
The final map contains no compiler division helper. Spawn, rotated, and moved
views were checked in mGBA without near-plane wrap. Movement can enter solid
brushes because this rendering experiment deliberately has no collision. The
wireframe also has no hidden-line removal, so background edges remain visible
through foreground surfaces.

A leaf transition synchronously rebuilds the PVS/runtime-face cache and can add
a one-frame cost; transition worst-case timing has not yet been exhaustively
measured. Performance is view-dependent, and the timing/register behavior still
needs confirmation on a physical GBA. The steady-state 504.7K result leaves only
about 54K cycles below the strict 30 FPS limit, rather than the desired 500K
game/audio margin.

### Experimental 32-pixel perspective texture spans

```sh
./scripts/run.sh bsp_textured 3
```

The BSP renderer uses a Quake-style unity source layout under `src/quake/`:

- `r_state.c` owns renderer types, generated data and working memory.
- `r_bsp.c` owns BSP/PVS traversal and visible geometry construction.
- `r_clip.c` owns projected-edge clipping.
- `r_surf.c` owns textured surface preparation and submission.
- `r_main.c` owns input, frame scheduling and profiling.
- `d_draw.c` contains low-level wireframe drawing.
- `d_scan.c` contains texture-plane math and the C scan-converter reference.
- `d_polyset_arm.inc` is the production ARM textured-polygon routine.
- `r_fixed.h` names Q8/Q14/Q16 arithmetic and provides compile-time
  `TO_F8`, `TO_F16` and `TO_F32` float-to-fixed conversions.

`r_unity.c` includes these modules into one translation unit. This preserves
local assembly symbols, generated map data ownership and cross-module optimization
without returning to one oversized source file.

The standalone ARM assembly is split by ownership under `src/asm/`:

- `r_math_arm.S` contains projection and fixed-point helpers.
- `d_frame_arm.S` contains framebuffer clearing and 2× expansion.
- `r_bsp_arm.S` contains BSP plane distance and batched transform/project.
- `r_affine_arm.S` contains the older affine raster-racing experiments.

Because these are separate objects, the linker can now discard the complete
legacy affine module from the BSP ROM. Textured BSP IWRAM use consequently falls
from about 7.45KB to about 5.98KB.

`bsp_textured` preserves the wireframe ROM and adds a separate filled-face test.
It triangulates accepted convex BSP faces, clips them against the near plane,
keeps a 120x80 inverse-depth buffer, and maps DM1's embedded miptex data using
the BSP texinfo axes and the original Quake palette. Texels stay in ROM; only
compact descriptors and runtime geometry consume RAM. On every triangle scanline,
perspective-correct `u` and `v` are evaluated at boundaries no more than 32
logical pixels apart; pixels inside each piece use affine `du/dx` and `dv/dx`.
Non-power-of-two source textures are padded offline so wrapping remains a mask.

The textured ELF contains no compiler division helper. Plane/edge setup uses a
normalized reciprocal-table quotient, short spans use reciprocal constants, and
perspective correction uses the existing reciprocal LUT. Faces are scan-converted
as convex polygons rather than triangle fans. Their near-to-far BSP order is built
once when the camera enters a leaf; ordinary frames use first-hit coverage and can
discard already covered spans without a depth buffer. The ARM span kernel runs
from IWRAM.

At the DM1 spawn, SDL mGBA 0.10.5 now measures approximately 1,415,166 cycles per
newly rendered frame, or 11.85 FPS. This is 8.2x faster than the first native-texture
version (11,605,622 cycles, 1.45 FPS), while retaining the same miptex data,
near-plane clipping, 120x80 output and 32-pixel perspective correction. The stable
breakdown is approximately 123K face culling/list construction, 78K fused vertex
transform/projection, 1.110M textured polygon rendering, 96.6K expansion, 26.5K
clear, and the remaining timer/cache overhead. Scanline edge positions, plane row
origins, and the three perspective-plane values at successive 32-pixel boundaries
now advance by addition instead of being multiplied from their origins repeatedly.
That milestone saved about 37.3K cycles (2.4%) with unchanged sampling.

A 32KB EWRAM hot-texture cache is populated lazily in near-to-far draw order.
At spawn it fills completely and reduces the frame by a further 111K cycles (7.3%),
confirming that scattered Game Pak ROM texel reads were a substantial cost. Cached
textures are byte-identical copies; textures that do not fit continue to sample ROM,
so cache capacity affects speed rather than correctness. The cache currently has no
eviction policy, making this a controlled proof of value rather than the final
leaf-aware cache design. EWRAM use is now about 258.2KB, leaving only about 3.9KB.

A linear EWRAM span-command buffer plus IWRAM ARM consumer was measured and
reverted: it reached only about 1.555M cycles because creating and later rereading
the commands outweighed the cheaper inner loop. A four-word-per-row coverage mask
was also reverted after increasing the frame to about 1.73M cycles, largely from
fragmenting runs at word boundaries. Paired logical-framebuffer stores measured
about 1.560M cycles and were reverted for failing the 1% retention threshold. The
software path remains well above the 559K-cycle 30 FPS budget; the next useful
target is reducing polygon/gradient setup or testing a PPU-assisted representation,
not moving the same per-pixel work through another EWRAM queue.

Pre-scaling `v` and `dv` by the power-of-two texture-width shift before each span
was also measured. Although it removes the texel-row multiply from the pixel loop,
the generated ARM loop increased to about 1.442M cycles versus 1.415M for the
multiply form. ARM7TDMI multiplication terminates early for the small 16/64/128
width operands, while the pre-scaled form increases masking and register pressure,
so the original multiply was retained.

A runtime-generated complete mip-3 set was tested in 2,756 bytes of EWRAM. The
best version kept texture coordinates in their original units and selected mip-3
texels with an immediate shift, but still measured about 1.428M cycles versus
1.415M for the 32KB full-resolution cache. Mip 3 recovers roughly 30KB of EWRAM
and reduces visual detail, but does not reduce pixel/span count; it was reverted
because this experiment is currently optimizing frame time.

Two diagnostic ROMs isolate the current textured inner loop. At spawn,
`bsp_textured_solid` measures about 1.015M cycles when it retains scan conversion,
perspective and coverage but replaces texel addressing/loading with a constant
color. `bsp_textured_nocoverage` measures about 1.309M cycles while sampling every
polygon pixel without testing or updating coverage. These results put roughly
400K cycles in texel addressing/loading and at least 100K in byte coverage.

A hand-written IWRAM ARM texel/coverage kernel was then tested with one call per
32-pixel piece. It measured about 1.443M cycles, 27K slower than the 1.415M C
reference: building eleven arguments and saving registers roughly one thousand
times per frame outweighed its tighter pixel loop. It was removed from production.
Any further assembly version must consume a whole polygon scanline or polygon so
that setup and register-save overhead are amortized across multiple pieces.

The production renderer now calls `draw_textured_polygon_arm`, a standalone full
polygon routine in `src/quake/d_polyset_arm.inc`. It includes gradient
and reciprocal setup, edge-walker construction, scan conversion, 32-pixel
perspective correction, coverage, EWRAM texture-cache selection, and texel drawing
in one ARM/IWRAM call per polygon. The initial assembly is mechanically seeded
from the verified optimized C instruction stream, with private labels isolated,
so it is bit-identical and establishes a safe hand-optimization baseline. It
initially measured the same 1.415M cycles as the C version; simply expressing the
same instructions in an `.inc` file was not itself a speedup. The uncalled C reference
is discarded from the production ELF but remains available to regenerate and
compare the assembly. `scripts/extract_polygon_asm.py` performs that extraction.

The first manual assembly pass now loads the packed texture width/height descriptor
once per polygon instead of issuing two ROM halfword reads for every uncovered
pixel. It then keeps the height mask and width in registers throughout scanline
drawing, reconstructing neither for each texel. These changes reduce the stable
spawn result from about 1.415M to **1.376M cycles**, with identical texture
addressing.

Coverage is now four 32-bit words per 120-pixel row. Perspective pieces end at
global 32-pixel boundaries, so the ARM loop loads one coverage word, keeps it in
a register while testing/setting pixels, and stores it once. A fully occupied
`0xffffffff` word bypasses texel drawing immediately. This replaces per-pixel
EWRAM coverage byte loads/stores without introducing a command buffer. The stable
spawn result is now about **1.352M cycles**, approximately 12.4 FPS: roughly 63K
cycles (4.5%) below the initial full-assembly baseline. The optimized `.inc` is
the authoritative production source; `extract_polygon_asm.py` regenerates the
compiler-derived reference baseline and is not intended to overwrite hand edits.

Every timed ROM updates lines 8 through 151. Commands target timer values 0,
128, ..., 896 after detecting the start of HDraw. `observed_cycles[]` records the
timer value immediately before each write, so a debugger can quantify loop
overshoot. The first command is intentionally diagnostic: because HBlank polling
and timer setup take cycles, it cannot land at x=0.

Run one ROM at a time and capture a lossless screenshot. Locate each transition
relative to the intended x positions 0, 32, ..., 224. The difference is the
observable emulator pipeline/software offset. Repeat frames should be identical;
any shimmer means the timing method is not deterministic enough.

## Texture correctness (perspective-correct BSP spans)

The textured BSP renderer used to swim: textures slid across surfaces as the
camera moved or turned. It was not the 32-pixel affine approximation. A
host-side model of the on-target fixed-point pipeline
(`scripts/profile.py` measures the ROM; the model itself is throwaway) put the
error at **110.6 texels mean and 588 worst** at real `dm1` coordinates — the
texture was effectively unrelated to the surface, and because every error term
depends on depth and screen position it moved with the camera.

Four independent defects compounded, in order of contribution:

1. **Absolute world texture coordinates.** `s = dot(vertex, axis) + offset`
   reaches several thousand texels on this map. Quake subtracts a per-face
   origin (`texturemins`); this did not, so `u/z` had no fixed-point headroom
   left. The extractor now emits `u_base_q8`/`v_base_q8` per face, floored to a
   whole multiple of the texture size so masking still wraps to the same texel.
2. **A saturating reciprocal table.** `reciprocal_q24` is `uint16_t`, so
   `2^24 / n` pins at 65535 for every index below 256 — that is every surface
   further away than 256 world units, with the error growing as `z / 256`. The
   span endpoints now use the unsaturated `uint32_t` table. This is the same
   defect that was previously found and fixed in the clipper.
3. **Integer plane gradients.** `d(1/z)/dx` is a fraction of a unit per pixel
   across a face this size, so it truncated to zero and perspective correction
   stopped happening. Gradients are now fractional.
4. **Integer screen positions in the fit.** The interpolation planes were
   fitted through snapped pixel centres, shearing the plane by up to half a
   pixel of lever arm and re-shearing every time a vertex crossed a pixel
   boundary. Vertices now carry a Q8 sub-pixel position for the fit, and the
   planes are evaluated at pixel centres.

Measured against an exact reference, the four together take the error from
**110.6 to 0.46 mean texels (588 to 1.65 worst)**.

Two supporting changes matter as much as the four above:

- The screen position and `1/z` now come from **one** reciprocal lookup per
  vertex. They previously used different roundings of the depth index — one
  rounded, one truncated — so the fitted plane disagreed with where the vertex
  was drawn.
- Fixed-point widths are chosen for range, not just precision. `1/z` keeps Q16;
  `u/z` and `v/z` use Q4, because the per-face origin bounds where a face's
  texture *starts* but not how far it *extends*, and a large floor overflows a
  32-bit Q8 accumulator. Q4 measures within 0.0007 texels of Q16.

### Reproducible measurement

`scripts/profile.py` drives the ROM through mGBA's GDB stub: it runs the ROM,
halts it, reads the `BspProfile` counters straight out of EWRAM, and can dump
the visible Mode 4 page as a PNG.

```sh
python3 scripts/profile.py bsp_textured 5 --shot=/tmp/spawn.png
```

`bsp_textured_walk` follows a fixed camera path so motion tests and timings
repeat frame for frame. `bsp_textured_cref` adds work counters (they cost about
157K cycles a frame, so they stay out of the measured build), and
`bsp_textured_solid`, `_nocoverage`, `_nospans`, `_norows` and `_nowalkers`
progressively remove stages so the cost ladder can be attributed.

## Performance status

At the `dm1` spawn, with correct back-face culling and the corrected texturing:

```text
stage                                   cycles   share
framebuffer clear                        3,970    0.2%
BSP/PVS rebuild                          5,884    0.3%
face culling                            81,814    4.2%
vertex transform                        76,344    3.9%
face front end (ring/uv/project)       290,892   14.8%
fit triple + polygon call               94,165    4.8%
gradient solve                          81,824    4.2%
edge walkers                            77,704    3.9%
row scan conversion                    262,075   13.3%
span setup + fill                      937,926   47.6%
2x expansion                            58,123    2.9%
TOTAL                                1,970,721            8.51 FPS
```

Work counters:

```text
candidates 668 | accepted 195 | drawn faces 160 | rows 1,825 | spans 2,328
texels 9,599 | spans clear 1,234 / hidden 792 / mixed 302 | near-clipped faces 58
```

Per unit: 122 cycles a culled candidate, 168 a transformed vertex, **3,404 a
drawn face**, 144 a row, **403 a span**.

### What the true workload changed

Every figure above predates nothing -- this is the first ladder measured on a
scene that draws all its geometry. Against the old half-scene ladder:

- **Span setup and fill went from about a third of the frame to 47.6%.** The
  renderer is no longer purely setup-bound at the top level.
- **Per-unit costs barely moved** (403 cycles a span against ~390 before, 144
  a row against ~170). There is simply about twice the work. That matters:
  it means the earlier optimisation results still hold, and so do the
  measured dead ends.
- Spans still average **4.1 texels**, down from 5.5. So *within* the span
  bucket the cost is still per-span setup, not per-pixel work -- packing
  wider stores or shortening the pixel loop remains the wrong target.
- **58 of 160 drawn faces now take the frustum clip path**, against 4 of 122
  before. Large near-field walls are exactly what the old scene was missing,
  so this cost was never exercised. It sits inside the face front end.

The reverted experiments were all rejected for reasons the new numbers do not
overturn: the scan-conversion rewrites lost to per-face amortisation over
11.4 rows a face (was 8.8, still far too few), wide stores lost on spans that
are now *shorter*, and the assembly span kernel lost on state marshalling,
which is workload-independent.

### Hand-written assembly: three attempts, three losses

`src/asm/r_project_arm.S` projects a face's whole vertex ring -- camera
lookup, texture coordinates, projection, viewport test -- in one call. It is
bit-exact against the C and **20% slower**: the face front end went from
290,892 cycles to 348,349.

That is the third such attempt, and they all lose the same way:

| kernel | boundary | result |
|---|---|---|
| `d_span_arm.S` | per 32-pixel segment | +27K, argument setup ~1,000x a frame |
| `d_span_arm.S` | per scanline | +57K, sweep state spilled per segment |
| `r_project_arm.S` | per face ring | +57K, axis terms reloaded per vertex |

The C wins because it is *inlined into the caller*, so the compiler allocates
registers across the whole face loop and keeps axis terms and table bases live
between vertices. A standalone routine cannot: it pays a nine-register
prologue and seven argument loads per face, then reloads all ten axis terms
per vertex, because eight axis values plus six base pointers plus an
accumulator do not fit the register file.

Assembly only wins here at a boundary wide enough that state never leaves
registers, which for this renderer means the *entire* face pipeline as one
routine -- project, fit, walk, scan, fill -- not a stage of it. The single
place hand assembly did pay was replacing `__clzsi2`, a libgcc call the
compiler could not avoid.

### Analytic surface gradients: right idea, wrong machine

Quake derives `d_sdivzstepu` and friends analytically from the surface plane
and texture axes rather than fitting a plane through three projected vertices.
The derivation was verified exact here: with this renderer's camera transform,
a world plane becomes `nh*h + nv*v + nz*z = D` in camera space, and dividing
through by z gives

```text
1/z = (nh*sx + nv*sy + 56*nz) / (56*D)          sx = x-60, sy = 40-y
u/z = (ah*sx + av*sy)/56 + az + a0*(1/z)        a0 = dot(a,camera) + a[3] - u_base
```

Checked against a three-vertex fit in floating point, every coefficient agrees
to eight decimals.

Implemented, it **cost 23K cycles more than it saved**. The fit it replaced
disappeared, but computing the coefficients in fixed point needs a division by
the camera-to-plane distance and several by the focal length, and each of
those is a normalised reciprocal on a CPU with no divider. Cut to a single
reciprocal per face with the focal length folded into a constant multiply, it
still lost: the setup inlines to about 320 instructions a face, half of them
the `smull`/`lsr`/`orr` triples that every 64-bit shift becomes. It also draws
21 faces a frame the fit used to reject as degenerate, worth another 11K.

This is worth stating plainly because it explains something about the
comparison to the GBA Quake port: **the analytic form is cheap on a machine
with an FPU and expensive on one without a divider.** Quake computes those
gradients with a handful of float divides per surface, near-free and pipelined
on a Pentium. Here each becomes a table lookup, a normalisation and a 64-bit
multiply. The architecture that makes Quake fast does not transfer to this CPU
unchanged.

What it *would* buy, if the setup could be made cheap, is exactness: no
three-vertex fit means no conditioning problem, and the widest-spread triple
search, its clamped ranking and the degenerate guards all exist only to prop
that fit up.

### Where the remaining time is

Ranked by size, with what each would need:

1. **Span setup, 938K over 2,328 spans.** 792 are fully hidden and already
   cost only a mask test. The 1,536 that draw cost roughly 610 cycles each for
   about six texels, so the four reciprocal multiplies and two span divides
   per segment dominate. Carrying the previous segment's far endpoint into the
   next as its near endpoint would halve them.
2. **Per-face work, 545K over 160 faces (27.6%).** 3,404 cycles a face, of
   which the front end is 1,818. The clip path taken by 58 faces is the part
   never previously measured.
3. **Row scan conversion, 262K over 1,825 rows.** Three restructurings have
   already lost here; the per-face amortisation is still too thin.

30 FPS is 559,000 cycles, so the frame is **3.5x over budget** -- not the ~2x
the half-empty scene suggested. The earlier estimate of a 20-23 FPS
runtime-only ceiling was made against that scene and should be read as
optimistic.

### The BSP and geometry side

Splitting the front end with `bsp_textured_nopoly` and `bsp_textured_nograd`
showed the per-face work was not where it looked. Retained, each measured:

- **Near-clip fast path**: the front end walked the vertex ring three times,
  copying camera+uv, then the clip ring, then the projection -- 68 bytes of
  struct traffic per vertex. Only 4 of 122 faces at the spawn actually cross
  the near plane, so the other 118 now project straight into the output ring:
  **-44.5K**.
- **The widest-spread triple ranked in 32-bit**: it was computing every
  candidate's cross product at Q8 in 64-bit purely to compare magnitudes, at
  591 cycles a face -- more than the six gradient solves it feeds. Ranking in
  clamped whole pixels and computing only the winner's determinant at full
  precision: **-45.9K**.
- **No libgcc CLZ**: `divide_s64_s32` used `__builtin_clz`, which on ARMv4T is
  a call to `__clzsi2` -- a Thumb routine in ROM reached from ARM code in IWRAM
  through an interworking branch -- once per polygon edge. An inlined bit
  ladder removes it: **-29.7K**. It no longer appears in the link map.
- **Integer-unit culling**: both tests are conservative bounding-sphere
  rejections, so the camera's sub-unit position cannot change them once it is
  rounded rather than truncated. Every product then fits 32 bits: **-14.6K**.
- **Side folded into the stored normal** at leaf-cache rebuild, making the
  back-face test one sign check with no branch and no extra load: **-10.9K**.
- **Explicit per-face vertex ring emitted offline**: a face's vertices were
  otherwise reached through surfedge -> edge -> vertex, three dependent loads
  walked twice per frame. `bsp_face_vertices[]` gives one ROM halfword per
  vertex: **-32.3K**, and it retires `frame_edges`, `edge_stamp` and
  `runtime_edges` from the textured build, freeing about 23KB of EWRAM.

Measured and reverted:

- **Scan conversion by two chains walked around the ring.** A convex face has
  exactly two edges crossing any row, so the walker array and its per-row scan
  over every edge is O(rows x edges) where a chain walk is O(rows + edges).
  Implemented twice -- once with the chains live through the texturing loop
  (**+176K**) and once decoupled through a per-face row table to rule out
  register pressure (**+186K**). Both bit-exact, both decisively slower. Faces
  average 8.8 rows and 5 edges, so there is nothing for a per-row state machine
  to amortise, while the naive scan stays a tight register-resident loop. This
  is the third restructuring of the row loop to lose, after the active-edge
  sweep; the tiny-face workload is the reason every time.

### Offline preparation

The extractor does more than transcribe the BSP. Each step below was measured
on the target, and the ones that did not pay were dropped.

- **Explicit per-face vertex rings.** The largest single BSP-side win,
  **-32.3K**; see above.
- **Coplanar face merging.** Edge-adjacent faces sharing a plane and texinfo
  are spliced. The constraint used to be convexity, because the row sweep took
  min/max x and would have filled straight across a notch; with the crossings
  sweep below the merged polygon only has to stay *simple*, and the yield goes
  from **12.2%** to **38.3%** (2,283 -> 1,409 faces, ring vertices 11,374 ->
  7,180 -- merging removes the shared edges, so the rings get shorter as well
  as fewer). At the spawn: 668 -> 448 candidates, 195 -> 128 accepted,
  160 -> 108 drawn faces, 1,825 -> 1,627 rows, 2,328 -> 2,220 spans.
  **-194,398** cycles against the sweep's **+88,587**, so **-105,811 net**
  (1,979,344 -> 1,873,533, 8.48 -> 8.96 FPS). ROM falls 68KB and EWRAM 20KB
  with it: most of the per-face tables are sized by the face count.
  Marksurfaces are remapped and de-duplicated and node face ranges renumbered;
  faces on one plane share a node, so near-to-far ordering survives.
  A merge is refused if the spliced ring repeats a vertex. The union then
  touches itself at a point, and while an even-odd fill draws that correctly,
  every later stage -- the plane fit's widest-spread triple, the bounding
  sphere, the lightmap union rectangle -- would be reasoning about a shape
  that is no longer one region. 10 merges out of 886 on dm1.
  The check that the merge is right is geometric, not visual: a merged ring's
  shoelace area must equal the sum of its parts' areas, because the even-odd
  fill covers exactly the interior of a simple polygon. It agrees to
  **0.0000%** worst case. Screenshots cannot settle this -- two builds running
  at different frame rates sample the scripted walk at different camera
  positions, so their frames are not comparable past the first stationary
  pose.
- **Baked per-ring texture coordinates.** `s` and `t` are a function of the
  world vertex and the face's texinfo alone, so evaluating them per frame
  evaluates a constant: six multiplies and eight ROM axis reads per ring
  vertex, plus lifting the ten-word axis record onto the stack once per face.
  A stub that skipped the computation bounded the win at **93,271 cycles**;
  the table realises **79,585** of it (2,058,929 -> 1,979,344, 8.15 -> 8.48
  FPS), the gap being the two ROM words it reads instead. Output is
  pixel-identical -- the extractor evaluates the same integer expression.
  Per ring entry, not per vertex: a vertex shared by three faces has three
  different pairs, because each face subtracts its own texture origin, so
  there is nothing to cache at runtime. 9,554 entries, 76,432 bytes of ROM.
  What remained of the per-face texture record was the texture index, so
  `FaceTexture` and its stack copy went away with it.
- **Texture downsampling to mip 1.** The originals are authored for 320x200
  against this renderer's 120x80, so level 0 is detail the screen cannot
  resolve. Costs nothing at runtime -- the texture axes are scaled by the same
  factor, so u and v arrive in the stored level's units. Texture bytes
  176,384 -> 44,096, ROM 704,460 -> 549,776, and the spawn working set drops
  from a full 32KB cache with ROM fallback to 8KB. Frame cost unchanged: both
  levels are read from the same EWRAM cache, so this buys memory, not speed.

Measured and reverted:

- **Splitting the runtime face record by access pattern**, so culling could
  stream 16 contiguous bytes instead of striding 28 through records whose
  other half it never reads: **+4.9K**. The premise was wrong -- the compiler
  already loads only the fields each pass uses, so there was no striding waste
  to reclaim, and the split just added a second array base for the accepted
  faces to chase.
- **Precomputing log2(width) into the texture record** rather than deriving it
  with a short loop per face: **+0.8K**. A ROM halfword read costs about what
  the seven-iteration shift loop did.
- **Moving the texture cache into IWRAM** for one-cycle texel reads. IWRAM
  already holds the framebuffer, the coverage bitmap, the hot code and the
  stack, and the face loop alone takes a 2,352-byte frame; a 10KB cache there
  starved the stack and the renderer ran away to 78M cycles a frame.

The pattern across these: the remaining BSP indirections are per-face, and at
120 drawn faces a frame that is already cheap. The ones that mattered were per
vertex -- the explicit ring, and then the coordinates on it.

What is left is camera-dependent and cannot be baked: the vertex transform
(67,075), the per-face plane test (57,106), the projection in the face front
end, and the whole span loop. With drawing removed entirely the frame is
**404,345** cycles against a 559,333-cycle 30 FPS budget -- 72% of it spent
before a single pixel, down from 91% before the coordinate bake and the
non-convex merge. Layout work has taken what it can reach; what moved the
number after that was cutting the face count.

### Specialising the span loop on du/dv: analysed, not built

The address in the pixel loop is four instructions -- mask the row, mask the
column, add the texture base, load -- and three of them disappear if one of the
two coordinates holds still across the run: the row (or column) offset becomes
a loop invariant and the address is one mask and one load. Both cases reduce to
the same five instructions over different `(base, coordinate, step, mask)`, so
one specialised body would serve both.

**The ceiling is real.** A build that pretends the row is invariant for every
span -- wrong picture, right cost -- runs at **1,760,511** against 1,873,533,
so **113,022 cycles**, 11.8 a texel. The loop is not load-bound; the address
arithmetic is a sixth of the frame.

**The coverage is bimodal, which is what kills it.** `bsp_textured_shape`
counts the two cases disjointly:

| pose | texels | one row | one column | covered |
|---|---|---|---|---|
| spawn | 9,599 | 3,847 | 3,689 | **78.5%** |
| walk, 4s (facing a wall) | 9,600 | 9,152 | 0 | **95.3%** |
| walk, 8s | 9,600 | 9,152 | 0 | **95.3%** |
| walk, 14s (oblique) | 9,587 | 143 | 13 | **1.6%** |

The condition is really "is this surface face on", and it collapses the moment
the camera turns. Weakening it from `dv == 0` to "v crosses no texel boundary
across the run" was measured and added almost nothing -- 120 texels to 143 at
the oblique pose -- so there is no useful middle ground either.

So the specialisation would be worth **4.7%** at the benchmark pose, **13%**
pressed against a wall, and **0.1%** looking diagonally across a room -- and
the oblique views are not the cheap ones: 14s draws 113 faces against the
spawn's 108. It buys the most where there is least to gain, costs a second
32-case unrolled body in the IWRAM the drawer is already sized against, and
adds a per-span test that misses 98% of the time in the views that need help.
Recorded rather than built.

### Filling non-convex polygons

The row sweep collects every edge crossing on the row, sorts them, and fills
between consecutive pairs -- the even-odd rule. It replaced a min/max sweep,
which is the same thing for a convex polygon and wrong for any other: it fills
straight across a notch.

On its own it is a **+88,587** cycle loss (4.5%), and on convex data it is
pixel-identical to what it replaced, which is how it was checked. It earns its
place by lifting the convexity constraint off the offline face merge, which
then returns 194,398.

The sort is an insertion sort because the arrays are tiny and almost always
already ordered: a convex face produces exactly two crossings, so the loop body
runs once and compares once. Only the 321 dm1 faces that actually have a
reflex corner ever produce four or more. Each edge is half-open in y, so a
closed ring contributes an even number of crossings on every row and the pairs
always match up.

### Lightmaps

Pre-baked at extract time, applied per drawing segment. **+93K cycles (+4.7%)**
at the spawn: 1,965,903 -> 2,058,929, 8.54 -> 8.15 FPS. ROM 549,776 ->
758,596; EWRAM 204,680 -> 221,064 of 262,144; IWRAM +264 bytes of drawer code.
`bsp_textured_nolight` is the same build with `-DBSP_TEXTURED_NO_LIGHT=1`, for
the A/B.

The map this repo builds from is **not** stock Quake lighting, and reading it
as though it were produces plausible bytes and wrong light. Its worldspawn
carries `"_lmscale" "4"` and `"_lm_border" "1"`: luxels are 4 world units, not
16, every face's block has a one-luxel bleed ring, and softquake's light tool
writes an unlit surface as byte 96 with 128 as the neutral point, so the bytes
are bipolar rather than a 0..255 darkening factor. The vanilla `extents/16 + 1`
reader claims 68,574 bytes of an 888,849-byte lump and never notices, because
every block it reads is in bounds -- it just belongs to a different face. The
check that catches it is structural: walk the faces in `lightofs` order,
predict `w * h * numstyles` rounded up to 4 bytes, and see whether the walk
ends exactly at the lump size. It does, at scale 4 with a border of 1.

What the extractor emits:

- **Style layers resolved to style 0.** Layer *m* belongs to `styles[m]`, and
  `styles[m]` is not *m*: 30 of dm1's 46 multi-layer faces store the animated
  style 2 first and the static style 0 second. Taking layer 0 blindly renders
  those faces from a strobe frozen at an arbitrary phase.
- **Box-filtered from 4 units to 16**, which is what stock Quake shipped and is
  already finer per screen pixel at 120x80 than 16 units was at 320x200.
  126,303 luxel bytes, padded to 131,072.
- **Merged faces re-stitched.** The coplanar merge above drops the absorbed
  faces' lightmaps and keeps the survivor's, whose rectangle covers a fraction
  of the polygon now being drawn -- 198 of 2,005 survivors end up with geometry
  outside their own block, up to 34.5 luxels past its edge. Merging requires
  identical texinfo, so the merged faces share one texture-space
  parameterisation and their grids are subgrids of the same lattice: each
  source block is pasted into the union rectangle at an exact integer offset,
  interior luxels winning over a neighbour's extrapolated border, and the cells
  no block covered are flooded from the ones that were.
- **The shade row, not the luxel.** The row is the luxel's top six bits and
  nothing at runtime wants the other two.
- **A replicated one-luxel border** around every block, so the rasteriser needs
  no clamp. Costs 55KB of ROM; the clamp it replaces was measured at 39.7K
  cycles a frame, because a two-sided clamp on each axis is what pushes the
  compiler into spilling the segment tail.
- **The colormap**, 64 rows of 256, built by scaling each palette entry and
  snapping to the nearest palette index. The neutral row is written as the
  identity rather than searched: nearest-RGB is only self-inverse for 213 of
  the 256 entries, so an unlit wall would otherwise come out recoloured.
  Palette indices 224-255 are Quake's fullbrights and map to themselves in
  every row -- and are excluded as match targets, or a darkened brown lands on
  a bright fire colour that happens to be closest in RGB.

At runtime the address is one shift and one lookup. The luxel grid is defined
in the texinfo's own units -- the mip-0 texel grid -- while `u` and `v` arrive
in the loaded mip level's units with the face's texture origin subtracted.
Both differences are whole luxels (the origin is a multiple of the texture
width, which at this mip is a multiple of a luxel), so the shift distributes
over them and all three offsets collapse into the face's base index:

```c
luxel = base + ((v + v1) >> (BSP_LUXEL_SHIFT + 1)) * width
             + ((u + u1) >> (BSP_LUXEL_SHIFT + 1));
shade_row = shade_table + (bsp_lightmap_luxels[luxel & BSP_LUXEL_MASK] << 8);
```

Sampled once per perspective segment, at its midpoint, before `u` and `v` are
wrapped to the texture -- the texture tiles across a face, the lightmap must
not. `BSP_LUXEL_SHIFT + 1` folds the halving of the two-ended sum into the
shift. The mask is the only bounds test: every face vertex is checked at build
time to land a luxel inside its block, and the affine error between
perspective corrections is 0.86 texels, a tenth of a luxel, so the border
covers everything short of a near-plane segment with a pathological 1/z --
where wrapping is as good an answer as clamping and costs one instruction.

The pixel loop grows by exactly one instruction, and the shade row stays a
loop invariant in a register:

```asm
and  lr, r6, r2, asr #8       @ row offset, v pre-scaled + shifted mask
and  r5, sl, ip, asr #8       @ column
add  lr, r9, lr               @ texture base + row
ldrb lr, [lr, r5]             @ texel
ldrb lr, [r4, lr]             @ shade_row[texel]
strb lr, [r0], #1             @ pixel
add  r2, r2, r3 / add ip, ip, r1
```

The colormap is the one per-texel table and lives in EWRAM (16KB, copied from
ROM at boot); the luxels are per-segment and stay in ROM. Quake's own
architecture -- apply the colormap once per cached surface texel -- does not
port: texture times lightmap over dm1's faces at mip 1 is 2.6MB.

Measured and reverted:

- **Interpolating the light per pixel** rather than holding it constant across
  a segment: **117,713 cycles against 67,738** for the constant version at the
  same pose, nearly twice the price. A luxel is 16 world units and a segment
  is at most 32 screen pixels, so a segment spans two or three luxels; half of
  all adjacent luxel pairs on this map are identical and 95% are within two
  shade rows, which is below what the texture's own texel-to-texel noise
  contributes.
- **A compare-and-branch bounds test** on the luxel index instead of the mask:
  **+30K**. Padding the luxel array to a power of two costs 4,769 bytes of ROM
  and turns the test into one AND.

The map source, luxel scale and exposure are Makefile variables: `BSP_LMSCALE`
(default 16) and `BSP_LMGAIN` (percent, default 100). dm1's bake centres on
0.75x, so the map renders at about two thirds of raw texture brightness --
faithful to how softquake shows it, dim on a handheld. The gain scales every
row of the colormap and costs nothing at runtime.

`scripts/preview_lightmaps.py` renders faces unlit next to lit, straight out
of the generated header and with the runtime's own integer expression. There
is no way to check a lightmap by reading the numbers: an origin one luxel out,
a block one column too wide, a colour snapped to the wrong ramp -- all of them
produce plausible bytes.

### What 30 FPS would take

The 30 FPS budget is 559K cycles. The stages outside rendering already cost
**295K** of it. Extrapolating each remaining bucket to a hand-written ARM
equivalent puts the runtime-only ceiling at roughly **750-850K cycles, or
20-23 FPS**. Reaching 30 FPS needs the *work volume* reduced, not the work made
cheaper — the candidates are merging coplanar same-texture faces (offline in
the extractor, or at leaf-cache rebuild time to stay runtime-only), a coarser
logical resolution, or PPU-assisted fill.

The hand-optimised `src/quake/d_polyset_arm.inc` is currently **out of the
build**: it encodes the old `TextureVertex` layout and the saturating
reciprocal table. `bsp_textured` runs the corrected C kernel from IWRAM until
that routine is rewritten against the new math.

## Back-face culling was inverted

For most of this project's life the renderer drew roughly half the geometry it
should have, and drew the wrong half: the back-face test rejected exactly the
faces pointing at the camera and kept their backs.

`dface_t.side` says whether a face lies in front of or behind its plane, so
after folding it into the stored normal, a POSITIVE camera distance means the
camera is in front of the face and it must be drawn. The test skipped on
`>= 0`.

It was hard to spot because the result still looks like a room -- you see the
outsides of the far walls through the missing near ones, so the image is
plausible, just sparse and full of black gaps.

Found by disabling each culling stage in turn and diffing the frame:

| stage disabled | accepted | drawn | verdict |
|---|---:|---:|---|
| none (as shipped) | 120 | 100 | |
| frustum cull | 256 | 100 | correct: extra faces all clip away |
| PVS | 340 | 268 | 14,692 black pixels light up |
| back-face sign fixed | 212 | 176 | the missing geometry appears |

The PVS looked guilty and was not. With the sign corrected, rendering with
and without the PVS produces **pixel-identical** frames, which is exactly what
a correct PVS should do -- it removes only what contributes nothing. Before
the fix the same comparison differed by 40% of the screen.

The cost is real: the spawn view goes from 100 to 160 drawn faces, 957 to
1,825 rows and about 5,000 to 9,599 texels, and the frame from 1,091,169 to
1,970,721 cycles. **Every performance figure in this file that predates this
section was measured on a scene missing half its polygons.**

## Player movement

`src/quake/p_move.c` follows Quake's own physics: `SV_RecursiveHullCheck` for
tracing, `SV_FlyMove` for sliding, `SV_WalkMove` for step climbing.

The map's hull 1 clipnodes were already expanded by qbsp for the 32x32x56
player box, so the player traces through them as a *point* and no box sweeping
is needed at runtime. That is also why the player comes to rest at exactly
z = 24 on a floor at z = 0: the expanded hull puts the solid boundary an
origin-height above the real surface.

Everything is Q8 world units. Positions share the camera's existing
coordinates; velocities are Q8 units per second and integrate at a fixed 64 Hz
substep, which makes the per-step position delta a shift rather than a divide
and keeps movement independent of the frame rate. Turning is on the same clock,
so the view sweeps at one rate whatever the view costs.

The substep count comes from a free-running timer 3 at 16384 Hz, read at the
same point every frame, with the leftover ticks carried rather than truncated.
Driving it from the renderer's own cycle count instead was measurably wrong:
that number excludes the VBlank wait, and with truncation on top the clock ran
at 40 Hz rather than 64. Timers 0 and 1 cannot be used for this -- the profiler
resets them every frame. The count is clamped to 8 so a slow frame cannot
spiral.

Two details differ from Quake and are deliberate:

- Collision planes use a Q8 distance from `bsp_plane_distance_q8[]`, not the
  renderer's whole-unit one. Half a unit is invisible on a 120x80 screen but
  would let the player sink into floors.
- Friction is exponential, `v *= (1 - k)`, rather than Quake's ramp against a
  stop speed. It matches the same 4/second decay and needs neither a square
  root nor a divide. Walk speed is clamped with an octagonal length
  approximation for the same reason.

Verified on target rather than by inspection. `bsp_textured_walk` drives the
same physics the player does from scripted input, so a run exercises collision,
gravity and stepping reproducibly. Over a 25-second scripted walk into walls:
the origin is never inside solid (`player_solid_frames` 0, contents always
CONTENTS_EMPTY), forward motion stops dead against walls, the player rests at
the spawn height with zero vertical velocity, and the step-up branch of
`walk_move` is exercised.

Controls: D-pad turns and walks, the shoulder buttons strafe, A jumps, Start
respawns.

Sliding comes from `clip_velocity` removing only the component heading into a
surface, so a wall met at an angle is slid along rather than stopped. Met
head-on there is no tangential component left and the player does stop, which
is correct. Measured over a scripted run: 1,795 clipped moves across 1,249
substeps, and the wedged-in-solid bail that would freeze movement outright
never fired once.

Costs 2,385 cycles a frame, about 0.2%.

## Assumptions being tested

1. BG2PA/BG2PC are sampled during a scanline rather than latched only at its start.
2. A visible-period write to BG2X/BG2Y updates the internal affine reference for
   the current scanline immediately (documented, but tested here at pixel scale).
3. Writing both groups from a hard-coded IWRAM routine can restart a
   piecewise-affine mapping every 32 pixels, without any EWRAM-stream dependency.
4. An IWRAM ARM consumer can read a 16-byte EWRAM command and meet successive
   128-cycle deadlines. The timer timestamps distinguish lateness from PPU latency.
5. WIN0 clipping does not perturb the register-update behavior.

## Observation log (mGBA 0.10.5)

| Test | mGBA observation | Measured offset/timing | Hardware status |
|---|---|---|---|
| baseline | stable checkerboard | n/a | pending |
| pa_pc | later pixels change slope on the same scanline | boundaries stable frame-to-frame | required |
| xy | current-line source point visibly jumps | boundaries stable frame-to-frame | required |
| combined32 | hard-coded PA/PC + X/Y pieces are visible | 128-cycle targets | required |
| stream32 | EWRAM-fed result is stable at 59.7 fps | actual timer samples below | required |
| window | clipping is correct and updates remain stable | no additional drift observed | optional after core tests |
| quad_reference | clean rotating perspective plane | exact precomputed pixels | emulator reference |
| quad_affine | substantial live flicker despite coherent stills | <=32-pixel pieces | failed; isolate with static ROM |
| quad_affine_static | intermittent flicker with immutable geometry and commands | same commands every frame | failed; scheduling instability confirmed |

For `stream32`, the EWRAM `observed_cycles` buffer contained:

```text
target:    0  128  256  384  512  640  768  896 cycles
actual:    9  131  260  389  518  640  769  898 cycles
overshoot: 9    3    4    5    6    0    1    2 cycles
```

After the unavoidable first-command setup cost, the polling loop was never more
than six cycles (1.5 pixels) late in mGBA. This measures the CPU write issue time,
not the PPU's register-to-pixel sampling latency. The screenshots in
`observations/` are the evidence from this run; `stream32-cycles-mgba.jpg` shows
the raw little-endian timer values at EWRAM address `0x02000000`.

The visual tests confirm same-scanline effects in mGBA, but the checker pattern is
not by itself a precise latency ruler. A follow-up calibration should use a
single-color discontinuity and compare a lossless 1x framebuffer capture before
turning the observed offset into a hard-coded deadline correction.

Emulator success is evidence about mGBA behavior, not proof of LCD pipeline timing.
At minimum, repeat `pa_pc`, `xy`, `combined32`, and `stream32` on one real GBA model
with a flash cart/capture setup before relying on the offsets. Different GBA/AGS
revisions should be checked if exact pixel boundaries matter.

## Scope boundary

The exact cube is precomputed rather than projected on the GBA. Runtime
projection, clipping, reciprocal lookup, and polygon setup remain excluded. The
affine cube experiment is currently falsified as a visually correct renderer;
the small one-variable register tests remain useful independently.
