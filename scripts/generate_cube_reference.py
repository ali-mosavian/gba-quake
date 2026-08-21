#!/usr/bin/env python3
"""Generate exact per-pixel Mode 4 reference frames for the rotating cube."""
import math
import pathlib
import sys

import generate_cube as cube


def pixel(r, x, y):
    h = cube.hit_for_face(r, x + 0.5, y + 0.5)
    if h is None:
        return 0
    face, u, v = h
    ox, oy = cube.ATLAS[face]
    checker = ((int(u - ox) >> 2) ^ (int(v - oy) >> 2)) & 1
    return 1 + face * 2 + checker


def emit(path):
    out = ["/* Generated exact Mode 4 reference frames; do not edit. */",
           "#ifndef CUBE_REFERENCE_FRAMES_H", "#define CUBE_REFERENCE_FRAMES_H",
           f"enum {{ CUBE_REFERENCE_FRAME_COUNT = {cube.FRAMES} }};",
           "static const uint16_t cube_reference_frames[CUBE_REFERENCE_FRAME_COUNT][19200] = {"]
    for f in range(cube.FRAMES):
        r = cube.mat(2.0 * math.pi * f / cube.FRAMES)
        words = []
        for y in range(cube.H):
            for x in range(0, cube.W, 2):
                words.append(pixel(r, x, y) | (pixel(r, x + 1, y) << 8))
        out.append("  {")
        for i in range(0, len(words), 24):
            out.append("    " + ",".join(str(v) for v in words[i:i + 24]) + ",")
        out.append("  },")
    out += ["};", "#endif"]
    pathlib.Path(path).write_text("\n".join(out) + "\n")


if __name__ == "__main__":
    emit(sys.argv[1])

