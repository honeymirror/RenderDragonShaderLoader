"""
Reader for the container that ALBITE writes for each compiled shader stage.

ALBITE only reports what it found in the shader. Everything bgfx specific is
derived here: the io hash, the attribute list, the uniform type bits and the
shader chunk itself.
"""

import json
import os
import struct
from dataclasses import dataclass, field

from lazurite import util
from lazurite.material.platform import ShaderPlatform
from lazurite.material.shader_pass.bgfx_shader import BgfxShader, BgfxUniform
from lazurite.material.shader_pass.shader_input import ShaderInput
from lazurite.material.stage import ShaderStage

MAGIC = b"ALBI"
VERSION = 1

TAG_META = b"META"
TAG_CODE = b"CODE"

_HEADER = struct.Struct("<4sHH")  # magic, version, section count
_TABLE_ENTRY = struct.Struct("<4sII")  # tag, absolute offset, length

_STAGE_NAMES = {
    ShaderStage.Vertex: "vertex",
    ShaderStage.Fragment: "fragment",
    ShaderStage.Compute: "compute",
}

_BACKEND_PLATFORM = {
    ShaderPlatform.ESSL_310: "glsl",
    ShaderPlatform.Metal: "msl",
    ShaderPlatform.Direct3D_SM60: "dxil",
    ShaderPlatform.Direct3D_SM65: "dxil",
}

# bgfx uniform type ids, plus the bit that marks a fragment shader uniform.
_UNIFORM_TYPES = {"sampler": 1, "vec4": 2, "mat3": 3, "mat4": 4, "sampler_dxil": 32}
_UNIFORM_FRAGMENT_BIT = 16


class AlbiteFormatError(Exception):
    pass


@dataclass
class IoVar:
    name: str  # GLSL name, e.g. "a_position", "v_color0", "i_data0"
    location: int
    type: str
    semantic: str
    per_instance: bool
    precision: str
    interpolation: str
    attrib_id: int | None = None


@dataclass
class StageIo:
    """
    Vertex shader: `inputs` are attributes, `outputs` are varyings.
    Fragment shader: `inputs` are varyings, `outputs` is empty.
    Compute shader: both are empty.
    """

    inputs: list[IoVar] = field(default_factory=list)
    outputs: list[IoVar] = field(default_factory=list)


@dataclass
class AlbiteResult:
    bgfx_shader: BgfxShader
    # The material level inputs of this stage (see `ShaderDefinition.inputs`).
    inputs: list[ShaderInput]
    # The raw reflection result, used to check that stages link.
    io: StageIo


def read_sections(data: bytes) -> dict[bytes, bytes]:
    if len(data) < _HEADER.size or data[: len(MAGIC)] != MAGIC:
        raise AlbiteFormatError(
            "Albite output is not an ALBI container."
        )

    _, version, count = _HEADER.unpack_from(data)
    if version != VERSION:
        raise AlbiteFormatError(
            f"Unsupported ALBI container version: {version}, expected {VERSION}. "
            "Update lazurite or the ALBITE compiler."
        )

    table_end = _HEADER.size + _TABLE_ENTRY.size * count
    if table_end > len(data):
        raise AlbiteFormatError("Truncated ALBI container: section table is cut off.")

    sections = {}
    for i in range(count):
        tag, offset, length = _TABLE_ENTRY.unpack_from(
            data, _HEADER.size + _TABLE_ENTRY.size * i
        )
        if offset < table_end or offset + length > len(data):
            raise AlbiteFormatError(
                f"Corrupt ALBI container: section {tag!r} is out of bounds."
            )
        if tag in sections:
            raise AlbiteFormatError(
                f"Corrupt ALBI container: duplicate section {tag!r}."
            )
        sections[tag] = data[offset : offset + length]

    for tag in (TAG_META, TAG_CODE):
        if tag not in sections:
            raise AlbiteFormatError(
                f"Corrupt ALBI container: missing section {tag!r}."
            )

    return sections


def _load_io_vars(variables: list[dict]) -> list[IoVar]:
    return [
        IoVar(
            name=v["name"],
            location=v["location"],
            type=v["type"],
            semantic=v["semantic"],
            per_instance=v["per_instance"],
            precision=v["precision"],
            interpolation=v["interpolation"],
            attrib_id=v.get("attrib_id"),
        )
        for v in variables
    ]


def compute_io_hash(io: StageIo, stage: ShaderStage) -> int:
    """
    The hash bgfx compares to check that a vertex shader and a fragment shader
    link: the vertex shader hashes its output names, the fragment shader its
    input names. Names are sorted and concatenated before hashing.
    """
    if stage == ShaderStage.Vertex:
        names = [var.name for var in io.outputs]
    elif stage == ShaderStage.Fragment:
        names = [var.name for var in io.inputs]
    else:
        return 0

    if not names:
        return 0
    return util.hash_murmur2a("".join(sorted(names)).encode())


