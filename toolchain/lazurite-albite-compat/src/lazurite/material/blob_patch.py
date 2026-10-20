"""Patch changed BGFX shader bytecode into an existing material binary."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import os
import struct
import tempfile

from .encryption import EncryptionType
from .material import Material


class BlobPatchIncompatible(Exception):
    pass


@dataclass(frozen=True)
class BlobSpan:
    definition_offset: int
    outer_length_offset: int
    outer_data_offset: int
    outer_data_length: int
    length_offset: int
    data_offset: int
    data_length: int
    slot: tuple
    payload: bytes

    @property
    def outer_end(self) -> int:
        return self.outer_data_offset + self.outer_data_length


def material_blob_spans(material: Material, raw: bytes) -> list[BlobSpan]:
    if material.encryption != EncryptionType.NONE:
        raise BlobPatchIncompatible("encrypted material")

    spans: list[BlobSpan] = []
    for pass_index, shader_pass in enumerate(material.passes):
        for variant_index, variant in enumerate(shader_pass.variants):
            flags = tuple(sorted(variant.flags.items()))
            for shader in variant.shaders:
                bgfx = shader.bgfx_shader
                definition_offset = shader.definition_offset
                outer_length_offset = shader.bgfx_array_length_offset
                outer_data_offset = shader.bgfx_array_offset
                outer_data_length = shader.bgfx_array_length
                outer_end = outer_data_offset + outer_data_length
                length_offset = shader.bgfx_array_offset + bgfx.shader_bytes_length_offset
                data_offset = shader.bgfx_array_offset + bgfx.shader_bytes_offset
                data_length = len(bgfx.shader_bytes)
                data_end = data_offset + data_length
                if (
                    definition_offset < 0
                    or outer_length_offset < 0
                    or outer_data_offset < 0
                    or bgfx.shader_bytes_length_offset < 0
                    or bgfx.shader_bytes_offset < 0
                    or outer_length_offset + 4 > len(raw)
                    or outer_end > len(raw)
                    or length_offset + 4 > len(raw)
                    or data_end > len(raw)
                ):
                    raise BlobPatchIncompatible("invalid shader blob offsets")
                stored_outer_length = struct.unpack(
                    "<I", raw[outer_length_offset : outer_length_offset + 4]
                )[0]
                if stored_outer_length != outer_data_length:
                    raise BlobPatchIncompatible("encoded shader length does not match material")
                stored_length = struct.unpack("<I", raw[length_offset : length_offset + 4])[0]
                if stored_length != data_length:
                    raise BlobPatchIncompatible("shader blob length does not match material")
                if raw[data_offset:data_end] != bgfx.shader_bytes:
                    raise BlobPatchIncompatible("shader blob bytes do not match material")

                slot = (
                    pass_index,
                    shader_pass.name,
                    variant_index,
                    flags,
                    shader.platform.name,
                    shader.stage.name,
                )
                spans.append(
                    BlobSpan(
                        definition_offset=definition_offset,
                        outer_length_offset=outer_length_offset,
                        outer_data_offset=outer_data_offset,
                        outer_data_length=outer_data_length,
                        length_offset=length_offset,
                        data_offset=data_offset,
                        data_length=data_length,
                        slot=slot,
                        payload=bgfx.shader_bytes,
                    )
                )
    return spans


def _outer_skeleton(raw: bytes, spans: list[BlobSpan]) -> bytes:
    pieces: list[bytes] = []
    position = 0
    for span in sorted(spans, key=lambda item: item.definition_offset):
        if span.definition_offset < position:
            raise BlobPatchIncompatible("overlapping shader definition ranges")
        pieces.append(raw[position : span.definition_offset])
        position = span.outer_end
    pieces.append(raw[position:])
    return b"".join(pieces)


def _shader_template(raw: bytes, span: BlobSpan) -> bytes:
    definition_header = raw[span.definition_offset : span.outer_length_offset]
    outer = raw[span.outer_data_offset : span.outer_end]
    length_offset = span.length_offset - span.outer_data_offset
    data_offset = span.data_offset - span.outer_data_offset
    data_end = data_offset + span.data_length
    return definition_header + outer[:length_offset] + outer[data_end:]


def _load(path: Path) -> tuple[Material, bytes]:
    raw = path.read_bytes()
    material = Material.load_bin_file(str(path))
    return material, raw


def _verify_patched(
    raw: bytes,
    base_spans: list[BlobSpan],
    base_raw: bytes,
    target_spans: list[BlobSpan],
    expected_outer_skeleton: bytes,
) -> None:
    with tempfile.NamedTemporaryFile(suffix=".material.bin", delete=False) as handle:
        temp_path = Path(handle.name)
        handle.write(raw)
    try:
        material = Material.load_bin_file(str(temp_path))
        spans = material_blob_spans(material, raw)
    finally:
        try:
            temp_path.unlink()
        except OSError:
            pass

    spans_by_slot = {span.slot: span for span in spans}
    base_by_slot = {span.slot: span for span in base_spans}
    target_by_slot = {span.slot: span for span in target_spans}
    if set(spans_by_slot) != set(base_by_slot) or set(spans_by_slot) != set(target_by_slot):
        raise BlobPatchIncompatible("patched material shader layout changed")
    if _outer_skeleton(raw, spans) != expected_outer_skeleton:
        raise BlobPatchIncompatible("patched material outer skeleton changed")
    for slot, span in spans_by_slot.items():
        if _shader_template(raw, span) != _shader_template(base_raw, base_by_slot[slot]):
            raise BlobPatchIncompatible("patched shader metadata changed")
        if span.payload != target_by_slot[slot].payload:
            raise BlobPatchIncompatible("patched shader payload does not match target")


def patch_material(base_path: Path, target_path: Path) -> int:
    base_material, base_raw = _load(base_path)
    target_material, target_raw = _load(target_path)
    base_spans = material_blob_spans(base_material, base_raw)
    target_spans = material_blob_spans(target_material, target_raw)
    base_by_slot = {span.slot: span for span in base_spans}
    target_by_slot = {span.slot: span for span in target_spans}

    if len(base_by_slot) != len(base_spans) or len(target_by_slot) != len(target_spans):
        raise BlobPatchIncompatible("duplicate shader slots")
    if set(base_by_slot) != set(target_by_slot):
        raise BlobPatchIncompatible("pass/variant/platform/stage layout changed")
    outer_skeleton = _outer_skeleton(target_raw, target_spans)
    if _outer_skeleton(base_raw, base_spans) != outer_skeleton:
        raise BlobPatchIncompatible("material metadata changed")
    for slot, base_span in base_by_slot.items():
        if _shader_template(base_raw, base_span) != _shader_template(
            target_raw, target_by_slot[slot]
        ):
            raise BlobPatchIncompatible("shader metadata or signatures changed")

    changed = [
        (base_span, target_by_slot[slot].payload)
        for slot, base_span in base_by_slot.items()
        if base_span.payload != target_by_slot[slot].payload
    ]
    if not changed:
        return 0

    patched = base_raw
    for base_span, payload in sorted(
        changed, key=lambda item: item[0].outer_length_offset, reverse=True
    ):
        outer = patched[
            base_span.outer_data_offset : base_span.outer_data_offset
            + base_span.outer_data_length
        ]
        length_offset = base_span.length_offset - base_span.outer_data_offset
        data_offset = base_span.data_offset - base_span.outer_data_offset
        data_end = data_offset + base_span.data_length
        new_outer = (
            outer[:length_offset]
            + struct.pack("<I", len(payload))
            + payload
            + outer[data_end:]
        )
        patched = (
            patched[: base_span.outer_length_offset]
            + struct.pack("<I", len(new_outer))
            + new_outer
            + patched[base_span.outer_end :]
        )

    _verify_patched(
        patched,
        base_spans,
        base_raw,
        target_spans,
        outer_skeleton,
    )

    fd, temp_name = tempfile.mkstemp(
        prefix=target_path.name + ".", suffix=".blob-patch.tmp", dir=target_path.parent
    )
    try:
        with os.fdopen(fd, "wb") as handle:
            handle.write(patched)
        os.replace(temp_name, target_path)
    except Exception:
        try:
            os.unlink(temp_name)
        except OSError:
            pass
        raise
    return len(changed)


def sanitize_material_name(name: str) -> str:
    invalid = '/\\:*?"<>|'
    return "".join("_" if char in invalid else char for char in name)


def apply_directory(
    cache_dir: Path,
    target_dir: Path,
    materials: list[str],
) -> int:
    if not materials:
        materials = sorted(
            path.stem.removesuffix(".material")
            for path in target_dir.glob("*.material.bin")
        )

    patched_materials = 0
    patched_blobs = 0
    unchanged = 0
    fallback = 0

    for material in materials:
        target_path = target_dir / (material + ".material.bin")
        if not target_path.is_file():
            print(f"[blob-patch] skip {material}: target missing")
            fallback += 1
            continue

        cache_material_dir = cache_dir / sanitize_material_name(material)
        candidates = list(cache_material_dir.glob("*.material.bin"))
        candidates.sort(
            key=lambda path: path.stat().st_mtime_ns,
            reverse=True,
        )
        if not candidates:
            print(f"[blob-patch] skip {material}: no previous material")
            fallback += 1
            continue

        last_reason = ""
        selected = None
        count = 0
        for candidate in candidates:
            try:
                count = patch_material(candidate, target_path)
                selected = candidate
                break
            except BlobPatchIncompatible as error:
                last_reason = str(error)
            except Exception as error:
                last_reason = f"{type(error).__name__}: {error}"

        if selected is None:
            print(f"[blob-patch] fallback {material}: {last_reason}")
            fallback += 1
        elif count == 0:
            print(f"[blob-patch] unchanged {material}")
            unchanged += 1
        else:
            print(
                f"[blob-patch] patched {material}: blobs={count} "
                f"base={selected.name}"
            )
            patched_materials += 1
            patched_blobs += count

    print(
        "[blob-patch] summary "
        f"materials={len(materials)} patchedMaterials={patched_materials} "
        f"patchedBlobs={patched_blobs} unchanged={unchanged} fallback={fallback}"
    )
    return 0
