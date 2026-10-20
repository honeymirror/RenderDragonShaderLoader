"""Inventory and validate explicit native RenderDragon IO mappings.

This tool does not rewrite materials yet. It establishes the strict mapping
contract and proves that a proposed mapping resolves the candidate issues while
keeping the result compatible with the engine contract.

Usage:
  py -3 tools/native_io_map.py inventory `
    --candidate <dir-or-contract> --engine <contract> [--json-out report.json]

  py -3 tools/native_io_map.py validate `
    --mapping rdloader.io-mappings.json `
    --candidate <dir-or-contract> --engine <contract> [--check]
"""

from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path
import re
import shutil
import sys


ROOT = Path(__file__).resolve().parents[1]
TOOL_DIR = Path(__file__).resolve().parent
COMPAT_SRC = ROOT / "toolchain" / "lazurite-albite-compat" / "src"
sys.path.insert(0, str(TOOL_DIR))
sys.path.insert(0, str(COMPAT_SRC))

import native_io_validate  # noqa: E402
from lazurite import util  # noqa: E402
from lazurite.material import Material  # noqa: E402


MAPPING_VERSION = 1
TARGET_PROFILE = "RenderDragon-26.51"
SECTION_NAMES = ("attributes", "buffers", "passes")


def load_mapping(path: Path) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError(f"mapping root must be an object: {path}")
    if data.get("version") != MAPPING_VERSION:
        raise ValueError(f"mapping version must be {MAPPING_VERSION}")
    target = data.get("target")
    if target and target != TARGET_PROFILE:
        raise ValueError(f"mapping target {target!r} is not {TARGET_PROFILE}")
    for section in SECTION_NAMES:
        values = data.get(section, [])
        if not isinstance(values, list):
            raise ValueError(f"mapping section '{section}' must be an array")
    return data


def _required_text(item: dict, field: str, label: str) -> str:
    value = item.get(field)
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"{label}.{field} must be a non-empty string")
    return value


def validate_mapping_document(mapping: dict) -> None:
    seen_ids: set[str] = set()
    seen_sources: set[tuple] = set()
    for section in SECTION_NAMES:
        for index, item in enumerate(mapping.get(section, [])):
            label = f"{section}[{index}]"
            if not isinstance(item, dict):
                raise ValueError(f"{label} must be an object")
            item_id = _required_text(item, "id", label)
            material = _required_text(item, "material", label)
            _required_text(item, "reason", label)
            if item_id in seen_ids:
                raise ValueError(f"{label}.id is duplicated: {item_id}")
            seen_ids.add(item_id)

            if section == "attributes":
                pass_name = _required_text(item, "pass", label)
                source = _required_text(item, "sourceSemantic", label)
                target = _required_text(item, "targetSemantic", label)
                source_key = (section, material, pass_name, source)
            elif section == "buffers":
                source = _required_text(item, "sourceBuffer", label)
                target = _required_text(item, "targetBuffer", label)
                source_key = (section, material, source)
            else:
                pass_name = _required_text(item, "pass", label)
                targets = item.get("targetRenderTargets")
                if not isinstance(targets, list) or not targets:
                    raise ValueError(f"{label}.targetRenderTargets must be a non-empty array")
                for target in targets:
                    if not isinstance(target, str) or not target.startswith("Color"):
                        raise ValueError(f"{label}.targetRenderTargets contains an invalid target")
                source = pass_name
                target = ",".join(targets)
                source_key = (section, material, pass_name)

            if source == target:
                raise ValueError(f"{label} maps a value to itself")
            if source_key in seen_sources:
                raise ValueError(f"{label} duplicates a mapping source")
            seen_sources.add(source_key)


def _issue(code: str, material: str, message: str, pass_name: str = "", buffer: str = "") -> dict:
    return {
        "code": code,
        "material": material,
        "pass": pass_name,
        "buffer": buffer,
        "message": message,
    }


def _target_signature(targets: list[str]) -> tuple[str, list[str]]:
    indices = []
    errors = []
    for target in targets:
        if not target.startswith("Color") or not target[5:].isdigit():
            errors.append(f"invalid render target {target!r}")
            continue
        index = int(target[5:])
        if index > 3:
            errors.append(f"render target {target} is outside verified Color0..Color3")
            continue
        indices.append(index)
    if errors:
        return "", errors
    names = [f"Color{index}" for index in sorted(set(indices))]
    signature = util.get_output_binding_signature(names)
    return f"0x{signature:08X}", []


