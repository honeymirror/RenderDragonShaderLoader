"""Check that a parameter change only rebuilds the materials that use it.

Runs the real CompilerService (tools/compile_smoke.cpp must be built first)
against a shader pack twice: once with the given macro set, once with a single
macro changed. Prints the per-material cache-key width and how many materials
hit the cache on the second run.

Usage:
  py -3 tools/verify_incremental.py <packDirOrZip> <workDir> <vanillaDir> \
      <macro> [MACRO=VALUE ...]

Example (ExamplePack-adapted, 13 materials):
  py -3 tools/verify_incremental.py \
      data/shaderpacks/ExamplePack-adapted.zip build/incr E:/.../data/renderer/materials \
      NL_PLANTS_WAVE=0.0 NL_GAMMA=1.33

Expectation after the include trimming work: changing a wave macro
(NL_PLANTS_WAVE) rebuilds only RenderChunk and Weather, not the Actor family.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import time
import zipfile
from pathlib import Path


def material_dirs(src: Path):
    mats = src / "materials"
    if not mats.is_dir():
        mats = src
    out = []
    for entry in sorted(mats.iterdir()):
        if not entry.is_dir():
            continue
        if any(
            f.suffix in (".sc", ".glsl", ".h", ".inc", ".frag", ".vert", ".comp")
            for f in entry.rglob("*")
            if f.is_file()
        ):
            out.append(entry.name)
    return out


def changed_value(macro: str) -> str:
    """Pick a *valid* new value for the varied macro.

    The old version wrote ``NAME=__changed__``, which is not a legal shader
    literal: the rebuild path then died in shaderc with "undeclared identifier
    __changed__" and the run looked like a compile failure instead of a cache
    measurement. Nudge numeric values instead, and fall back to 1 for
    booleans / flags.
    """
    if "=" not in macro:
        return macro + "=1"
    name, _, value = macro.partition("=")
    try:
        number = float(value)
    except ValueError:
        return f"{name}=1"
    return f"{name}={number + 0.1234!r}"


def main() -> int:
    if len(sys.argv) < 6:
        print(__doc__)
        return 2
    pack = Path(sys.argv[1])
    work = Path(sys.argv[2])
    vanilla = Path(sys.argv[3])
    macros = sys.argv[4:]

    repo = Path(__file__).resolve().parent.parent
    smoke = repo / "build" / "windows" / "x64" / "release" / "compile_smoke.exe"
    if not smoke.is_file():
        print(f"missing {smoke}; run `xmake build compile_smoke` first")
        return 1

    src = work / "src"
    shutil.rmtree(work, ignore_errors=True)
    src.mkdir(parents=True, exist_ok=True)
    if pack.suffix == ".zip":
        zipfile.ZipFile(pack).extractall(src)
    else:
        shutil.copytree(pack, src, dirs_exist_ok=True)

    mats = material_dirs(src)
    vdst = src / "vanilla"
    vdst.mkdir(exist_ok=True)
    staged = 0
    for name in mats:
        source = vanilla / (name + ".material.bin")
        if source.is_file():
            shutil.copyfile(source, vdst / (name + ".material.bin"))
            staged += 1
    print(f"materials={len(mats)} vanilla staged={staged} macros={len(macros)}")

    out = work / "out" / "compiled"
    cache = work / "out" / "cache"

    def run(tag, macro_list, force_keys=False):
        if force_keys and cache.exists():
            shutil.rmtree(cache, ignore_errors=True)
        started = time.time()
        proc = subprocess.run(
            [str(smoke), str(src), str(out), str(vanilla)] + macro_list,
            capture_output=True,
            text=True,
            cwd=str(repo),
        )
        elapsed = time.time() - started
        keys = {}
        for line in proc.stdout.splitlines():
            found = re.search(r"cache key: (\S+) macros=(\d+)/(\d+)", line)
            if found:
                keys[found.group(1)] = (int(found.group(2)), int(found.group(3)))
        hits = sum(1 for line in proc.stdout.splitlines() if "cache hit" in line)
        took = [line for line in proc.stdout.splitlines() if "lazurite process took" in line]
        print(f"[{tag}] wall={elapsed:.2f}s hits={hits} {took[0] if took else ''}")
        if force_keys:
            for name in mats:
                print(f"    {name:24} key macros={keys.get(name)}")
        if proc.returncode != 0:
            print(proc.stdout[-1500:])
            print(proc.stderr[-1500:])
        return proc.returncode

    code = run("cold", macros, force_keys=True)
    if code != 0:
        return code
    first = macros[0].split("=", 1)[0]
    changed = [changed_value(m) if m.split("=", 1)[0] == first else m for m in macros]
    print(f"changing {first} only")
    return run("incremental", changed)


if __name__ == "__main__":
    raise SystemExit(main())
