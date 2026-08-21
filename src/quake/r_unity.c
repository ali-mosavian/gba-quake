/*
 * Quake-style unity build for the BSP experiments.
 *
 * Keeping these modules in one translation unit preserves local renderer data,
 * ARM assembly integration, and cross-module optimization.
 */
#include "r_state.c"
#include "r_bsp.c"
#include "r_clip.c"
#include "p_move.c"
#include "d_draw.c"
#ifdef BSP_TEXTURED
#include "d_scan.c"
#include "r_surf.c"
#endif
#include "r_main.c"
