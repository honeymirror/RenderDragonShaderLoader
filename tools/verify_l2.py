"""Verify that an L2 rebuild changed only compiled shader slots.

Usage:
  py -3 tools/verify_l2.py <baseline-compiled> <rebuilt-compiled> [--expect N]
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
COMPAT_SRC = ROOT / "toolchain" / "lazurite-albite-compat" / "src"
sys.path.insert(0, str(COMPAT_SRC))

from lazurite.material import Material  # noqa: E402


def shader_slots(material: Material) -> dict:
    slots = {}
    for pass_index, shader_pass in enumerate(material.passes):
        for variant_index, variant in enumerate(shader_pass.variants):
            for shader in variant.shaders:
                key = (
                    pass_index,
                    shader_pass.name,
                    variant_index,
                    tuple(sorted(variant.flags.items())),
                    shader.platform.name,
                    shader.stage.name,
                )
                slots[key] = shader
    return slots


def material_structure(
    material: Material,
    ignored_uniform_prefixes=(),
    ignore_bgfx_register_layout=False,
) -> dict:
    def item_name(item):
        return item.get("name", "")

    def keep_uniform(name):
        return not any(name.startswith(prefix) for prefix in ignored_uniform_prefixes)

    def shader_structure(shader):
        bgfx = shader.bgfx_shader.serialize_properties()
        bgfx["uniforms"] = [
            uniform
            for uniform in bgfx["uniforms"]
            if keep_uniform(uniform["name"])
        ]
        if ignore_bgfx_register_layout:
            for uniform in bgfx["uniforms"]:
                uniform.pop("reg_index", None)
                uniform.pop("reg_count", None)
            bgfx.pop("size", None)
        return {
            "stage": shader.stage.name,
            "platform": shader.platform.name,
            "inputs": [
                shader_input.serialize_properties() for shader_input in shader.inputs
            ],
            "hash": shader.hash,
            "bgfx": bgfx,
        }

    def variant_structure(variant):
        shaders = [shader_structure(shader) for shader in variant.shaders]
        shaders.sort(key=lambda shader: (shader["platform"], shader["stage"]))
        return {
            "is_supported": variant.is_supported,
            "flags": variant.flags,
            "shaders": shaders,
        }

    def pass_structure(shader_pass):
        variants = [
            variant_structure(variant) for variant in shader_pass.variants
        ]
        variants.sort(key=lambda variant: tuple(sorted(variant["flags"].items())))
        return {
            "name": shader_pass.name,
            "supported_platforms": shader_pass.supported_platforms.serialize(
                material.version
            ),
            "fallback_pass": shader_pass.fallback_pass,
            "default_blend_mode": shader_pass.default_blend_mode.name,
            "flag_domain": shader_pass.flag_domain,
            "output_binding_signature": shader_pass.output_binding_signature,
            "variants": variants,
        }

    passes = [pass_structure(shader_pass) for shader_pass in material.passes]
    passes.sort(key=item_name)
    return {
        "version": material.version,
        "name": material.name,
        "encryption": material.encryption.name,
        "parent": material.parent,
        "buffers": sorted(
            (buffer.serialize_properties() for buffer in material.buffers),
            key=item_name,
        ),
        "uniforms": sorted(
            (
                uniform.serialize_properties()
                for uniform in material.uniforms
                if keep_uniform(uniform.name)
            ),
            key=item_name,
        ),
        "uniform_overrides": material.uniform_overrides,
        "passes": passes,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("baseline")
    parser.add_argument("rebuilt")
    parser.add_argument("--expect", type=int)
    parser.add_argument("--ignore-uniform-prefix", action="append", default=[])
    parser.add_argument("--ignore-bgfx-register-layout", action="store_true")
    args = parser.parse_args()

    baseline = Path(args.baseline)
    rebuilt = Path(args.rebuilt)
    files = sorted(baseline.glob("*.material.bin"))
    if not files:
        raise SystemExit(f"no materials in baseline: {baseline}")
    baseline_names = {path.name for path in files}
    rebuilt_names = {path.name for path in rebuilt.glob("*.material.bin")}

    changed = []
    structural = []
    for name in sorted(rebuilt_names - baseline_names):
        structural.append(f"{name}: unexpected rebuilt material")
    for before_path in files:
        after_path = rebuilt / before_path.name
        if not after_path.is_file():
            structural.append(f"{before_path.name}: missing rebuilt material")
            continue
        before = Material.load_bin_file(str(before_path))
        after = Material.load_bin_file(str(after_path))
        if material_structure(
            before,
            args.ignore_uniform_prefix,
            args.ignore_bgfx_register_layout,
        ) != material_structure(
            after,
            args.ignore_uniform_prefix,
            args.ignore_bgfx_register_layout,
        ):
            structural.append(f"{before_path.name}: material metadata or signatures changed")
            continue
        before_slots = shader_slots(before)
        after_slots = shader_slots(after)
        if set(before_slots) != set(after_slots):
            structural.append(f"{before_path.name}: pass/variant/platform/stage shape changed")
            continue
        for key, old_shader in before_slots.items():
            new_shader = after_slots[key]
            if old_shader.bgfx_shader.shader_bytes != new_shader.bgfx_shader.shader_bytes:
                changed.append(
                    (
                        before_path.name,
                        key[1],
                        key[2],
                        key[4],
                        key[5],
                        len(old_shader.bgfx_shader.shader_bytes),
                        len(new_shader.bgfx_shader.shader_bytes),
                    )
                )

    for row in changed:
        print(
            f"CHANGED {row[0]} pass={row[1]} variant={row[2]} "
            f"platform={row[3]} stage={row[4]} bytes={row[5]}->{row[6]}"
        )
    for issue in structural:
        print(f"STRUCT {issue}")
    print(f"materials={len(files)} changed_slots={len(changed)} structural_issues={len(structural)}")

    if structural:
        return 1
    if args.expect is not None and len(changed) != args.expect:
        print(f"error: expected {args.expect} changed slots, got {len(changed)}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
