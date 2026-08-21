#!/usr/bin/env python3
"""Freeze one frame of the renderer as a PPU display list.

The beam-raced architecture replaces the CPU's pixel loop with the PPU: every
visible span becomes one affine-background command -- a (PA, PC, X, Y) walk
through a texture atlas -- issued at the span's start column by the
cycle-exact feeder that the quad experiment proved stable on hardware. This
script builds everything that ROM needs, offline, for the spawn pose:

  1. An exact visibility solution at native 240x160: every face rasterised
     against a float z-buffer, each pixel owning a face and a texture
     coordinate. No PVS, no clipping, no span pipeline -- brute force is fine
     offline and cannot disagree with itself about coverage.
  2. Pixel runs -> spans -> commands. A span on a WALL is cut by the same
     depth-drift rule the software renderer uses, because u along a scanline
     is perspective there; a span on a FLOOR is one command however long it
     is, because at fixed screen y a horizontal plane has constant depth and
     its texture walk is exactly affine.
  3. A texture atlas in the affine BG's map space. Tiling cannot be expressed
     by the affine walk, so each texture is replicated in a block and every
     command is re-anchored into the central period; spans whose walk still
     leaves the block are split. Affine map entries are bytes: 256 distinct
     tiles is a hard ceiling.

Unlit by design -- the PPU samples raw texels; lighting is the architecture's
known open problem and this artifact is meant to price everything else.
"""
import math, pathlib, re, sys
import numpy as np

HEADER = pathlib.Path("src/generated/bsp_wireframe_map.h")
# Logical resolution, matching the software renderer; the PPU doubles it.
# Feeding at native 240 was measured infeasible: 18.9 commands per line with a
# median span of 9 pixels, against a ~36-cycle (9-pixel) minimum spacing for
# an EWRAM-fed writer. At 120 logical each pixel is 8 beam cycles and odd
# lines repeat for free (PB = PD = 0 holds the internal reference), so the
# same spans get four times the timing headroom.
WIDTH, HEIGHT = 120, 80
FOCAL = 56.0
EYE_HEIGHT = 22.0
NEAR = 8.0
MAX_COMMANDS = 24                 # per line; the shortest spans merge away
# The feeder's fall-through issue path measures ~35 cycles, so back-to-back
# commands need five logical pixels (40 cycles) between starts.
MIN_SPAN = 5
DRIFT_SPLIT = 32                  # pixels between corrections on walls


def load(text, name):
    match = re.search(r"\b%s\[[^\]]*\][^=]*= \{(.*?)\n\};" % name, text, re.S)
    body = match.group(1)
    if "{" not in body:
        return [int(v) for v in body.replace("\n", " ").split(",") if v.strip()]
    records, depth, start = [], 0, None
    for i, ch in enumerate(body):
        if ch == "{":
            if depth == 0:
                start = i
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                records.append([int(v) for v in re.findall(r"-?\d+", body[start:i])])
    return records


