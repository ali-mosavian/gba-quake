#!/usr/bin/env python3
"""Read a Quake BSP's LIGHTING lump and re-bake it onto a coarser luxel grid.

Quake stores one 8-bit lightmap block per face, addressed by `dface_t.lightofs`
and sized from the face's extent on a fixed world-unit grid. Everything about
that block -- its dimensions, its origin, how many style layers it holds -- is
implied rather than stored, so all of it has to be recomputed here exactly the
way the light compiler computed it, or the blocks decode shifted or overlapping.

The two conventions that are NOT part of vanilla Quake and that this reader
takes from worldspawn are `_lmscale` (world units per luxel; vanilla is a
hardcoded 16) and `_lm_border` (a ring of extra luxels around every face, for
bilinear filtering; vanilla has none). The maps under softquake/maps are baked
at scale 4 with a 1-luxel border, so a vanilla reader mis-sizes every block.

Terms used below:
  s, t      texture coordinates in the texinfo's own units, i.e. the mip-0
            texel grid. Luxel grids are defined in this space, never in the
            mip level a renderer happens to load.
  luxel     one lightmap sample. `scale` world units of s or t wide.
  block     one face's luxels, row-major, `numstyles` layers back to back.
"""
import math, struct

ENTITIES, PLANES, VERTICES, TEXINFO, FACES, LIGHTING, EDGES, SURFEDGES = 0, 1, 3, 6, 7, 8, 12, 13


def worldspawn_keys(entity_text):
    """Return the worldspawn key/value pairs. It is always the first block."""
    block = entity_text[:entity_text.find("}")]
    return dict((m[0], m[1]) for m in
                __import__("re").findall(r'"([^"]*)"\s+"([^"]*)"', block))


class FaceLightmap:
    """One face's decoded lightmap block, still on the source luxel grid."""
    __slots__ = ("width", "height", "luxels", "grid_min_s", "grid_min_t",
                 "scale", "styles")

    def __init__(self, width, height, luxels, grid_min_s, grid_min_t, scale, styles):
        self.width, self.height, self.luxels = width, height, luxels
        self.grid_min_s, self.grid_min_t = grid_min_s, grid_min_t
        self.scale, self.styles = scale, styles

    def sample(self, s, t):
        """Nearest luxel for a texinfo-space (s, t), clamped to the block."""
        x = int(math.floor(s / self.scale)) - self.grid_min_s
        y = int(math.floor(t / self.scale)) - self.grid_min_t
        x = 0 if x < 0 else (self.width - 1 if x >= self.width else x)
        y = 0 if y < 0 else (self.height - 1 if y >= self.height else y)
        return self.luxels[y * self.width + x]


def read_lightmaps(data, lumps, vertices, edges, surfedges, texinfo, faces,
                   bipolar=None):
    """Decode every face's lightmap block.

    Returns (blocks, info) where blocks[i] is a FaceLightmap or None for an
    unlit face, and info carries the conventions that were detected.

    Style layers are resolved here, not at runtime: a face's layers are the
    same surface lit by different switchable light styles, and this renderer
    has no switchable lights.
    """
    entity_offset, entity_size = lumps[ENTITIES]
    keys = worldspawn_keys(data[entity_offset:entity_offset + entity_size]
                           .decode("latin-1", "ignore"))
    scale = int(keys.get("_lmscale", 16))
    border = int(keys.get("_lm_border", 0))
    if bipolar is None:
        # A bipolar bake centres neutral at 128 and can brighten as well as
        # darken; vanilla Quake's bytes are a pure 0..255 darkening factor.
        # `_lmscale` is only ever stamped by the fork that also writes bipolar.
        bipolar = "_lmscale" in keys
    light_offset, light_size = lumps[LIGHTING]
    lightdata = data[light_offset:light_offset + light_size]

    blocks, allocated = [], []
    animated = out_of_order = 0
    for face in faces:
        _plane, _side, first_edge, edge_count, tex_index, s0, s1, s2, s3, lightofs = face
        styles = [s for s in (s0, s1, s2, s3) if s != 255]
        axes = texinfo[tex_index][:8]
        mins = [1e30, 1e30]
        maxs = [-1e30, -1e30]
        for e in range(edge_count):
            directed = surfedges[first_edge + e]
            vertex = vertices[edges[directed][0] if directed >= 0
                              else edges[-directed][1]]
            for k in range(2):
                axis = axes[4 * k:4 * k + 4]
                value = (vertex[0] * axis[0] + vertex[1] * axis[1] +
                         vertex[2] * axis[2] + axis[3])
                mins[k] = min(mins[k], value)
                maxs[k] = max(maxs[k], value)
        # The grid runs from floor(min/scale) to ceil(max/scale) inclusive, so
        # every point of the face has a luxel on each side of it, then the
        # border ring is added outside that.
        grid_min = [int(math.floor(mins[k] / scale)) - border for k in range(2)]
        width = int(math.ceil(maxs[0] / scale) - math.floor(mins[0] / scale)) + 1 + 2 * border
        height = int(math.ceil(maxs[1] / scale) - math.floor(mins[1] / scale)) + 1 + 2 * border
        if not styles or lightofs < 0:
            blocks.append(None)
            continue
        count = width * height
        layers = [lightdata[lightofs + m * count:lightofs + (m + 1) * count]
                  for m in range(len(styles))]
        if len(layers[-1]) != count:
            raise ValueError("lightmap block runs past the LIGHTING lump")
        # Take the style-0 layer, which is the light that is always on, and
        # drop the rest.
        #
        # Layer m belongs to styles[m], and styles[m] is NOT m: on dm1, 30 of
        # the 46 multi-layer faces store the animated style 2 first and the
        # static style 0 second, so taking layer 0 blindly renders those faces
        # from a strobe frozen at whatever phase the compiler happened to bake.
        # Summing the layers instead is what the engine does with every style
        # held at full strength, and it is arithmetically fine, but it still
        # freezes an animation at its brightest -- an arbitrary choice for a
        # renderer that has no light animation to return it to.
        layer = styles.index(0) if 0 in styles else 0
        luxels = bytearray(layers[layer])
        if len(styles) > 1:
            animated += 1
            if layer != 0:
                out_of_order += 1
        blocks.append(FaceLightmap(width, height, luxels,
                                   grid_min[0], grid_min[1], scale, styles))
        # Blocks are padded to a 4-byte boundary by the compiler that wrote
        # them; the tiling check below only holds if that padding is modelled.
        allocated.append((lightofs, (count * len(styles) + 3) & ~3))

    info = {"scale": scale, "border": border, "bipolar": bipolar,
            "lump_bytes": light_size, "lit_faces": len(allocated),
            "animated_faces": animated, "styles_out_of_order": out_of_order}
    info["tiling_gaps"] = _check_tiling(allocated, light_size)
    return blocks, info


