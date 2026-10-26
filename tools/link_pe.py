#!/usr/bin/env python3
"""Link the CrossSteamOver stub into a CrossOver builtin steamclient64.dll.

clang -target x86_64-windows-gnu emits COFF. Host ld cannot link it, and a
normal PE at the default 0x180000000 image base collides with the game's
other DLLs (they all prefer that address, and this image has no relocations).
The base is therefore a high, otherwise unused address, relocations are
stripped, and the DOS stub carries the "Wine builtin DLL" mark so CrossOver
loads it as a builtin and the unix companion can be dlopened.
"""
import struct
import sys
from pathlib import Path

IMAGE_REL_AMD64_REL32 = 4
IMAGE_BASE = 0x1A0000000


def main() -> None:
    obj_path, out_path = Path(sys.argv[1]), Path(sys.argv[2])
    d = obj_path.read_bytes()
    _, nsects, _, symptr, nsyms, optsz, _ = struct.unpack_from("<HHIIIHH", d, 0)
    off = 20 + optsz
    sections = []
    for _i in range(nsects):
        name = d[off:off + 8].split(b"\0")[0].decode("latin1")
        vsz, _va, rsz, raw = struct.unpack_from("<IIII", d, off + 8)
        relptr, _ = struct.unpack_from("<II", d, off + 24)
        nrel, _ = struct.unpack_from("<HH", d, off + 32)
        blob = d[raw:raw + rsz] if rsz else b""
        rels = [struct.unpack_from("<IIH", d, relptr + j * 10) for j in range(nrel)]
        sections.append({"name": name, "vsz": vsz, "raw": blob, "rels": rels})
        off += 40
    strptr = symptr + nsyms * 18

    def symname(i: int) -> str:
        raw = d[symptr + i * 18:symptr + i * 18 + 8]
        if raw[:4] == b"\0\0\0\0":
            o = struct.unpack_from("<I", raw, 4)[0]
            return d[strptr + o:].split(b"\0")[0].decode("latin1")
        return raw.split(b"\0")[0].decode("latin1")

    symbols = []
    for i in range(nsyms):
        val, sec, _typ, cls, _naux = struct.unpack_from("<IHHBB", d, symptr + i * 18 + 8)
        symbols.append({"name": symname(i), "val": val, "sec": sec, "cls": cls})

    keep = [s for s in sections if s["name"] in (".text", ".rdata", ".data", ".bss")]
    rva = 0x1000
    for s in keep:
        s["rva"] = rva
        span = s["vsz"] if s["name"] == ".bss" else max(len(s["raw"]), s["vsz"])
        s["span"] = max((max(span, 1) + 0xFFF) & ~0xFFF, 0x1000)
        rva += s["span"]

    def sym_addr(sym: dict) -> int:
        return sections[sym["sec"] - 1]["rva"] + sym["val"]

    for s in keep:
        if s["name"] == ".bss":
            s["bytes"] = b""
            continue
        buf = bytearray(s["raw"])
        for rva_off, si, typ in s["rels"]:
            if typ != IMAGE_REL_AMD64_REL32:
                raise SystemExit(f"unsupported reloc type {typ} in {s['name']}")
            target = sym_addr(symbols[si])
            here = s["rva"] + rva_off + 4
            addend = struct.unpack_from("<i", buf, rva_off)[0]
            struct.pack_into("<i", buf, rva_off, (target + addend) - here)
        s["bytes"] = bytes(buf)

    exports = [
        (sym["name"], sym_addr(sym))
        for sym in symbols
        if sym["cls"] == 2 and sym["sec"] > 0 and sym["name"] != "DllMain"
    ]
    exports.sort(key=lambda item: item[0])
    dllmain = next(sym_addr(sym) for sym in symbols if sym["name"] == "DllMain")

    def align(n: int, a: int) -> int:
        return (n + a - 1) & ~(a - 1)

    exp_rva = rva
    n = len(exports)
    funcs_off, names_off, ords_off = 40, 40 + n * 4, 40 + n * 8
    blob = bytearray(ords_off + n * 2)
    name_rva = exp_rva + len(blob)
    blob += b"steamclient64.dll\0"
    name_rvas = []
    for name, _addr in exports:
        name_rvas.append(exp_rva + len(blob))
        blob += name.encode() + b"\0"
    struct.pack_into("<I", blob, 12, name_rva)
    struct.pack_into("<I", blob, 16, 1)
    struct.pack_into("<I", blob, 20, n)
    struct.pack_into("<I", blob, 24, n)
    struct.pack_into("<III", blob, 28, exp_rva + funcs_off, exp_rva + names_off, exp_rva + ords_off)
    for i, (_name, addr) in enumerate(exports):
        struct.pack_into("<I", blob, funcs_off + i * 4, addr)
        struct.pack_into("<I", blob, names_off + i * 4, name_rvas[i])
        struct.pack_into("<H", blob, ords_off + i * 2, i)
    exp_size = len(blob)
    exp_span = align(exp_size, 0x1000)

    out_sects = [s for s in keep if s["name"] != ".bss"]
    out_sects.append({"name": ".edata", "rva": exp_rva, "bytes": bytes(blob), "span": exp_span})
    bss = next(s for s in keep if s["name"] == ".bss")
    file_align = 0x200
    headers_size = align(0x80 + 24 + 0xF0 + len(out_sects) * 40, file_align)
    file_off = headers_size
    for s in out_sects:
        s["file_off"] = file_off
        s["rawsz"] = align(len(s["bytes"]), file_align)
        file_off += s["rawsz"]
    size_image = exp_rva + exp_span

    buf = bytearray(headers_size)
    buf[0:2] = b"MZ"
    struct.pack_into("<I", buf, 0x3C, 0x80)
    buf[0x40:0x51] = b"Wine builtin DLL\0"
    pe = 0x80
    buf[pe:pe + 4] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", buf, pe + 4, 0x8664, len(out_sects), 0, 0, 0, 0xF0, 0x2023)
    opt = pe + 24
    struct.pack_into("<H", buf, opt, 0x20B)
    buf[opt + 2] = 14
    struct.pack_into("<I", buf, opt + 16, dllmain)
    struct.pack_into("<Q", buf, opt + 24, IMAGE_BASE)
    struct.pack_into("<II", buf, opt + 32, 0x1000, file_align)
    struct.pack_into("<HHHH", buf, opt + 40, 6, 0, 0, 0)
    struct.pack_into("<HH", buf, opt + 48, 6, 0)
    struct.pack_into("<II", buf, opt + 56, size_image, headers_size)
    struct.pack_into("<HH", buf, opt + 68, 3, 0x100)
    for q, val in ((72, 0x100000), (80, 0x1000), (88, 0x100000), (96, 0x1000)):
        struct.pack_into("<Q", buf, opt + q, val)
    struct.pack_into("<I", buf, opt + 108, 16)
    struct.pack_into("<II", buf, opt + 112, exp_rva, exp_size)
    chars = {".text": 0x60000020, ".rdata": 0x40000040, ".data": 0xC0000040, ".edata": 0x40000040}
    sh = opt + 0xF0
    for s in out_sects:
        buf[sh:sh + 8] = s["name"].encode().ljust(8, b"\0")[:8]
        vsize = len(s["bytes"]) + (bss["vsz"] if s["name"] == ".data" else 0)
        struct.pack_into("<IIII", buf, sh + 8, vsize, s["rva"], s["rawsz"], s["file_off"])
        struct.pack_into("<I", buf, sh + 36, chars[s["name"]])
        sh += 40
    out = bytearray(buf)
    for s in out_sects:
        need = s["file_off"] + s["rawsz"]
        if len(out) < need:
            out.extend(b"\0" * (need - len(out)))
        out[s["file_off"]:s["file_off"] + len(s["bytes"])] = s["bytes"]
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(out)
    print(f"linked {out_path} ({len(out)} bytes, {n} exports, base {IMAGE_BASE:#x})")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: link_pe.py pe_bridge.o steamclient64.dll")
    main()
