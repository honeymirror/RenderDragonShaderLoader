"""Dump and validate attributes, buffers, and render targets from material binaries.

The output is the compatibility contract used by the source adapters. It is
intentionally based on compiled materials rather than GLSL regexes: BGFX
attribute masks, sampler bindings, buffer slots, pass output signatures, and
variant metadata are all available there.

Usage:
  py -3 tools/material_io_dump.py <compiled-dir> [--json-out report.json]
      [--contract-out contract.json] [--baseline contract.json] [--check]
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
COMPAT_SRC = ROOT / "toolchain" / "lazurite-albite-compat" / "src"
sys.path.insert(0, str(COMPAT_SRC))

from lazurite import util  # noqa: E402
from lazurite.material import Material  # noqa: E402


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sorted_unique(values) -> list:
    return sorted(set(values), key=lambda value: str(value))


def input_record(shader_input) -> dict:
    return shader_input.serialize_properties()


def target_names(signature: int, material_name: str, pass_name: str, errors: list[str]):
    if signature == 0:
        return []
    names = util.reconstruct_fragment_outputs(signature)
    if names is None:
        errors.append(
            f"{material_name}/{pass_name}: cannot decode render target signature "
            f"0x{signature:08X}"
        )
        return []
    return sorted_unique(names)


def material_record(path: Path, errors: list[str]) -> dict:
    material = Material.load_bin_file(str(path))
    buffers = []
    seen_binding_slots: dict[int, str] = {}
    buffer_by_slot: dict[int, dict] = {}
    for buffer in material.buffers:
        item = buffer.serialize_properties()
        buffers.append(item)
        binding_slot = item.get("binding_slot")
        if isinstance(binding_slot, int):
            previous = seen_binding_slots.get(binding_slot)
            if previous is not None and previous != item["name"]:
                errors.append(
                    f"{material.name}: buffer binding slot {binding_slot} is shared "
                    f"by {previous} and {item['name']}"
                )
            else:
                seen_binding_slots[binding_slot] = item["name"]
            buffer_by_slot[binding_slot] = item

    passes = []
    variant_count = 0
    shader_count = 0
    material_attributes: dict[str, dict] = {}
    material_varyings: dict[str, dict] = {}

    for shader_pass in material.passes:
        pass_attributes: dict[str, dict] = {}
        pass_varyings: dict[str, dict] = {}
        sampler_bindings: dict[str, dict] = {}
        missing_samplers: set[tuple[str, int]] = set()
        shader_stages = set()

        for variant in shader_pass.variants:
            variant_count += 1
            for shader in variant.shaders:
                shader_count += 1
                shader_stages.add(shader.stage.name)

                for shader_input in shader.inputs:
                    item = input_record(shader_input)
                    if shader.stage.name == "Vertex":
                        bucket = pass_attributes
                    elif shader.stage.name == "Fragment":
                        bucket = pass_varyings
                    else:
                        continue
                    aggregate = bucket.setdefault(
                        item["semantic"],
                        {
                            "semantic": item["semantic"],
                            "names": set(),
                            "types": set(),
                            "perInstance": set(),
                            "interpolation": set(),
                        },
                    )
                    aggregate["names"].add(item["name"])
                    aggregate["types"].add(item["type"])
                    aggregate["perInstance"].add(item["per_instance"])
                    aggregate["interpolation"].add(item["interpolation"])

                for uniform in shader.bgfx_shader.uniforms:
                    if not uniform.name.startswith("s_"):
                        continue
                    buffer_name = uniform.name[2:]
                    existing = sampler_bindings.setdefault(
                        uniform.name,
                        {
                            "uniform": uniform.name,
                            "bindings": {},
                            "nameMatchesBuffer": set(),
                        },
                    )
                    buffer = buffer_by_slot.get(uniform.reg_index)
                    if buffer is not None:
                        binding_key = (
                            buffer["name"],
                            buffer["binding_slot"],
                            uniform.reg_index,
                            uniform.count,
                        )
                        binding = existing["bindings"].setdefault(
                            binding_key,
                            {
                                "buffer": buffer["name"],
                                "bindingSlot": buffer["binding_slot"],
                                "regIndex": uniform.reg_index,
                                "count": uniform.count,
                                "stages": set(),
                            },
                        )
                        binding["stages"].add(shader.stage.name)
                        existing["nameMatchesBuffer"].add(
                            buffer_name == buffer["name"]
                        )
                    if buffer is None and (uniform.name, uniform.reg_index) not in missing_samplers:
                        missing_samplers.add((uniform.name, uniform.reg_index))
                        errors.append(
                            f"{material.name}/{shader_pass.name}: sampler "
                            f"{uniform.name} has no buffer at binding slot "
                            f"{uniform.reg_index}"
                        )

        def finalize_inputs(
            values: dict[str, dict],
            material_target: dict[str, dict],
        ) -> list[dict]:
            output = []
            for semantic in sorted(values):
                item = values[semantic]
                output.append(
                    {
                        "semantic": item["semantic"],
                        "names": sorted_unique(item["names"]),
                        "types": sorted_unique(item["types"]),
                        "perInstance": sorted_unique(item["perInstance"]),
                        "interpolation": sorted_unique(item["interpolation"]),
                    }
                )
                target = material_target.setdefault(
                    semantic,
                    {
                        "semantic": semantic,
                        "names": set(),
                        "types": set(),
                        "passes": set(),
                    },
                )
                target["names"].update(item["names"])
                target["types"].update(item["types"])
                target["passes"].add(shader_pass.name)
            return output

        attributes = finalize_inputs(pass_attributes, material_attributes)
        varyings = finalize_inputs(pass_varyings, material_varyings)
        render_targets = target_names(
            shader_pass.output_binding_signature,
            material.name,
            shader_pass.name,
            errors,
        )
        passes.append(
            {
                "name": shader_pass.name,
                "fallbackPass": shader_pass.fallback_pass,
                "outputBindingSignature": f"0x{shader_pass.output_binding_signature:08X}",
                "renderTargets": render_targets,
                "stages": sorted_unique(shader_stages),
                "attributes": attributes,
                "varyings": varyings,
                "samplers": [
                    {
                        "uniform": item["uniform"],
                        "nameMatchesBuffer": sorted_unique(item["nameMatchesBuffer"]),
                        "bindings": [
                            {
                                "buffer": binding["buffer"],
                                "bindingSlot": binding["bindingSlot"],
                                "regIndex": binding["regIndex"],
                                "count": binding["count"],
                                "stages": sorted_unique(binding["stages"]),
                            }
                            for binding in sorted(
                                item["bindings"].values(),
                                key=lambda value: (
                                    value["bindingSlot"],
                                    value["buffer"],
                                    value["count"],
                                ),
                            )
                        ],
                    }
                    for item in sorted(
                        sampler_bindings.values(), key=lambda value: value["uniform"]
                    )
                ],
                "variantCount": len(shader_pass.variants),
            }
        )

    return {
        "name": material.name,
        "path": path.name,
        "sha256": sha256(path),
        "size": path.stat().st_size,
        "version": material.version,
        "buffers": sorted(buffers, key=lambda value: (value["binding_slot"], value["name"])),
        "attributes": [
            {
                "semantic": item["semantic"],
                "names": sorted_unique(item["names"]),
                "types": sorted_unique(item["types"]),
                "passes": sorted_unique(item["passes"]),
            }
            for item in sorted(material_attributes.values(), key=lambda value: value["semantic"])
        ],
        "varyings": [
            {
                "semantic": item["semantic"],
                "names": sorted_unique(item["names"]),
                "types": sorted_unique(item["types"]),
                "passes": sorted_unique(item["passes"]),
            }
            for item in sorted(material_varyings.values(), key=lambda value: value["semantic"])
        ],
        "passes": sorted(passes, key=lambda value: value["name"]),
        "variantCount": variant_count,
        "shaderCount": shader_count,
    }


def build_report(compiled: Path, errors: list[str]) -> dict:
    materials = []
    for path in sorted(compiled.glob("*.material.bin")):
        materials.append(material_record(path, errors))

    buffers: dict[str, dict] = {}
    attributes: dict[str, dict] = {}
    varyings: dict[str, dict] = {}
    render_targets: dict[str, dict] = {}
    sampler_aliases: dict[tuple[str, str], dict] = {}

    for material in materials:
        for buffer in material["buffers"]:
            aggregate = buffers.setdefault(
                buffer["name"],
                {
                    "name": buffer["name"],
                    "types": set(),
                    "formats": set(),
                    "access": set(),
                    "unorderedAccess": set(),
                    "bindingSlots": set(),
                    "materials": set(),
                },
            )
            aggregate["types"].add(buffer["type"])
            aggregate["formats"].add(buffer.get("texture_format", ""))
            aggregate["access"].add(buffer.get("access", ""))
            aggregate["unorderedAccess"].add(bool(buffer.get("unordered_access", False)))
            aggregate["bindingSlots"].add(buffer["binding_slot"])
            aggregate["materials"].add(material["name"])

        for bucket, target in (
            ("attributes", attributes),
            ("varyings", varyings),
        ):
            for item in material[bucket]:
                aggregate = target.setdefault(
                    item["semantic"],
                    {
                        "semantic": item["semantic"],
                        "names": set(),
                        "types": set(),
                        "materials": set(),
                        "passes": set(),
                    },
                )
                aggregate["names"].update(item["names"])
                aggregate["types"].update(item["types"])
                aggregate["materials"].add(material["name"])
                aggregate["passes"].update(item["passes"])

        for shader_pass in material["passes"]:
            for sampler in shader_pass["samplers"]:
                for binding in sampler["bindings"]:
                    if sampler["uniform"] == "s_" + binding["buffer"]:
                        continue
                    key = (sampler["uniform"], binding["buffer"])
                    aggregate = sampler_aliases.setdefault(
                        key,
                        {
                            "uniform": sampler["uniform"],
                            "buffer": binding["buffer"],
                            "bindingSlots": set(),
                            "materials": set(),
                        },
                    )
                    aggregate["bindingSlots"].add(binding["bindingSlot"])
                    aggregate["materials"].add(material["name"])

            for render_target in shader_pass["renderTargets"]:
                aggregate = render_targets.setdefault(
                    render_target,
                    {
                        "name": render_target,
                        "signatures": set(),
                        "materials": set(),
                        "passes": set(),
                    },
                )
                aggregate["signatures"].add(shader_pass["outputBindingSignature"])
                aggregate["materials"].add(material["name"])
                aggregate["passes"].add(f"{material['name']}/{shader_pass['name']}")

    def finalize_aggregate(values: dict[str, dict], set_fields: list[str]) -> dict:
        output = {}
        for key in sorted(values):
            item = values[key]
            output[key] = {
                field: sorted_unique(item[field]) if field in set_fields else item[field]
                for field in item
            }
        return output

    return {
        "schemaVersion": 1,
        "source": str(compiled.resolve()),
        "summary": {
            "materials": len(materials),
            "passes": sum(len(material["passes"]) for material in materials),
            "variants": sum(material["variantCount"] for material in materials),
            "shaders": sum(material["shaderCount"] for material in materials),
            "buffers": len(buffers),
            "attributes": len(attributes),
            "varyings": len(varyings),
            "renderTargets": len(render_targets),
            "samplerAliases": len(sampler_aliases),
            "errors": len(errors),
        },
        "buffers": finalize_aggregate(
            buffers,
            [
                "types",
                "formats",
                "access",
                "unorderedAccess",
                "bindingSlots",
                "materials",
            ],
        ),
        "attributes": finalize_aggregate(
            attributes, ["names", "types", "materials", "passes"]
        ),
        "varyings": finalize_aggregate(
            varyings, ["names", "types", "materials", "passes"]
        ),
        "renderTargets": finalize_aggregate(
            render_targets, ["signatures", "materials", "passes"]
        ),
        "samplerAliases": [
            {
                "uniform": item["uniform"],
                "buffer": item["buffer"],
                "bindingSlots": sorted_unique(item["bindingSlots"]),
                "materials": sorted_unique(item["materials"]),
            }
            for item in sorted(
                sampler_aliases.values(),
                key=lambda value: (value["uniform"], value["buffer"]),
            )
        ],
        "materials": materials,
        "errors": errors,
    }


def build_contract(report: dict) -> dict:
    materials = {}
    for material in report["materials"]:
        materials[material["name"]] = {
            "version": material["version"],
            "buffers": {
                buffer["name"]: {
                    "bindingSlot": buffer["binding_slot"],
                    "type": buffer["type"],
                    "format": buffer.get("texture_format", ""),
                    "access": buffer.get("access", ""),
                    "unorderedAccess": bool(buffer.get("unordered_access", False)),
                }
                for buffer in material["buffers"]
            },
            "attributes": {
                item["semantic"]: item["types"] for item in material["attributes"]
            },
            "varyings": {
                item["semantic"]: item["types"] for item in material["varyings"]
            },
            "passes": {
                item["name"]: {
                    "renderTargets": item["renderTargets"],
                    "outputBindingSignature": item["outputBindingSignature"],
                    "attributes": [
                        attribute["semantic"] for attribute in item["attributes"]
                    ],
                    "varyings": [
                        varying["semantic"] for varying in item["varyings"]
                    ],
                }
                for item in material["passes"]
            },
        }
    return {
        "contractVersion": 1,
        "summary": report["summary"],
        "buffers": report["buffers"],
        "attributes": report["attributes"],
        "varyings": report["varyings"],
        "renderTargets": report["renderTargets"],
        "samplerAliases": report["samplerAliases"],
        "materials": materials,
    }


def compare_contract(expected: dict, actual: dict) -> list[str]:
    differences = []

    def compare_map(expected_map: dict, actual_map: dict, label: str) -> None:
        for key in sorted(set(expected_map) - set(actual_map)):
            differences.append(f"{label}: removed {key}")
        for key in sorted(set(actual_map) - set(expected_map)):
            differences.append(f"{label}: added {key}")
        for key in sorted(set(expected_map) & set(actual_map)):
            if expected_map[key] != actual_map[key]:
                differences.append(f"{label}: changed {key}")

    compare_map(expected["attributes"], actual["attributes"], "attributes")
    compare_map(expected["buffers"], actual["buffers"], "buffers")
    compare_map(expected["renderTargets"], actual["renderTargets"], "renderTargets")
    expected_materials = expected["materials"]
    actual_materials = actual["materials"]
    for name in sorted(set(expected_materials) - set(actual_materials)):
        differences.append(f"materials: removed {name}")
    for name in sorted(set(actual_materials) - set(expected_materials)):
        differences.append(f"materials: added {name}")
    for name in sorted(set(expected_materials) & set(actual_materials)):
        before = expected_materials[name]
        after = actual_materials[name]
        if before["version"] != after["version"]:
            differences.append(f"{name}: material version changed")
        compare_map(before["buffers"], after["buffers"], f"{name}/buffers")
        compare_map(before["attributes"], after["attributes"], f"{name}/attributes")
        compare_map(before["varyings"], after["varyings"], f"{name}/varyings")
        before_passes = before["passes"]
        after_passes = after["passes"]
        for pass_name in sorted(set(before_passes) - set(after_passes)):
            differences.append(f"{name}/passes: removed {pass_name}")
        for pass_name in sorted(set(after_passes) - set(before_passes)):
            differences.append(f"{name}/passes: added {pass_name}")
        for pass_name in sorted(set(before_passes) & set(after_passes)):
            if before_passes[pass_name] != after_passes[pass_name]:
                differences.append(f"{name}/{pass_name}: render target or IO contract changed")
    return differences


def print_summary(report: dict) -> None:
    summary = report["summary"]
    print(
        "materials={materials} passes={passes} variants={variants} "
        "shaders={shaders}".format(**summary)
    )
    print(
        "buffers={buffers} attributes={attributes} varyings={varyings} "
        "renderTargets={renderTargets} samplerAliases={samplerAliases} "
        "errors={errors}".format(**summary)
    )
    print("attributes: " + ", ".join(sorted(report["attributes"])))
    print("render targets: " + ", ".join(sorted(report["renderTargets"])))
    for error in report["errors"]:
        print("ERROR " + error)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Dump and validate compiled material IO compatibility metadata."
    )
    parser.add_argument("compiled", type=Path, help="directory containing *.material.bin")
    parser.add_argument("--json-out", type=Path, help="write the full report as JSON")
    parser.add_argument(
        "--contract-out",
        type=Path,
        help="write the compact baseline contract as JSON",
    )
    parser.add_argument(
        "--baseline",
        type=Path,
        help="compare against a full report or compact contract",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="return a non-zero exit code when compatibility errors are found",
    )
    args = parser.parse_args()

    compiled = args.compiled.resolve()
    if not compiled.is_dir():
        print(f"compiled directory not found: {compiled}", file=sys.stderr)
        return 2
    if not any(compiled.glob("*.material.bin")):
        print(f"no .material.bin files in: {compiled}", file=sys.stderr)
        return 2

    errors: list[str] = []
    report = build_report(compiled, errors)
    contract = build_contract(report)
    print_summary(report)

    if args.contract_out:
        output = args.contract_out
        if not output.is_absolute():
            output = Path.cwd() / output
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(
            json.dumps(contract, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
        print(f"wrote contract {output}")

    if args.baseline:
        baseline_path = args.baseline
        if not baseline_path.is_absolute():
            baseline_path = Path.cwd() / baseline_path
        try:
            baseline_data = json.loads(baseline_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            print(f"cannot read baseline {baseline_path}: {error}", file=sys.stderr)
            return 2
        if not isinstance(baseline_data, dict):
            print(f"baseline root must be an object: {baseline_path}", file=sys.stderr)
            return 2
        expected = (
            build_contract(baseline_data)
            if "materials" in baseline_data and isinstance(baseline_data["materials"], list)
            else baseline_data
        )
        differences = compare_contract(expected, contract)
        for difference in differences:
            print("DIFF " + difference)
        if differences:
            errors.append(f"compatibility contract differs in {len(differences)} place(s)")

    if args.json_out:
        output = args.json_out
        if not output.is_absolute():
            output = Path.cwd() / output
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(
            json.dumps(report, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
        print(f"wrote {output}")

    return 1 if args.check and errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
