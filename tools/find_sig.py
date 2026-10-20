#!/usr/bin/env python3
"""Find byte signatures (IDA-style, '?' = wildcard byte) in a PE file.

Usage:
    python tools/find_sig.py <exe_path> [SIGNATURE ...]

With no SIGNATURE arguments, scans a built-in list of BetterRenderDragon
signatures relevant to this project and prints file offset / section / RVA.
"""

from __future__ import annotations

import struct
import sys


def parse_sig(text: str) -> list[int | None]:
    out: list[int | None] = []
    for tok in text.split():
        tok = tok.strip()
        if not tok:
            continue
        if tok == "?":
            out.append(None)
            continue
        out.append(int(tok, 16))
    return out


def read_sections(data: bytes) -> list[tuple[str, int, int, int]]:
    """Return [(name, virtual_address, virtual_size, raw_offset), ...]."""
    if data[:2] != b"MZ":
        raise ValueError("not a PE file (missing MZ)")
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_off : pe_off + 4] != b"PE\0\0":
        raise ValueError("bad PE header")
    num_sections = struct.unpack_from("<H", data, pe_off + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe_off + 20)[0]
    sec_off = pe_off + 24 + opt_size
    sections: list[tuple[str, int, int, int]] = []
    for i in range(num_sections):
        base = sec_off + i * 40
        name = data[base : base + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize = struct.unpack_from("<I", data, base + 8)[0]
        vaddr = struct.unpack_from("<I", data, base + 12)[0]
        raw_size = struct.unpack_from("<I", data, base + 16)[0]
        raw_off = struct.unpack_from("<I", data, base + 20)[0]
        sections.append((name, vaddr, vsize, raw_off))
    return sections


def image_base(data: bytes) -> int:
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    magic = struct.unpack_from("<H", data, pe_off + 24)[0]
    if magic == 0x20B:
        return struct.unpack_from("<Q", data, pe_off + 24 + 24)[0]
    return struct.unpack_from("<I", data, pe_off + 24 + 28)[0]


def rva_for(offset: int, sections: list[tuple[str, int, int, int]]) -> tuple[str, int]:
    """Map a file offset to (section_name, RVA) using section raw ranges."""
    # sections carry (name, vaddr, vsize, raw_off); raw size is recomputed from
    # the next section's raw offset so file offsets map correctly.
    raw_offs = [s[3] for s in sections] + [2**63]
    best = ("?", 0)
    for i, (name, vaddr, vsize, raw_off) in enumerate(sections):
        if raw_off <= offset < raw_offs[i + 1]:
            return (name, offset - raw_off + vaddr)
    return best


def scan(data: bytes, sig: list[int | None]) -> list[int]:
    # Find the longest run of literal (non-wildcard) bytes and use it as an
    # anchor, so we only verify a handful of candidates instead of scanning
    # every byte in pure Python.
    best_run: tuple[int, int] = (0, 0)  # (start, length)
    run_start, run_len = 0, 0
    for i, b in enumerate(sig):
        if b is not None:
            if run_len == 0:
                run_start = i
            run_len += 1
            if run_len > best_run[1]:
                best_run = (run_start, run_len)
        else:
            run_len = 0
    start, length = best_run
    if length == 0:
        raise ValueError("signature has no literal bytes")
    anchor = bytes(sig[start : start + length])  # type: ignore[arg-type]
    n = len(sig)
    hits: list[int] = []
    pos = 0
    while True:
        idx = data.find(anchor, pos)
        if idx < 0:
            break
        candidate = idx - start
        if candidate < 0:
            pos = idx + 1
            continue
        ok = True
        for j, want in enumerate(sig):
            if want is not None and data[candidate + j] != want:
                ok = False
                break
        if ok:
            hits.append(candidate)
        pos = idx + 1
    return hits


BUILTIN: list[tuple[str, str]] = [
    ("reloadMaterial 1.26.20",
     "C6 81 ? ? ? ? ? C3 CC CC CC CC CC CC CC CC 55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 0F 29 75"),
    ("reloadMaterial 1.21.130",
     "C6 81 ? ? ? ? ? C3 CC CC CC CC CC CC CC CC 48 83 EC 28 48 8B 41 ? 4C 8D 05 ? ? ? ? 48 8D 15 ? ? ? ? 48 8D 4C 24 ? 4C 8B 48 ? 49 81 C1 08 03 00 00 E8 ? ? ? ? B8 01 00 00 00 48 83 C4 28 C3"),
    ("endFrame 1.26.30",
     "55 41 57 41 56 41 55 41 54 56 57 53 B8 ? ? ? ? E8 ? ? ? ? 48 29 C4 48 8D AC 24 ? ? ? ? 44 0F 29 8D ? ? ? ? 44 0F 29 85 ? ? ? ? 0F 29 BD ? ? ? ? 0F 29 B5 ? ? ? ? 48 C7 85 ? ? ? ? ? ? ? ? 48 89 95 ? ? ? ? 48 89 8D"),
    ("readFile 1.26.30",
     "55 41 57 41 56 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 48 C7 85 ? ? ? ? ? ? ? ? 48 89 D6 41 0F 10 00"),
    ("clientInstance_Update 1.26.30",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 48 C7 85 ? ? ? ? ? ? ? ? 89 D3 48 89 CE 48 8B 01"),
]


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    exe = sys.argv[1]
    data = open(exe, "rb").read()
    sections = read_sections(data)
    tasks: list[tuple[str, str]]
    if len(sys.argv) > 2:
        tasks = [("cli", " ".join(sys.argv[2:]))]
    else:
        tasks = BUILTIN
    for name, text in tasks:
        sig = parse_sig(text)
        hits = scan(data, sig)
        if not hits:
            print(f"[MISS] {name}")
            continue
        first = hits[0]
        sec, rva = rva_for(first, sections)
        va = image_base(data) + rva
        print(f"[HIT ] {name}  file=0x{first:X}  section={sec}  rva=0x{rva:X}  va=0x{va:X}  count={len(hits)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