def validate_mapping(
    mapping: dict,
    candidate: dict,
    engine: dict,
    catalog: dict | None = None,
) -> dict:
    validate_mapping_document(mapping)
    if catalog is not None:
        validate_catalog_targets(mapping, catalog)
    projected = copy.deepcopy(candidate)
    errors: list[dict] = []
    applied: list[dict] = []
    candidate_materials = candidate.get("materials", {})
    engine_materials = engine.get("materials", {})

    for index, item in enumerate(mapping.get("attributes", [])):
        material_name = item["material"]
        pass_name = item["pass"]
        source = item["sourceSemantic"]
        target = item["targetSemantic"]
        candidate_material = candidate_materials.get(material_name)
        engine_material = engine_materials.get(material_name)
        if candidate_material is None or engine_material is None:
            errors.append(_issue("mapping.material-missing", material_name, "mapping material is missing", pass_name))
            continue
        candidate_pass = candidate_material.get("passes", {}).get(pass_name)
        engine_pass = engine_material.get("passes", {}).get(pass_name)
        if candidate_pass is None or engine_pass is None:
            errors.append(_issue("mapping.pass-missing", material_name, "mapping pass is missing", pass_name))
            continue
        if source not in candidate_pass.get("attributes", []):
            errors.append(_issue("mapping.unused", material_name, f"source semantic {source} is not present", pass_name))
            continue
        if target not in engine_pass.get("attributes", []):
            errors.append(_issue("mapping.target-unavailable", material_name, f"engine pass does not expose {target}", pass_name))
            continue
        if target in candidate_pass.get("attributes", []):
            errors.append(_issue("mapping.ambiguous", material_name, f"candidate pass already exposes {target}", pass_name))
            continue
        source_types = candidate_material.get("attributes", {}).get(source, [])
        target_types = engine_material.get("attributes", {}).get(target, [])
        incompatible = [
            value
            for value in source_types
            if not any(native_io_validate.input_type_compatible(value, target_type) for target_type in target_types)
        ]
        if incompatible:
            errors.append(
                _issue(
                    "mapping.type-incompatible",
                    material_name,
                    f"{source} type(s) {incompatible} cannot map to {target} {target_types}",
                    pass_name,
                )
            )
            continue

        projected_material = projected["materials"][material_name]
        projected_pass = projected_material["passes"][pass_name]
        projected_pass["attributes"] = [
            value for value in projected_pass.get("attributes", []) if value != source
        ]
        projected_pass["attributes"].append(target)
        projected_material.setdefault("attributes", {}).pop(source, None)
        projected_material["attributes"][target] = list(target_types)
        applied.append({"section": "attributes", "id": item["id"], "source": source, "target": target})

    for index, item in enumerate(mapping.get("buffers", [])):
        material_name = item["material"]
        source = item["sourceBuffer"]
        target = item["targetBuffer"]
        candidate_material = candidate_materials.get(material_name)
        engine_material = engine_materials.get(material_name)
        if candidate_material is None or engine_material is None:
            errors.append(_issue("mapping.material-missing", material_name, "mapping material is missing", buffer=source))
            continue
        source_buffer = candidate_material.get("buffers", {}).get(source)
        target_buffer = engine_material.get("buffers", {}).get(target)
        if source_buffer is None:
            errors.append(_issue("mapping.unused", material_name, f"source buffer {source} is not present", buffer=source))
            continue
        if target_buffer is None:
            errors.append(_issue("mapping.target-unavailable", material_name, f"engine material does not expose {target}", buffer=source))
            continue
        for field in ("type", "format", "access", "unorderedAccess"):
            if source_buffer.get(field) != target_buffer.get(field):
                errors.append(
                    _issue(
                        "mapping.buffer-field-mismatch",
                        material_name,
                        f"{source}.{field}={source_buffer.get(field)!r} cannot alias {target}.{field}={target_buffer.get(field)!r}",
                        buffer=source,
                    )
                )
                break
        else:
            projected_material = projected["materials"][material_name]
            projected_material["buffers"].pop(source, None)
            projected_material["buffers"][target] = copy.deepcopy(target_buffer)
            applied.append({"section": "buffers", "id": item["id"], "source": source, "target": target})

    for index, item in enumerate(mapping.get("passes", [])):
        material_name = item["material"]
        pass_name = item["pass"]
        targets = item["targetRenderTargets"]
        candidate_material = candidate_materials.get(material_name)
        engine_material = engine_materials.get(material_name)
        if candidate_material is None or engine_material is None:
            errors.append(_issue("mapping.material-missing", material_name, "mapping material is missing", pass_name))
            continue
        candidate_pass = candidate_material.get("passes", {}).get(pass_name)
        engine_pass = engine_material.get("passes", {}).get(pass_name)
        if candidate_pass is None or engine_pass is None:
            errors.append(_issue("mapping.pass-missing", material_name, "mapping pass is missing", pass_name))
            continue
        target_signature, signature_errors = _target_signature(targets)
        if signature_errors:
            errors.extend(
                _issue("mapping.invalid-target", material_name, value, pass_name)
                for value in signature_errors
            )
            continue
        if target_signature != engine_pass.get("outputBindingSignature"):
            errors.append(
                _issue(
                    "mapping.signature-not-engine",
                    material_name,
                    f"{target_signature} does not match engine {engine_pass.get('outputBindingSignature')}",
                    pass_name,
                )
            )
            continue
        if candidate_pass.get("outputBindingSignature") == target_signature:
            errors.append(_issue("mapping.unused", material_name, "candidate already has the target signature", pass_name))
            continue
        projected["materials"][material_name]["passes"][pass_name]["outputBindingSignature"] = target_signature
        projected["materials"][material_name]["passes"][pass_name]["renderTargets"] = targets
        applied.append({"section": "passes", "id": item["id"], "source": pass_name, "target": target_signature})

    result = native_io_validate.validate(projected, engine)
    entry_count = sum(len(mapping.get(section, [])) for section in SECTION_NAMES)
    summary = result["summary"]
    return {
        "schemaVersion": 1,
        "mappingVersion": mapping.get("version"),
        "target": mapping.get("target", TARGET_PROFILE),
        "summary": {
            "mappingEntries": entry_count,
            "applied": len(applied),
            "mappingErrors": len(errors),
            "remainingErrors": summary["errors"],
            "remainingWarnings": summary["warnings"],
        },
        "applied": applied,
        "errors": errors,
        "remainingIssues": result["issues"],
    }


