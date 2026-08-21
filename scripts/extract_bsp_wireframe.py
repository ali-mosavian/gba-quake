#!/usr/bin/env python3
"""Convert a Quake 1 BSP into compact, ROM-resident wireframe data."""
import collections
import math, pathlib, re, struct, sys

ENTITIES, PLANES, TEXTURES, VERTICES, VISIBILITY = 0, 1, 2, 3, 4
NODES, TEXINFO, FACES, LEAVES, MARKSURFACES, EDGES, SURFEDGES = 5, 6, 7, 10, 11, 12, 13

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

    return new_faces, new_rings, new_nodes, new_leaves, new_marks, len(absorbed)


def extract(input_path, output_path, pak_path=None, merge=True):
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
    marks = [x[0] for x in records(data, lumps[MARKSURFACES], "<H")]
    vo, vs = lumps[VISIBILITY]; visibility = data[vo:vo + vs]
    to, ts = lumps[TEXTURES]; texture_lump = data[to:to + ts]
    texture_count = struct.unpack_from("<i", texture_lump)[0]
    texture_offsets = struct.unpack_from(f"<{texture_count}i", texture_lump, 4)
    textures, texture_pixels = [], bytearray()
    for offset in texture_offsets:
        if offset < 0:
            textures.append((0, 1, 1)); continue
        _, width, height, mip0, _, _, _ = struct.unpack_from("<16s6I", texture_lump, offset)
        pixels = texture_lump[offset + mip0:offset + mip0 + width * height]
        stored_width = 1 << (width - 1).bit_length()
        stored_height = 1 << (height - 1).bit_length()
        textures.append((len(texture_pixels), stored_width, stored_height))
        for y in range(stored_height):
            source_y = y % height
            for x in range(stored_width):
                texture_pixels.append(pixels[source_y * width + (x % width)])
    if pak_path:
        palette_rgb = pak_entry(pak_path, "gfx/palette.lmp")
        palette = [(palette_rgb[i] >> 3) | ((palette_rgb[i+1] >> 3) << 5) |
                   ((palette_rgb[i+2] >> 3) << 10) for i in range(0, 768, 3)]
    else:
        palette = [((i >> 3) * 0x421) for i in range(256)]
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

    if merge:
        faces, rings, nodes, leaves, marks, absorbed = merge_coplanar_faces(
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
    lines += [f"    BSP_VISIBILITY_BYTES = {len(visibility)},",
              f"    BSP_SPAWN_X = {round(spawn[0])}, BSP_SPAWN_Y = {round(spawn[1])},",
              f"    BSP_SPAWN_Z = {round(spawn[2])}, BSP_SPAWN_YAW = {round(angle * 256 / 360)},", "};"]
    emit(lines, "static const MapVertex bsp_vertices[BSP_VERTEX_COUNT]",
         [f"{{{round(x)}, {round(y)}, {round(z)}}}" for x, y, z in vertices], 4)
    emit(lines, "static const MapEdge bsp_edges[BSP_EDGE_COUNT]", [f"{{{a}, {b}}}" for a, b in edges], 8)
    emit(lines, "static const int16_t bsp_surfedges[BSP_SURFEDGE_COUNT]", [str(x) for x in surfedges], 16)
    emit(lines, "static const MapPlane bsp_planes[BSP_PLANE_COUNT]",
         [f"{{{round(x*16384)}, {round(y*16384)}, {round(z*16384)}, {round(d)}}}" for x,y,z,d,_ in planes], 3)
    emit(lines, "static const MapNode bsp_nodes[BSP_NODE_COUNT]",
         [f"{{{node[0]}, {{{node[1]}, {node[2]}}}, {node[9]}, {node[10]}}}" for node in nodes], 4)
    # Quantised texture axes, matching what the GBA reads at runtime. Face
    # texture origins are derived from these so host and target agree exactly.
    axes_q12 = [[round(value * 4096) for value in info[:8]] for info in texinfo]

    def runtime_uv_q8(vertex, texture_info):
        """Reproduce r_surf.c world_texture_coordinates for one vertex."""
        wx, wy, wz = (round(component) for component in vertex)
        a = axes_q12[texture_info]
        u = (a[0] * wx + a[1] * wy + a[2] * wz + a[3]) >> 4
        v = (a[4] * wx + a[5] * wy + a[6] * wz + a[7]) >> 4
        return u, v

    face_vertex_ring = []
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
        face_values.append(f"{{{plane}, {side}, {first}, {len(ring)}, {texture_info}, {round(center[0])}, "
                           f"{round(center[1])}, {round(center[2])}, {math.ceil(radius)}, "
                           f"{u_base << 8}, {v_base << 8}, {ring_start}}}")
    emit(lines, "static const MapFace bsp_faces[BSP_FACE_COUNT]", face_values, 2)
    emit(lines, "static const uint16_t bsp_face_vertices[]",
         [str(v) for v in face_vertex_ring], 16)
    emit(lines, "static const MapTexInfo bsp_texinfo[]",
         ["{{{%d, %d, %d, %d}, {%d, %d, %d, %d}}, %d}" % tuple(
             [round(value * 4096) for value in info[:8]] + [info[8]]) for info in texinfo], 2)
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
    print(f"BSP package: {len(vertices)} vertices, {len(edges)} edges, {len(faces)} faces, {len(leaves)} leaves")

if __name__ == "__main__":
    extract(sys.argv[1], sys.argv[2],
            sys.argv[3] if len(sys.argv) > 3 else None,
            merge="--no-merge" not in sys.argv)