def _material_inputs(variables: list[IoVar]) -> list[ShaderInput]:
    inputs = []
    for var in variables:
        # `ShaderInput.load` reads these keys.
        inputs.append(
            ShaderInput().load(
                {
                    "name": var.name,
                    "type": var.type,
                    "semantic": var.semantic,
                    "per_instance": var.per_instance,
                    "precision": var.precision,
                    "interpolation": var.interpolation,
                }
            )
        )
    return inputs


def _build_bgfx_shader(
    meta: dict,
    code: bytes,
    io: StageIo,
    platform: ShaderPlatform,
    stage: ShaderStage,
) -> BgfxShader:
    resources = meta["resources"]
    shader = BgfxShader()

    shader.hash = compute_io_hash(io, stage)

    fragment_bit = _UNIFORM_FRAGMENT_BIT if stage == ShaderStage.Fragment else 0
    for uniform in resources["uniforms"]:
        base = _UNIFORM_TYPES.get(uniform["type"])
        if base is None:
            raise AlbiteFormatError(
                f'Unknown uniform type "{uniform["type"]}" for "{uniform["name"]}".'
            )
        shader.uniforms.append(
            BgfxUniform().load(
                {
                    "name": uniform["name"],
                    "type_bits": base | fragment_bit,
                    "count": uniform["count"],
                    "reg_index": uniform["reg_index"],
                    "reg_count": uniform["reg_count"],
                }
            )
        )

    if platform == ShaderPlatform.Metal and stage == ShaderStage.Compute:
        local_size = resources.get("local_size")
        if local_size is None:
            raise AlbiteFormatError(
                "Metal compute shader has no local_size, "
                "ALBITE could not find its layout(local_size_*) declaration."
            )
        shader.group_size = list(local_size)

    shader.shader_bytes = code

    if stage == ShaderStage.Vertex:
        shader.attributes = [var.attrib_id for var in io.inputs]
    shader.size = resources["ubo_size"]

    return shader


def parse_stage(
    data: bytes, platform: ShaderPlatform, stage: ShaderStage
) -> AlbiteResult:
    sections = read_sections(data)
    meta = json.loads(sections[TAG_META].decode())

    expected_stage = _STAGE_NAMES[stage]
    if meta["stage"] != expected_stage:
        raise AlbiteFormatError(
            f'ALBITE compiled a {meta["stage"]} shader, expected {expected_stage}.'
        )
    expected_backend = _BACKEND_PLATFORM.get(platform)
    if meta["backend"] != expected_backend:
        raise AlbiteFormatError(
            f'ALBITE used the {meta["backend"]} backend, but {platform.name} needs {expected_backend}.'
        )

    io = StageIo(
        inputs=_load_io_vars(meta["io"]["inputs"]),
        outputs=_load_io_vars(meta["io"]["outputs"]),
    )
    if stage == ShaderStage.Vertex and any(v.attrib_id is None for v in io.inputs):
        raise AlbiteFormatError("A vertex input has no attrib_id.")

    return AlbiteResult(
        bgfx_shader=_build_bgfx_shader(meta, sections[TAG_CODE], io, platform, stage),
        # Compute shaders have no material level inputs.
        inputs=_material_inputs(io.inputs) if stage != ShaderStage.Compute else [],
        io=io,
    )


def validate_link(vertex: StageIo, fragment: StageIo) -> list[str]:
    """
    Checks that a vertex shader's outputs match a fragment shader's inputs.
    Returns one message per problem, empty if they link.
    """
    written = {var.name: var for var in vertex.outputs}
    read = {var.name: var for var in fragment.inputs}
    problems = []

    for name in sorted(read.keys() - written.keys()):
        var = read[name]
        problems.append(
            f"The fragment shader reads {var.type} {name} (location {var.location}), "
            "but the vertex shader doesn't write it."
        )
    # 注意：vertex 写了某个 varying、fragment 没读，在 GLSL/Vulkan 里是合法的 ——
    # 未使用的 varying 会被优化掉。实测某延迟包的月亮材质：玩家把
    # MOON_RADIANCE_MULTIPLIER 调成 0 后，fragment 里用 v_texcoord0 的月亮采样分支
    # 被死代码消除，albite 反射里就没有这个 input 了；而 vertex 仍然无条件写它。
    # 这不该算链接失败，否则"调 0"就会让整包编译失败。
    # RDL_LINK_STRICT=1 可恢复旧行为用于调试。
    strict = os.environ.get("RDL_LINK_STRICT", "").strip().lower() in ("1", "true", "yes", "on")
    if strict:
        for name in sorted(written.keys() - read.keys()):
            var = written[name]
            problems.append(
                f"The vertex shader writes {var.type} {name} (location {var.location}), "
                "but the fragment shader doesn't declare it or unused."
            )

    for name in sorted(written.keys() & read.keys()):
        out_var, in_var = written[name], read[name]
        for what, a, b in (
            ("type", out_var.type, in_var.type),
            ("location", out_var.location, in_var.location),
            ("interpolation", out_var.interpolation or "smooth", in_var.interpolation or "smooth"),
        ):
            if a != b:
                problems.append(
                    f"{name}: the vertex shader writes {what} {a}, "
                    f"but the fragment shader reads {what} {b}."
                )

    return problems
