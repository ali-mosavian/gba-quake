# Experiment ledger

Everything measured on this renderer, so nothing gets tried twice. Numbers are
cycles per frame at the dm1 spawn unless noted, measured with
`scripts/profile.py` against mGBA 0.10.5. The README carries the reasoning;
this file is the index.

Current: **1,892,224 cycles, 8.87 FPS** at the spawn with brush entities in
view (the drawbridge); ~1,741K where no mover is visible. 30 FPS is 559,333.

## Kept

| change | result |
|---|---|
| Explicit per-face vertex ring, emitted offline | **-32.3K**, frees ~23KB EWRAM |
| Widest-spread triple ranked in 32-bit, not Q8/64-bit | **-45.9K** |
| Near-clip fast path (only 4 of 122 faces cross it) | **-44.5K** |
| Inlined bit-ladder CLZ, no libgcc `__clzsi2` | **-29.7K** |
| Integer-unit bounding-sphere culling | **-14.6K** |
| `side` folded into the stored normal offline | **-10.9K** |
| Baked per-ring texture coordinates | **-79.6K** (ceiling 93.3K) |
| Non-convex coplanar merge + even-odd crossings fill | **-105.8K** net |
| Single-axis span specialisation | **-0.88%** avg; -3.1% spawn, +2.2% worst |
| Segment endpoint carry (fit across `pixels + 1`) | **-1.25% to -3.82%** at all six yaws |
| Whole-face affine for low-drift faces (`BSP_AFFINE_DIVISOR=8`) | **-0.6% to -8.3%** at all six yaws |
| Lightmaps, pre-baked, one shade row per segment | **+93K** (a feature, not a win) |
| Texture mip 1 | memory only: 176KB -> 44KB, no frame cost |
| Framebuffer in IWRAM | byte and word stores both 1 cycle |
| Hecker sub-texel correction, adaptive perspective interval | correctness |

### Brush entities (doors, buttons, teleporter)
| aspect | outcome |
|---|---|
| Six movers + teleporter, extracted with Quake's own movement derivation | works; states, triggers, wait times from entity keys |
| Faces, lightmaps, texcoords all baked -- translation just works | zero new bake machinery |
| Collision: player traces through each mover's own hull, translated | bbox pre-reject first; without it every trace walked six hulls |
| Draw before the world, coverage-first | rooms behind doors are in the PVS, so drawing after would vanish them |
| Entity pass stack | 2.7KB of ring buffers on the IWRAM stack overflowed into .bss and corrupted culling statics -- presenting as a 456K "speedup". EWRAM statics now |
| `angle` variable shadowed the spawn yaw in the extractor | BSP_SPAWN_YAW came out 0; every measurement against that header was of a different scene |
| Cost | **+151K when movers are visible, ~0 otherwise.** The clip/project helpers run as out-of-line ROM-ARM copies for the entity path; inlining them into IWRAM would spend stack headroom already at 4.7K |

### Dynamic lighting
| aspect | outcome |
|---|---|
| Quake's projection: light onto the face plane, falloff in texture space | per-segment cost is two subtractions, two multiplies, a shift |
| Perpendicular falloff folded into a per-face base boost | the sampler sees only the 2D remainder |
| Sources: carried light (SELECT toggles) + 3 nearest torches, shimmering | torch steady glow is already baked; dynamic adds only flicker |
| Falloff tuned against eye height | at shift 7 the boost was spent before the nearest visible floor pixel (~33 texels from the projection point); shift 8, boost 22 reaches ~75 texels |
| Copying the per-face light state into drawer locals | +75K of spills -- fourth instance of the shared-live-state trap; the state stays in globals, one flag load per segment |
| Cost | **+54K infrastructure, ~+105K with the carried light active at the spawn** (8.9 -> 8.2 FPS worst case) |

## Rejected

### Row and scan conversion
| attempt | result |
|---|---|
| Active-edge sweep | +6.5% |
| Two chains walked around the ring | +176K |
| Same, decoupled through a per-face row table | +186K |

Faces average 8.8 rows and 5 edges. There is nothing for a per-row state
machine to amortise; the naive scan stays register-resident.

### Hand-written assembly
| kernel | boundary | result |
|---|---|---|
| `d_span_arm.S` | per 32-pixel segment | +27K |
| `d_span_arm.S` | per scanline | +57K |
| `r_project_arm.S` | per face ring | +57K (20% slower) |

The C wins because it is *inlined into the caller* and the compiler allocates
registers across the whole face loop. Assembly would need the entire face
pipeline as one routine. The only assembly that ever paid was removing a
libgcc call.

