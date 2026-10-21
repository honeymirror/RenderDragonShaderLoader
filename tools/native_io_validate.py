"""Validate a native RenderDragon pack against the target engine material contract."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys


TOOL_DIR = Path(__file__).resolve().parent
ROOT = TOOL_DIR.parent
sys.path.insert(0, str(TOOL_DIR))

MATERIAL_IO_IMPORT_ERROR: Exception | None = None
try:
    import material_io_dump  # noqa: E402
except Exception as error:  # ImportError or a native dependency load failure
    material_io_dump = None
    MATERIAL_IO_IMPORT_ERROR = error


KNOWN_ISSUE_CODES = {
    "attribute.semantic-not-provided",
    "attribute.type-mismatch",
    "buffer.access-mismatch",
    "buffer.format-mismatch",
    "buffer.invalid-slot",
    "buffer.name-slot-mismatch",
    "buffer.slot-not-provided",
    "buffer.type-mismatch",
    "material.candidate-only",
    "pass.candidate-only",
    "pass.engine-pass-missing",
    "pass.output-signature-mismatch",
}


def load_contract(path: Path) -> dict:
    if material_io_dump is None:
        raise RuntimeError(f"material IO parser is unavailable: {MATERIAL_IO_IMPORT_ERROR}")
    if path.is_dir():
        errors: list[str] = []
        report = material_io_dump.build_report(path.resolve(), errors)
        if errors:
            raise ValueError("; ".join(errors))
        return material_io_dump.build_contract(report)

    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError(f"JSON root must be an object: {path}")
    if data.get("contractVersion") == 1:
        return data
    if isinstance(data.get("materials"), list):
        return material_io_dump.build_contract(data)
    raise ValueError(f"unsupported material IO contract: {path}")


def issue(
    kind: str,
    code: str,
    material: str,
    message: str,
    pass_name: str = "",
    buffer: str = "",
) -> dict:
    return {
        "kind": kind,
        "code": code,
        "material": material,
        "pass": pass_name,
        "buffer": buffer,
        "message": message,
    }


def compare_attributes(
    material_name: str,
    pass_name: str,
    candidate: dict,
    engine: dict,
) -> list[dict]:
    issues = []
    engine_attributes = engine.get("attributes", {})
    for semantic in candidate.get("attributes", []):
        if semantic not in engine_attributes:
            issues.append(
                issue(
                    "error",
                    "attribute.semantic-not-provided",
                    material_name,
                    f"engine pass does not provide vertex semantic {semantic}",
                    pass_name,
                )
            )
    candidate_types = candidate.get("attributeTypes", {})
    for semantic, types in candidate_types.items():
        engine_types = set(engine_attributes.get(semantic, []))
        unsupported = sorted(
            candidate_type
            for candidate_type in types
            if not any(
                input_type_compatible(candidate_type, engine_type)
                for engine_type in engine_types
            )
        )
        if engine_types and unsupported:
            issues.append(
                issue(
                    "error",
                    "attribute.type-mismatch",
                    material_name,
                    f"{semantic} uses {unsupported}, engine provides {sorted(engine_types)}",
                    pass_name,
                )
            )
    return issues


def input_type_compatible(candidate: str, engine: str) -> bool:
    if candidate == engine:
        return True
    match_candidate = re.fullmatch(r"(i?vec|uvec)([234])", candidate)
    match_engine = re.fullmatch(r"(i?vec|uvec)([234])", engine)
    if not match_candidate or not match_engine:
        return False
    return (
        match_candidate.group(1) == match_engine.group(1)
        and int(match_candidate.group(2)) <= int(match_engine.group(2))
    )


def compare_buffers(material_name: str, candidate: dict, engine: dict) -> list[dict]:
    issues = []
    candidate_buffers = candidate.get("buffers", {})
    engine_buffers = engine.get("buffers", {})
    engine_by_slot = {
        item.get("bindingSlot"): (name, item)
        for name, item in engine_buffers.items()
        if isinstance(item.get("bindingSlot"), int)
    }

    for name, item in candidate_buffers.items():
        slot = item.get("bindingSlot")
        if not isinstance(slot, int):
            issues.append(
                issue(
                    "error",
                    "buffer.invalid-slot",
                    material_name,
                    f"buffer {name} has no valid binding slot",
                    buffer=name,
                )
            )
            continue
        occupied = engine_by_slot.get(slot)
        if occupied is None:
            issues.append(
                issue(
                    "error",
                    "buffer.slot-not-provided",
                    material_name,
                    f"buffer {name} uses slot {slot}, which the engine material does not expose",
                    buffer=name,
                )
            )
            continue
        engine_name, engine_item = occupied
        if name != engine_name:
            issues.append(
                issue(
                    "error",
                    "buffer.name-slot-mismatch",
                    material_name,
                    f"buffer {name} uses slot {slot}, occupied by engine buffer {engine_name}",
                    buffer=name,
                )
            )
            continue
        if item.get("type") != engine_item.get("type"):
            issues.append(
                issue(
                    "error",
                    "buffer.type-mismatch",
                    material_name,
                    f"buffer {name} type {item.get('type')} != engine {engine_item.get('type')}",
                    buffer=name,
                )
            )
        if item.get("format") != engine_item.get("format"):
            issues.append(
                issue(
                    "error",
                    "buffer.format-mismatch",
                    material_name,
                    f"buffer {name} format {item.get('format')!r} != engine {engine_item.get('format')!r}",
                    buffer=name,
                )
            )
        if item.get("unorderedAccess") != engine_item.get("unorderedAccess"):
            issues.append(
                issue(
                    "error",
                    "buffer.access-mismatch",
                    material_name,
                    f"buffer {name} unorderedAccess differs from engine",
                    buffer=name,
                )
            )

    return issues


def load_overrides(path: Path) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError(f"override root must be an object: {path}")
    if data.get("version") != 1:
        raise ValueError("override version must be 1")
    target = data.get("target")
    if target and target != "RenderDragon-26.51":
        raise ValueError(f"override target {target!r} is not RenderDragon-26.51")
    allow = data.get("allow")
    if not isinstance(allow, list):
        raise ValueError("override file requires an 'allow' array")

    seen = set()
    seen_ids = set()
    for index, item in enumerate(allow):
        label = f"allow[{index}]"
        if not isinstance(item, dict):
            raise ValueError(f"{label} must be an object")
        for field in ("id", "code", "material", "reason"):
            value = item.get(field)
            if not isinstance(value, str) or not value.strip():
                raise ValueError(f"{label}.{field} must be a non-empty string")
        code = item["code"]
        if (code.startswith("attribute.") or code.startswith("pass.")) and not item.get(
            "pass"
        ):
            raise ValueError(f"{label} requires pass for {code}")
        if code.startswith("buffer.") and not item.get("buffer"):
            raise ValueError(f"{label} requires buffer for {code}")
        key = (
            item["code"],
            item["material"],
            item.get("pass", ""),
            item.get("buffer", ""),
        )
        if key in seen:
            raise ValueError(f"{label} duplicates an existing override")
        seen.add(key)
        if item["id"] in seen_ids:
            raise ValueError(f"{label}.id is duplicated")
        seen_ids.add(item["id"])
        if code not in KNOWN_ISSUE_CODES:
            raise ValueError(f"{label}.code is not a known IO issue: {code}")
    return data


def override_matches(override: dict, item: dict) -> bool:
    if override["code"] != item["code"] or override["material"] != item["material"]:
        return False
    if override.get("pass") and override["pass"] != item.get("pass", ""):
        return False
    if override.get("buffer") and override["buffer"] != item.get("buffer", ""):
        return False
    return True


def apply_overrides(report: dict, overrides: dict | None) -> dict:
    allow = list((overrides or {}).get("allow", []))
    used = set()
    kept = []
    overridden = []
    for item in report["issues"]:
        match_index = next(
            (
                index
                for index, override in enumerate(allow)
                if override_matches(override, item)
            ),
            None,
        )
        if match_index is None:
            kept.append(item)
            continue
        used.add(match_index)
        override = allow[match_index]
        overridden_item = dict(item)
        overridden_item["overrideId"] = override["id"]
        overridden_item["reason"] = override["reason"]
        overridden.append(overridden_item)

    for index, override in enumerate(allow):
        if index in used:
            continue
        kept.append(
            issue(
                "error",
                "override.unused",
                override["material"],
                f"override {override['id']} did not match any issue",
                override.get("pass", ""),
                override.get("buffer", ""),
            )
        )

    report["issues"] = kept
    report["overriddenIssues"] = overridden
    report["summary"]["overridesApplied"] = len(overridden)
    report["summary"]["overridesUnused"] = len(allow) - len(used)
    report["summary"]["errors"] = sum(item["kind"] == "error" for item in kept)
    report["summary"]["warnings"] = sum(item["kind"] == "warning" for item in kept)
    return report


def validate(candidate: dict, engine: dict) -> dict:
    issues: list[dict] = []
    candidate_materials = candidate.get("materials", {})
    engine_materials = engine.get("materials", {})
    common = sorted(set(candidate_materials) & set(engine_materials))

    for material_name in sorted(set(candidate_materials) - set(engine_materials)):
        issues.append(
            issue(
                "warning",
                "material.candidate-only",
                material_name,
                "candidate material has no same-name engine material",
            )
        )

    for material_name in common:
        candidate_material = candidate_materials[material_name]
        engine_material = engine_materials[material_name]
        issues.extend(compare_buffers(material_name, candidate_material, engine_material))

        candidate_passes = candidate_material.get("passes", {})
        engine_passes = engine_material.get("passes", {})
        for pass_name in sorted(set(candidate_passes) - set(engine_passes)):
            issues.append(
                issue(
                    "warning",
                    "pass.candidate-only",
                    material_name,
                    "candidate pass has no same-name engine pass",
                    pass_name,
                )
            )
        for pass_name in sorted(set(engine_passes) - set(candidate_passes)):
            issues.append(
                issue(
                    "warning",
                    "pass.engine-pass-missing",
                    material_name,
                    "candidate omits an engine pass",
                    pass_name,
                )
            )
        for pass_name in sorted(set(candidate_passes) & set(engine_passes)):
            candidate_pass = candidate_passes[pass_name]
            engine_pass = engine_passes[pass_name]
            if candidate_pass.get("outputBindingSignature") != engine_pass.get(
                "outputBindingSignature"
            ):
                issues.append(
                    issue(
                        "error",
                        "pass.output-signature-mismatch",
                        material_name,
                        f"{candidate_pass.get('outputBindingSignature')} != "
                        f"{engine_pass.get('outputBindingSignature')}",
                        pass_name,
                    )
                )
            candidate_pass["attributeTypes"] = {
                semantic: candidate_material.get("attributes", {}).get(semantic, [])
                for semantic in candidate_pass.get("attributes", [])
            }
            issues.extend(
                compare_attributes(
                    material_name,
                    pass_name,
                    candidate_pass,
                    engine_material,
                )
            )

    errors = sum(item["kind"] == "error" for item in issues)
    warnings = sum(item["kind"] == "warning" for item in issues)
    return {
        "schemaVersion": 1,
        "summary": {
            "candidateMaterials": len(candidate_materials),
            "engineMaterials": len(engine_materials),
            "commonMaterials": len(common),
            "engineMaterialsUnused": len(set(engine_materials) - set(candidate_materials)),
            "errors": errors,
            "warnings": warnings,
        },
        "issues": issues,
    }


def load_failure_report(message: str, candidate: Path, engine: Path, overrides: Path | None) -> dict:
    report = {
        "schemaVersion": 1,
        "target": "RenderDragon-26.51",
        "candidatePath": str(candidate),
        "engineContractPath": str(engine),
        "summary": {
            "candidateMaterials": 0,
            "engineMaterials": 0,
            "commonMaterials": 0,
            "engineMaterialsUnused": 0,
            "errors": 1,
            "warnings": 0,
            "overridesApplied": 0,
            "overridesUnused": 0,
        },
        "issues": [
            issue(
                "error",
                "contract.load-failed",
                "<pack>",
                message,
            )
        ],
        "overriddenIssues": [],
    }
    if overrides is not None:
        report["overridesPath"] = str(overrides)
    return report


def write_json_report(report: dict, output: Path | None) -> None:
    if output is None:
        return
    if not output.is_absolute():
        output = Path.cwd() / output
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(f"wrote {output}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate native RenderDragon material IO against an engine contract."
    )
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--overrides", type=Path)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--strict", action="store_true", help="treat warnings as failures")
    args = parser.parse_args()

    candidate_path = args.candidate.resolve()
    engine_path = args.engine.resolve()
    overrides_path = args.overrides.resolve() if args.overrides else None
    if MATERIAL_IO_IMPORT_ERROR is not None:
        message = (
            f"cannot import material IO parser: {MATERIAL_IO_IMPORT_ERROR}. "
            "Install the runtime dependencies with: "
            "py -3 -m pip install -r toolchain/requirements.txt"
        )
        report = load_failure_report(message, candidate_path, engine_path, overrides_path)
        summary = report["summary"]
        print(
            f"candidate={summary['candidateMaterials']} engine={summary['engineMaterials']} "
            f"common={summary['commonMaterials']} errors={summary['errors']} "
            f"warnings={summary['warnings']}"
        )
        print(f"ERROR [contract.load-failed] <pack>: {message}")
        write_json_report(report, args.json_out)
        return 1

    try:
        candidate = load_contract(candidate_path)
        engine = load_contract(engine_path)
        overrides = load_overrides(overrides_path) if overrides_path else None
    except (OSError, json.JSONDecodeError, ValueError) as error:
        message = f"cannot load contract: {error}"
        report = load_failure_report(message, candidate_path, engine_path, overrides_path)
        summary = report["summary"]
        print(
            f"candidate={summary['candidateMaterials']} engine={summary['engineMaterials']} "
            f"common={summary['commonMaterials']} errors={summary['errors']} "
            f"warnings={summary['warnings']}"
        )
        print(f"ERROR [contract.load-failed] <pack>: {message}")
        write_json_report(report, args.json_out)
        return 1

    report = validate(candidate, engine)
    report = apply_overrides(report, overrides)
    report["target"] = "RenderDragon-26.51"
    report["candidatePath"] = str(args.candidate.resolve())
    report["engineContractPath"] = str(args.engine.resolve())
    if args.overrides:
        report["overridesPath"] = str(args.overrides.resolve())
    summary = report["summary"]
    print(
        f"candidate={summary['candidateMaterials']} engine={summary['engineMaterials']} "
        f"common={summary['commonMaterials']} errors={summary['errors']} "
        f"warnings={summary['warnings']}"
    )
    for item in report["issues"]:
        target = item["material"] + (f"/{item['pass']}" if item["pass"] else "")
        print(f"{item['kind'].upper()} [{item['code']}] {target}: {item['message']}")
    for item in report["overriddenIssues"]:
        target = item["material"] + (f"/{item['pass']}" if item["pass"] else "")
        print(
            f"OVERRIDDEN [{item['code']}] {target}: "
            f"{item['overrideId']} ({item['reason']})"
        )

    write_json_report(report, args.json_out)

    failed = summary["errors"] > 0 or (args.strict and summary["warnings"] > 0)
    return 1 if args.check and failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
