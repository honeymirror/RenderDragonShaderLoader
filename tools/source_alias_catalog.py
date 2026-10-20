"""Reference-only validator for Java shader variable-name mapping notes.

No runtime or compiler path consumes this catalog. Java shader programs are not
importable into RenderDragon through this module.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys


VALID_STATUSES = {"direct", "contextual", "incompatible", "unsupported"}


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


def require_fields(name: str, item: dict, fields: tuple[str, ...], errors: list[str]) -> None:
    for field in fields:
        if field not in item:
            errors.append(f"{name}: missing field '{field}'")


def validate_entry(
    section: str,
    name: str,
    item: dict,
    errors: list[str],
) -> None:
    if not isinstance(item, dict):
        errors.append(f"{section}.{name}: entry must be an object")
        return

    status = item.get("status")
    if status not in VALID_STATUSES:
        errors.append(f"{section}.{name}: invalid status {status!r}")
        return

    if status in {"contextual", "incompatible"}:
        contexts = item.get("contexts")
        if not isinstance(contexts, list) or not contexts:
            errors.append(f"{section}.{name}: {status} entry requires contexts")

    if status in {"incompatible", "unsupported"}:
        if not item.get("reason"):
            errors.append(f"{section}.{name}: {status} entry requires a reason")


def check_target_names(
    section: str,
    catalog: dict,
    contracts: list[dict],
    errors: list[str],
) -> None:
    available_attributes = {}
    available_buffers = set()
    available_targets = set()
    for contract in contracts:
        for semantic, item in contract.get("attributes", {}).items():
            available_attributes.setdefault(semantic, set()).update(
                item.get("names", [])
            )
        available_buffers.update(contract.get("buffers", {}))
        available_targets.update(contract.get("renderTargets", {}))

    if section == "attributes":
        for name, item in catalog.get("attributes", {}).items():
            if item.get("status") != "direct":
                continue
            semantic = item.get("semantic")
            bedrock = item.get("bedrock")
            require_fields(f"attributes.{name}", item, ("semantic", "bedrock"), errors)
            if semantic not in available_attributes:
                errors.append(f"attributes.{name}: semantic {semantic!r} is not in any contract")
            elif bedrock not in available_attributes[semantic]:
                errors.append(
                    f"attributes.{name}: Bedrock name {bedrock!r} is not bound to "
                    f"semantic {semantic!r}"
                )

    elif section == "buffers":
        for name, item in catalog.get("buffers", {}).items():
            targets = item.get("bedrock", [])
            if not isinstance(targets, list):
                errors.append(f"buffers.{name}: bedrock must be an array")
                continue
            for target in targets:
                if target not in available_buffers:
                    errors.append(f"buffers.{name}: unknown Bedrock buffer {target!r}")

    elif section == "renderTargets":
        for name, item in catalog.get("renderTargets", {}).items():
            status = item.get("status")
            target = item.get("bedrock")
            require_fields(f"renderTargets.{name}", item, ("bedrock",), errors)
            if status != "unsupported" and target not in available_targets:
                errors.append(f"renderTargets.{name}: unknown Bedrock target {target!r}")


def validate_catalog(catalog: dict, contracts: list[dict]) -> tuple[list[str], dict]:
    errors: list[str] = []
    if catalog.get("catalogVersion") != 1:
        errors.append("unsupported catalogVersion")
    if not catalog.get("targetProfile"):
        errors.append("missing targetProfile")

    counts = {}
    for section in ("attributes", "buffers", "renderTargets"):
        entries = catalog.get(section)
        if not isinstance(entries, dict) or not entries:
            errors.append(f"missing or empty section '{section}'")
            counts[section] = {}
            continue
        status_counts = {status: 0 for status in sorted(VALID_STATUSES)}
        for name, item in entries.items():
            validate_entry(section, name, item, errors)
            if isinstance(item, dict) and item.get("status") in status_counts:
                status_counts[item["status"]] += 1
        counts[section] = status_counts
        check_target_names(section, catalog, contracts, errors)

    return errors, counts


def print_summary(catalog: dict, counts: dict, errors: list[str]) -> None:
    print(f"catalogVersion={catalog.get('catalogVersion')}")
    print(f"targetProfile={catalog.get('targetProfile')}")
    for section in ("attributes", "buffers", "renderTargets"):
        values = counts.get(section, {})
        rendered = " ".join(f"{key}={value}" for key, value in values.items())
        print(f"{section}: {rendered}")
    print(f"errors={len(errors)}")
    for error in errors:
        print("ERROR " + error)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate Java-to-Bedrock alias catalog against material IO contracts."
    )
    parser.add_argument("catalog", type=Path)
    parser.add_argument(
        "--contract",
        type=Path,
        action="append",
        required=True,
        help="material IO contract; repeat for multiple packs",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="return non-zero when catalog validation fails",
    )
    args = parser.parse_args()

    try:
        catalog = load_json(args.catalog.resolve())
        contracts = [load_contract(path.resolve()) for path in args.contract]
    except (OSError, json.JSONDecodeError, ValueError) as error:
        print(f"cannot load input: {error}", file=sys.stderr)
        return 2

    errors, counts = validate_catalog(catalog, contracts)
    print_summary(catalog, counts, errors)
    return 1 if args.check and errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
