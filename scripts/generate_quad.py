#!/usr/bin/env python3
"""Generate synchronized exact and affine-span rotating textured quad frames."""
import math
import pathlib
import sys

SCREEN_WIDTH = 240
SCREEN_HEIGHT = 160
FOCAL_LENGTH = 128.0
FRAME_COUNT = 24
PIXEL_CENTER = 0.5
MAX_SPAN_PIXELS = 32


def rotation_matrix(angle):
    ay, ax = angle, 0.42 + math.sin(angle) * 0.32
    cy, sy, cx, sx = math.cos(ay), math.sin(ay), math.cos(ax), math.sin(ax)
    return ((cy, sy * sx, sy * cx), (0.0, cx, -sx), (-sy, cy * sx, cy * cx))


def inverse_rotate(matrix, vector):
    """Multiply a vector by the transpose of an orthonormal matrix."""
    return tuple(
        sum(matrix[row][column] * vector[row] for row in range(3))
        for column in range(3)
    )


def sample_quad(matrix, screen_x, screen_y, clamp_to_quad=False):
    """Return continuous texture coordinates where a camera ray hits the quad."""
    ray_origin = inverse_rotate(matrix, (0.0, 0.0, -4.0))
    camera_ray = (
        (screen_x - SCREEN_WIDTH / 2) / FOCAL_LENGTH,
        (screen_y - SCREEN_HEIGHT / 2) / FOCAL_LENGTH,
        1.0,
    )
    ray_direction = inverse_rotate(matrix, camera_ray)
    if abs(ray_direction[2]) < 1e-8:
        return None
    distance = -ray_origin[2] / ray_direction[2]
    if distance <= 0:
        return None
    quad_x = ray_origin[0] + distance * ray_direction[0]
    quad_y = ray_origin[1] + distance * ray_direction[1]
    if not clamp_to_quad and (abs(quad_x) > 1.0 or abs(quad_y) > 1.0):
        return None
    quad_x = max(-1.0, min(1.0, quad_x))
    quad_y = max(-1.0, min(1.0, quad_y))
    texture_u = 2.0 + (quad_x + 1.0) * 29.5
    texture_v = 2.0 + (1.0 - quad_y) * 29.5
    return texture_u, texture_v


def texel_round(value, gradient):
    """Hecker's direction-stable nearest-texel rule."""
    return (math.floor(value + 0.5) if gradient >= 0.0
            else math.ceil(value - 0.5))


def build_scanline(matrix, screen_y):
    # Pixel-center coverage is the sub-pixel rule: an edge crossing between
    # integer pixels changes coverage only when it crosses the next center.
    inside = [
        sample_quad(matrix, x + PIXEL_CENTER, screen_y + PIXEL_CENTER) is not None
        for x in range(SCREEN_WIDTH)
    ]
    if not any(inside):
        return 0, []
    left = inside.index(True)
    right = SCREEN_WIDTH - inside[::-1].index(True)
    width = right - left
    piece_count = max(1, (width + MAX_SPAN_PIXELS - 1) // MAX_SPAN_PIXELS)
    commands = []
    for piece in range(piece_count):
        start_x = left + (width * piece) // piece_count
        end_x = left + (width * (piece + 1)) // piece_count
        start_uv = sample_quad(
            matrix, start_x + PIXEL_CENTER, screen_y + PIXEL_CENTER, True
        )
        end_uv = sample_quad(
            matrix, end_x - 1 + PIXEL_CENTER, screen_y + PIXEL_CENTER, True
        )
        pixel_intervals = max(1, end_x - start_x - 1)
        du = (end_uv[0] - start_uv[0]) / pixel_intervals
        dv = (end_uv[1] - start_uv[1]) / pixel_intervals
        pa, pc = round(du * 256), round(dv * 256)
        # BG2X/Y map to screen pixel index zero. Anchor the quantized line so
        # pixel `a` lands on the first covered pixel center exactly.
        # The PPU floors 24.8 when selecting a texel. At an exact half-texel
        # tie, use 128 for a positive gradient and 127 for a negative one.
        # This is floor(C+1/2) versus ceil(C-1/2) expressed with one loop.
        reference_x = round(start_uv[0] * 256 - pa * start_x)
        reference_y = round(start_uv[1] * 256 - pc * start_x)
        reference_x += 128 if pa >= 0 else 127
        reference_y += 128 if pc >= 0 else 127
        commands.append((start_x * 4, pa, pc, 0, reference_x, reference_y))
    return (left << 8) | right, commands


def emit_affine(path):
    out = ["/* Generated affine quad commands. */", "#ifndef QUAD_AFFINE_FRAMES_H",
           "#define QUAD_AFFINE_FRAMES_H", f"enum {{ QUAD_FRAME_COUNT = {FRAME_COUNT} }};",
           "static const QuadFrame quad_frames[QUAD_FRAME_COUNT] = {"]
    for frame in range(FRAME_COUNT):
        matrix = rotation_matrix(2 * math.pi * frame / FRAME_COUNT)
        out.append("  { .scanlines = {")
        for screen_y in range(SCREEN_HEIGHT):
            winh, cmds = build_scanline(matrix, screen_y)
            padded = cmds + [(0, 0, 0, 0, 0, 0)] * (12 - len(cmds))
            cs = ",".join("{%d,%d,%d,%d,%d,%d}" % c for c in padded)
            out.append(f"    {{0x{winh:04x},{len(cmds)},{{{cs}}}}},")
        out.append("  }},")
    out += ["};", "#endif"]
    pathlib.Path(path).write_text("\n".join(out) + "\n")


def emit_reference(path):
    out = ["/* Generated exact Mode 4 quad frames. */", "#ifndef QUAD_REFERENCE_FRAMES_H",
           "#define QUAD_REFERENCE_FRAMES_H", f"enum {{ QUAD_REFERENCE_FRAME_COUNT = {FRAME_COUNT} }};",
           "static const uint16_t quad_reference_frames[QUAD_REFERENCE_FRAME_COUNT][19200] = {"]
    for frame in range(FRAME_COUNT):
        matrix, words = rotation_matrix(2 * math.pi * frame / FRAME_COUNT), []
        for y in range(SCREEN_HEIGHT):
            for x in range(0, SCREEN_WIDTH, 2):
                pair = []
                for xx in (x, x + 1):
                    h = sample_quad(matrix, xx + PIXEL_CENTER, y + PIXEL_CENTER)
                    hn = sample_quad(matrix, xx + 1 + PIXEL_CENTER, y + PIXEL_CENTER, True)
                    if h is None:
                        pair.append(0)
                    else:
                        tu = texel_round(h[0], hn[0] - h[0])
                        tv = texel_round(h[1], hn[1] - h[1])
                        pair.append(1 + ((((tu >> 2) ^ (tv >> 2)) & 1)))
                words.append(pair[0] | (pair[1] << 8))
        out.append("  {")
        for i in range(0, len(words), 24): out.append("    " + ",".join(map(str, words[i:i+24])) + ",")
        out.append("  },")
    out += ["};", "#endif"]
    pathlib.Path(path).write_text("\n".join(out) + "\n")


if __name__ == "__main__":
    (emit_affine if sys.argv[1] == "affine" else emit_reference)(sys.argv[2])
