"""Reference-only preflight for Java shader packs.

Java GLSL programs cannot execute directly on RenderDragon. This tool only
reports candidate name mappings, blockers, and target signatures. It does not
produce runnable Bedrock materials or a playable shader pack.
"""

from __future__ import annotations

import argparse
from fnmatch import fnmatchcase
import json
from pathlib import Path
import shutil
import sys
import zipfile


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import source_compat  # noqa: E402


PREFLIGHT_NAME = "rdloader.source-preflight.json"


def load_json(path: Path) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError(f"JSON root must be an object: {path}")
    return data


def find_shader_root(pack_root: Path) -> Path:
    candidates = [pack_root / "shaders"]
    candidates.extend(
        path
        for path in pack_root.rglob("shaders")
        if path.is_dir()
    )
    for candidate in candidates:
        if candidate.is_dir() and any(
            file.suffix.lower() in source_compat.SHADER_SUFFIXES
            for file in candidate.rglob("*")
            if file.is_file()
        ):
            return candidate
    raise ValueError(f"cannot find Java shaders directory under {pack_root}")


def program_name(path: Path) -> str:
    return path.stem


def contexts_for_program(rule_set: dict, program: str) -> tuple[list[str], list[str]]:
    matched = []
    contexts = set()
    for rule in rule_set.get("rules", []):
        if any(fnmatchcase(program, pattern) for pattern in rule.get("programPatterns", [])):
            matched.append(rule["id"])
            contexts.update(rule.get("contexts", []))
    if not contexts:
        contexts.update(rule_set.get("defaultContexts", []))
    return sorted(contexts), matched


def stage_pack(input_path: Path, output_dir: Path) -> Path:
    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True)
    if input_path.is_dir():
        shutil.copytree(input_path.resolve(), output_dir, dirs_exist_ok=True)
    elif input_path.is_file() and input_path.suffix.lower() == ".zip":
        with zipfile.ZipFile(input_path) as archive:
            archive.extractall(output_dir)
    else:
        raise ValueError(f"input must be a Java shader pack directory or zip: {input_path}")
    return output_dir


def import_pack(
    input_path: Path,
    output_dir: Path,
    catalog: dict,
    program_rules: dict,
) -> dict:
    staged_root = stage_pack(input_path, output_dir)
    shader_root = find_shader_root(staged_root)
    all_contexts = set(program_rules.get("defaultContexts", []))
    for rule in program_rules.get("rules", []):
        all_contexts.update(rule.get("contexts", []))

    programs = {}
    files_report = []
    blockers = []
    changed_files = 0

    for file in sorted(shader_root.rglob("*")):
        if not file.is_file() or file.suffix.lower() not in source_compat.SHADER_SUFFIXES:
            continue
        program = program_name(file)
        contexts, matched_rules = contexts_for_program(program_rules, program)
        if not contexts:
            contexts = sorted(all_contexts)
            matched_rules = ["include-or-unknown"]

        text = file.read_text(encoding="utf-8", errors="replace")
        rewritten, report = source_compat.rewrite_text(text, catalog, set(contexts))
        relative = file.relative_to(staged_root).as_posix()
        report.update(
            {
                "path": relative,
                "program": program,
                "contexts": contexts,
                "matchedProgramRules": matched_rules,
                "changed": rewritten != text,
            }
        )
        files_report.append(report)
        changed_files += int(rewritten != text)
        if rewritten != text:
            file.write_text(rewritten, encoding="utf-8")

        entry = programs.setdefault(
            program,
            {
                "contexts": contexts,
                "matchedProgramRules": matched_rules,
                "files": [],
                "renderTargets": set(),
                "outputBindingSignature": "0x00000000",
                "errors": [],
            },
        )
        entry["files"].append(relative)
        entry["renderTargets"].update(report["renderTargets"])
        if report["renderTargets"]:
            entry["outputBindingSignature"] = report["outputBindingSignature"]
        entry["errors"].extend(report["errors"])
        blockers.extend(f"{relative}: {error}" for error in report["errors"])

    for entry in programs.values():
        entry["renderTargets"] = sorted(entry["renderTargets"])

    manifest = {
        "schemaVersion": 1,
        "scope": "reference-analysis-only",
        "runtimePlayable": False,
        "generatesBedrockMaterials": False,
        "pack": str(input_path.resolve()),
        "shaderRoot": shader_root.relative_to(staged_root).as_posix(),
        "summary": {
            "programs": len(programs),
            "shaderFiles": len(files_report),
            "changedFiles": changed_files,
            "blockers": len(blockers),
        },
        "programs": programs,
        "files": files_report,
        "blockers": blockers,
        "readyForMaterialSynthesis": len(blockers) == 0,
    }
    manifest_path = shader_root.parent / PREFLIGHT_NAME
    manifest_path.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    manifest["preflightPath"] = str(manifest_path.resolve())
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Preflight a Java shader pack with the declarative compatibility catalog."
    )
    parser.add_argument("pack", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--catalog",
        type=Path,
        default=ROOT / "samples" / "compat" / "source-bedrock-aliases.json",
    )
    parser.add_argument(
        "--program-rules",
        type=Path,
        default=ROOT / "samples" / "compat" / "program-contexts.json",
    )
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    try:
        catalog = source_compat.load_catalog(args.catalog.resolve())
        program_rules = load_json(args.program_rules.resolve())
        output = args.output.resolve()
        manifest = import_pack(args.pack.resolve(), output, catalog, program_rules)
    except (OSError, ValueError, json.JSONDecodeError, zipfile.BadZipFile) as error:
        print(f"cannot import pack: {error}", file=sys.stderr)
        return 2

    summary = manifest["summary"]
    print(
        f"programs={summary['programs']} files={summary['shaderFiles']} "
        f"changed={summary['changedFiles']} blockers={summary['blockers']}"
    )
    print("scope=reference-analysis-only runtimePlayable=false")
    for blocker in manifest["blockers"]:
        print("BLOCKER " + blocker)
    print(f"preflight={manifest['preflightPath']}")
    return 1 if args.check and manifest["blockers"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
