"""Create a deterministic non-engine material candidate for mapping tests."""

from __future__ import annotations

import argparse
from io import BytesIO
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
COMPAT_SRC = ROOT / "toolchain" / "lazurite-albite-compat" / "src"
sys.path.insert(0, str(COMPAT_SRC))

from lazurite.material import Material  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--source-buffer", required=True)
    parser.add_argument("--target-buffer", required=True)
    parser.add_argument("--pass", dest="pass_name", required=True)
    parser.add_argument("--signature", required=True)
    args = parser.parse_args()

    material = Material.load_bin_file(args.input.resolve())
    buffer = next(
        (item for item in material.buffers if item.name == args.source_buffer),
        None,
    )
    if buffer is None:
        raise ValueError(f"buffer {args.source_buffer!r} was not found")
    buffer.name = args.target_buffer

    shader_pass = next(
        (item for item in material.passes if item.name == args.pass_name),
        None,
    )
    if shader_pass is None:
        raise ValueError(f"pass {args.pass_name!r} was not found")
    shader_pass.output_binding_signature = int(args.signature, 16)

    data = BytesIO()
    material.write(data)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data.getvalue())
    print(
        f"probe={material.name} buffer={args.target_buffer} "
        f"pass={args.pass_name} signature={args.signature}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