### Per-pixel loop
| attempt | result |
|---|---|
| Sharing the segment-finish code as an 18-param always_inline function | **+2.8% to +3.5%** structural; the perspective path's values went through the stack. A textual `#include` is free |
| Gating affine faces by bbox area (256 px^2) | **+1.5%** at most poses; small faces profit too -- the bit-ladder divides are cheap on small quotients |
| `BSP_AFFINE_DIVISOR=4` | 2.1% faster on average, stills clean, but admits 25% depth change across a face -- the swimming regime. A knob, not the default |
| Sixteen-deep Duff's device for the run dispatch | **+0.9% to +4.0%** -- the rounds counter stays live across the body. Profile builds use it anyway: their counters add ~3KB of IWRAM and the full unroll pushed code+bss into the stack, crashing them at boot |
| Four texels packed into one word store | -3.6K, then +30K once aligned |
| Analytic surface gradients (Quake's `d_sdivzstepu`) | +23K, plus 11K of newly-drawn degenerates |
| Shared `(base, coord, step, mask)` tuple for both specialised cases | +4% oblique: the *general* path spilled its masks and reloaded them per pixel |
| Row-only specialisation instead of row+column | -0.80% vs -0.88% |
| Weakening `dv == 0` to "crosses no texel boundary" | 120 -> 143 texels oblique |
| A third specialised case | nothing to specialise on, see below |

Analytic gradients are cheap with an FPU and expensive without a divider. That
is the real answer to how the GBA Quake port afforded techniques we cannot.

**No third case exists.** Texels binned by boundary crossings on the *cheaper*
axis are bimodal -- 0 or 5+, with 2.5% (spawn) to 7% (oblique) in between:

| pose | 0 | 1 | 2 | 3 | 4 | 5+ |
|---|---|---|---|---|---|---|
| +0° | 66.6% | 0.9% | 0.6% | 0.5% | 0.5% | 30.9% |
| +150° | 1.2% | 2.3% | 1.2% | 3.6% | 0.5% | 91.2% |

### Geometry and memory
| attempt | result |
|---|---|
| Texture cache in IWRAM | 78M cycles/frame -- it starved the stack |
| Face record split by access pattern | +4.9K |
| Precomputed `log2(width)` in the texture record | +0.8K |
| Q8 u/z accumulators | range, not precision; Q4 settled |
| Merging across texinfo (mip-2 composited textures) | 38.3% -> **41.1%** absorbed, for 917KB ROM |
| Bundling disjoint coplanar faces into multi-ring faces | radius grows 2.68x mean; +5% spawn, +20% elsewhere |

**Merging is limited by adjacency, not texture**: 14,885 of 15,872 rejected
merges are "no shared edge", none are about texture.

**Bundling's asymmetry is structural**: the culling penalty is paid per
*candidate*, the saving earned per *drawn* face, and three quarters of
candidates are rejected. `BSP_RADIUS_SCALE` prices it — x4 costs +8.4% at the
spawn and ~+31% at two other yaws, ~2,900 cycles per extra accepted face.

### Lightmaps
| attempt | result |
|---|---|
| Per-pixel light interpolation | 117.7K vs 67.7K for a constant row per segment |
| Compare-and-branch luxel bounds test | +30K vs a power-of-two mask |
| Clamped addressing with 5 per-face constants | +39.7K; a replicated border is free |
| Bilinear filtering | 384-576K estimated |
| Surface cache (texture x lightmap) | 2.6MB at mip 1; 41KB free |
| Loading the LIGHTING lump into RAM | 888,849 B; read from ROM in place |

## Open leads

- **Beam-raced PPU spans: DISPROVEN ON HARDWARE, at the register level.**
  The full arc, so nobody walks it again. Cycle-exact feeding works (see
  below). The display list, atlas and IWRAM feeder all work -- the hardware
  photo of `beam_frame` showed span boundaries at different columns per line,
  following the display list. But the picture stayed corrupted through a
  timing recalibration and atlas padding, and `beam_xytest` isolated why:
  **BG2X/BG2Y writes during HDraw take effect on the NEXT line; only PA/PC
  apply mid-draw.** The reference point is line-latched. The quad experiments
  could never detect this: their pieces continue one plane's walk, so each
  piece's X/Y was already where the ignored write pointed, and PA/PC did all
  the visible work. Arbitrary spans jump anchors between faces -- exactly the
  operation the hardware lacks. No feeding scheme fixes this.
  What survives: per-line-anchored techniques (the Mode 7 floor/ceiling
  path, X/Y written at HBlank), and mid-line PA/PC changes along a
  continuous walk (a single plane per scanline region).
- **Beam-raced feeding mechanics, for whatever reuses them.** On a real GBA
  (SuperCard SD, SuperFW), `quad_affine_exact_static` holds its seams still
  -- minor static artifacts, no shimmer -- while the polled
  `quad_affine_static` shimmers. The flicker that killed the 2023-era quad
  experiments was poll-granularity jitter, not a hardware limitation:
  raster_exact_arm.S anchors Timer 0 to HBlank by DMA and lands each write
  through a computed NOP sled, deterministic after one timer read. The real
  per-command cost is ~35 cycles (one ldmia, four IO writes, loop) -- a first
  version at 66 made every dense line overrun by one and each VCOUNT equality
  wait then charged a full frame, which rendered as striped bands. mGBA
  latches one affine state per line and does not emulate HBlank DMA to the
  timer: it can validate data, never the picture.
- **PPU floor (Mode 7).** Proven in `floor_mode7`: one floor plane drawn
  entirely by the PPU from a pre-lit plan, per-scanline affine state via
  HBlank DMA. 23-39% of drawn texels are on horizontal faces. Integration
  needs the Mode 4 -> tiled-affine-framebuffer move and accepts one plane
  height per scanline per BG. The quad experiments' flicker does not apply:
  that was mid-scanline polling, this is hardware-latched per line.

## Data quirks that cost time

- **`maps/dm1.bsp` is not stock Quake lighting.** `_lmscale 4`, `_lm_border 1`,
  bipolar bytes with unlit = 96, neutral = 128. A vanilla `extents/16 + 1`
  reader claims 68,574 of 888,849 bytes and never errors -- every block it
  reads is in bounds, just the wrong face's. Check structurally: walk faces in
  `lightofs` order, predict `w*h*numstyles` rounded up to 4 bytes, and see
  whether the walk ends exactly at the lump size.
- **`styles[m]` is not style `m`.** 30 of dm1's 46 multi-layer faces store the
  animated style 2 first.
- **Luxels live in mip-0 texture space**; `u` arrives at mip 1. A factor of 2 —
  `BSP_LUXEL_SHIFT` is 11, not 12.
- **The coplanar merge orphans lightmaps.** 198 of 2,005 survivors would have
  geometry outside their own block, up to 34.5 luxels past its edge.
- **`floor`/`ceil`, not C truncation**, host-side: 856 of 4,554 lit face-axes
  differ. `lightofs` blocks are 4-byte aligned. `lightofs == -1` on 6 faces.

## Measurement traps

- **mGBA cannot display beam-raced spans, in either frontend.** The trace
  build proves the feeder issues every command on schedule (pass N starts at
  VCOUNT N-1, ends in order), yet both the SDL and Qt frontends show
  uniform-in-Y striped bands: mid-scanline affine register writes are not
  applied at sub-scanline granularity. mGBA also does not emulate the HBlank
  DMA pair restarting timer 0 (the timer free-runs and wraps every ~53
  lines). Both mechanisms are hardware-validated by the exact-quad test;
  for this technique the emulator can only validate data, never the picture.
- **SuperCard SDRAM cannot serve fast ROM wait states.** WAITCNT = 0x4317 as
  the renderer's first act means the next instruction fetch misreads and the
  machine white-screens before drawing anything. The `_sc` targets never
  leave the BIOS default; cost measured at +1.0% (hot code is IWRAM, textures
  EWRAM).
- **Never compare two builds through the scripted walk.** They run at different
  speeds, so at any wall-clock second their cameras are somewhere else. This
  invalidated two measurements before `BSP_YAW_OFFSET_Q8` existed; use fixed
  yaws.
- **`make` does not see recipe or variable changes.** `src/generated/.bsp_settings`
  stamps `BSP_MIP`/`BSP_LMGAIN`/`BSP_MERGE` so the header rebuilds; a changed
  *recipe* still needs a manual `rm` of the object.
- **The container cannot reach the BSP or the pak.** `scripts/build.sh` bind
  mounts both directories and forwards every `BSP_*` variable, or the
  container's `make` stamps different settings and invalidates a good header.
- **Profile counters cost ~157K a frame**, a quarter of the 30 FPS budget, so
  they are opt-in. They also cost ~3KB of IWRAM code, which is why profile
  builds use the Duff dispatch: with the full unroll they overflow the stack
  and crash at boot -- all counters read zero and the frame reads 1 cycle.
  The shape build runs with ~600B of stack margin; treat new IWRAM growth
  with suspicion. `BSP_PROFILE_SPAN_SHAPE` is separate from
  `BSP_PROFILE_COUNTS` because its test costs two multiplies a span.
- **Verify pixel-exactness before trusting a speedup.** Every kept change above
  that should be bit-exact was diffed against the previous frame.

## Where the time is now

| | cycles | share |
|---|---|---|
| Span loop | 1,041K | 56% |
| Walkers + row sweep | 319K | 17% |
| Face front end (project + clip) | ~178K | 10% |
| Outside the render pass | 226K | 12% |
| Gradient fit + plane setup | 109K | 6% |

With drawing removed entirely the frame is **404,345** against a 559,333
budget. The face-count lever is spent; what remains is the span loop, the row
sweep, or resolution.
