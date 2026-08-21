#!/usr/bin/env python3
"""Bake a top-down, pre-lit picture of one floor plane into an affine BG.

The PPU can draw a flat plane in perspective by itself: an affine background
whose matrix is rewritten every HBlank sweeps one scaled, rotated line of a
texture per scanline, which is exactly what a floor at a fixed height looks
like through a camera that cannot pitch or roll. What the PPU needs is that
floor as a single flat image -- texture and lightmap already multiplied
through, because the affine BG samples raw bytes.

This reads the generated map header (so it agrees byte-for-byte with what the
software renderer draws), picks the floor plane under the spawn point, and
rasterises every up-facing face at that height into a 256x256-pixel plan at
PLAN_UNITS world units per pixel, lit through the same shade table.

An affine BG map entry is one byte, so the plan may use at most 256 distinct
8x8 tiles. The tile budget is reported; overflow falls back to the closest
existing tile and is a proof-quality artefact, not a correctness bug.
"""
import re, sys, pathlib

HEADER = pathlib.Path("src/generated/bsp_wireframe_map.h")
PLAN_SIZE = 256          # pixels on a side
PLAN_UNITS = 8           # world units per plan pixel


def load(text, name):
    match = re.search(r"\b%s\[[^\]]*\][^=]*= \{(.*?)\n\};" % name, text, re.S)
    body = match.group(1)
    if "{" not in body:
        return [int(v) for v in body.replace("\n", " ").split(",") if v.strip()]
    records, depth, start = [], 0, None
    for i, character in enumerate(body):
        if character == "{":
            if depth == 0:
                start = i
            depth += 1
        elif character == "}":
            depth -= 1
            if depth == 0:
                records.append([int(v) for v in re.findall(r"-?\d+", body[start:i])])
    return records


