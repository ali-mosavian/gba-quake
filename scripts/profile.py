#!/usr/bin/env python3
"""Read the BspProfile counters out of a running ROM via mGBA's GDB stub.

Launches the local SDL mGBA with -g, lets the ROM run for a settling period so
the camera-leaf caches are warm, halts, and reads the profile struct straight
out of EWRAM. Prints one line per counter plus the derived frame rate.

Usage: scripts/profile.py <rom-name> [seconds] [--keys=A,B,...]
"""
import re
import socket
import struct
import subprocess
import sys
import time
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
MGBA = ROOT / "work/mgba-0.10.5/build-sdl/sdl/mgba"
PORT = 2345

FIELDS = [
    ("clear", "I"), ("pvs_rebuild", "I"), ("face_cull", "I"),
    ("transform", "I"), ("projection", "I"), ("clipping", "I"),
    ("line_draw", "I"), ("expand", "I"), ("total", "I"),
    ("candidate_faces", "H"), ("accepted_faces", "H"), ("unique_edges", "H"),
    ("unique_vertices", "H"), ("drawn_edges", "H"), ("camera_leaf", "H"),
    ("trivial_accepted", "H"), ("trivial_rejected", "H"),
    ("near_clipped", "H"), ("screen_clipped", "H"), ("degenerate_faces", "H"),
    ("drawn_faces", "I"), ("drawn_rows", "I"), ("drawn_spans", "I"),
    ("pixel_iterations", "I"), ("texel_samples", "I"),
    ("spans_clear", "I"), ("spans_hidden", "I"), ("spans_mixed", "I"),
    ("rom_fallbacks", "I"), ("cache_bytes", "I"), ("near_clipped_faces", "I"),
    ("player_x", "i"), ("player_y", "i"), ("player_z", "i"),
    ("player_velocity_z", "i"), ("player_on_ground", "I"), ("player_substeps", "I"),
    ("player_contents", "i"), ("player_solid_frames", "I"), ("steps_climbed", "I"),
]
LAYOUT = "<" + "".join(code for _, code in FIELDS[:19]) + "2x" + "".join(code for _, code in FIELDS[19:])


def symbol_address(elf_map, name):
    pattern = re.compile(r"^\s+(0x[0-9a-f]+)\s+" + re.escape(name) + r"\s*$")
    for line in pathlib.Path(elf_map).read_text().splitlines():
        match = pattern.match(line)
        if match:
            return int(match.group(1), 16)
    raise SystemExit(f"symbol {name} not found in {elf_map}")


def checksum(payload):
    return sum(payload.encode()) & 0xff


def send(sock, payload):
    sock.sendall(f"${payload}#{checksum(payload):02x}".encode())


def recv_packet(sock, timeout=5.0):
    sock.settimeout(timeout)
    data = b""
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            raise SystemExit("gdb stub closed the connection")
        data += chunk
        if b"#" in data and len(data.split(b"#")[-1]) >= 2:
            break
    body = data.split(b"$")[-1].split(b"#")[0]
    sock.sendall(b"+")
    return body.decode(errors="replace")


def read_memory(sock, address, length):
    """Read GBA memory in chunks the stub is happy with."""
    data = bytearray()
    while length:
        step = min(length, 512)
        send(sock, f"m{address + len(data):x},{step:x}")
        reply = recv_packet(sock)
        if reply.startswith("E") or len(reply) < step * 2:
            raise SystemExit(f"memory read failed at {address:x}: {reply[:32]!r}")
        data += bytes.fromhex(reply[:step * 2])
        length -= step
    return bytes(data)


def write_png(path, width, height, rgb_rows):
    """Minimal PNG writer so screenshots need no image library."""
    import zlib
    raw = b"".join(b"\x00" + bytes(row) for row in rgb_rows)
    def chunk(tag, payload):
        body = tag + payload
        return (struct.pack(">I", len(payload)) + body +
                struct.pack(">I", zlib.crc32(body) & 0xffffffff))
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    pathlib.Path(path).write_bytes(
        b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
        chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def capture_screen(sock, path):
    """Grab the visible Mode 4 page and its palette, and save it as a PNG."""
    dispcnt = struct.unpack("<H", read_memory(sock, 0x04000000, 2))[0]
    page = 0x0600a000 if dispcnt & (1 << 4) else 0x06000000
    palette_raw = read_memory(sock, 0x05000000, 512)
    palette = []
    for index in range(256):
        value = struct.unpack_from("<H", palette_raw, index * 2)[0]
        red, green, blue = value & 31, (value >> 5) & 31, (value >> 10) & 31
        palette.append((red << 3 | red >> 2, green << 3 | green >> 2,
                        blue << 3 | blue >> 2))
    pixels = read_memory(sock, page, 240 * 160)
    rows = []
    for y in range(160):
        row = bytearray()
        for x in range(240):
            row += bytes(palette[pixels[y * 240 + x]])
        rows.append(row)
    write_png(path, 240, 160, rows)
    return path


def main():
    rom = sys.argv[1] if len(sys.argv) > 1 else "bsp_textured"
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 4.0
    shots = [arg.split("=", 1)[1] for arg in sys.argv[1:] if arg.startswith("--shot=")]
    address = symbol_address(ROOT / f"build/{rom}.elf.map", "bsp_profile")

    emulator = subprocess.Popen(
        [str(MGBA), "-g", str(ROOT / f"build/{rom}.gba")],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        sock = None
        for _ in range(60):
            try:
                sock = socket.create_connection(("127.0.0.1", PORT), timeout=1.0)
                break
            except OSError:
                time.sleep(0.25)
        if sock is None:
            raise SystemExit("could not reach the mGBA gdb stub on port 2345")

        send(sock, "qSupported")
        recv_packet(sock)
        send(sock, "c")                       # run freely while the ROM settles
        time.sleep(seconds)
        sock.sendall(b"\x03")                 # interrupt
        recv_packet(sock)

        size = struct.calcsize(LAYOUT)
        send(sock, f"m{address:x},{size:x}")
        reply = recv_packet(sock)
        if reply.startswith("E") or len(reply) < size * 2:
            raise SystemExit(f"memory read failed: {reply!r}")
        values = struct.unpack(LAYOUT, bytes.fromhex(reply[:size * 2]))
        for shot in shots:
            capture_screen(sock, shot)
            print(f"  screenshot -> {shot}")
        try:
            send(sock, "k")          # the stub closes on kill; that is expected
        except OSError:
            pass
    finally:
        emulator.terminate()
        try:
            emulator.wait(timeout=5)
        except subprocess.TimeoutExpired:
            emulator.kill()

    named = dict(zip((name for name, _ in FIELDS), values))
    total = named["total"] or 1
    print(f"{rom}: {total:,} cycles/frame   {16780000/total:.2f} FPS")
    print("  stage cycles")
    for name, _ in FIELDS[:9]:
        if name == "total":
            continue
        print(f"    {name:<16}{named[name]:>10,}  {100.0*named[name]/total:5.1f}%")
    print("  counts")
    for name, _ in FIELDS[9:]:
        print(f"    {name:<16}{named[name]:>10,}")


if __name__ == "__main__":
    main()