def inventory(candidate: dict, engine: dict) -> dict:
    result = native_io_validate.validate(candidate, engine)
    by_section = {"attributes": 0, "buffers": 0, "passes": 0, "other": 0}
    for item in result["issues"]:
        code = item["code"]
        if code.startswith("attribute."):
            by_section["attributes"] += 1
        elif code.startswith("buffer."):
            by_section["buffers"] += 1
        elif code.startswith("pass."):
            by_section["passes"] += 1
        else:
            by_section["other"] += 1
    summary = dict(result["summary"])
    summary["issuesBySection"] = by_section
    return {
        "schemaVersion": 1,
        "target": TARGET_PROFILE,
        "summary": summary,
        "issues": result["issues"],
    }


def build_mapping_catalog(engine: dict) -> dict:
    attributes: dict[str, dict] = {}
    buffers: dict[str, dict] = {}
    render_targets: dict[str, dict] = {}
    materials = engine.get("materials", {})

    for material_name, material in sorted(materials.items()):
        for semantic, types in material.get("attributes", {}).items():
            item = attributes.setdefault(
                semantic,
                {"types": set(), "materials": set(), "passes": 0},
            )
            item["types"].update(types)
            item["materials"].add(material_name)
            for pass_name, pass_data in material.get("passes", {}).items():
                if semantic in pass_data.get("attributes", []):
                    item["passes"] += 1

        for name, buffer in material.get("buffers", {}).items():
            item = buffers.setdefault(
                name,
                {
                    "bindingSlots": set(),
                    "types": set(),
                    "formats": set(),
                    "accessModes": set(),
                    "unorderedAccess": set(),
                    "materials": set(),
                },
            )
            item["bindingSlots"].add(buffer.get("bindingSlot"))
            item["types"].add(buffer.get("type"))
            item["formats"].add(buffer.get("format"))
            item["accessModes"].add(buffer.get("access"))
            item["unorderedAccess"].add(bool(buffer.get("unorderedAccess", False)))
            item["materials"].add(material_name)

        for pass_name, pass_data in material.get("passes", {}).items():
            signature = pass_data.get("outputBindingSignature")
            for target in pass_data.get("renderTargets", []):
                item = render_targets.setdefault(
                    target,
                    {"signatures": set(), "materials": set(), "passes": 0},
                )
                item["signatures"].add(signature)
                item["materials"].add(material_name)
                item["passes"] += 1

    def clean(value):
        if isinstance(value, set):
            return sorted(value, key=lambda entry: str(entry))
        if isinstance(value, dict):
            return {key: clean(entry) for key, entry in value.items()}
        return value

    return {
        "catalogVersion": 1,
        "target": TARGET_PROFILE,
        "source": "RenderDragon-26.51.vanilla.material-io.json",
        "summary": {
            "materials": len(materials),
            "attributes": len(attributes),
            "buffers": len(buffers),
            "renderTargets": len(render_targets),
        },
        "attributes": {
            name: {
                "types": clean(item["types"]),
                "materialCount": len(item["materials"]),
                "passCount": item["passes"],
            }
            for name, item in sorted(attributes.items())
        },
        "buffers": {
            name: {
                "bindingSlots": clean(item["bindingSlots"]),
                "types": clean(item["types"]),
                "formats": clean(item["formats"]),
                "accessModes": clean(item["accessModes"]),
                "unorderedAccess": clean(item["unorderedAccess"]),
                "materialCount": len(item["materials"]),
            }
            for name, item in sorted(buffers.items())
        },
        "renderTargets": {
            name: {
                "signatures": clean(item["signatures"]),
                "materialCount": len(item["materials"]),
                "passCount": item["passes"],
            }
            for name, item in sorted(render_targets.items())
        },
        "verifiedAliases": [],
    }