def main(output_path):
    text = HEADER.read_text()
    enum = dict(re.findall(r"BSP_(\w+) = (-?\d+)", text))
    faces = load(text, "bsp_faces")
    runtime = load(text, "bsp_planes")
    lights = load(text, "bsp_face_lights")
    texinfo = load(text, "bsp_texinfo")
    textures = load(text, "bsp_textures")
    pixels = load(text, "bsp_texture_pixels")
    luxels = load(text, "bsp_lightmap_luxels")
    shade = load(text, "bsp_shade_table")
    palette = load(text, "bsp_palette")
    vertices = load(text, "bsp_vertices")
    ring = load(text, "bsp_face_vertices")
    luxel_shift = int(enum["LUXEL_SHIFT"])
    luxel_mask = int(re.search(r"BSP_LUXEL_MASK 0x([0-9a-f]+)", text).group(1), 16)
    spawn = (int(enum["SPAWN_X"]), int(enum["SPAWN_Y"]), int(enum["SPAWN_Z"]))

    # Up-facing faces, with their rings as 2D polygons.
    floors = []
    for index, face in enumerate(faces):
        plane = runtime[face[0]]
        nz = -plane[2] if face[1] else plane[2]
        if nz != 16384:
            continue
        count, first = face[3], face[11]
        poly = [(vertices[ring[first + i]][0], vertices[ring[first + i]][1])
                for i in range(count)]
        z = vertices[ring[first]][2]
        floors.append((index, z, poly))

    def contains(poly, x, y):
        inside = False
        for i in range(len(poly)):
            (x1, y1), (x2, y2) = poly[i], poly[(i + 1) % len(poly)]
            if (y1 > y) != (y2 > y) and \
               x < x1 + (x2 - x1) * (y - y1) / (y2 - y1):
                inside = not inside
        return inside

    # The floor plane under the spawn: the highest up-facing face containing
    # the spawn point at or below it.
    below = [(z, i) for i, z, poly in floors
             if z <= spawn[2] and contains(poly, spawn[0], spawn[1])]
    if not below:
        raise SystemExit("no floor found under the spawn point")
    floor_z = max(below)[0]
    chosen = [(i, poly) for i, z, poly in floors if z == floor_z]
    print(f"floor plane z={floor_z}: {len(chosen)} faces "
          f"(of {len(floors)} up-facing)")

    # Centre the plan on the map's extent at this height.
    xs = [x for _, poly in chosen for x, _ in poly]
    ys = [y for _, poly in chosen for _, y in poly]
    extent = PLAN_SIZE * PLAN_UNITS
    origin_x = (min(xs) + max(xs)) // 2 - extent // 2
    origin_y = (min(ys) + max(ys)) // 2 - extent // 2
    print(f"plan origin ({origin_x}, {origin_y}), {extent} units square; "
          f"faces span x {min(xs)}..{max(xs)} y {min(ys)}..{max(ys)}")

    plan = bytearray(PLAN_SIZE * PLAN_SIZE)
    covered = 0
    for face_index, poly in chosen:
        face = faces[face_index]
        axes = texinfo[face[4]][:8]
        base, width, height = textures[texinfo[face[4]][8]]
        u_base, v_base = face[9], face[10]
        lo, lw = lights[face_index]
        min_px = max(0, (min(x for x, _ in poly) - origin_x) // PLAN_UNITS)
        max_px = min(PLAN_SIZE - 1, (max(x for x, _ in poly) - origin_x) // PLAN_UNITS)
        min_py = max(0, (min(y for _, y in poly) - origin_y) // PLAN_UNITS)
        max_py = min(PLAN_SIZE - 1, (max(y for _, y in poly) - origin_y) // PLAN_UNITS)
        for py in range(min_py, max_py + 1):
            wy = origin_y + py * PLAN_UNITS + PLAN_UNITS // 2
            for px in range(min_px, max_px + 1):
                wx = origin_x + px * PLAN_UNITS + PLAN_UNITS // 2
                if not contains(poly, wx, wy):
                    continue
                u = ((axes[0] * wx + axes[1] * wy + axes[2] * floor_z + axes[3])
                     >> 4) - u_base
                v = ((axes[4] * wx + axes[5] * wy + axes[6] * floor_z + axes[7])
                     >> 4) - v_base
                texel = pixels[base + ((v >> 8) % height) * width +
                               ((u >> 8) % width)]
                row = luxels[(lo + (v >> luxel_shift) * lw +
                              (u >> luxel_shift)) & luxel_mask]
                plan[py * PLAN_SIZE + px] = shade[row * 256 + texel]
                covered += 1
    print(f"covered {covered} plan pixels ({100.0 * covered / len(plan):.1f}%)")

    # Tile the plan: 8x8 cells, at most 256 distinct (map entries are bytes).
    tiles, tile_index = [], {}
    cells = PLAN_SIZE // 8
    plan_map = bytearray(cells * cells)
    fallback = 0
    for cy in range(cells):
        for cx in range(cells):
            tile = bytes(plan[(cy * 8 + row) * PLAN_SIZE + cx * 8 + column]
                         for row in range(8) for column in range(8))
            if tile not in tile_index:
                if len(tiles) < 256:
                    tile_index[tile] = len(tiles)
                    tiles.append(tile)
                else:
                    fallback += 1
                    tile_index[tile] = min(
                        range(len(tiles)),
                        key=lambda t: sum((a - b) ** 2
                                          for a, b in zip(tiles[t], tile)))
            plan_map[cy * cells + cx] = tile_index[tile]
    print(f"{len(tiles)} distinct tiles" +
          (f", {fallback} cells fell back to a nearest tile" if fallback else ""))

    lines = ["/* Generated by generate_floor_plan.py; do not edit. */",
             "#ifndef FLOOR_PLAN_H", "#define FLOOR_PLAN_H", "enum {",
             f"    FLOOR_PLAN_Z = {floor_z},",
             f"    FLOOR_PLAN_ORIGIN_X = {origin_x},",
             f"    FLOOR_PLAN_ORIGIN_Y = {origin_y},",
             f"    FLOOR_PLAN_UNITS = {PLAN_UNITS},",
             f"    FLOOR_PLAN_TILE_COUNT = {len(tiles)},",
             f"    FLOOR_SPAWN_X = {spawn[0]}, FLOOR_SPAWN_Y = {spawn[1]},",
             "};"]

    def emit(name, values, per_line):
        lines.append(f"static const {name} = {{")
        for start in range(0, len(values), per_line):
            lines.append("    " + ", ".join(str(v) for v in
                                            values[start:start + per_line]) + ",")
        lines.append("};")

    flat_tiles = [b for tile in tiles for b in tile]
    emit("uint8_t floor_plan_tiles[]", flat_tiles, 24)
    emit("uint8_t floor_plan_map[]", list(plan_map), 24)
    emit("uint16_t floor_plan_palette[256]", palette, 16)
    lines.append("#endif")
    pathlib.Path(output_path).write_text("\n".join(lines) + "\n")
    print(f"wrote {output_path}: {len(flat_tiles)} tile bytes, "
          f"{len(plan_map)} map bytes")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "src/generated/floor_plan.h")
