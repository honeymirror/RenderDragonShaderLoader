"""Reference-only analysis of Java shader source identifiers.

Only verified `direct` attributes are rewritten automatically. Buffer names and
render-target directives are analyzed against explicit pass contexts; they are
not renamed until a concrete Bedrock pass resolver supplies unambiguous
targets. RenderDragonShaderLoader does not run this rewriter at runtime, and
rewriting Java shader source does not make a Java shader pack renderable.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
COMPAT_SRC = ROOT / "toolchain" / "lazurite-albite-compat" / "src"
sys.path.insert(0, str(COMPAT_SRC))

from lazurite import util  # noqa: E402


SHADER_SUFFIXES = {
    ".frag",
    ".vert",
    ".comp",
    ".glsl",
    ".vsh",
    ".fsh",
    ".gsh",
    ".csh",
    ".inc",
}
DRAW_TARGET_RE = re.compile(
    r"/\*\s*(?:DRAWBUFFERS|RENDERTARGETS)\s*:\s*([0-9]+)\s*\*/",
    re.IGNORECASE,
)


def load_catalog(path: Path) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or data.get("catalogVersion") != 1:
        raise ValueError(f"unsupported source alias catalog: {path}")
    return data


def identifier_pattern(names: set[str]) -> re.Pattern | None:
    if not names:
        return None
    alternatives = "|".join(
        re.escape(name) for name in sorted(names, key=len, reverse=True)
    )
    return re.compile(rf"\b(?:{alternatives})\b")


def iter_segments(text: str):
    """Yield (is_code, segment) while isolating comments and quoted strings."""
    index = 0
    code_start = 0

    while index < len(text):
        marker = None
        if text.startswith("//", index):
            marker = "//"
        elif text.startswith("/*", index):
            marker = "/*"
        elif text[index] == '"':
            marker = '"'
        elif text[index] == "'":
            marker = "'"

        if marker is None:
            index += 1
            continue

        if code_start < index:
            yield True, text[code_start:index]
        if marker in ("//",):
            end = text.find("\n", index)
            end = len(text) if end < 0 else end
        elif marker == "/*":
            end = text.find("*/", index + 2)
            end = len(text) if end < 0 else end + 2
        else:
            end = index + 1
            while end < len(text):
                if text[end] == "\\":
                    end += 2
                    continue
                if text[end] == marker:
                    end += 1
                    break
                end += 1
        yield False, text[index:end]
        index = end
        code_start = index

    if code_start < len(text):
        yield True, text[code_start:]


def transform_code_segments(text: str, transform) -> str:
    return "".join(
        transform(segment) if is_code else segment
        for is_code, segment in iter_segments(text)
    )


def matching_tokens(text: str, names: set[str]) -> set[str]:
    pattern = identifier_pattern(names)
    if pattern is None:
        return set()
    tokens = set()
    for is_code, segment in iter_segments(text):
        if is_code:
            tokens.update(pattern.findall(segment))
    return tokens


def rewrite_text(
    text: str,
    catalog: dict,
    contexts: set[str] | None = None,
) -> tuple[str, dict]:
    contexts = set(contexts or ())
    errors: list[str] = []
    replacements: dict[str, str] = {}
    resolved_buffers: dict[str, dict] = {}
    render_targets: list[str] = []

    attributes = catalog.get("attributes", {})
    attribute_names = set(attributes)
    used_attributes = matching_tokens(text, attribute_names)
    direct: dict[str, str] = {}
    for name in sorted(used_attributes):
        item = attributes[name]
        status = item.get("status")
        if status == "direct":
            direct[name] = item["bedrock"]
        elif status == "contextual":
            if not contexts.intersection(item.get("contexts", [])):
                errors.append(f"attribute {name}: missing required context")
        elif status == "incompatible":
            errors.append(f"attribute {name}: incompatible: {item.get('reason', '')}")
        else:
            errors.append(f"attribute {name}: unsupported: {item.get('reason', '')}")

    pattern = identifier_pattern(set(direct))
    if pattern is not None:
        def replace_attribute(match: re.Match) -> str:
            source = match.group(0)
            target = direct[source]
            replacements[source] = target
            return target

        text = transform_code_segments(
            text, lambda segment: pattern.sub(replace_attribute, segment)
        )

    buffers = catalog.get("buffers", {})
    for name in sorted(matching_tokens(text, set(buffers))):
        item = buffers[name]
        status = item.get("status")
        if status == "unsupported":
            errors.append(f"buffer {name}: unsupported: {item.get('reason', '')}")
            continue
        if status == "incompatible":
            errors.append(f"buffer {name}: incompatible: {item.get('reason', '')}")
            continue
        matched_contexts = sorted(contexts.intersection(item.get("contexts", [])))
        if not matched_contexts:
            errors.append(f"buffer {name}: missing required context")
            continue
        targets = item.get("bedrock", [])
        if not targets:
            errors.append(
                f"buffer {name}: pass context {matched_contexts} found, "
                "but no concrete Bedrock buffer was selected"
            )
            continue
        if len(targets) != 1:
            errors.append(
                f"buffer {name}: ambiguous Bedrock targets {targets} in context "
                f"{matched_contexts}"
            )
            continue
        resolved_buffers[name] = {
            "bedrock": targets[0],
            "contexts": matched_contexts,
        }

    targets_catalog = catalog.get("renderTargets", {})
    for match in DRAW_TARGET_RE.finditer(text):
        for index in match.group(1):
            name = f"colortex{index}"
            item = targets_catalog.get(name)
            if item is None:
                errors.append(f"render target {name}: not declared in catalog")
                continue
            status = item.get("status")
            if status in {"unsupported", "incompatible"}:
                errors.append(
                    f"render target {name}: {status}: {item.get('reason', '')}"
                )
                continue
            matched_contexts = sorted(contexts.intersection(item.get("contexts", [])))
            if not matched_contexts:
                errors.append(f"render target {name}: missing required context")
                continue
            target = item.get("bedrock")
            if target:
                render_targets.append(target)

    output_signature = util.get_output_binding_signature(
        sorted(set(render_targets))
    )
    return text, {
        "replacements": replacements,
        "buffers": resolved_buffers,
        "renderTargets": sorted(set(render_targets)),
        "outputBindingSignature": f"0x{output_signature:08X}",
        "errors": errors,
    }


def shader_files(path: Path) -> list[Path]:
    if path.is_file():
        return [path]
    return [
        file
        for file in sorted(path.rglob("*"))
        if file.is_file() and file.suffix.lower() in SHADER_SUFFIXES
    ]


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Rewrite verified Java shader attributes and validate pass-context resources."
    )
    parser.add_argument("--catalog", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--context", action="append", default=[])
    parser.add_argument("--output", type=Path)
    parser.add_argument("--in-place", action="store_true")
    parser.add_argument("--report-json", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    if args.output and args.in_place:
        print("--output and --in-place are mutually exclusive", file=sys.stderr)
        return 2

    try:
        catalog = load_catalog(args.catalog.resolve())
    except (OSError, json.JSONDecodeError, ValueError) as error:
        print(f"cannot load catalog: {error}", file=sys.stderr)
        return 2

    source = args.source.resolve()
    files = shader_files(source)
    if not files:
        print(f"no shader files found under {source}", file=sys.stderr)
        return 2

    report = {"catalog": str(args.catalog.resolve()), "files": []}
    changed_files = 0
    error_count = 0
    contexts = set(args.context)

    for file in files:
        relative = Path(file.name) if source.is_file() else file.relative_to(source)
        text = file.read_text(encoding="utf-8", errors="replace")
        rewritten, file_report = rewrite_text(text, catalog, contexts)
        file_report["path"] = relative.as_posix()
        file_report["changed"] = rewritten != text
        report["files"].append(file_report)
        error_count += len(file_report["errors"])
        changed_files += int(rewritten != text)

        if args.in_place and rewritten != text:
            file.write_text(rewritten, encoding="utf-8")
        elif args.output:
            destination = args.output.resolve() / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(rewritten, encoding="utf-8")

    report["summary"] = {
        "files": len(files),
        "changedFiles": changed_files,
        "errors": error_count,
        "contexts": sorted(contexts),
    }

    print(
        f"files={len(files)} changed={changed_files} errors={error_count} "
        f"contexts={','.join(sorted(contexts)) or '-'}"
    )
    for file_report in report["files"]:
        if file_report["replacements"]:
            pairs = ", ".join(
                f"{source_name}->{target_name}"
                for source_name, target_name in sorted(
                    file_report["replacements"].items()
                )
            )
            print(f"REWRITE {file_report['path']}: {pairs}")
        for error in file_report["errors"]:
            print(f"ERROR {file_report['path']}: {error}")

    if args.report_json:
        output = args.report_json
        if not output.is_absolute():
            output = Path.cwd() / output
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(
            json.dumps(report, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
        print(f"wrote {output}")

    return 1 if args.check and error_count else 0


if __name__ == "__main__":
    raise SystemExit(main())