def validate_catalog_targets(mapping: dict, catalog: dict) -> None:
    attributes = catalog.get("attributes", {})
    buffers = catalog.get("buffers", {})
    render_targets = catalog.get("renderTargets", {})
    errors = []
    for item in mapping.get("attributes", []):
        if item["targetSemantic"] not in attributes:
            errors.append(
                f"{item['id']}: target attribute {item['targetSemantic']} is not in catalog"
            )
    for item in mapping.get("buffers", []):
        if item["targetBuffer"] not in buffers:
            errors.append(
                f"{item['id']}: target buffer {item['targetBuffer']} is not in catalog"
            )
    for item in mapping.get("passes", []):
        for target in item["targetRenderTargets"]:
            if target not in render_targets:
                errors.append(f"{item['id']}: target {target} is not in catalog")
    if errors:
        raise ValueError("catalog target validation failed: " + "; ".join(errors))


def apply_binary_mapping(
    mapping: dict,
    materials_dir: Path,
    engine_materials_dir: Path,
    output_dir: Path,
    engine: dict,
    attributes_applied: bool = False,
    catalog: dict | None = None,
) -> dict:
    present_materials = {
        path.name.removesuffix(".material.bin")
        for path in materials_dir.glob("*.material.bin")
    }
    attribute_entries = [] if attributes_applied else [
        item
        for item in mapping.get("attributes", [])
        if item.get("material") in present_materials
    ]
    effective_mapping = {
        "version": mapping.get("version"),
        "target": mapping.get("target", TARGET_PROFILE),
        **{
            "attributes": attribute_entries,
            **{
                section: [
                    item
                    for item in mapping.get(section, [])
                    if item.get("material") in present_materials
                ]
                for section in ("buffers", "passes")
            },
        },
    }
    validate_mapping_document(effective_mapping)
    if catalog is not None:
        validate_catalog_targets(effective_mapping, catalog)
    if effective_mapping.get("attributes") and not attributes_applied:
        raise ValueError(
            "compiled attribute rewriting is not enabled yet; apply attribute mappings in the source stage"
        )
    if materials_dir.resolve() == output_dir.resolve():
        raise ValueError("mapping output must be a separate directory")

    candidate = native_io_validate.load_contract(materials_dir.resolve())
    preview = validate_mapping(effective_mapping, candidate, engine, catalog)
    if preview["errors"] or preview["summary"]["remainingErrors"]:
        details = [f"{item['code']}: {item['message']}" for item in preview["errors"]]
        details.extend(
            f"{item['code']}: {item['message']}" for item in preview["remainingIssues"]
        )
        raise ValueError("mapping preflight failed: " + "; ".join(details))

    by_material: dict[str, dict[str, list[dict]]] = {}
    for section in ("buffers", "passes"):
        for item in effective_mapping.get(section, []):
            by_material.setdefault(item["material"], {}).setdefault(section, []).append(item)

    shutil.rmtree(output_dir, ignore_errors=True)
    output_dir.mkdir(parents=True, exist_ok=True)
    applied = []
    try:
        for source_file in sorted(materials_dir.glob("*.material.bin")):
            material_name = source_file.name.removesuffix(".material.bin")
            target_file = output_dir / source_file.name
            entries = by_material.get(material_name)
            if not entries:
                shutil.copy2(source_file, target_file)
                continue

            material = Material.load_bin_file(source_file)
            engine_file = engine_materials_dir / source_file.name
            engine_material = Material.load_bin_file(engine_file)
            if material.name != material_name:
                raise ValueError(
                    f"{source_file.name}: material name {material.name!r} does not match filename"
                )

            for item in entries.get("buffers", []):
                source_buffer = next(
                    (buffer for buffer in material.buffers if buffer.name == item["sourceBuffer"]),
                    None,
                )
                target_buffer = next(
                    (buffer for buffer in engine_material.buffers if buffer.name == item["targetBuffer"]),
                    None,
                )
                if source_buffer is None or target_buffer is None:
                    raise ValueError(f"{material_name}: buffer disappeared during apply")
                if (
                    source_buffer.register_slot != target_buffer.register_slot
                    or source_buffer.binding_slot != target_buffer.binding_slot
                ):
                    raise ValueError(
                        f"{material_name}/{source_buffer.name}: register/binding slot changes are not enabled yet"
                    )
                source_name = source_buffer.name
                source_buffer.name = target_buffer.name
                applied.append(
                    {
                        "section": "buffers",
                        "id": item["id"],
                        "material": material_name,
                        "source": source_name,
                        "target": target_buffer.name,
                        "reason": item["reason"],
                    }
                )

            for item in entries.get("passes", []):
                shader_pass = next(
                    (value for value in material.passes if value.name == item["pass"]),
                    None,
                )
                if shader_pass is None:
                    raise ValueError(f"{material_name}/{item['pass']}: pass disappeared during apply")
                signature, signature_errors = _target_signature(item["targetRenderTargets"])
                if signature_errors:
                    raise ValueError("; ".join(signature_errors))
                shader_pass.output_binding_signature = int(signature, 16)
                applied.append(
                    {
                        "section": "passes",
                        "id": item["id"],
                        "material": material_name,
                        "source": shader_pass.name,
                        "target": signature,
                        "reason": item["reason"],
                    }
                )

            from io import BytesIO

            data = BytesIO()
            material.write(data)
            target_file.write_bytes(data.getvalue())

        for source_file in materials_dir.iterdir():
            if not source_file.is_file() or source_file.name.endswith(".material.bin"):
                continue
            shutil.copy2(source_file, output_dir / source_file.name)

        applied_contract = native_io_validate.load_contract(output_dir.resolve())
        validation = native_io_validate.validate(applied_contract, engine)
        if validation["summary"]["errors"]:
            raise ValueError(
                "mapped output still has "
                f"{validation['summary']['errors']} engine error(s)"
            )
    except Exception:
        shutil.rmtree(output_dir, ignore_errors=True)
        raise

    return {
        "schemaVersion": 1,
        "summary": {
            "mappingEntries": sum(
                len(effective_mapping.get(section, []))
                for section in ("buffers", "passes")
            ),
            "applied": len(applied),
            "remainingErrors": validation["summary"]["errors"],
            "remainingWarnings": validation["summary"]["warnings"],
        },
        "applied": applied,
        "remainingIssues": validation["issues"],
        "output": str(output_dir.resolve()),
    }


IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def _token_pattern(value: str) -> re.Pattern:
    return re.compile(rf"(?<![A-Za-z0-9_]){re.escape(value)}(?![A-Za-z0-9_])")


def _insert_after_version(text: str, lines: list[str], marker: str) -> str:
    if marker in text:
        return text
    insertion = marker + "\n" + "\n".join(lines) + "\n"
    match = re.search(r"^\s*#version[^\n]*(?:\n|$)", text, flags=re.MULTILINE)
    if match is None:
        return insertion + text
    position = match.end()
    return text[:position] + insertion + text[position:]


def _source_uses_token(material_dir: Path, source_dir: Path, token: str) -> bool:
    pattern = _token_pattern(token)
    roots = [material_dir]
    include_dir = source_dir / "include"
    if include_dir.is_dir():
        roots.append(include_dir)
    suffixes = {".vert", ".vsh", ".frag", ".fsh", ".comp", ".glsl", ".h", ".inc"}
    for root in roots:
        for path in root.rglob("*"):
            if not path.is_file() or path.suffix.lower() not in suffixes:
                continue
            try:
                if pattern.search(path.read_text(encoding="utf-8")):
                    return True
            except (OSError, UnicodeDecodeError):
                continue
    return False


def _rewrite_varying_file(
    path: Path,
    source_name: str,
    source_semantic: str,
    target_name: str,
    target_semantic: str,
) -> tuple[str, int]:
    text = path.read_text(encoding="utf-8")
    pattern = re.compile(
        r"^(\s*(?:(?:flat|noperspective|centroid|sample)\s+)*"
        r"[A-Za-z_][A-Za-z0-9_]*\s+)"
        + re.escape(source_name)
        + r"(\s*:\s*)"
        + re.escape(source_semantic)
        + r"(\s*;.*)$",
        flags=re.MULTILINE,
    )
    rewritten, count = pattern.subn(
        lambda match: (
            match.group(1)
            + target_name
            + match.group(2)
            + target_semantic
            + match.group(3)
        ),
        text,
    )
    return rewritten, count


