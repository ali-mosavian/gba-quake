#!/usr/bin/env python3
"""Render lit faces to a PNG, straight out of the generated map header.

There is no way to check a lightmap by reading the numbers: an origin that is
one luxel out, a block whose width is wrong, a shade table that snapped a
colour to the wrong ramp -- all of them produce plausible bytes. This unrolls
each face in its own texture space and applies the lightmap with the exact
integer expression the rasteriser uses, so what comes out is what the GBA will
draw, minus the perspective.

Usage: preview_lightmaps.py [out.png] [--faces=N] [--scale=N]
"""
import re, sys, pathlib
from PIL import Image

HEADER = pathlib.Path("src/generated/bsp_wireframe_map.h")


def load(text, name):
    """One generated array, as a flat list of ints or a list of records.

    Records are split on top-level commas rather than by regex: MapTexInfo
    nests two brace groups inside each record, and any pattern that matches
    braces non-greedily finds the inner ones first.
    """
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


def main(out_path="lightmap_preview.png", faces_wanted=12, zoom=2):
    text = HEADER.read_text()
    enum = dict(re.findall(r"BSP_(\w+) = (-?\d+)", text))
    luxel_shift = int(enum["LUXEL_SHIFT"])
    faces = load(text, "bsp_faces")
    lights = load(text, "bsp_face_lights")
    texinfo = load(text, "bsp_texinfo")
    textures = load(text, "bsp_textures")
    pixels = load(text, "bsp_texture_pixels")
    luxels = load(text, "bsp_lightmap_luxels")
    shade = load(text, "bsp_shade_table")
    mask = int(re.search(r"BSP_LUXEL_MASK 0x([0-9a-f]+)", text).group(1), 16)
    palette = load(text, "bsp_palette")
    vertices = load(text, "bsp_vertices")
    ring = load(text, "bsp_face_vertices")

    def rgb(index):
        value = palette[index]
        return tuple(((value >> s) & 31) * 255 // 31 for s in (0, 5, 10))

    # Widest faces first: they are the ones a wrong origin or a wrong block
    # width shows up on, and the ones the merge produced.
    order = sorted(range(len(faces)), key=lambda i: -lights[i][1])[:faces_wanted]

    tiles = []
    for face_index in order:
        (_plane, _side, _first, count, tex_index, _cx, _cy, _cz, _radius,
         u_base_q8, v_base_q8, first_vertex) = faces[face_index]
        luxel_base, luxel_w = lights[face_index]
        axes = texinfo[tex_index][:8]
        texture_index = texinfo[tex_index][8]
        base, width, height = textures[texture_index]

        us, vs = [], []
        for i in range(count):
            x, y, z = vertices[ring[first_vertex + i]]
            us.append(((axes[0] * x + axes[1] * y + axes[2] * z + axes[3]) >> 4) - u_base_q8)
            vs.append(((axes[4] * x + axes[5] * y + axes[6] * z + axes[7]) >> 4) - v_base_q8)
        u0, u1 = min(us) >> 8, (max(us) >> 8) + 1
        v0, v1 = min(vs) >> 8, (max(vs) >> 8) + 1
        tile_w, tile_h = min(u1 - u0, 256), min(v1 - v0, 256)
        if tile_w < 2 or tile_h < 2:
            continue

        flat = Image.new("RGB", (tile_w, tile_h))
        lit = Image.new("RGB", (tile_w, tile_h))
        flat_px, lit_px = flat.load(), lit.load()
        for ty in range(tile_h):
            v_q8 = (v0 + ty) << 8
            for tx in range(tile_w):
                u_q8 = (u0 + tx) << 8
                texel = pixels[base + ((v_q8 >> 8) % height) * width +
                               ((u_q8 >> 8) % width)]
                row = luxels[(luxel_base + (v_q8 >> luxel_shift) * luxel_w +
                              (u_q8 >> luxel_shift)) & mask]
                flat_px[tx, ty] = rgb(texel)
                lit_px[tx, ty] = rgb(shade[row * 256 + texel])
        tiles.append((face_index, luxel_w, flat, lit))

    pad, label = 6, 12
    cell_w = max(t[2].width for t in tiles) * zoom
    cell_h = max(t[2].height for t in tiles) * zoom
    columns = 4
    rows = (len(tiles) + columns - 1) // columns
    sheet = Image.new("RGB", (columns * (2 * cell_w + 3 * pad),
                              rows * (cell_h + pad + label)), (24, 24, 28))
    for i, (face_index, lw, flat, lit) in enumerate(tiles):
        cx = (i % columns) * (2 * cell_w + 3 * pad) + pad
        cy = (i // columns) * (cell_h + pad + label) + label
        sheet.paste(flat.resize((flat.width * zoom, flat.height * zoom), Image.NEAREST), (cx, cy))
        sheet.paste(lit.resize((lit.width * zoom, lit.height * zoom), Image.NEAREST),
                    (cx + cell_w + pad, cy))
        print(f"  face {face_index}: {flat.width}x{flat.height} texels, "
              f"lightmap {lw} luxels wide")
    sheet.save(out_path)
    print(f"wrote {out_path} ({sheet.width}x{sheet.height}), "
          f"unlit left / lit right in each pair")


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    main(args[0] if args else "lightmap_preview.png",
         next((int(a.split("=")[1]) for a in sys.argv if a.startswith("--faces=")), 12),
         next((int(a.split("=")[1]) for a in sys.argv if a.startswith("--scale=")), 2))
