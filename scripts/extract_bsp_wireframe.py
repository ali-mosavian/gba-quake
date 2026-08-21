#!/usr/bin/env python3
"""Convert a Quake 1 BSP into compact, ROM-resident wireframe data."""
import collections
import math, pathlib, re, struct, sys

import bsp_lightmap
import quake_palette

ENTITIES, PLANES, TEXTURES, VERTICES, VISIBILITY = 0, 1, 2, 3, 4
NODES, TEXINFO, FACES, LEAVES, MARKSURFACES, EDGES, SURFEDGES = 5, 6, 7, 10, 11, 12, 13
CLIPNODES, MODELS = 9, 14

def records(data, lump, fmt):
    size = struct.calcsize(fmt); offset, length = lump
    return [struct.unpack_from(fmt, data, p) for p in range(offset, offset + length, size)]

def player_spawn(text):
    for block in re.findall(r"\{(.*?)\}", text, re.S):
        if '"classname" "info_player_start"' in block:
            origin = re.search(r'"origin"\s+"([^\"]+)"', block)
            angle = re.search(r'"angle"\s+"([^\"]+)"', block)
            if origin:
                return tuple(map(float, origin.group(1).split())), float(angle.group(1)) if angle else 0.0
    raise ValueError("BSP has no info_player_start")

def emit(lines, declaration, values, per_line):
    lines.append(declaration + " = {")
    for start in range(0, len(values), per_line):
        lines.append("    " + ", ".join(values[start:start + per_line]) + ",")
    lines.append("};")

def pak_entry(path, wanted):
    data = pathlib.Path(path).read_bytes()
    if data[:4] != b"PACK": raise ValueError("expected Quake PACK file")
    directory, length = struct.unpack_from("<ii", data, 4)
    for offset in range(directory, directory + length, 64):
        name, start, size = struct.unpack_from("<56sii", data, offset)
        if name.split(b"\0", 1)[0].decode("latin1").lower() == wanted.lower():
            return data[start:start + size]
    raise ValueError(f"{wanted} not found in {path}")


def merge_coplanar_faces(faces, rings, normals, vertices, nodes, leaves, marks):
    """Merge edge-adjacent coplanar faces that share a texinfo, keeping every
    merged polygon convex.

    The scanline fill takes min/max x per row, so it can only draw convex
    polygons; a merge that produced a concavity would fill across it. This is
    the same rule qbsp's own face merging uses. Quake windings run clockwise
    seen from the front, so a convex turn is NEGATIVE against the face normal.

    Faces sharing a plane also share a BSP node (a node lists the faces lying
    on its plane), so merging never moves a face between nodes and the
    near-to-far ordering survives. Order among the merged faces themselves is
    irrelevant: they are coplanar and cannot occlude one another.
    """
    def sub(a, b): return (a[0]-b[0], a[1]-b[1], a[2]-b[2])
    def cross(a, b): return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
    def dot(a, b): return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]
    EPS = 1e-3

    def try_merge(r1, r2, normal):
        shared = None
        for i in range(len(r1)):
            a, b = r1[i], r1[(i + 1) % len(r1)]
            for j in range(len(r2)):
                c, d = r2[j], r2[(j + 1) % len(r2)]
                if a == d and b == c:
                    if shared is not None:
                        return None          # more than one shared edge
                    shared = (i, j)
        if shared is None:
            return None
        i, j = shared
        spliced = ([r1[(i + 1 + k) % len(r1)] for k in range(len(r1))][:-1] +
                   [r2[(j + 1 + k) % len(r2)] for k in range(len(r2))][:-1])
        out, n = [], len(spliced)
        for k in range(n):
            prev, cur, nxt = spliced[(k - 1) % n], spliced[k], spliced[(k + 1) % n]
            e1 = sub(vertices[cur], vertices[prev])
            e2 = sub(vertices[nxt], vertices[cur])
            turn = -dot(cross(e1, e2), normal)
            scale = math.sqrt(dot(e1, e1)) * math.sqrt(dot(e2, e2))
            if scale < 1e-9:
                continue
            if turn / scale < -EPS:
                return None                  # concave joint
            if abs(turn) / scale <= EPS:
                continue                     # collinear vertex, drop it
            out.append(cur)
        return out if len(out) >= 3 else None

    groups = collections.defaultdict(list)
    for i, (plane, side, first, count, tinfo, *_) in enumerate(faces):
        groups[(plane, side, tinfo)].append(i)

    survivor = list(range(len(faces)))
    working = {i: list(rings[i]) for i in range(len(faces))}
    absorbed = set()
    for members in groups.values():
        if len(members) < 2:
            continue
        pool = [m for m in members]
        changed = True
        while changed:
            changed = False
            for a in range(len(pool)):
                if pool[a] is None:
                    continue
                for b in range(a + 1, len(pool)):
                    if pool[b] is None:
                        continue
                    merged = try_merge(working[pool[a]], working[pool[b]],
                                       normals[pool[a]])
                    if merged:
                        working[pool[a]] = merged
                        survivor[pool[b]] = pool[a]
                        absorbed.add(pool[b])
                        pool[b] = None
                        changed = True
            pool = [p for p in pool if p is not None]

    # resolve chains, then renumber the survivors keeping their original order
    def root(i):
        while survivor[i] != i:
            i = survivor[i]
        return i
    keep = [i for i in range(len(faces)) if i not in absorbed]
    remap = {old: new for new, old in enumerate(keep)}
    face_map = [remap[root(i)] for i in range(len(faces))]

    new_faces = [faces[i] for i in keep]
    new_rings = [working[i] for i in keep]

    # a node's faces are those on its plane, so its range stays contiguous
    new_nodes = []
    for node in nodes:
        first, count = node[9], node[10]
        kept = [f for f in range(first, first + count) if f not in absorbed]
        new_first = remap[kept[0]] if kept else 0
        new_nodes.append(node[:9] + (new_first, len(kept)) + node[11:])

    # marksurfaces: remap, then drop duplicates a merge introduced
    new_marks, new_leaves = [], []
    for leaf in leaves:
        first, count = leaf[8], leaf[9]
        seen, start = [], len(new_marks)
        for m in marks[first:first + count]:
            mapped = face_map[m]
            if mapped not in seen:
                seen.append(mapped)
        new_marks.extend(seen)
        new_leaves.append(leaf[:8] + (start, len(seen)) + leaf[10:])

    return (new_faces, new_rings, new_nodes, new_leaves, new_marks,
            len(absorbed), face_map)