def apply_source_attributes(
    mapping: dict,
    source_dir: Path,
    engine: dict,
    catalog: dict | None = None,
) -> dict:
    validate_mapping_document(mapping)
    if catalog is not None:
        validate_catalog_targets(mapping, catalog)
    planned: dict[Path, str] = {}
    applied = []
    errors = []
    engine_materials = engine.get("materials", {})

    for item in mapping.get("attributes", []):
        material = item["material"]
        pass_name = item["pass"]
        source_semantic = item["sourceSemantic"]
        target_semantic = item["targetSemantic"]
        source_name = item.get("sourceName")
        target_name = item.get("targetName")
        label = f"{material}/{pass_name} {source_name}->{target_name}"
        if not isinstance(source_name, str) or not IDENTIFIER.fullmatch(source_name):
            errors.append(f"{label}: sourceName must be a valid identifier")
            continue
        if not isinstance(target_name, str) or not IDENTIFIER.fullmatch(target_name):
            errors.append(f"{label}: targetName must be a valid identifier")
            continue
        engine_material = engine_materials.get(material)
        engine_pass = (
            engine_material.get("passes", {}).get(pass_name)
            if engine_material is not None
            else None
        )
        if engine_pass is None or target_semantic not in engine_pass.get("attributes", []):
            errors.append(f"{label}: engine pass does not expose {target_semantic}")
            continue

        material_dir = source_dir / "materials" / material
        if not material_dir.is_dir():
            errors.append(f"{label}: material source directory is missing")
            continue
        varying_file = material_dir / "varying.def.sc"
        if varying_file.is_file():
            rewritten, count = _rewrite_varying_file(
                varying_file,
                source_name,
                source_semantic,
                target_name,
                target_semantic,
            )
            if count == 0:
                errors.append(f"{label}: source declaration is not present in varying.def.sc")
                continue
            planned[varying_file] = rewritten
            for shader_file in material_dir.rglob("*.sc"):
                if shader_file == varying_file or not shader_file.is_file():
                    continue
                original = planned.get(shader_file, shader_file.read_text(encoding="utf-8"))
                planned[shader_file] = _token_pattern(source_name).sub(target_name, original)
            applied.append(
                {
                    "section": "attributes",
                    "id": item["id"],
                    "material": material,
                    "pass": pass_name,
                    "mode": "bgfx-varyings",
                    "sourceSemantic": source_semantic,
                    "targetSemantic": target_semantic,
                    "sourceName": source_name,
                    "targetName": target_name,
                    "reason": item["reason"],
                }
            )
            continue

        vertex_files = [
            path
            for path in material_dir.rglob("*")
            if path.is_file() and path.suffix.lower() in {".vert", ".vsh"}
        ]
        if not vertex_files:
            errors.append(f"{label}: no vertex shader source found for albite mapping")
            continue
        if not _source_uses_token(material_dir, source_dir, source_name):
            errors.append(f"{label}: sourceName is not referenced by material or include sources")
            continue
        marker = f"// RDL native IO mapping: {item['id']}"
        define_line = f"#define {source_name} {target_name}"
        for vertex_file in vertex_files:
            original = planned.get(vertex_file, vertex_file.read_text(encoding="utf-8"))
            planned[vertex_file] = _insert_after_version(
                original,
                [define_line],
                marker,
            )
        applied.append(
            {
                "section": "attributes",
                "id": item["id"],
                "material": material,
                "pass": pass_name,
                "mode": "albite-define",
                "sourceSemantic": source_semantic,
                "targetSemantic": target_semantic,
                "sourceName": source_name,
                "targetName": target_name,
                "reason": item["reason"],
            }
        )

    if errors:
        raise ValueError("source attribute mapping failed: " + "; ".join(errors))
    for path, text in planned.items():
        temporary = path.with_name(path.name + ".rdl-map.tmp")
        temporary.write_text(text, encoding="utf-8")
        temporary.replace(path)

    return {
        "schemaVersion": 1,
        "summary": {
            "attributeEntries": len(mapping.get("attributes", [])),
            "applied": len(applied),
            "deferredBinaryEntries": sum(
                len(mapping.get(section, [])) for section in ("buffers", "passes")
            ),
            "filesChanged": len(planned),
            "errors": 0,
        },
        "applied": applied,
    }


