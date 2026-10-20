"""Reference-only context analysis for Java-style shader programs.

The native RenderDragon compatibility path uses material IO contracts directly.
This helper does not execute Java shader programs or synthesize materials.
"""

from __future__ import annotations

import argparse
from fnmatch import fnmatchcase
import json
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
COMPAT_SRC = ROOT / "toolchain" / "lazurite-albite-compat" / "src"
sys.path.insert(0, str(COMPAT_SRC))

from lazurite import util  # noqa: E402


def load_json(path: Path) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError(f"JSON root must be an object: {path}")
    return data


def load_contract(path: Path) -> dict:
    data = load_json(path)
    if data.get("contractVersion") != 1:
        raise ValueError(f"unsupported contractVersion in {path}")
    return data


def matches_any(value: str, patterns: list[str]) -> bool:
    return any(fnmatchcase(value, pattern) for pattern in patterns)


def rule_matches(
    rule: dict,
    material_name: str,
    pass_name: str,
    buffers: set[str],
    targets: set[str],
) -> bool:
    material_patterns = rule.get("materialPatterns")
    if material_patterns and not matches_any(material_name, material_patterns):
        return False
    pass_patterns = rule.get("passPatterns")
    if pass_patterns and not matches_any(pass_name, pass_patterns):
        return False
    if rule.get("requireNoTargets") and targets:
        return False
    required_targets = set(rule.get("requireTargets", []))
    if not required_targets.issubset(targets):
        return False
    required_buffers = set(rule.get("requireBuffers", []))
    if not required_buffers.issubset(buffers):
        return False
    return True


def resolve_pass(
    rule_set: dict,
    material_name: str,
    material: dict,
    pass_name: str,
    pass_data: dict,
) -> dict:
    buffers = set(material.get("buffers", {}))
    targets = set(pass_data.get("renderTargets", []))
    matched = []
    for rule in rule_set.get("rules", []):
        if rule_matches(rule, material_name, pass_name, buffers, targets):
            matched.append(rule)
    matched.sort(key=lambda item: (-int(item.get("priority", 0)), item["id"]))
    contexts = sorted({rule["context"] for rule in matched})
    if not contexts:
        default = rule_set.get("defaultContext")
        if default:
            contexts = [default]
    return {
        "contexts": contexts,
        "matchedRules": [rule["id"] for rule in matched],
        "renderTargets": sorted(targets),
        "outputBindingSignature": pass_data.get("outputBindingSignature", "0x00000000"),
    }


def resolve_contracts(rule_set: dict, contracts: list[dict]) -> dict:
    errors: list[str] = []
    if rule_set.get("rulesVersion") != 1:
        errors.append("unsupported rulesVersion")
    rule_ids = set()
    for rule in rule_set.get("rules", []):
        rule_id = rule.get("id")
        if not rule_id:
            errors.append("rule without id")
            continue
        if rule_id in rule_ids:
            errors.append(f"duplicate rule id {rule_id}")
        rule_ids.add(rule_id)
        if not rule.get("context"):
            errors.append(f"rule {rule_id}: missing context")

    contracts_out = []
    unresolved = 0
    for contract in contracts:
        name = contract.get("summary", {}).get("source", "")
        materials_out = {}
        for material_name, material in sorted(contract.get("materials", {}).items()):
            passes_out = {}
            for pass_name, pass_data in sorted(material.get("passes", {}).items()):
                resolved = resolve_pass(
                    rule_set,
                    material_name,
                    material,
                    pass_name,
                    pass_data,
                )
                if not resolved["contexts"]:
                    unresolved += 1
                    errors.append(f"{material_name}/{pass_name}: no context resolved")
                passes_out[pass_name] = resolved
            materials_out[material_name] = passes_out
        contracts_out.append({"source": name, "materials": materials_out})

    return {
        "schemaVersion": 1,
        "rulesVersion": rule_set.get("rulesVersion"),
        "defaultContext": rule_set.get("defaultContext"),
        "summary": {
            "contracts": len(contracts),
            "passes": sum(
                len(passes)
                for contract in contracts_out
                for passes in contract["materials"].values()
            ),
            "unresolved": unresolved,
            "errors": len(errors),
        },
        "contracts": contracts_out,
        "errors": errors,
    }


def parse_target_indices(value: str) -> list[int]:
    tokens = value.replace(",", " ").split()
    if len(tokens) == 1 and tokens[0].isdigit() and len(tokens[0]) > 1:
        tokens = list(tokens[0])
    indices = []
    for token in tokens:
        if not token.isdigit() or int(token) > 15:
            raise ValueError(f"invalid color target index: {token}")
        indices.append(int(token))
    return sorted(set(indices))


def generate_signature(catalog: dict, contexts: set[str], indices: list[int]) -> dict:
    targets = []
    errors = []
    mappings = catalog.get("renderTargets", {})
    for index in indices:
        name = f"colortex{index}"
        item = mappings.get(name)
        if item is None:
            errors.append(f"{name}: not declared in catalog")
            continue
        status = item.get("status")
        if status in {"unsupported", "incompatible"}:
            errors.append(f"{name}: {status}: {item.get('reason', '')}")
            continue
        if not contexts.intersection(item.get("contexts", [])):
            errors.append(f"{name}: missing required context")
            continue
        target = item.get("bedrock")
        if not target:
            errors.append(f"{name}: no concrete Bedrock target")
            continue
        targets.append(target)

    signature = util.get_output_binding_signature(sorted(set(targets)))
    return {
        "contexts": sorted(contexts),
        "indices": indices,
        "targets": sorted(set(targets)),
        "outputBindingSignature": f"0x{signature:08X}",
        "errors": errors,
    }


def command_resolve(args) -> int:
    try:
        rules = load_json(args.rules.resolve())
        contracts = [load_contract(path.resolve()) for path in args.contract]
    except (OSError, json.JSONDecodeError, ValueError) as error:
        print(f"cannot load input: {error}", file=sys.stderr)
        return 2

    report = resolve_contracts(rules, contracts)
    print(
        f"contracts={report['summary']['contracts']} passes={report['summary']['passes']} "
        f"unresolved={report['summary']['unresolved']} errors={report['summary']['errors']}"
    )
    for error in report["errors"]:
        print("ERROR " + error)
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
    return 1 if args.check and report["errors"] else 0


def command_signature(args) -> int:
    try:
        catalog = load_json(args.catalog.resolve())
        indices = parse_target_indices(args.targets)
    except (OSError, json.JSONDecodeError, ValueError) as error:
        print(f"cannot load input: {error}", file=sys.stderr)
        return 2
    result = generate_signature(catalog, set(args.context), indices)
    print(json.dumps(result, indent=2))
    return 1 if args.check and result["errors"] else 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Resolve Java shader material/pass contexts and generate target signatures."
    )
    commands = parser.add_subparsers(dest="command", required=True)

    resolve_parser = commands.add_parser("resolve")
    resolve_parser.add_argument("--rules", type=Path, required=True)
    resolve_parser.add_argument("--contract", type=Path, action="append", required=True)
    resolve_parser.add_argument("--json-out", type=Path)
    resolve_parser.add_argument("--check", action="store_true")
    resolve_parser.set_defaults(func=command_resolve)

    signature_parser = commands.add_parser("signature")
    signature_parser.add_argument("--catalog", type=Path, required=True)
    signature_parser.add_argument("--context", action="append", default=[])
    signature_parser.add_argument("--targets", required=True)
    signature_parser.add_argument("--check", action="store_true")
    signature_parser.set_defaults(func=command_signature)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
