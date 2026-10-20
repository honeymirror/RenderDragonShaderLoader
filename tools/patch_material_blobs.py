"""Apply L2.2 shader-bytecode patches to compiled material binaries."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
COMPAT_SRC = ROOT / "toolchain" / "lazurite-albite-compat" / "src"
sys.path.insert(0, str(COMPAT_SRC))

from lazurite.material.blob_patch import apply_directory  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)

    apply_parser = subparsers.add_parser("apply-directory")
    apply_parser.add_argument("--cache-dir", required=True)
    apply_parser.add_argument("--target-dir", required=True)
    apply_parser.add_argument("materials", nargs="*")
    args = parser.parse_args()

    if args.command == "apply-directory":
        return apply_directory(
            cache_dir=Path(args.cache_dir),
            target_dir=Path(args.target_dir),
            materials=args.materials,
        )
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