def write_json(path: Path | None, value: dict) -> None:
    if path is None:
        return
    if not path.is_absolute():
        path = Path.cwd() / path
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"wrote {path}")


def load_contracts(candidate_path: Path, engine_path: Path) -> tuple[dict, dict]:
    return (
        native_io_validate.load_contract(candidate_path.resolve()),
        native_io_validate.load_contract(engine_path.resolve()),
    )


def command_inventory(args) -> int:
    candidate, engine = load_contracts(args.candidate, args.engine)
    report = inventory(candidate, engine)
    summary = report["summary"]
    print(
        f"candidate={summary['candidateMaterials']} engine={summary['engineMaterials']} "
        f"issues={len(report['issues'])} errors={summary['errors']} warnings={summary['warnings']}"
    )
    for item in report["issues"]:
        print(f"{item['kind'].upper()} [{item['code']}] {item['material']}: {item['message']}")
    write_json(args.json_out, report)
    failed = summary["errors"] > 0 or (args.strict and summary["warnings"] > 0)
    return 1 if args.check and failed else 0


def command_validate(args) -> int:
    mapping = load_mapping(args.mapping.resolve())
    candidate, engine = load_contracts(args.candidate, args.engine)
    catalog = (
        json.loads(args.catalog.read_text(encoding="utf-8"))
        if args.catalog
        else None
    )
    report = validate_mapping(mapping, candidate, engine, catalog)
    summary = report["summary"]
    print(
        f"mapping={summary['mappingEntries']} applied={summary['applied']} "
        f"mappingErrors={summary['mappingErrors']} remainingErrors={summary['remainingErrors']} "
        f"remainingWarnings={summary['remainingWarnings']}"
    )
    for item in report["errors"]:
        print(f"ERROR [{item['code']}] {item['material']}: {item['message']}")
    for item in report["remainingIssues"]:
        print(f"REMAINING [{item['code']}] {item['material']}: {item['message']}")
    write_json(args.json_out, report)
    failed = summary["mappingErrors"] > 0 or summary["remainingErrors"] > 0
    return 1 if args.check and failed else 0


