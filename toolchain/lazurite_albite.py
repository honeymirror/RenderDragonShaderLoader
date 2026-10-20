"""Run the bundled lazurite + albite compatibility layer.

The normal Python environment may have an older lazurite installed. This
launcher prepends the repository copy so albite support is used only by the
mod's compiler subprocess.

For ``build`` it also turns on the ALBITE shader cache + batch preprocessing
(ported from an upstream albite build workflow). The cache directory defaults to
``<project>/../cache/albite``; override with ``RDL_ALBITE_CACHE=<path>`` or
disable with ``RDL_ALBITE_CACHE=off``.
"""

from pathlib import Path
import os
import sys


ROOT = Path(__file__).resolve().parent
COMPAT_SRC = ROOT / "lazurite-albite-compat" / "src"
sys.path.insert(0, str(COMPAT_SRC))

from lazurite.cli import main  # noqa: E402


def _argv_value(argv, name):
    for index, item in enumerate(argv):
        if item == name and index + 1 < len(argv):
            return argv[index + 1]
    return None


def _install_cache():
    argv = sys.argv[1:]
    if not argv or argv[0] != "build":
        return None

    setting = os.environ.get("RDL_ALBITE_CACHE", "").strip()
    if setting.lower() in ("0", "off", "false", "no"):
        return None

    albite = _argv_value(argv, "--albite")
    if not albite:
        return None  # shaderc backend: nothing to cache

    project = next((item for item in argv[1:] if not item.startswith("-")), None)
    if not project or not Path(project).is_dir():
        return None
    project_root = Path(project).resolve()

    if setting:
        cache_dir = Path(setting)
        if not cache_dir.is_absolute():
            cache_dir = (Path.cwd() / cache_dir).resolve()
    else:
        cache_dir = project_root.parent / "cache" / "albite"

    from lazurite.compiler import albite_cache

    return albite_cache.install(project_root, str(albite), cache_dir)


def _run():
    cache = None
    try:
        cache = _install_cache()
    except Exception as error:  # a cache must never break a build
        print(f"[albite-cache] disabled: {error}")
    if cache is not None:
        print(f"[albite-cache] enabled: {cache.dir}")
    try:
        main()
    finally:
        if cache is not None:
            print(f"[albite-cache] {cache.summary()}")


if __name__ == "__main__":
    _run()