def main():
    text = HEADER.read_text()
    enum = dict(re.findall(r"BSP_(\w+) = (-?\d+)", text))
    faces = load(text, "bsp_faces")
    planes = load(text, "bsp_planes")
    texinfo = load(text, "bsp_texinfo")
    textures = load(text, "bsp_textures")
    pixels = load(text, "bsp_texture_pixels")
    palette = load(text, "bsp_palette")
    vertices = np.array(load(text, "bsp_vertices"), dtype=np.float64)
    ring = load(text, "bsp_face_vertices")

    spawn = np.array([float(enum["SPAWN_X"]), float(enum["SPAWN_Y"]),
                      float(enum["SPAWN_Z"]) + EYE_HEIGHT])
    yaw = int(enum["SPAWN_YAW"]) * 2.0 * math.pi / 256.0
    sin, cos = math.sin(yaw), math.cos(yaw)

    # Camera basis, exactly the renderer's: depth = cos*dx + sin*dy,
    # horizontal = cos*dy - sin*dx, vertical = dz.
    forward = np.array([cos, sin, 0.0])
    right = np.array([-sin, cos, 0.0])
    up = np.array([0.0, 0.0, 1.0])

    # Ray direction per pixel, in world space, unnormalised (the plane
    # intersection divides it out).
    xs = (np.arange(WIDTH) + 0.5 - WIDTH / 2) / FOCAL
    ys = (HEIGHT / 2 - (np.arange(HEIGHT) + 0.5)) / FOCAL
    ray = (forward[None, None, :]
           + xs[None, :, None] * right[None, None, :]
           + ys[:, None, None] * up[None, None, :])

    depth_buffer = np.full((HEIGHT, WIDTH), np.inf)
    face_buffer = np.full((HEIGHT, WIDTH), -1, dtype=np.int32)
    u_buffer = np.zeros((HEIGHT, WIDTH))
    v_buffer = np.zeros((HEIGHT, WIDTH))

    def project(points):
        rel = points - spawn
        depth = rel @ forward
        horizontal = rel @ right
        vertical = rel @ up
        with np.errstate(divide="ignore", invalid="ignore"):
            sx = WIDTH / 2 + horizontal * FOCAL / depth
            sy = HEIGHT / 2 - vertical * FOCAL / depth
        return sx, sy, depth

    drawn = 0
    for index, face in enumerate(faces):
        plane = planes[face[0]]
        normal = np.array(plane[:3], dtype=np.float64) / 16384.0
        distance = float(plane[3])
        if face[1]:
            normal, distance = -normal, -distance
        if normal @ spawn - distance <= 0:
            continue                      # back-facing
        count, first = face[3], face[11]
        poly = vertices[[ring[first + i] for i in range(count)]]
        # Near-clip the ring in camera space before projecting; a vertex
        # behind the near plane has a meaningless projection. The texture
        # coordinates come from ray-plane intersection per pixel, so clipping
        # only has to fix the silhouette.
        rel = poly - spawn
        camera = np.stack([rel @ forward, rel @ right, rel @ up], axis=1)
        clipped = []
        for i in range(len(camera)):
            a, b = camera[i], camera[(i + 1) % len(camera)]
            if a[0] >= NEAR:
                clipped.append(a)
            if (a[0] >= NEAR) != (b[0] >= NEAR):
                t = (NEAR - a[0]) / (b[0] - a[0])
                clipped.append(a + t * (b - a))
        if len(clipped) < 3:
            continue
        camera = np.array(clipped)
        sx = WIDTH / 2 + camera[:, 1] * FOCAL / camera[:, 0]
        sy = HEIGHT / 2 - camera[:, 2] * FOCAL / camera[:, 0]
        count = len(camera)
        x0 = max(0, int(np.floor(sx.min())))
        x1 = min(WIDTH - 1, int(np.ceil(sx.max())))
        y0 = max(0, int(np.floor(sy.min())))
        y1 = min(HEIGHT - 1, int(np.ceil(sy.max())))
        if x0 > x1 or y0 > y1:
            continue
        block = ray[y0:y1 + 1, x0:x1 + 1]
        denominator = block @ normal
        with np.errstate(divide="ignore", invalid="ignore"):
            t = (distance - spawn @ normal) / denominator
        world = spawn[None, None, :] + t[:, :, None] * block
        wx, wy = world[:, :, 0], world[:, :, 1]
        # 2D point-in-polygon on the projected ring, even-odd, at pixel
        # centres -- the same convention as the renderer's crossings fill.
        px = np.arange(x0, x1 + 1) + 0.5
        py = np.arange(y0, y1 + 1) + 0.5
        inside = np.zeros((y1 - y0 + 1, x1 - x0 + 1), dtype=bool)
        for i in range(count):
            ax, ay = sx[i], sy[i]
            bx, by = sx[(i + 1) % count], sy[(i + 1) % count]
            if ay == by:
                continue
            crosses = ((ay > py[:, None]) != (by > py[:, None]))
            with np.errstate(divide="ignore", invalid="ignore"):
                cx = ax + (py[:, None] - ay) * (bx - ax) / (by - ay)
            inside ^= crosses & (px[None, :] < cx)
        visible = inside & (t > 0) & (t * (block @ forward) <
                                      depth_buffer[y0:y1 + 1, x0:x1 + 1]) \
                         & (t * (block @ forward) > NEAR)
        if not visible.any():
            continue
        axes = np.array(texinfo[face[4]][:8], dtype=np.float64)
        u = (axes[0] * wx + axes[1] * wy + axes[2] * world[:, :, 2] + axes[3]) \
            / 4096.0 - face[9] / 256.0
        v = (axes[4] * wx + axes[5] * wy + axes[6] * world[:, :, 2] + axes[7]) \
            / 4096.0 - face[10] / 256.0
        region = (slice(y0, y1 + 1), slice(x0, x1 + 1))
        depth_here = t * (block @ forward)
        depth_buffer[region] = np.where(visible, depth_here, depth_buffer[region])
        face_buffer[region] = np.where(visible, index, face_buffer[region])
        u_buffer[region] = np.where(visible, u, u_buffer[region])
        v_buffer[region] = np.where(visible, v, v_buffer[region])
        drawn += 1

    covered = (face_buffer >= 0).sum()
    print(f"{drawn} faces rasterised, {covered}/{WIDTH*HEIGHT} pixels covered "
          f"({100.0*covered/(WIDTH*HEIGHT):.1f}%)")

    # ---- pixel runs -> spans ----------------------------------------------
    spans_per_line = []
    all_spans = []          # (y, x0, x1, face)
    for y in range(HEIGHT):
        row = face_buffer[y]
        spans = []
        x = 0
        while x < WIDTH:
            face = row[x]
            end = x
            while end + 1 < WIDTH and row[end + 1] == face:
                end += 1
            if face >= 0:
                spans.append((x, end, int(face)))
            x = end + 1
        # Walls get drift-split like the renderer's segments; floors do not
        # need it (constant depth along the row), and the split would multiply
        # their command count for nothing.
        split = []
        for x0, x1, face in spans:
            nz = abs(planes[faces[face][0]][2])
            horizontal = nz == 16384
            if horizontal or x1 - x0 + 1 <= DRIFT_SPLIT:
                split.append((x0, x1, face))
            else:
                x = x0
                while x <= x1:
                    end = min(x1, x + DRIFT_SPLIT - 1)
                    split.append((x, end, face))
                    x = end + 1
        spans_per_line.append(split)
        for s in split:
            all_spans.append((y, *s))

    counts = np.array([len(s) for s in spans_per_line])
    lengths = np.array([x1 - x0 + 1 for line in spans_per_line for x0, x1, _ in line])
    tiny = int((lengths < MIN_SPAN).sum())
    print(f"spans: total {counts.sum()}, per line mean {counts.mean():.1f} "
          f"median {int(np.median(counts))} max {counts.max()}, "
          f"length median {int(np.median(lengths))}, "
          f"under {MIN_SPAN}px: {tiny} ({100.0*tiny/len(lengths):.1f}%)")
    over = int((counts > MAX_COMMANDS).sum())
    print(f"lines over {MAX_COMMANDS} commands: {over}")

    build_output(face_buffer, u_buffer, v_buffer, depth_buffer, faces, planes,
                 texinfo, textures, pixels, palette,
                 sys.argv[1] if len(sys.argv) > 1
                 else "src/generated/beam_frame.h")