def command_apply(args) -> int:
    mapping = load_mapping(args.mapping.resolve())
    engine = native_io_validate.load_contract(args.engine.resolve())
    catalog = (
        json.loads(args.catalog.read_text(encoding="utf-8"))
        if args.catalog
        else None
    )
    report = apply_binary_mapping(
        mapping,
        args.materials.resolve(),
        args.engine_materials.resolve(),
        args.output.resolve(),
        engine,
        attributes_applied=args.attributes_applied,
        catalog=catalog,
    )
    summary = report["summary"]
    print(
        f"mapping={summary['mappingEntries']} applied={summary['applied']} "
        f"remainingErrors={summary['remainingErrors']} remainingWarnings={summary['remainingWarnings']}"
    )
    write_json(args.json_out, report)
    return 0


def command_apply_source(args) -> int:
    mapping = load_mapping(args.mapping.resolve())
    engine = native_io_validate.load_contract(args.engine.resolve())
    catalog = (
        json.loads(args.catalog.read_text(encoding="utf-8"))
        if args.catalog
        else None
    )
    report = apply_source_attributes(mapping, args.source.resolve(), engine, catalog)
    summary = report["summary"]
    print(
        f"attributes={summary['attributeEntries']} applied={summary['applied']} "
        f"files={summary['filesChanged']} deferred={summary['deferredBinaryEntries']}"
    )
    write_json(args.json_out, report)
    return 0


def command_catalog(args) -> int:
    engine = native_io_validate.load_contract(args.engine.resolve())
    report = build_mapping_catalog(engine)
    summary = report["summary"]
    print(
        f"materials={summary['materials']} attributes={summary['attributes']} "
        f"buffers={summary['buffers']} targets={summary['renderTargets']}"
    )
    write_json(args.json_out, report)
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Inventory and validate native IO mappings.")
    subparsers = parser.add_subparsers(dest="command", required=True)

    inventory_parser = subparsers.add_parser("inventory")
    inventory_parser.add_argument("--candidate", type=Path, required=True)
    inventory_parser.add_argument("--engine", type=Path, required=True)
    inventory_parser.add_argument("--json-out", type=Path)
    inventory_parser.add_argument("--check", action="store_true")
    inventory_parser.add_argument("--strict", action="store_true")
    inventory_parser.set_defaults(func=command_inventory)

    validate_parser = subparsers.add_parser("validate")
    validate_parser.add_argument("--mapping", type=Path, required=True)
    validate_parser.add_argument("--candidate", type=Path, required=True)
    validate_parser.add_argument("--engine", type=Path, required=True)
    validate_parser.add_argument("--catalog", type=Path)
    validate_parser.add_argument("--json-out", type=Path)
    validate_parser.add_argument("--check", action="store_true")
    validate_parser.set_defaults(func=command_validate)

    apply_parser = subparsers.add_parser("apply")
    apply_parser.add_argument("--mapping", type=Path, required=True)
    apply_parser.add_argument("--materials", type=Path, required=True)
    apply_parser.add_argument("--engine", type=Path, required=True)
    apply_parser.add_argument("--catalog", type=Path)
    apply_parser.add_argument("--engine-materials", type=Path, required=True)
    apply_parser.add_argument("--output", type=Path, required=True)
    apply_parser.add_argument("--json-out", type=Path)
    apply_parser.add_argument("--attributes-applied", action="store_true")
    apply_parser.set_defaults(func=command_apply)

    source_parser = subparsers.add_parser("apply-source")
    source_parser.add_argument("--mapping", type=Path, required=True)
    source_parser.add_argument("--source", type=Path, required=True)
    source_parser.add_argument("--engine", type=Path, required=True)
    source_parser.add_argument("--catalog", type=Path)
    source_parser.add_argument("--json-out", type=Path)
    source_parser.set_defaults(func=command_apply_source)

    catalog_parser = subparsers.add_parser("catalog")
    catalog_parser.add_argument("--engine", type=Path, required=True)
    catalog_parser.add_argument("--json-out", type=Path, required=True)
    catalog_parser.set_defaults(func=command_catalog)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        return args.func(args)
    except (OSError, json.JSONDecodeError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
