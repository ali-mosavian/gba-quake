/* One drawing segment, after its endpoints are known: the lightmap row, the
 * Hecker bias, the wrap, and the pixel dispatch.
 *
 * Included textually into BOTH row loops of the reference drawer rather than
 * shared as a function. The function form was measured at +2.8% to +3.5% on
 * the poses where the affine path never fires: eighteen always-inline
 * parameters were enough to push the perspective path's values through the
 * stack at the boundary. As text, each loop's copy allocates registers
 * against its own live state, and the per-face constants come straight from
 * the enclosing scope.
 *
 * Expects in scope: u, v, u1, v1, du, dv, row, span, end, segment_pixels,
 * blocked (and the coverage word set unless BSP_TEXTURED_NO_COVERAGE), plus
 * the drawer's per-face constants.
 */
#if !defined(BSP_TEXTURED_SOLID) && !defined(BSP_TEXTURED_NO_FETCH) && \
        !defined(BSP_TEXTURED_NO_LIGHT)
                /* One luxel for the whole segment, taken at its midpoint.
                 *
                 * A luxel is 16 world units and a segment is at most 32 screen
                 * pixels, so a segment spans two or three luxels at most, and the
                 * lightmap is smooth at that scale: half of all adjacent luxel
                 * pairs on this map are identical and 95% are within two shade
                 * rows of each other. Interpolating the light per pixel instead
                 * was built and measured, and cost 117,713 cycles a frame against
                 * this version's 67,738 -- nearly twice the price for a difference
                 * the segment-boundary column statistics cannot separate from the
                 * texture's own noise.
                 *
                 * Sampled BEFORE the wrap below: u and v are still absolute texel
                 * coordinates here, and a texture that tiles across the face wraps
                 * them back to the same few texels while the lightmap must keep
                 * running across the whole surface.
                 *
                 * The shift is one past BSP_LUXEL_SHIFT because the two ends are
                 * summed rather than averaged; halving is folded into it.
                 *
                 * No per-axis clamp. Every face vertex is checked at build time
                 * to land at least one luxel inside its block, and a point inside
                 * a segment cannot stray much further: the affine error between
                 * perspective corrections is bounded at 0.86 texels at this mip,
                 * which is a tenth of a luxel. The replicated border covers the
                 * rest. The mask is the only bounds test, and it is there so that
                 * a segment surviving near-plane clipping with a very large 1/z
                 * cannot read past the array -- it wraps instead of clamping
                 * because at that point one segment is already wrong either way,
                 * and wrapping is one instruction. */
                unsigned luxel_index = (unsigned)(
                    luxel_base + (((v + v1) >> (BSP_LUXEL_SHIFT + 1)) * luxel_width) +
                                 (((u + u1) >> (BSP_LUXEL_SHIFT + 1))));
                unsigned shade_index =
                    bsp_lightmap_luxels[luxel_index & BSP_LUXEL_MASK];
                /* Dynamic light: the projected light's texture-space position
                 * arrived with the face; the segment adds a quadratic-falloff
                 * boost to the baked row. Inactive faces pay one register
                 * test. Sampled at the same midpoint as the luxel, in texels:
                 * the >> 9 folds the sum-of-endpoints halving into the Q8
                 * drop. */
                if (dlight_active) {
                    int32_t across = ((u + u1) >> 9) - (dlight_u_q8 >> 8);
                    int32_t along = ((v + v1) >> 9) - (dlight_v_q8 >> 8);
                    int32_t boost = dlight_base_boost -
                        ((across * across + along * along) >> DLIGHT_FALLOFF_SHIFT);
                    if (boost > 0) {
                        shade_index += (unsigned)boost;
                        if (shade_index > 63) shade_index = 63;
                    }
                }
                const uint8_t *const shade_row =
                    shade_table + (shade_index << 8);