# The shade table is Quake's colormap: one row per light level, 256 entries
# per row. 64 rows is what Quake itself used and costs 16KB of ROM; the row is
# just the top six bits of the luxel, so the runtime needs no lookup to find it.
SHADE_ROWS = 64
SHADE_SHIFT = 2


def extract(input_path, output_path, pak_path=None, merge=True, mip=0,
            lmscale=16, lmgain=100):
    data = pathlib.Path(input_path).read_bytes()
    if struct.unpack_from("<i", data)[0] != 29: raise ValueError("expected Quake BSP v29")
    lumps = [struct.unpack_from("<ii", data, 4 + i * 8) for i in range(15)]
    eo, es = lumps[ENTITIES]
    spawn, angle = player_spawn(data[eo:eo + es].decode("latin1", "ignore"))
    vertices = records(data, lumps[VERTICES], "<fff")
    edges = records(data, lumps[EDGES], "<HH")
    surfedges = [x[0] for x in records(data, lumps[SURFEDGES], "<i")]
    planes = records(data, lumps[PLANES], "<ffffi")
    nodes = records(data, lumps[NODES], "<i2h3h3h2H")
    faces = records(data, lumps[FACES], "<hhihh4Bi")
    texinfo = records(data, lumps[TEXINFO], "<8fii")
    leaves = records(data, lumps[LEAVES], "<ii3h3h2H4B")
    # Collision uses the pre-expanded clip hull, so the player traces as a
    # point: qbsp already grew hull 1's clipnodes by the 32x32x56 player box.
    clipnodes = records(data, lumps[CLIPNODES], "<i2h")
    models = records(data, lumps[MODELS], "<9f4i3i")
    marks = [x[0] for x in records(data, lumps[MARKSURFACES], "<H")]
    vo, vs = lumps[VISIBILITY]; visibility = data[vo:vo + vs]
    to, ts = lumps[TEXTURES]; texture_lump = data[to:to + ts]
    texture_count = struct.unpack_from("<i", texture_lump)[0]
    texture_offsets = struct.unpack_from(f"<{texture_count}i", texture_lump, 4)
    textures, texture_pixels = [], bytearray()
    # Quake miptex carries four mip levels. The originals are authored for
    # 320x200; this renderer draws 120x80 and doubles, so level 0 is far more
    # texture than the screen can show. Selecting a lower level costs nothing
    # at runtime: the texture axes below are scaled by the same factor, so u
    # and v arrive already in the chosen level's texel units and the GBA code
    # is unchanged.
    for offset in texture_offsets:
        if offset < 0:
            textures.append((0, 1, 1)); continue
        _, width, height, *mip_offsets = struct.unpack_from("<16s6I", texture_lump, offset)
        level = mip
        while level and ((width >> level) < 1 or (height >> level) < 1):
            level -= 1
        base = mip_offsets[level]
        width, height = width >> level, height >> level
        pixels = texture_lump[offset + base:offset + base + width * height]
        stored_width = 1 << (width - 1).bit_length()
        stored_height = 1 << (height - 1).bit_length()
        textures.append((len(texture_pixels), stored_width, stored_height))
        for y in range(stored_height):
            source_y = y % height
            for x in range(stored_width):
                texture_pixels.append(pixels[source_y * width + (x % width)])
    palette_rgb = None
    if pak_path:
        try:
            palette_rgb = pak_entry(pak_path, "gfx/palette.lmp")
        except (OSError, ValueError) as error:
            print(f"  pak unavailable ({error}); using the bundled palette")
    if palette_rgb is None:
        palette_rgb = quake_palette.QUAKE_PALETTE
    palette = [(palette_rgb[i] >> 3) | ((palette_rgb[i+1] >> 3) << 5) |
               ((palette_rgb[i+2] >> 3) << 10) for i in range(0, 768, 3)]
    # Explicit vertex ring per face.
    #
    # At runtime a face's vertices are otherwise reached through
    # surfedge -> edge -> vertex: three dependent loads across ROM and EWRAM,
    # walked once to build the transform list and again to project. The ring
    # is fixed at build time, so emit it directly and let the GBA read one
    # halfword per vertex. Rings are also what makes merging possible: a merged
    # face is no longer expressible as a run of the original surfedges.
    rings = []
    normals = []
    for plane, side, first, count, texture_info, *_ in faces:
        ring = []
        for directed_edge in surfedges[first:first + count]:
            edge = edges[abs(directed_edge)]
            ring.append(edge[0] if directed_edge >= 0 else edge[1])
        rings.append(ring)
        n = planes[plane][:3]
        normals.append(tuple(-c for c in n) if side else n)

    # Lightmaps are addressed by the ORIGINAL face list, so decode them before
    # merging rewrites it, and keep the map from old face to new.
    source_lightmaps, light_info = bsp_lightmap.read_lightmaps(
        data, lumps, vertices, edges, surfedges, texinfo, faces)
    print(f"  lightmaps: {light_info['lit_faces']} lit faces, "
          f"{light_info['lump_bytes']} bytes at {light_info['scale']} units/luxel, "
          f"border {light_info['border']}, "
          f"{'bipolar' if light_info['bipolar'] else 'vanilla'}, "
          f"{light_info['tiling_gaps']} tiling gaps, "
          f"{light_info['animated_faces']} faces with animated styles "
          f"({light_info['styles_out_of_order']} storing style 0 second)")
    face_map = list(range(len(faces)))
    if merge:
        faces, rings, nodes, leaves, marks, absorbed, face_map = merge_coplanar_faces(
            faces, rings, normals, vertices, nodes, leaves, marks)
        print(f"  merged {absorbed} coplanar faces away "
              f"({100.0 * absorbed / (len(faces) + absorbed):.1f}%), "
              f"{len(faces)} remain")

    lines = ["/* Generated from a Quake 1 BSP; do not edit. */",
             "#ifndef BSP_WIREFRAME_MAP_H", "#define BSP_WIREFRAME_MAP_H", "enum {"]
    counts = [("VERTEX", vertices), ("EDGE", edges), ("SURFEDGE", surfedges),
              ("PLANE", planes), ("NODE", nodes), ("FACE", faces),
              ("LEAF", leaves), ("MARKSURFACE", marks)]
    lines += [f"    BSP_{name}_COUNT = {len(values)}," for name, values in counts]
    lines += [f"    BSP_LUXEL_SHIFT = {8 + (lmscale.bit_length() - 1) - mip},",
              f"    BSP_SHADE_ROWS = {SHADE_ROWS},",
              f"    BSP_SHADE_SHIFT = {SHADE_SHIFT},",
              f"    BSP_CLIPNODE_COUNT = {len(clipnodes)},",
              f"    BSP_PLAYER_HULL_HEAD = {models[0][10]},",
              f"    BSP_VISIBILITY_BYTES = {len(visibility)},",
              f"    BSP_SPAWN_X = {round(spawn[0])}, BSP_SPAWN_Y = {round(spawn[1])},",
              f"    BSP_SPAWN_Z = {round(spawn[2])}, BSP_SPAWN_YAW = {round(angle * 256 / 360)},", "};"]
    emit(lines, "static const MapVertex bsp_vertices[BSP_VERTEX_COUNT]",
         [f"{{{round(x)}, {round(y)}, {round(z)}}}" for x, y, z in vertices], 4)
    emit(lines, "static const MapEdge bsp_edges[BSP_EDGE_COUNT]", [f"{{{a}, {b}}}" for a, b in edges], 8)
    emit(lines, "static const int16_t bsp_surfedges[BSP_SURFEDGE_COUNT]", [str(x) for x in surfedges], 16)
    emit(lines, "static const MapPlane bsp_planes[BSP_PLANE_COUNT]",
         [f"{{{round(x*16384)}, {round(y*16384)}, {round(z*16384)}, {round(d)}}}" for x,y,z,d,_ in planes], 3)
    # Rendering rounds plane distances to whole units, which is invisible on a
    # 120x80 screen but would let the player sink or float by up to half a
    # unit. Collision gets the distance at Q8 instead.
    emit(lines, "static const int32_t bsp_plane_distance_q8[BSP_PLANE_COUNT]",
         [str(round(d * 256)) for _, _, _, d, _ in planes], 8)
    emit(lines, "static const MapClipNode bsp_clipnodes[BSP_CLIPNODE_COUNT]",
         [f"{{{planenum}, {{{c0}, {c1}}}}}" for planenum, c0, c1 in clipnodes], 4)
    emit(lines, "static const MapNode bsp_nodes[BSP_NODE_COUNT]",
         [f"{{{node[0]}, {{{node[1]}, {node[2]}}}, {node[9]}, {node[10]}}}" for node in nodes], 4)
    # Quantised texture axes, matching what the GBA reads at runtime. Face
    # texture origins are derived from these so host and target agree exactly.
    axes_q12 = [[round(value * (4096.0 / (1 << mip))) for value in info[:8]]
                for info in texinfo]

    def runtime_uv_q8(vertex, texture_info):
        """Reproduce r_surf.c world_texture_coordinates for one vertex."""
        wx, wy, wz = (round(component) for component in vertex)
        a = axes_q12[texture_info]
        u = (a[0] * wx + a[1] * wy + a[2] * wz + a[3]) >> 4
        v = (a[4] * wx + a[5] * wy + a[6] * wz + a[7]) >> 4
        return u, v

    # Lightmaps, re-baked onto a coarser grid and re-addressed into the same
    # per-face texture space the rasteriser already interpolates.
    #
    # The luxel grid is defined in the texinfo's own units -- the mip-0 texel
    # grid -- but u and v arrive at the rasteriser in the loaded mip level's
    # units and with this face's texture origin already subtracted. Both of
    # those are constant per face, so the whole conversion collapses to one add
    # and one shift: luxel = (u + bias) >> BSP_LUXEL_SHIFT.
    luxel_shift = 8 + (lmscale.bit_length() - 1) - mip
    face_sources = collections.defaultdict(list)
    for old_face, new_face in enumerate(face_map):
        face_sources[new_face].append(old_face)
    neutral_byte = 128 if light_info["bipolar"] else 255
    unlit = bsp_lightmap.add_border(
        bsp_lightmap.FaceLightmap(1, 1, bytes([neutral_byte]), 0, 0, lmscale, []))
    luxel_bytes = bytearray()
    luxel_cache = {}
    light_values = []
    luxels_out_of_range = 0

    def face_lightmap(new_face):
        """One face's luxels, whether it is one BSP face or several merged."""
        parts = [source_lightmaps[i] for i in face_sources[new_face]
                 if source_lightmaps[i]]
        if not parts:
            return unlit
        block = (parts[0] if len(parts) == 1
                 else bsp_lightmap.paste_blocks(parts, light_info["scale"]))
        return bsp_lightmap.add_border(bsp_lightmap.resample(block, lmscale))

    face_vertex_ring = []
    face_vertex_texcoords = []
    face_values = []
    for face_index, (plane, side, first, count, texture_info, *_) in enumerate(faces):
        ring = rings[face_index]
        points = [vertices[index] for index in ring]
        center = tuple(sum(point[axis] for point in points) / len(points) for axis in range(3))
        radius = max(math.sqrt(sum((point[axis] - center[axis]) ** 2 for axis in range(3))) for point in points)
        # Per-face texture origin, as Quake's texturemins does. Absolute world
        # texture coordinates reach several thousand texels on a map this size,
        # which costs the fixed-point pipeline the precision it needs for u/z.
        # The base must be a whole multiple of the (power-of-two) texture size
        # so masking the low bits still wraps to exactly the same texel.
        width, height = textures[texinfo[texture_info][8]][1:3]
        uv = [runtime_uv_q8(vertices[index], texture_info) for index in ring]
        u_base = ((min(u for u, _ in uv) >> 8) // width) * width
        v_base = ((min(v for _, v in uv) >> 8) // height) * height
        ring_start = len(face_vertex_ring)
        face_vertex_ring.extend(ring)
        face_vertex_texcoords.extend(
            (u - (u_base << 8), v - (v_base << 8)) for u, v in uv)
        face_values.append(f"{{{plane}, {side}, {first}, {len(ring)}, {texture_info}, {round(center[0])}, "
                           f"{round(center[1])}, {round(center[2])}, {math.ceil(radius)}, "
                           f"{u_base << 8}, {v_base << 8}, {ring_start}}}")

        block = face_lightmap(face_index)
        # Store the shade row, not the luxel. The row is the top six bits of
        # the luxel and nothing at runtime wants the other two, so doing the
        # shift here saves an instruction per segment and lets the table be
        # indexed by a plain byte.
        rows = bytes(value >> SHADE_SHIFT for value in block.luxels)
        key = (block.width, rows)
        if key not in luxel_cache:
            luxel_cache[key] = len(luxel_bytes)
            luxel_bytes.extend(rows)
        # Fold the whole address into one offset.
        #
        # The rasteriser's u is the absolute texel coordinate minus u_base, and
        # the luxel grid starts at grid_min. Both offsets are whole luxels --
        # u_base is a multiple of the texture width, which at this mip is a
        # multiple of a luxel, and grid_min is in luxels by construction -- so
        # the shift distributes over them and they collapse into the array
        # index rather than being added to u at runtime.
        offset_u = (u_base << 8) - (block.grid_min_s << luxel_shift)
        offset_v = (v_base << 8) - (block.grid_min_t << luxel_shift)
        if offset_u % (1 << luxel_shift) or offset_v % (1 << luxel_shift):
            raise ValueError(
                f"face {face_index}: texture origin is not a whole luxel "
                f"({offset_u}, {offset_v} at 1<<{luxel_shift}); the runtime "
                "folds it into the array index and cannot carry a remainder")
        base = (luxel_cache[key] + (offset_v >> luxel_shift) * block.width +
                (offset_u >> luxel_shift))
        light_values.append(f"{{{base}, {block.width}}}")
        # Check the runtime expression, not the derivation behind it: every
        # vertex of the face must land on the block's interior, leaving the
        # replicated border for the rounding. An origin one luxel out still
        # renders -- it renders the neighbouring luxel, and the seam shows up
        # as a faint band along one edge of one wall.
        if block is not unlit:
            for index in ring:
                u, v = runtime_uv_q8(vertices[index], texture_info)
                x = ((u - (u_base << 8)) >> luxel_shift) + (offset_u >> luxel_shift)
                y = ((v - (v_base << 8)) >> luxel_shift) + (offset_v >> luxel_shift)
                if not (1 <= x < block.width - 1 and 1 <= y < block.height - 1):
                    luxels_out_of_range += 1
    emit(lines, "static const MapFace bsp_faces[BSP_FACE_COUNT]", face_values, 2)
    print(f"  lightmap grid: {lmscale} units/luxel, {len(luxel_bytes)} luxel bytes "
          f"({len(luxel_cache)} distinct blocks), shift {luxel_shift}, "
          f"{luxels_out_of_range} vertices outside their block")
    emit(lines, "static const MapFaceLight bsp_face_lights[BSP_FACE_COUNT]",
         light_values, 2)
    # Padded to a power of two so the rasteriser's bounds test is one AND
    # rather than a compare and a branch. The test is there for safety, not
    # accuracy -- a segment that survives near-plane clipping with a very
    # large 1/z can produce an index far outside its own face -- so wrapping
    # is as good an answer as clamping, and it costs one instruction.
    padded = 1 << (len(luxel_bytes) - 1).bit_length()
    lines.append(f"#define BSP_LUXEL_MASK 0x{padded - 1:x}u")
    luxel_bytes.extend(bytes(padded - len(luxel_bytes)))
    emit(lines, "static const uint8_t bsp_lightmap_luxels[]",
         [str(x) for x in luxel_bytes], 24)
    # An overall exposure, applied to the table so it costs nothing at
    # runtime. dm1's bake centres on 0.75x -- softquake's light tool writes an
    # unlit surface as byte 96, not 128 -- so the map renders at about two
    # thirds of the raw texture brightness, which is faithful but dim on a
    # handheld. --lmgain scales every row.
    shade = bsp_lightmap.build_shade_table(
        palette_rgb, rows=SHADE_ROWS,
        neutral_row=(neutral_byte >> SHADE_SHIFT), gain=lmgain / 100.0)
    emit(lines, "static const uint8_t bsp_shade_table[BSP_SHADE_ROWS * 256]"
                " __attribute__((aligned(4)))", [str(x) for x in shade], 24)
    emit(lines, "static const uint16_t bsp_face_vertices[]",
         [str(v) for v in face_vertex_ring], 16)
    # Texture coordinates per ring entry, not per vertex.
    #
    # s and t are a function of the world vertex and the face's texinfo alone,
    # so recomputing them every frame recomputes a constant -- six multiplies
    # and eight axis reads per ring vertex, measured at 93K cycles a frame.
    # They cannot be cached per vertex at runtime: a vertex shared by three
    # faces has three different pairs, because each face subtracts its own
    # texture origin. Per ring entry is the granularity that makes them
    # constant. Emitted with that origin already subtracted, exactly as
    # world_texture_coordinates left them.
    emit(lines, "static const int32_t bsp_face_texcoords[]",
         [str(value) for pair in face_vertex_texcoords for value in pair], 12)
    # Axes scaled by the mip factor so u and v come out in the stored level's
    # texel units; nothing downstream needs to know the level.
    axis_scale = 4096.0 / (1 << mip)
    emit(lines, "static const MapTexInfo bsp_texinfo[]",
         ["{{{%d, %d, %d, %d}, {%d, %d, %d, %d}}, %d}" % tuple(
             [round(value * axis_scale) for value in info[:8]] + [info[8]]) for info in texinfo], 2)
    emit(lines, "static const MapTexture bsp_textures[]",
         [f"{{{offset}, {width}, {height}}}" for offset, width, height in textures], 4)
    emit(lines, "static const uint8_t bsp_texture_pixels[]", [str(x) for x in texture_pixels], 24)
    emit(lines, "static const uint16_t bsp_palette[256]", [str(x) for x in palette], 16)
    emit(lines, "static const MapLeaf bsp_leaves[BSP_LEAF_COUNT]",
         [f"{{{leaf[0]}, {leaf[1]}, {leaf[8]}, {leaf[9]}}}" for leaf in leaves], 4)
    emit(lines, "static const uint16_t bsp_marksurfaces[BSP_MARKSURFACE_COUNT]", [str(x) for x in marks], 16)
    emit(lines, "static const uint8_t bsp_visibility[BSP_VISIBILITY_BYTES]", [str(x) for x in visibility], 24)
    lines.append("#endif")
    pathlib.Path(output_path).write_text("\n".join(lines) + "\n")
    print(f"BSP package: {len(vertices)} vertices, {len(edges)} edges, {len(faces)} faces, "
          f"{len(leaves)} leaves, mip {mip}, texture bytes {len(texture_pixels)}")

if __name__ == "__main__":
    extract(sys.argv[1], sys.argv[2],
            sys.argv[3] if len(sys.argv) > 3 else None,
            merge="--no-merge" not in sys.argv,
            mip=next((int(a.split("=")[1]) for a in sys.argv if a.startswith("--mip=")), 0),
            lmscale=next((int(a.split("=")[1]) for a in sys.argv
                          if a.startswith("--lmscale=")), 16),
            lmgain=next((int(a.split("=")[1]) for a in sys.argv
                         if a.startswith("--lmgain=")), 100))