def _check_tiling(allocated, light_size):
    """Every byte of the LIGHTING lump should be claimed by exactly one block.

    If the dimension formula is wrong, blocks still decode -- they just decode
    the neighbouring face's luxels. This is the check that catches it.
    """
    allocated = sorted(allocated)
    gaps = 0
    end = 0
    for offset, size in allocated:
        if offset != end:
            gaps += 1
        end = max(end, offset + size)
    if end != light_size:
        gaps += 1
    return gaps


def paste_blocks(blocks, source_scale):
    """Paste several faces' blocks into one grid on the shared luxel grid.

    Coplanar faces that share a texinfo also share texture space, so their
    luxel grids are aligned to the same multiples of `scale` and pasting is an
    exact integer copy -- no resampling and no seam. This is what lets the
    offline face merge keep its lightmaps: a merged face has no block of its
    own, only the blocks of the faces it swallowed.

    Border luxels are extrapolated data, so they lose to a neighbour's real
    interior luxels wherever the two overlap.
    """
    min_s = min(b.grid_min_s for b in blocks)
    min_t = min(b.grid_min_t for b in blocks)
    width = max(b.grid_min_s + b.width for b in blocks) - min_s
    height = max(b.grid_min_t + b.height for b in blocks) - min_t
    luxels = bytearray(width * height)
    solid = bytearray(width * height)          # 0 empty, 1 border, 2 interior
    for block in blocks:
        offset_x, offset_y = block.grid_min_s - min_s, block.grid_min_t - min_t
        for y in range(block.height):
            edge_y = y == 0 or y == block.height - 1
            row = (offset_y + y) * width + offset_x
            for x in range(block.width):
                rank = 1 if (edge_y or x == 0 or x == block.width - 1) else 2
                if solid[row + x] >= rank:
                    continue
                solid[row + x] = rank
                luxels[row + x] = block.luxels[y * block.width + x]
    _fill_holes(luxels, solid, width, height)
    return FaceLightmap(width, height, luxels, min_s, min_t, source_scale, [0])


def _fill_holes(luxels, solid, width, height):
    """Flood the cells no block covered outward from the ones that were.

    A merged face's bounding box is bigger than the union of its parts
    whenever the parts form an L, so some cells get no data at all. They sit
    outside the polygon and are never sampled by a correct rasteriser, but a
    zero there reads as a black bite out of the wall the moment rounding puts
    a sample one luxel off the edge.
    """
    frontier = [i for i in range(width * height) if solid[i]]
    if not frontier or len(frontier) == width * height:
        return
    while frontier:
        nxt = []
        for i in frontier:
            x, y = i % width, i // width
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < width and 0 <= ny < height:
                    j = ny * width + nx
                    if not solid[j]:
                        solid[j] = 1
                        luxels[j] = luxels[i]
                        nxt.append(j)
        frontier = nxt