#endif
#if !defined(BSP_TEXTURED_SOLID) && !defined(BSP_TEXTURED_NO_FETCH)
                /* Nearest texel, with Hecker's direction-stable tie break.
                 *
                 * Truncating u and v biases every lookup half a texel toward -u
                 * and -v. Rounding fixes the bias but leaves exact halves landing
                 * on whichever side the rounding happens to favour, so a texel
                 * boundary alternates as a gradient changes sign. Adding 128 when
                 * the gradient is positive and 127 when it is negative is the
                 * fixed-point form of floor(x + 1/2) and ceil(x - 1/2), which
                 * breaks the tie consistently against the direction of travel.
                 *
                 * Applied once per segment: the bias is a constant and survives
                 * the per-pixel stepping, and du and dv cannot change sign inside
                 * a segment. */
                /* (x >> 31) is -1 for a negative gradient and 0 otherwise, so
                 * this is 128 or 127 without a branch. */
                u += 128 + (du >> 31);
                v += 128 + (dv >> 31);
                /* Wrap once here so the pre-scaled v cannot leave 32 bits, then
                 * fold the row shift into v and its step. Both are per segment,
                 * not per pixel. */
                u &= (int)u_wrap;
                v = (v & (int)v_wrap) << width_shift;
                dv <<= width_shift;
#if BSP_SPAN_FLAT
                /* Does this run hold one coordinate still?
                 *
                 * A surface seen face on steps only along the texture row, and
                 * one seen edge on only down the column; either way half the
                 * address is a constant for the whole run and folds into a
                 * base pointer, taking the pixel loop from eight instructions
                 * to five. Measured with the row assumed constant everywhere,
                 * that is worth 113,022 cycles a frame -- 11.8 a texel, a
                 * sixth of the frame.
                 *
                 * How much of it is reachable swings hard with where the
                 * camera looks: 78.5% of texels at the spawn, 95.3% facing a
                 * wall down a corridor, 1.6% looking diagonally across a room.
                 * The test is two compares, so the views it cannot help pay
                 * almost nothing for the ones it can.
                 *
                 * Exactly zero, not "crosses no texel boundary across the
                 * run": the weaker test was measured and returned 143 texels
                 * against 120 at the oblique pose, which does not pay for the
                 * two multiplies it needs. */
#endif
#ifdef BSP_PROFILE_SPAN_SHAPE
                /* How much of the frame a specialised inner loop could take.
                 *
                 * If a run stays in one texture row, the row offset is loop
                 * invariant and the address is one mask and one load instead
                 * of two masks and an add; the same is true with the axes
                 * swapped if it stays in one column. Both reduce to the same
                 * five instructions over different (base, coordinate, step,
                 * mask), so one specialised body would serve both. The two
                 * counters are kept disjoint so they sum to that coverage.
                 *
                 * Behind its own define, not BSP_PROFILE_COUNTS: the test
                 * costs two multiplies a span, which would show up in every
                 * ladder measurement taken with the ordinary profile build. */
                if ((v >> 8) == ((v + dv * segment_pixels) >> 8)) {
                    ++span_flat_v_count;
                    texel_flat_v_count += (uint32_t)segment_pixels + 1;
                } else if ((u >> 8) == ((u + du * segment_pixels) >> 8)) {
                    ++span_flat_u_count;
                    texel_flat_u_count += (uint32_t)segment_pixels + 1;
                }
                {
                    int rows = ((v + dv * segment_pixels) >> 8) - (v >> 8);
                    int columns = ((u + du * segment_pixels) >> 8) - (u >> 8);
                    if (rows < 0) rows = -rows;
                    if (columns < 0) columns = -columns;
                    int cheaper = rows < columns ? rows : columns;
                    if (cheaper > 5) cheaper = 5;
                    texel_cross_bucket[cheaper] += (uint32_t)segment_pixels + 1;
                }
#endif
#endif
#ifdef BSP_TEXTURED_SOLID
                (void)u; (void)v; (void)du; (void)dv;   /* coverage-only diagnostic */
#endif
#if BSP_SPAN_FLAT
                if (!dv) {
                    const uint8_t *const flat_base =
                        texture_pixels + (((unsigned)(v >> 8)) & row_mask);
                    DRAW_RUN(WRITE_FLAT_ROW, TEXEL_FLAT_ROW, SHIFT_FLAT_ROW)
                } else if (!du) {
                    const uint8_t *const flat_base =
                        texture_pixels + (((unsigned)(u >> 8)) & u_mask);
                    DRAW_RUN(WRITE_FLAT_COL, TEXEL_FLAT_COL, SHIFT_FLAT_COL)
                } else
#endif
                {
                    DRAW_RUN(WRITE_ONE, TEXEL, SHIFT_ONE)
                }
