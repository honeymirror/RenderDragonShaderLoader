"""Inspect and patch existing vec4 uniforms in compiled material files.

This intentionally supports only the first safe subset for runtime parameters:
an existing inline vec4 uniform with count == 1 that is not driven by
uniform_overrides. Changing such a default does not require recompiling shaders.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
COMPAT_SRC = ROOT / "toolchain" / "lazurite-albite-compat" / "src"
sys.path.insert(0, str(COMPAT_SRC))

from lazurite.material import Material  # noqa: E402
from lazurite.material.uniform import UniformType  # noqa: E402


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_material(path: Path) -> Material:
    if not path.is_file():
        raise ValueError(f"material file not found: {path}")
    material = Material.load_bin_file(str(path))
    if material.encryption.name != "NONE":
        raise ValueError(f"encrypted material is not supported: {path}")
    return material


def uniform_payload(uniform) -> dict:
    return {
        "name": uniform.name,
        "type": uniform.type.name,
        "count": uniform.count,
        "default": list(uniform.default),
        "override": None,
    }


def describe_material(path: Path, material: Material, eligible_only: bool) -> dict:
    uniforms = []
    for uniform in material.uniforms:
        item = uniform_payload(uniform)
        item["override"] = material.uniform_overrides.get(uniform.name)
        if eligible_only and not (
            item["type"] == "vec4"
            and item["count"] == 1
            and item["override"] is None
        ):
            continue
        uniforms.append(item)
    return {
        "path": str(path),
        "sha256": sha256(path),
        "material": material.name,
        "version": material.version,
        "uniforms": uniforms,
    }


def find_uniform(material: Material, name: str):
    for uniform in material.uniforms:
        if uniform.name == name:
            return uniform
    names = ", ".join(sorted(uniform.name for uniform in material.uniforms))
    raise ValueError(f"uniform '{name}' not found; available: {names}")


def validate_target(material: Material, uniform) -> None:
    override = material.uniform_overrides.get(uniform.name)
    if override is not None:
        raise ValueError(f"uniform '{uniform.name}' is driven by {override}")
    if uniform.type != UniformType.vec4:
        raise ValueError(
            f"uniform '{uniform.name}' has type {uniform.type.name}; expected vec4"
        )
    if uniform.count != 1:
        raise ValueError(
            f"uniform '{uniform.name}' has count {uniform.count}; expected 1"
        )


def parse_components(values: list[str]) -> list[float]:
    if len(values) != 4:
        raise ValueError("expected exactly 4 float components")
    components = [float(value) for value in values]
    if not all(math.isfinite(value) for value in components):
        raise ValueError("all components must be finite")
    return components


def uniform_state(material: Material) -> list[tuple]:
    return [
        (
            uniform.name,
            uniform.type.value,
            uniform.count,
            tuple(uniform.default),
        )
        for uniform in material.uniforms
    ]


def values_equal(actual, expected) -> bool:
    if len(actual) != len(expected):
        return False
    return all(
        math.isclose(float(a), float(b), rel_tol=1e-6, abs_tol=1e-6)
        for a, b in zip(actual, expected)
    )


def states_equal(actual: list[tuple], expected: list[tuple]) -> bool:
    if len(actual) != len(expected):
        return False
    for (a_name, a_type, a_count, a_default), (
        e_name,
        e_type,
        e_count,
        e_default,
    ) in zip(actual, expected):
        if (a_name, a_type, a_count) != (e_name, e_type, e_count):
            return False
        if not values_equal(a_default, e_default):
            return False
    return True


def find_default_payload(data: bytes, uniform, default: list[float]) -> tuple[int, int]:
    name = uniform.name.encode()
    header = (
        struct.pack("<I", len(name))
        + name
        + struct.pack("<H", uniform.type.value)
        + struct.pack("<I", uniform.count)
        + b"\x01"
    )
    needle = header + struct.pack("<4f", *default)
    positions = []
    offset = 0
    while True:
        offset = data.find(needle, offset)
        if offset < 0:
            break
        positions.append(offset)
        offset += 1
    if len(positions) != 1:
        raise ValueError(
            f"could not locate a unique default payload for uniform '{uniform.name}'"
        )
    start = positions[0] + len(header)
    return start, start + 16


def verify_only_target_bytes(
    source: bytes,
    output: bytes,
    source_range: tuple[int, int],
    new_default: list[float],
) -> None:
    if len(source) != len(output):
        raise RuntimeError("serialized material changed size")
    start, end = source_range
    if output[start:end] != struct.pack("<4f", *new_default):
        raise RuntimeError("serialized default payload does not match the patch")
    if source[:start] != output[:start] or source[end:] != output[end:]:
        raise RuntimeError("serialized material changed bytes outside the target default")


def verify_only_target_ranges(
    source: bytes,
    output: bytes,
    ranges: list[tuple[int, int]],
) -> None:
    if len(source) != len(output):
        raise RuntimeError("serialized material changed size")
    allowed = set()
    for start, end in ranges:
        allowed.update(range(start, end))
    for index, (before, after) in enumerate(zip(source, output)):
        if before != after and index not in allowed:
            raise RuntimeError(
                f"serialized material changed byte {index} outside the target uniforms"
            )


def resolve_output(material_path: Path, material: Material, args) -> Path:
    if args.in_place:
        if args.output:
            raise ValueError("--in-place and --output are mutually exclusive")
        return material_path
    if not args.output:
        raise ValueError("choose --output <file-or-directory> or --in-place")
    output = Path(args.output)
    if output.exists() and output.is_dir():
        return output / f"{material.name}.material.bin"
    if output.suffix.lower() != ".material.bin":
        return output / f"{material.name}.material.bin"
    return output


def write_material_atomic(material: Material, output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    temp_path = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb",
            prefix=f".{output.name}.",
            suffix=".tmp",
            dir=output.parent,
            delete=False,
        ) as file:
            temp_path = Path(file.name)
            material.write(file)
        os.replace(temp_path, output)
    finally:
        if temp_path is not None and temp_path.exists():
            temp_path.unlink()


def command_list(args) -> int:
    path = Path(args.material)
    material = load_material(path)
    print(json.dumps(describe_material(path, material, args.eligible), indent=2))
    return 0


def command_patch(args) -> int:
    path = Path(args.material)
    material = load_material(path)
    uniform = find_uniform(material, args.uniform)
    validate_target(material, uniform)

    components = parse_components(args.value)
    before = uniform_state(material)
    old_default = list(uniform.default)
    source_bytes = path.read_bytes()
    source_range = find_default_payload(source_bytes, uniform, old_default)
    uniform.default = components

    if uniform_state(material) == before:
        source_sha = sha256(path)
        print(
            json.dumps(
                {
                    "changed": False,
                    "uniform": uniform.name,
                    "default": old_default,
                    "sha256": source_sha,
                },
                indent=2,
            )
        )
        return 0

    output = resolve_output(path, material, args)
    if output.resolve() == path.resolve() and not args.in_place:
        raise ValueError("refusing to overwrite input without --in-place")

    if args.dry_run:
        print(
            json.dumps(
                {
                    "changed": True,
                    "dry_run": True,
                    "material": str(path),
                    "output": str(output),
                    "uniform": uniform.name,
                    "old_default": old_default,
                    "new_default": components,
                },
                indent=2,
            )
        )
        return 0

    source_sha = sha256(path)
    write_material_atomic(material, output)

    patched = load_material(output)
    patched_uniform = find_uniform(patched, uniform.name)
    validate_target(patched, patched_uniform)
    if not values_equal(patched_uniform.default, components):
        raise RuntimeError("serialized uniform default does not match the patch")

    expected = [
        (name, kind, count, tuple(components) if name == uniform.name else default)
        for name, kind, count, default in before
    ]
    if not states_equal(uniform_state(patched), expected):
        raise RuntimeError("material read-back changed fields other than the target")
    verify_only_target_bytes(source_bytes, output.read_bytes(), source_range, components)

    print(
        json.dumps(
            {
                "changed": True,
                "in_place": output.resolve() == path.resolve(),
                "material": str(path),
                "output": str(output),
                "uniform": uniform.name,
                "old_default": old_default,
                "new_default": components,
                "source_sha256": source_sha,
                "output_sha256": sha256(output),
                "output_size": output.stat().st_size,
            },
            indent=2,
        )
    )
    return 0


def command_apply_json(args) -> int:
    directory = Path(args.directory)
    request_path = Path(args.request)
    if not directory.is_dir():
        raise ValueError(f"material directory not found: {directory}")
    with request_path.open("r", encoding="utf-8") as file:
        request = json.load(file)
    operations = request.get("operations") if isinstance(request, dict) else request
    if not isinstance(operations, list) or not operations:
        raise ValueError("request must contain a non-empty operations array")

    grouped = {}
    for operation in operations:
        if not isinstance(operation, dict):
            raise ValueError("every operation must be an object")
        material_name = str(operation.get("material", "")).strip()
        uniform_name = str(operation.get("uniform", "")).strip()
        if not material_name or not uniform_name:
            raise ValueError("operation requires material and uniform")
        wildcard = material_name in ("*", "all")
        if not wildcard and not material_name.lower().endswith(".material.bin"):
            material_name += ".material.bin"
        components = operation.get("components", [])
        values = operation.get("values", [])
        if not isinstance(components, list) or not isinstance(values, list):
            raise ValueError("components and values must be arrays")
        if len(components) != len(values) or not components:
            raise ValueError("components and values must have the same non-zero length")
        parsed_components = []
        parsed_values = []
        for component, value in zip(components, values):
            if not isinstance(component, int) or component < 0 or component > 3:
                raise ValueError("components must be integers in 0..3")
            if component in parsed_components:
                raise ValueError("duplicate component")
            number = float(value)
            if not math.isfinite(number):
                raise ValueError("uniform values must be finite")
            parsed_components.append(component)
            parsed_values.append(number)
        if wildcard:
            material_paths = sorted(directory.glob("*.material.bin"))
            if not material_paths:
                raise ValueError(f"no material files found in {directory}")
            for material_path in material_paths:
                grouped.setdefault(material_path.name, []).append(
                    (uniform_name, parsed_components, parsed_values)
                )
        else:
            grouped.setdefault(material_name, []).append(
                (uniform_name, parsed_components, parsed_values)
            )

    patched_materials = []
    for material_name, material_operations in grouped.items():
        path = directory / material_name
        material = load_material(path)
        before = uniform_state(material)
        source_bytes = path.read_bytes()
        expected = {}
        ranges = []
        range_names = set()

        for uniform_name, components, values in material_operations:
            uniform = find_uniform(material, uniform_name)
            validate_target(material, uniform)
            if uniform.name not in range_names:
                ranges.append(find_default_payload(
                    source_bytes,
                    uniform,
                    list(uniform.default),
                ))
                range_names.add(uniform.name)
            vector = list(expected.get(uniform.name, uniform.default))
            if len(vector) != 4:
                raise ValueError(
                    f"uniform '{uniform.name}' has no inline vec4 default to patch"
                )
            for component, value in zip(components, values):
                vector[component] = value
            expected[uniform.name] = vector

        for uniform_name, vector in expected.items():
            find_uniform(material, uniform_name).default = vector

        write_material_atomic(material, path)
        patched = load_material(path)
        for uniform_name, vector in expected.items():
            patched_uniform = find_uniform(patched, uniform_name)
            validate_target(patched, patched_uniform)
            if not values_equal(patched_uniform.default, vector):
                raise RuntimeError(
                    f"serialized uniform '{uniform_name}' does not match the patch"
                )

        expected_state = [
            (
                name,
                kind,
                count,
                tuple(expected[name]) if name in expected else default,
            )
            for name, kind, count, default in before
        ]
        if not states_equal(uniform_state(patched), expected_state):
            raise RuntimeError(
                f"material '{material.name}' changed fields other than target uniforms"
            )
        verify_only_target_ranges(source_bytes, path.read_bytes(), ranges)

        patched_materials.append(
            {
                "material": material.name,
                "path": str(path),
                "uniforms": sorted(expected),
                "sha256": sha256(path),
                "size": path.stat().st_size,
            }
        )

    print(
        json.dumps(
            {
                "changed": True,
                "materials": patched_materials,
            },
            indent=2,
        )
    )
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Inspect or patch an existing inline vec4 material uniform."
    )
    commands = parser.add_subparsers(dest="command", required=True)

    list_parser = commands.add_parser("list", help="list material uniforms as JSON")
    list_parser.add_argument("material")
    list_parser.add_argument(
        "--eligible",
        action="store_true",
        help="only show vec4/count=1 uniforms not controlled by uniform_overrides",
    )
    list_parser.set_defaults(func=command_list)

    patch_parser = commands.add_parser("patch", help="patch an inline vec4 default")
    patch_parser.add_argument("material")
    patch_parser.add_argument("uniform")
    patch_parser.add_argument("value", nargs=4, metavar="FLOAT")
    patch_parser.add_argument("-o", "--output", help="output .material.bin path or directory")
    patch_parser.add_argument(
        "--in-place",
        action="store_true",
        help="atomically replace the input file",
    )
    patch_parser.add_argument("--dry-run", action="store_true")
    patch_parser.set_defaults(func=command_patch)

    apply_parser = commands.add_parser(
        "apply-json",
        help="apply a batch of component-level uniform patches",
    )
    apply_parser.add_argument("--directory", required=True)
    apply_parser.add_argument("--request", required=True)
    apply_parser.set_defaults(func=command_apply_json)
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    try:
        return args.func(args)
    except (OSError, RuntimeError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