def resample(block, target_scale):
    """Box-filter a block onto a coarser luxel grid.

    `target_scale` must be a whole multiple of the source scale so target
    luxels tile source luxels exactly; that keeps the target grid aligned to
    the same texture-space multiples and leaves the address arithmetic a
    shift. Vanilla Quake's 16 units is the natural target: it is what the
    original maps shipped, and at 120x80 it is already finer per screen pixel
    than 16 units was at 320x200.
    """
    step = target_scale // block.scale
    if step * block.scale != target_scale:
        raise ValueError("target lightmap scale must be a multiple of the source")
    if step == 1:
        return block
    # The source grid starts at grid_min_s source luxels, i.e. world s
    # grid_min_s*scale. Snap the target grid down to the target multiple that
    # contains it so target luxel boundaries stay on multiples of target_scale.
    min_s = math.floor(block.grid_min_s / step)
    min_t = math.floor(block.grid_min_t / step)
    width = math.ceil((block.grid_min_s + block.width) / step) - min_s
    height = math.ceil((block.grid_min_t + block.height) / step) - min_t
    luxels = bytearray(width * height)
    solid = bytearray(width * height)
    for y in range(height):
        for x in range(width):
            total = count = 0
            for sy in range((min_t + y) * step, (min_t + y + 1) * step):
                row = sy - block.grid_min_t
                if not 0 <= row < block.height:
                    continue
                base = row * block.width
                for sx in range((min_s + x) * step, (min_s + x + 1) * step):
                    column = sx - block.grid_min_s
                    if 0 <= column < block.width:
                        total += block.luxels[base + column]
                        count += 1
            if count:
                luxels[y * width + x] = (total + count // 2) // count
                solid[y * width + x] = 1
    # A target luxel can straddle the grid edge and catch no source cell at
    # all. Tracking coverage rather than testing for a zero byte matters: a
    # luxel that is genuinely pitch black is also zero.
    _fill_holes(luxels, solid, width, height)
    return FaceLightmap(width, height, luxels, min_s, min_t, target_scale,
                        block.styles)


def build_shade_table(palette, rows=64, neutral_row=32, fullbright_from=224,
                      gain=1.0):
    """Build Quake's colormap: shade[row][texel] is the palette index that
    looks like `texel` lit by `row / neutral_row`.

    A paletted renderer cannot multiply a pixel by a light level, so the
    multiply is done once here, offline, against every (light, texel) pair and
    the result is a palette index the framebuffer can take directly.

    Three details are not optional:

    * The neutral row is written as the identity, not as the nearest match of
      each colour to itself. Nearest-RGB is only self-inverse for 213 of the
      256 entries -- the rest sit close enough to a neighbour that the search
      picks the neighbour, and an unlit wall comes out visibly recoloured.
      (`gain` deliberately gives that up: once every row is scaled there is no
      identity row left to preserve.)
    * The last 32 entries are Quake's fullbrights and are never shaded. They
      map to themselves in every row, which is also what keeps a torch or a
      screen glowing in an otherwise dark room.
    * Fullbrights are excluded as match *targets* too. Without that a dark
      brown texel darkened one step can land on a bright orange fire colour,
      because that colour happens to be closest in RGB.
    """
    entries = [tuple(palette[3 * i:3 * i + 3]) for i in range(256)]
    table = bytearray(rows * 256)
    for row in range(rows):
        scale = gain * row / float(neutral_row)
        base = row * 256
        for index, (r, g, b) in enumerate(entries):
            if index >= fullbright_from:
                table[base + index] = index
                continue
            if row == neutral_row and gain == 1.0:
                table[base + index] = index
                continue
            want = (min(255, int(r * scale + 0.5)),
                    min(255, int(g * scale + 0.5)),
                    min(255, int(b * scale + 0.5)))
            best = best_distance = None
            for candidate in range(fullbright_from):
                cr, cg, cb = entries[candidate]
                distance = ((cr - want[0]) ** 2 + (cg - want[1]) ** 2 +
                            (cb - want[2]) ** 2)
                if best_distance is None or distance < best_distance:
                    best, best_distance = candidate, distance
            table[base + index] = best
    return table


def add_border(block):
    """Replicate a one-luxel ring around a block.

    The rasteriser derives the luxel index from the same interpolated texel
    coordinate it draws with, and that coordinate is quantised: a pixel on the
    face's own boundary rounds one luxel outside the block often enough to
    matter. Clamping in the segment setup is the obvious fix and was measured
    at 39,729 cycles a frame, because the two-sided clamp on each axis is what
    pushes the compiler into spilling the segment tail. A replicated ring
    returns exactly what the clamp would have returned, for 2 * (w + h + 2)
    bytes of ROM and no instructions at all.
    """
    width, height = block.width + 2, block.height + 2
    luxels = bytearray(width * height)
    for y in range(height):
        source_y = min(max(y - 1, 0), block.height - 1)
        for x in range(width):
            source_x = min(max(x - 1, 0), block.width - 1)
            luxels[y * width + x] = block.luxels[source_y * block.width + source_x]
    return FaceLightmap(width, height, luxels,
                        block.grid_min_s - 1, block.grid_min_t - 1,
                        block.scale, block.styles)