def build_output(face_buffer, u_buffer, v_buffer, depth_buffer, faces, planes,
                 texinfo, textures, pixels, palette, out_header):
    """Spans -> commands -> atlas -> header. Returns preview arrays."""
    HORIZONTAL = 16384

    # ---- spans with drift-aware splitting and spacing merges --------------
    lines = []
    for y in range(HEIGHT):
        row = face_buffer[y]
        raw = []
        x = 0
        while x < WIDTH:
            face = row[x]
            end = x
            while end + 1 < WIDTH and row[end + 1] == face:
                end += 1
            raw.append((x, end, int(face)))
            x = end + 1
        spans = []
        for x0, x1, face in raw:
            if face < 0:
                # Sub-pixel seams between adjacent faces where the float
                # rasteriser left nobody owning the pixel. Extending the
                # previous span across them costs a texel or two and removes
                # the black speckle; only wide gaps stay backdrop.
                if spans and x1 - x0 + 1 <= 8:
                    spans[-1] = (spans[-1][0], x1, spans[-1][2])
                else:
                    spans.append((x0, x1, -1))
                continue
            nz = abs(planes[faces[face][0]][2])
            if nz == HORIZONTAL:
                spans.append((x0, x1, face))     # affine along the row
                continue
            # Split walls where the affine error would show: the renderer's
            # own rule, depth ratio per 32 pixels.
            x = x0
            while x <= x1:
                end = x1
                d0 = depth_buffer[y, x]
                while end > x:
                    if depth_buffer[y, end] / d0 < 1.25 and \
                       d0 / depth_buffer[y, end] < 1.25 and end - x < 64:
                        break
                    end = x + max(1, (end - x) // 2)
                spans.append((x, end, face))
                x = end + 1
        # Enforce the feeder's minimum ISSUE spacing, which binds command
        # starts, not span lengths: a command that would start too soon after
        # its predecessor is delayed to the minimum, so only the pixels in
        # the overlap take the previous walk. A span the delay swallows
        # entirely is absorbed. These are the architecture's texture-error
        # pixels, and they are counted.
        merged, lost = [], 0
        previous_start = -MIN_SPAN
        for x0, x1, face in spans:
            if face < 0:
                merged.append([x0, x1, face])
                continue
            start = max(x0, previous_start + MIN_SPAN)
            if start > x1:
                lost += x1 - x0 + 1
                if merged:
                    merged[-1][1] = x1
                continue
            lost += start - x0
            if merged and start > x0:
                merged[-1][1] = start - 1
            merged.append([start, x1, face])
            previous_start = start
        spans = merged
        while len([s for s in spans if s[2] >= 0]) > MAX_COMMANDS:
            widths = [(s[1] - s[0], i) for i, s in enumerate(spans) if s[2] >= 0]
            _, i = min(widths)
            lost += spans[i][1] - spans[i][0] + 1
            neighbour = i - 1 if i else i + 1
            spans[neighbour][1] = max(spans[neighbour][1], spans[i][1])
            spans[neighbour][0] = min(spans[neighbour][0], spans[i][0])
            del spans[i]
        lines.append((spans, lost))

    total = sum(len([s for s in spans if s[2] >= 0]) for spans, _ in lines)
    lost = sum(l for _, l in lines)
    print(f"after merging: {total} commands, "
          f"{lost} pixels absorbed into a neighbour's walk "
          f"({100.0*lost/(WIDTH*HEIGHT):.2f}%)")

    # ---- texture atlas ----------------------------------------------------
    # Per texture: the largest uv extent any single span walks, which sets
    # how many periods the block must replicate.
    used = {}
    for y, (spans, _) in enumerate(lines):
        for x0, x1, face in spans:
            if face < 0:
                continue
            t = texinfo[faces[face][4]][8]
            du = abs(u_buffer[y, x1] - u_buffer[y, x0])
            dv = abs(v_buffer[y, x1] - v_buffer[y, x0])
            best = used.get(t, (0.0, 0.0, 0))
            used[t] = (max(best[0], du), max(best[1], dv), best[2] + x1 - x0 + 1)

    ATLAS = 1024
    atlas_map = np.zeros((ATLAS // 8, ATLAS // 8), dtype=np.uint8)
    tiles = [bytes(64)]                       # tile 0 stays transparent
    tile_index = {bytes(64): 0}
    placement = {}
    cursor_x, cursor_y, row_height = 0, 0, 0
    dropped_textures = []
    for t, (eu, ev, coverage) in sorted(used.items(), key=lambda kv: -kv[1][2]):
        base, w, h = textures[t]
        # One extra period per axis beyond the measured walk extent: a span
        # issued late keeps its predecessor's walk for a few pixels, and that
        # walk must still land on texture, not on transparent cells past the
        # block. Replication costs map entries only, never tiles.
        rep_u = min(10, int(eu // w) + 3)
        rep_v = min(10, int(ev // h) + 3)
        bw, bh = w * rep_u, h * rep_v
        if cursor_x + bw > ATLAS:
            cursor_x, cursor_y = 0, cursor_y + row_height
            row_height = 0
        if cursor_y + bh > ATLAS:
            dropped_textures.append(t)
            continue
        # Tiles for one period, deduplicated across the whole atlas.
        period = np.frombuffer(bytes(pixels[base:base + w * h]),
                               dtype=np.uint8).reshape(h, w)
        indices = np.zeros((h // 8, w // 8), dtype=np.uint8)
        overflow = False
        for ty in range(h // 8):
            for tx in range(w // 8):
                tile = period[ty*8:ty*8+8, tx*8:tx*8+8].tobytes()
                if tile not in tile_index:
                    if len(tiles) >= 256:
                        overflow = True
                        break
                    tile_index[tile] = len(tiles)
                    tiles.append(tile)
                indices[ty, tx] = tile_index[tile]
            if overflow:
                break
        if overflow:
            dropped_textures.append(t)
            continue
        for ry in range(rep_v):
            for rx in range(rep_u):
                oy = (cursor_y + ry * h) // 8
                ox = (cursor_x + rx * w) // 8
                atlas_map[oy:oy + h // 8, ox:ox + w // 8] = indices
        placement[t] = (cursor_x, cursor_y, w, h, rep_u, rep_v)
        cursor_x += bw
        row_height = max(row_height, bh)
    print(f"atlas: {len(tiles)} tiles, {len(placement)} textures placed, "
          f"{len(dropped_textures)} dropped "
          f"({sum(used[t][2] for t in dropped_textures)} pixels)")

    # ---- commands ---------------------------------------------------------
    # Fixed-size line records for the feeder: u16 count, then commands of
    # 16 bytes {u16 issue cycle, i16 PA, i16 PC, u16 pad, i32 X, i32 Y}.
    HDRAW_CYCLES_PER_PIXEL = 8       # one logical pixel is two beam pixels
    records = []
    command_count = 0
    for y, (spans, _) in enumerate(lines):
        commands = []
        for x0, x1, face in spans:
            if face < 0 or faces[face][4] is None:
                continue
            t = texinfo[faces[face][4]][8]
            if t not in placement:
                continue
            ax, ay, w, h, rep_u, rep_v = placement[t]
            length = x1 - x0
            u0, v0 = u_buffer[y, x0], v_buffer[y, x0]
            if length:
                du = (u_buffer[y, x1] - u0) / length
                dv = (v_buffer[y, x1] - v0) / length
            else:
                du = dv = 0.0
            # Re-anchor into the block's first period; the replication
            # absorbs the walk. A walk that still leaves the block clamps
            # its step -- counted upstream by the extent measurement.
            su = ax + (u0 % w) + (w if du < 0 and rep_u > 1 else 0)
            sv = ay + (v0 % h) + (h if dv < 0 and rep_v > 1 else 0)
            commands.append((x0, du, dv, su, sv))
            command_count += 1
        record = bytearray()
        record += int.to_bytes(len(commands), 2, "little")
        record += b"\x00\x00"
        for x0, du, dv, su, sv in commands[:MAX_COMMANDS]:
            record += int.to_bytes(x0 * HDRAW_CYCLES_PER_PIXEL, 2, "little")
            record += int.to_bytes(int(round(du * 128)) & 0xffff, 2, "little")
            record += int.to_bytes(int(round(dv * 128)) & 0xffff, 2, "little")
            record += b"\x00\x00"
            record += int.to_bytes(int(round(su * 256)) & 0xffffffff, 4, "little")
            record += int.to_bytes(int(round(sv * 256)) & 0xffffffff, 4, "little")
        record += bytes(4 + MAX_COMMANDS * 16 - len(record))
        records.append(bytes(record))
    print(f"display list: {command_count} commands, "
          f"{len(records[0])} bytes per line record")

    # ---- compact stream for an IWRAM-resident list -------------------------
    # The whole list fits IWRAM in a standalone ROM, so the feeder reads
    # every command at one-cycle bus speed and needs no per-line staging.
    counts = []
    stream = []
    for record in records:
        n = int.from_bytes(record[0:2], "little")
        counts.append(min(n, MAX_COMMANDS))
        for i in range(min(n, MAX_COMMANDS)):
            stream.append(record[4 + 16 * i:4 + 16 * (i + 1)])
    print(f"IWRAM stream: {len(stream)} commands, {len(stream) * 16} bytes")

    # ---- header -----------------------------------------------------------
    lines_out = ["/* Generated by generate_beam_frame.py; do not edit. */",
                 "#ifndef BEAM_FRAME_H", "#define BEAM_FRAME_H", "enum {",
                 f"    BEAM_LINE_COUNT = {len(records)},",
                 f"    BEAM_COMMAND_COUNT = {len(stream)},",
                 f"    BEAM_TILE_COUNT = {len(tiles)},",
                 "};"]
    def emit(name, values, per_line):
        lines_out.append(f"static const {name} = {{")
        for start in range(0, len(values), per_line):
            lines_out.append("    " + ", ".join(str(v) for v in
                                                values[start:start+per_line]) + ",")
        lines_out.append("};")
    flat = [b for tile in tiles for b in tile]
    emit("uint8_t beam_tiles[]", flat, 24)
    emit("uint8_t beam_map[]", list(atlas_map.flatten()), 24)
    emit("uint16_t beam_palette[256]", palette, 16)
    emit("uint8_t beam_counts[]", counts, 20)
    words = []
    for command in stream:
        for w in range(4):
            words.append(int.from_bytes(command[4*w:4*w+4], "little"))
    emit("uint32_t beam_commands[]", words, 6)
    lines_out.append("#endif")
    pathlib.Path(out_header).write_text("\n".join(lines_out) + "\n")
    print(f"wrote {out_header}")
    return tiles, atlas_map, records

if __name__ == "__main__":
    main()
