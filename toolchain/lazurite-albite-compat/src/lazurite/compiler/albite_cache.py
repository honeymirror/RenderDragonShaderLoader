"""ALBITE shader-result cache + batch preprocessing.

Ported from an upstream albite build workflow so plain ``lazurite build --albite`` gets
the same two speedups:

* batch preprocessing (``albite --batch-preprocess``) computes the "expanded
  source" hash for many shader variants in one process;
* a disk cache keyed by that expanded hash, so variants whose preprocessed
  source is identical compile once -- across variants *and* across runs.

That second point matters a lot for large material sets: one material can expand
into hundreds of pass/flag variants, but most of them only differ in flags the
shader body ignores.

``toolchain/lazurite_albite.py`` installs the hooks. Disable with
``RDL_ALBITE_CACHE=off``; override the directory with ``RDL_ALBITE_CACHE=<path>``.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
import tempfile
import threading
from pathlib import Path

INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*(["<])([^">]+)[">]', re.M)
LINE_DIRECTIVE_RE = re.compile(r"^\s*#\s*line\b")
IDENTIFIER_RE = re.compile(r"[A-Za-z_]\w*")

ALBITE_PROFILES = {
    "ESSL_310": "310_es",
    "Metal": "msl",
    "Direct3D_SM60": "s_6_0",
    "Direct3D_SM65": "s_6_5",
}
ALBITE_STAGES = {"Vertex": "v", "Fragment": "f", "Compute": "c"}

BATCH_MIN_PER_PROCESS = 8


def hash_key(value) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()[:32]


def file_sha(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


class FileInfo:
    """Content hashes + include closure, memoized for the whole build."""

    def __init__(self, root: Path, search_paths: list[Path]):
        self.root = root
        self.search_paths = search_paths
        self._hash: dict[Path, str] = {}
        self._includes: dict[Path, list[tuple[bool, str]]] = {}
        self._sig: dict[Path, list[str]] = {}
        self._idents: dict[Path, set[str]] = {}

    def rel_key(self, path: Path) -> str:
        try:
            return Path(os.path.relpath(Path(path).resolve(), self.root)).as_posix()
        except (OSError, ValueError):
            return Path(path).as_posix()

    def sha(self, path: Path) -> str:
        if path not in self._hash:
            try:
                self._hash[path] = file_sha(path)
            except OSError:
                self._hash[path] = "missing"
        return self._hash[path]

    def includes(self, path: Path) -> list[tuple[bool, str]]:
        if path not in self._includes:
            try:
                text = path.read_text(errors="ignore")
            except OSError:
                text = ""
            self._includes[path] = [(q == '"', name) for q, name in INCLUDE_RE.findall(text)]
        return self._includes[path]

    def resolve(self, name: str, quoted: bool, including: Path, entry_dir: Path) -> Path | None:
        candidates: list[Path] = []
        if quoted:
            candidates.append(including.parent)
        candidates.append(entry_dir)
        candidates.extend(self.search_paths)
        for base in candidates:
            candidate = Path(os.path.normpath(base / name))
            if candidate.is_file():
                return candidate
        return None

    def closure(self, roots: list[Path]) -> set[Path]:
        found: set[Path] = set()
        for root in roots:
            entry_dir = root.parent
            seen = {root}
            stack = [root]
            while stack:
                current = stack.pop()
                for quoted, name in self.includes(current):
                    target = self.resolve(name, quoted, current, entry_dir)
                    if target is not None and target not in seen:
                        seen.add(target)
                        found.add(target)
                        stack.append(target)
        return found

    def signature(self, root: Path) -> list[str]:
        root = Path(os.path.normpath(root))
        if root not in self._sig:
            files = {root} | self.closure([root])
            self._sig[root] = [f"{self.rel_key(path)}:{self.sha(path)}" for path in sorted(files)]
        return self._sig[root]

    def identifiers(self, root: Path) -> set[str]:
        root = Path(os.path.normpath(root))
        if root not in self._idents:
            identifiers: set[str] = set()
            for path in {root} | self.closure([root]):
                try:
                    identifiers.update(
                        IDENTIFIER_RE.findall(path.read_text(errors="ignore"))
                    )
                except OSError:
                    pass
            self._idents[root] = identifiers
        return self._idents[root]


class ShaderCache:
    def __init__(self, root: Path, directory: Path, albite_path: str):
        self.root = root
        self.dir = directory
        self.albite_path = albite_path
        self.albite_sha = file_sha(Path(albite_path)) if os.path.isfile(albite_path) else str(albite_path)
        self._infos: dict[tuple[str, ...], FileInfo] = {}
        self._info_lock = threading.Lock()
        self._memo_lock = threading.Lock()
        self._flight_lock = threading.Lock()
        self._inflight: dict[str, threading.Event] = {}
        self.pre_memo: dict[tuple, bytes | None] = {}
        self._stats_lock = threading.Lock()
        self.compiled = 0
        self.hits = 0
        self.pp_hits = 0
        self.waits = 0

    def _info(self, include) -> FileInfo:
        key = tuple(os.path.normpath(str(item)) for item in (include or []))
        with self._info_lock:
            if key not in self._infos:
                self._infos[key] = FileInfo(self.root, [Path(item) for item in key])
            return self._infos[key]

    def _rel_key(self, path) -> str:
        try:
            return Path(os.path.relpath(Path(path).resolve(), self.root)).as_posix()
        except (OSError, ValueError):
            return Path(path).as_posix()

    def key(self, file, platform, stage, include, defines, options) -> str:
        info = self._info(include)
        return hash_key(
            [
                self.albite_sha,
                platform.name,
                stage.name,
                [info.rel_key(Path(item)) for item in (include or [])],
                [item.format_bgfx() for item in (defines or [])],
                list(options or []),
                info.signature(Path(file)),
            ]
        )

    def relevant_defines(self, file, include, defines):
        defines = list(defines or [])
        if not defines:
            return []
        identifiers = self._info(include).identifiers(Path(file))
        relevant = [item for item in defines if item.name in identifiers]
        # An unreadable/empty closure must stay conservative.
        return relevant if identifiers else defines

    def _preprocess_args(self, file, platform, stage, include, defines, options):
        profile = ALBITE_PROFILES.get(platform.name)
        stage_type = ALBITE_STAGES.get(stage.name)
        if profile is None or stage_type is None:
            return None
        args = ["-E", "-f", str(file), "-t", stage_type, "-p", profile]
        for item in include or []:
            args.extend(("-i", item))
        if defines:
            args.extend(("--define", ";".join(item.format_bgfx() for item in defines)))
        if options:
            args.extend(options)
        return args

    def _expanded_hash(self, stdout: bytes, file, platform, stage, options) -> str:
        text = stdout.decode(errors="replace").replace(str(self.root), "<root>")
        lines = [
            line.rstrip()
            for line in text.splitlines()
            if line.strip() and not LINE_DIRECTIVE_RE.match(line)
        ]
        # 只允许"同一个源文件的不同变体"共用产物。
        # 跨文件复用不安全：不同文件即使预处理文本一致，albite 产出的
        # varying/attribute 语义也可能按文件/入口不同（实测 SunMoonForwardPBR
        # 的 fragment 复用了别的文件的产物后，v_texcoord0 在链接检查里消失）。
        return hash_key(
            [
                self.albite_sha,
                platform.name,
                stage.name,
                list(options or []),
                self._rel_key(file),
                "\n".join(lines),
            ]
        )

    def expanded_key(self, file, platform, stage, include, defines, options):
        if _env_off("RDL_ALBITE_EXPANDED"):
            return None
        args = self._preprocess_args(file, platform, stage, include, defines, options)
        if args is None:
            return None
        memo_key = tuple(args)
        with self._memo_lock:
            if memo_key in self.pre_memo:
                stdout = self.pre_memo.pop(memo_key)
            else:
                stdout = _NOT_PREWARMED
        if stdout is _NOT_PREWARMED:
            try:
                proc = subprocess.run(
                    [self.albite_path, *args], capture_output=True, timeout=300
                )
            except (OSError, subprocess.SubprocessError):
                return None
            if proc.returncode != 0:
                return None
            stdout = proc.stdout
        if stdout is None:
            return None  # let the real compile report the error
        return self._expanded_hash(stdout, file, platform, stage, options)

    def prewarm_preprocess(self, items, workers: int) -> None:
        if _env_off("RDL_ALBITE_BATCH"):
            return
        if not albite_supports_batch(self.albite_path):
            return
        todo: list[list[str]] = []
        seen: set[tuple] = set()
        for file, platform, stage, include, defines, options in items:
            key_defines = self.relevant_defines(file, include, defines)
            key = self.key(file, platform, stage, include, key_defines, options)
            if self.has_raw(key):
                continue
            args = self._preprocess_args(file, platform, stage, include, defines, options)
            if args is None or any("\n" in item or not item for item in args):
                continue
            if tuple(args) not in seen:
                seen.add(tuple(args))
                todo.append(args)
        if len(todo) < 2:
            return

        procs = max(1, min(workers, len(todo) // BATCH_MIN_PER_PROCESS))
        chunks = [todo[i::procs] for i in range(procs)]
        results: list[dict] = [{} for _ in chunks]
        threads = [
            threading.Thread(
                target=_run_preprocess_batch,
                args=(self.albite_path, chunk, results[index]),
                daemon=True,
            )
            for index, chunk in enumerate(chunks)
        ]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        with self._memo_lock:
            for result in results:
                self.pre_memo.update(result)

    def _path(self, key: str, suffix: str = ".bin") -> Path:
        return self.dir / key[:2] / (key + suffix)

    def read_blob(self, key: str) -> bytes | None:
        try:
            return self._path(key).read_bytes()
        except OSError:
            return None

    def has_raw(self, key: str) -> bool:
        return self._path(key).is_file() or self._path(key, ".alias").is_file()

    def lookup(self, key: str, platform, stage, parse):
        try:
            alias = self._path(key, ".alias").read_text().strip()
        except OSError:
            alias = ""
        expanded = alias or key
        blob = self.read_blob(expanded)
        if blob is None:
            return None
        try:
            return parse(blob, platform, stage)
        except Exception:
            return None

    def _write(self, path: Path, data: bytes) -> None:
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            fd, tmp = tempfile.mkstemp(dir=path.parent, suffix=".tmp")
            with os.fdopen(fd, "wb") as handle:
                handle.write(data)
            os.replace(tmp, path)
        except OSError:
            pass

    def alias(self, key: str, expanded: str) -> None:
        self._write(self._path(key, ".alias"), expanded.encode())

    def store(self, key: str, expanded: str | None, raw: bytes) -> None:
        if expanded is None:
            self._write(self._path(key), raw)
        else:
            self._write(self._path(expanded), raw)
            self.alias(key, expanded)

    def record(self, hit: bool, via_preprocess: bool = False) -> None:
        with self._stats_lock:
            if hit:
                self.hits += 1
                if via_preprocess:
                    self.pp_hits += 1
            else:
                self.compiled += 1

    def claim(self, key: str) -> threading.Event:
        """Serialize compilation for one expanded key across worker threads."""
        while True:
            with self._flight_lock:
                event = self._inflight.get(key)
                if event is None:
                    event = threading.Event()
                    self._inflight[key] = event
                    return event
            with self._stats_lock:
                self.waits += 1
            event.wait()

    def release(self, key: str, event: threading.Event) -> None:
        with self._flight_lock:
            if self._inflight.get(key) is event:
                del self._inflight[key]
        event.set()

    def summary(self) -> str:
        with self._stats_lock:
            return (
                f"compiled={self.compiled} hits={self.hits} "
                f"(preprocess={self.pp_hits}, waited={self.waits}) dir={self.dir}"
            )


_NOT_PREWARMED = object()
_BATCH_SUPPORT: dict[str, bool] = {}
_TL = threading.local()
_ACTIVE: ShaderCache | None = None


def _env_off(name: str) -> bool:
    return os.environ.get(name, "").strip().lower() in ("0", "off", "false", "no")


def _log(message: str) -> None:
    setting = os.environ.get("RDL_ALBITE_CACHE_LOG", "").strip().lower()
    if setting not in ("", "0", "off", "false", "no"):
        print(f"[albite-cache] {message}", flush=True)


def albite_supports_batch(albite_path: str) -> bool:
    if albite_path not in _BATCH_SUPPORT:
        try:
            proc = subprocess.run([albite_path, "--help"], capture_output=True, timeout=30)
            _BATCH_SUPPORT[albite_path] = b"--batch-preprocess" in (proc.stdout + proc.stderr)
        except (OSError, subprocess.SubprocessError):
            _BATCH_SUPPORT[albite_path] = False
    return _BATCH_SUPPORT[albite_path]


def _parse_batch_output(data: bytes, count: int) -> list[bytes | None]:
    out: list[bytes | None] = []
    pos = 0
    while len(out) < count:
        eol = data.find(b"\n", pos)
        if eol < 0:
            break
        head = data[pos:eol].split(b" ")
        if len(head) != 2 or head[0] not in (b"OK", b"ERR") or not head[1].isdigit():
            break
        size = int(head[1])
        body = data[eol + 1 : eol + 1 + size]
        if len(body) != size:
            break
        out.append(body if head[0] == b"OK" else None)
        pos = eol + 1 + size
    return out


def _run_preprocess_batch(albite_path: str, requests: list[list[str]], results: dict) -> None:
    payload = "".join("\n".join(args) + "\n\n" for args in requests).encode()
    try:
        proc = subprocess.run(
            [albite_path, "--batch-preprocess"],
            input=payload,
            capture_output=True,
            timeout=600,
        )
    except (OSError, subprocess.SubprocessError):
        return
    for args, body in zip(requests, _parse_batch_output(proc.stdout, len(requests))):
        results[tuple(args)] = body


def active() -> ShaderCache | None:
    return _ACTIVE


def install(project_root: Path, albite_path: str, cache_dir: Path) -> ShaderCache | None:
    """Wrap AlbiteCompiler.compile/parse_stage + ThreadPoolExecutor.map."""
    global _ACTIVE
    if _ACTIVE is not None:
        return _ACTIVE

    import inspect

    import lazurite.compiler.albite as lz_albite
    import lazurite.project.project as lz_project

    orig_compile = lz_albite.AlbiteCompiler.compile
    orig_parse = lz_albite.parse_stage
    signature = inspect.signature(orig_compile)

    cache = ShaderCache(project_root, cache_dir, albite_path)

    def parse_hook(data, platform, stage):
        result = orig_parse(data, platform, stage)
        _TL.raw = data
        return result

    def compile_hook(self, *args, **kwargs):
        bound = signature.bind(self, *args, **kwargs)
        bound.apply_defaults()
        values = bound.arguments
        file = values["file"]
        platform = values["platform"]
        stage = values["stage"]
        include = values.get("include")
        defines = values.get("defines")
        options = values.get("options")

        key_defines = cache.relevant_defines(file, include, defines)
        key = cache.key(file, platform, stage, include, key_defines, options)
        hit = cache.lookup(key, platform, stage, orig_parse)
        if hit is not None:
            _log(f"HIT-RAW  {cache._rel_key(file)} {stage.name} key={key[:12]}")
            cache.record(True)
            return hit

        expanded = cache.expanded_key(file, platform, stage, include, defines, options)
        if expanded is not None:
            hit = cache.lookup(expanded, platform, stage, orig_parse)
            if hit is not None:
                cache.alias(key, expanded)
                _log(
                    f"HIT-EXP  {cache._rel_key(file)} {stage.name} "
                    f"key={key[:12]} exp={expanded[:12]}"
                )
                cache.record(True, via_preprocess=True)
                return hit

        flight_key = expanded or key
        flight = cache.claim(flight_key)
        try:
            # Another worker may have populated this expanded key while we waited.
            hit = cache.lookup(flight_key, platform, stage, orig_parse)
            if hit is not None:
                if expanded is not None and flight_key != key:
                    cache.alias(key, expanded)
                _log(
                    f"HIT-FLIGHT {cache._rel_key(file)} {stage.name} "
                    f"key={key[:12]} exp={(expanded or '-')[:12]}"
                )
                cache.record(True, via_preprocess=expanded is not None)
                return hit

            _TL.raw = None
            result = orig_compile(self, *args, **kwargs)
            raw = getattr(_TL, "raw", None)
            if raw:
                cache.store(key, expanded, raw)
                _log(
                    f"STORE    {cache._rel_key(file)} {stage.name} "
                    f"key={key[:12]} exp={(expanded or '-')[:12]}"
                )
            cache.record(False)
            return result
        finally:
            cache.release(flight_key, flight)

    lz_albite.AlbiteCompiler.compile = compile_hook
    lz_albite.parse_stage = parse_hook

    base_executor = lz_project.ThreadPoolExecutor

    class CachingExecutor(base_executor):
        def map(self, fn, *iterables, **kwargs):
            cache_ref = _ACTIVE
            if (
                cache_ref is not None
                and getattr(fn, "__name__", "") == "_compile_albite_shader_async"
                and len(iterables) >= 7
            ):
                try:
                    _compilers, files, stages, platforms, includes, defines, options = iterables[:7]
                    items = list(zip(files, platforms, stages, includes, defines, options))
                    workers = getattr(self, "_max_workers", None) or os.cpu_count() or 1
                    cache_ref.prewarm_preprocess(items, workers)
                except Exception:
                    pass
            return super().map(fn, *iterables, **kwargs)

    lz_project.ThreadPoolExecutor = CachingExecutor
    _ACTIVE = cache
    return cache


def uninstall() -> None:
    global _ACTIVE
    _ACTIVE = None
