"""Build isolated Mirai material probes for one runtime uniform at a time."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import sys
import uuid
import zipfile


ROOT = Path(__file__).resolve().parents[1]
TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

from patch_material_uniform import (  # noqa: E402
    find_default_payload,
    find_uniform,
    load_material,
    sha256,
    states_equal,
    uniform_state,
    validate_target,
    verify_only_target_bytes,
)


@dataclass(frozen=True)
class ProbeSpec:
    key: str
    uniform: str
    default: tuple[float, float, float, float]
    level: str
    value: float
    purpose: str
    look_for: str


HIGH_PROBES = (
    ProbeSpec(
        key="CirrusHigh",
        uniform="RDL_CirrusHeight",
        default=(580.0, 0.0, 0.0, 0.0),
        level="high",
        value=950.0,
        purpose="Move the cirrus intersection plane upward.",
        look_for="Cirrus should sit higher and project as a broader, more distant sheet.",
    ),
    ProbeSpec(
        key="AerialHigh",
        uniform="RDL_AerialScale",
        default=(1.0, 0.0, 0.0, 0.0),
        level="high",
        value=3.0,
        purpose="Increase atmospheric optical depth used by aerial perspective.",
        look_for="Remote terrain and sky colour should gain stronger aerial haze.",
    ),
    ProbeSpec(
        key="AtmosphereHigh",
        uniform="RDL_AtmosphereDensity",
        default=(1.0, 0.0, 0.0, 0.0),
        level="high",
        value=3.0,
        purpose="Scale Rayleigh, Mie and ozone density together.",
        look_for="The atmosphere should become denser and more strongly coloured.",
    ),
    ProbeSpec(
        key="BorderFogHigh",
        uniform="RDL_BorderFog",
        default=(0.1, 0.0, 0.0, 0.0),
        level="high",
        value=1.0,
        purpose="Raise the maximum distance-fog blend.",
        look_for="Distant terrain should fade much more strongly into atmospheric colour.",
    ),
    ProbeSpec(
        key="MiddleGrayHigh",
        uniform="RDL_MiddleGray",
        default=(0.18, 0.0, 0.0, 0.0),
        level="high",
        value=0.5,
        purpose="Raise the auto-exposure middle-grey target.",
        look_for="Expect adaptation and highlight/shadow balance, not a simple uniform brightness shift.",
    ),
)

LOW_PROBES = (
    ProbeSpec(
        key="CirrusLow",
        uniform="RDL_CirrusHeight",
        default=(580.0, 0.0, 0.0, 0.0),
        level="low",
        value=0.0,
        purpose="Move the cirrus plane below a normal ground camera.",
        look_for="Cirrus should disappear for upward rays instead of becoming a low cloud deck.",
    ),
    ProbeSpec(
        key="AerialLow",
        uniform="RDL_AerialScale",
        default=(1.0, 0.0, 0.0, 0.0),
        level="low",
        value=0.0,
        purpose="Remove aerial optical depth.",
        look_for="The atmosphere should lose aerial perspective and show a much clearer horizon.",
    ),
    ProbeSpec(
        key="AtmosphereLow",
        uniform="RDL_AtmosphereDensity",
        default=(1.0, 0.0, 0.0, 0.0),
        level="low",
        value=0.0,
        purpose="Remove atmospheric density and scattering.",
        look_for="Sky scattering should collapse toward black while direct light remains.",
    ),
    ProbeSpec(
        key="BorderFogLow",
        uniform="RDL_BorderFog",
        default=(0.1, 0.0, 0.0, 0.0),
        level="low",
        value=0.0,
        purpose="Disable the distance-fog blend.",
        look_for="Distant terrain should remain sharp instead of fading into fog.",
    ),
    ProbeSpec(
        key="MiddleGrayLow",
        uniform="RDL_MiddleGray",
        default=(0.18, 0.0, 0.0, 0.0),
        level="low",
        value=0.05,
        purpose="Lower the auto-exposure middle-grey target.",
        look_for="Expect adaptation and tonal balance changes; average brightness may barely move.",
    ),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build one-uniform Mirai probe packs from compiled material files."
    )
    parser.add_argument(
        "--compiled-dir",
        type=Path,
        required=True,
        help="Directory containing the canonical 54 *.material.bin files.",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        required=True,
        help="Directory that receives the probe zip files.",
    )
    parser.add_argument(
        "--set",
        choices=("high", "low", "both"),
        default="high",
        help="Which probe values to build (default: high).",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Validate the source materials without writing zips.",
    )
    return parser.parse_args()


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def deterministic_uuid(name: str) -> str:
    namespace = uuid.UUID("6f575602-53f9-4c5c-ae65-c520e0d91c9f")
    return str(uuid.uuid5(namespace, name))


def probe_specs(selected: str) -> tuple[ProbeSpec, ...]:
    if selected == "high":
        return HIGH_PROBES
    if selected == "low":
        return LOW_PROBES
    return HIGH_PROBES + LOW_PROBES


def write_json(zf: zipfile.ZipFile, name: str, payload: dict) -> None:
    zf.writestr(name, json.dumps(payload, indent=2) + "\n")


def build_probe(
    spec: ProbeSpec,
    materials: list[Path],
    out_dir: Path,
    dry_run: bool,
) -> dict:
    pack_name = f"Mirai-Probe-{spec.key}"
    target = list(spec.default)
    target[0] = spec.value
    out_path = out_dir / f"{pack_name}.zip"
    temp_zip = None
    out_dir.mkdir(parents=True, exist_ok=True)
    work_dir = out_dir / "_work"
    work_dir.mkdir(exist_ok=True)
    if not dry_run:
        temp_zip = out_dir / f".{pack_name}.tmp.zip"
        if temp_zip.exists():
            temp_zip.unlink()

    def patch_material(source: Path) -> Path:
        material = load_material(source)
        uniform = find_uniform(material, spec.uniform)
        validate_target(material, uniform)
        before = uniform_state(material)
        source_bytes = source.read_bytes()
        target_range = find_default_payload(
            source_bytes,
            uniform,
            list(uniform.default),
        )
        uniform.default = target

        patched_path = work_dir / source.name
        with patched_path.open("wb") as file:
            material.write(file)
        patched = load_material(patched_path)
        patched_uniform = find_uniform(patched, spec.uniform)
        validate_target(patched, patched_uniform)
        expected = [
            (
                name,
                kind,
                count,
                tuple(target) if name == spec.uniform else default,
            )
            for name, kind, count, default in before
        ]
        if not states_equal(uniform_state(patched), expected):
            raise RuntimeError(
                f"{source.name}: fields other than {spec.uniform} changed"
            )
        verify_only_target_bytes(
            source_bytes,
            patched_path.read_bytes(),
            target_range,
            target,
        )
        return patched_path

    if dry_run:
        for source in materials:
            patch_material(source).unlink()
        work_dir.rmdir()
        return {
            "pack": pack_name,
            "uniform": spec.uniform,
            "value": spec.value,
            "materialCount": len(materials),
            "dryRun": True,
        }

    assert temp_zip is not None
    with zipfile.ZipFile(
        temp_zip,
        "w",
        compression=zipfile.ZIP_DEFLATED,
        compresslevel=6,
    ) as zf:
        for source in materials:
            patched_path = patch_material(source)
            zf.write(patched_path, f"renderer/materials/{source.name}")
            patched_path.unlink()

        write_json(
            zf,
            "manifest.json",
            {
                "format_version": 2,
                "header": {
                    "name": pack_name,
                    "description": (
                        "Diagnostic Mirai pack: isolated "
                        f"{spec.uniform}={spec.value:g}"
                    ),
                    "uuid": deterministic_uuid(f"{pack_name}:manifest"),
                    "version": [1, 0, 0],
                    "min_engine_version": [1, 26, 51],
                },
                "modules": [
                    {
                        "type": "resources",
                        "uuid": deterministic_uuid(f"{pack_name}:module"),
                        "version": [1, 0, 0],
                    }
                ],
            },
        )
        write_json(
            zf,
            "rdloader.pack.json",
            {
                "packVersion": 1,
                "name": pack_name,
                "gameVersion": "1.26.51",
                "defaultPreset": "default",
                "profiles": ["windows"],
                "backend": "albite",
                "presets": [
                    {
                        "id": "default",
                        "label": "Default",
                        "bins": "renderer/materials",
                    }
                ],
            },
        )
        write_json(
            zf,
            "probe.json",
            {
                "base": "Mirai",
                "isolatedUniform": spec.uniform,
                "level": spec.level,
                "default": spec.default[0],
                "value": spec.value,
                "purpose": spec.purpose,
                "lookFor": spec.look_for,
            },
        )

    assert temp_zip is not None
    work_dir.rmdir()
    if out_path.exists():
        out_path.unlink()
    temp_zip.replace(out_path)
    return {
        "pack": pack_name,
        "path": str(out_path),
        "uniform": spec.uniform,
        "value": spec.value,
        "materialCount": len(materials),
        "size": out_path.stat().st_size,
        "sha256": file_sha256(out_path),
    }


def main() -> int:
    args = parse_args()
    compiled_dir = args.compiled_dir.resolve()
    materials = sorted(compiled_dir.glob("*.material.bin"))
    if not materials:
        raise SystemExit(f"no material files found in {compiled_dir}")

    results = []
    for spec in probe_specs(args.set):
        result = build_probe(spec, materials, args.out_dir.resolve(), args.dry_run)
        results.append(result)
        print(json.dumps(result, ensure_ascii=False))
    print(
        json.dumps(
            {
                "probeCount": len(results),
                "sourceMaterialCount": len(materials),
                "dryRun": args.dry_run,
            },
            ensure_ascii=False,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
