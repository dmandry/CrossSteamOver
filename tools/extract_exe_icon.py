#!/usr/bin/env python3
"""Write the largest icon embedded in a Windows exe as a PNG or ICO.

stdout is unused. The image path is the second argument. Exit status is 0 when
an icon was written, 1 when the exe has none this script can read.
"""
import struct
import sys
import zlib
from pathlib import Path


def _sections(data: bytes, pe: int):
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt = struct.unpack_from("<H", data, pe + 20)[0]
    off = pe + 24 + opt
    secs = []
    for i in range(nsec):
        o = off + i * 40
        vsz, va, rsz, raw = struct.unpack_from("<IIII", data, o + 8)
        secs.append((va, raw, max(vsz, rsz)))
    return secs


def _off(secs, rva: int) -> int:
    for va, raw, size in secs:
        if va <= rva < va + size:
            return raw + (rva - va)
    raise ValueError(f"rva {rva:#x} is not in a section")


def _dir(data, secs, rva: int):
    o = _off(secs, rva)
    named, ids = struct.unpack_from("<HH", data, o + 12)
    entries = []
    p = o + 16
    for _ in range(named + ids):
        name, child = struct.unpack_from("<II", data, p)
        entries.append((name, child))
        p += 8
    return entries


def _leaf(data, secs, rva: int):
    o = _off(secs, rva)
    data_rva, size = struct.unpack_from("<II", data, o)
    start = _off(secs, data_rva)
    return data[start:start + size]


def _icons(data: bytes):
    if data[:2] != b"MZ":
        return []
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        return []
    magic = struct.unpack_from("<H", data, pe + 24)[0]
    if magic != 0x20B:
        return []
    res_rva, res_size = struct.unpack_from("<II", data, pe + 24 + 112 + 16)
    if not res_rva or not res_size:
        return []
    secs = _sections(data, pe)
    images = []
    for type_id, type_child in _dir(data, secs, res_rva):
        if type_id != 3 or type_child & 0x80000000 == 0:
            continue
        for _name, name_child in _dir(data, secs, res_rva + (type_child & 0x7FFFFFFF)):
            if name_child & 0x80000000 == 0:
                blob = _leaf(data, secs, res_rva + name_child)
            else:
                # One more level: language.
                langs = _dir(data, secs, res_rva + (name_child & 0x7FFFFFFF))
                if not langs:
                    continue
                lang_child = langs[0][1]
                if lang_child & 0x80000000:
                    continue
                blob = _leaf(data, secs, res_rva + lang_child)
            if len(blob) >= 16:
                images.append(blob)
    images.sort(key=len, reverse=True)
    return images


def _png_from_dib(blob: bytes):
    if len(blob) < 40:
        return None
    header = struct.unpack_from("<IiiHHIIiiII", blob, 0)
    size, width, height, _planes, bitcount, compression = header[:6]
    if size < 40 or width <= 0 or width > 1024 or bitcount != 32 or compression not in (0, 3):
        return None
    real_h = height // 2 if height > width else height
    if real_h <= 0 or real_h > 1024:
        return None
    pixels = blob[size:size + width * real_h * 4]
    if len(pixels) < width * real_h * 4:
        return None
    # DIB rows are bottom-up BGRA. PNG wants top-down RGBA.
    raw = bytearray()
    stride = width * 4
    for y in range(real_h - 1, -1, -1):
        raw.append(0)
        row = pixels[y * stride:(y + 1) * stride]
        for x in range(0, stride, 4):
            b, g, r, a = row[x:x + 4]
            raw.extend((r, g, b, a))

    def chunk(tag: bytes, payload: bytes) -> bytes:
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", width, real_h, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")


def main() -> int:
    exe, dest = Path(sys.argv[1]), Path(sys.argv[2])
    images = _icons(exe.read_bytes())
    for blob in images:
        if blob.startswith(b"\x89PNG"):
            dest.write_bytes(blob)
            print(dest)
            return 0
        png = _png_from_dib(blob)
        if png:
            dest.write_bytes(png)
            print(dest)
            return 0
    return 1


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: extract_exe_icon.py Game.exe icon.png")
    raise SystemExit(main())
