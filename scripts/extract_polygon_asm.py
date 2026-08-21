#!/usr/bin/env python3
"""Extract the verified ARM polygon routine from GCC output as a reference baseline.

The production .inc contains additional hand optimization. Generate to a separate
file and compare; do not overwrite the production routine while optimizing it.
"""
import pathlib
import re
import sys

source = pathlib.Path(sys.argv[1]).read_text().splitlines()
start = next(i for i, line in enumerate(source) if line == "draw_textured_polygon:")
end = next(i for i in range(start, len(source))
           if source[i].startswith("\t.size\tdraw_textured_polygon,"))
body = source[start:end + 1]

result = ["/* Generated from the bit-exact C reference; hand optimization baseline. */",
          ".section .iwram,\"ax\",%progbits", ".align 2", ".arm",
          ".global draw_textured_polygon_arm", ".type draw_textured_polygon_arm, %function"]
for line in body:
    if line == "draw_textured_polygon:":
        result.append("draw_textured_polygon_arm:")
        continue
    if line.startswith("\t.size\tdraw_textured_polygon,"):
        result.append(".size draw_textured_polygon_arm, .-draw_textured_polygon_arm")
        continue
    if any(token in line for token in (".loc ", ".cfi_", ".LVL", ".LFB", ".LFE",
                                        ".LBB", ".LBE", ".LBI")):
        continue
    line = re.sub(r"\.L(\d+)", r".Lpolygon_\1", line)
    result.append(line)

assembly = "\n".join(result) + "\n"
assembly = assembly.replace(
    "\tadd\tr10, r3, r2, lsl #3\n",
    "\tadd\tr10, r3, r2, lsl #3\n"
    "\tldr\tr10, [r10, #4]\n"
    "\tmov\tr9, r10, lsl #16\n\tmov\tr9, r9, lsr #16\n"
    "\tstr\tr9, [sp, #608]\n\tmov\tr10, r10, lsr #16\n"
    "\tsub\tr10, r10, #1\n",
    1)
assembly = assembly.replace(
    "\tmov\tr9, #1\n", "\tldr\tr9, [sp, #608]\n", 1)
assembly = assembly.replace(
    "\tldrh\tr2, [r10, #6]\n\tldrh\tfp, [r10, #4]\n"
    "\tsub\tr2, r2, #1\n\tand\tr2, r2, r0, asr #8\n"
    "\tmla\tr2, fp, r2, r3\n\tsub\tfp, fp, #1\n",
    "\tand\tr2, r10, r0, asr #8\n\tmla\tr2, r9, r2, r3\n"
    "\tsub\tfp, r9, #1\n",
    1)
assembly = assembly.replace(
    "\tstrb\tr9, [r1]\n\tstrb\tr2, [lr]\n",
    "\tstrb\tr2, [lr]\n\tmov\tr2, #1\n\tstrb\tr2, [r1]\n", 1)
pathlib.Path(sys.argv[2]).write_text(assembly)
