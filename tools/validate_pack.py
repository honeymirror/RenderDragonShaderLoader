"""静态校验光影包：把着色器源码里引用的宏和 options.json / shaders.properties
里声明的选项对齐，找出"源码用了但没人定义"的宏（编译会失败或行为异常）。

用法：
    py -3 tools/validate_pack.py <包目录或 zip> [--vanilla <解包后的原版材质目录>]

它做三件事：
  1. 解析包里的 options.json / shaders.properties，列出全部选项宏与默认值；
  2. 顺着每个材质的 main 着色器 `#include` 链，收集源码里**引用**到的所有宏；
  3. 报告：
       - 被引用但既不在选项、也不在源码默认定义里的宏（危险：会导致 X3004 undeclared）；
       - 声明了但源码从未引用的选项（提醒：可能是死选项）。

这是一个纯静态检查，不启动游戏、不调用编译器。
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import sys
import tempfile
import zipfile
from pathlib import Path

# Windows 控制台默认 GBK，中文报告会乱码；强制 UTF-8 输出。
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:  # noqa: BLE001
    pass

DEFINE_RE = re.compile(
    r"^\s*(//)?\s*#define\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s+([^/\n]+?))?"
)
IDENT_RE = re.compile(r"\b([A-Z][A-Z0-9_]{2,})\b")
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]')

SKIP_IDENTS = {
    # GLSL / HLSL 内建与常见类型
    "GL_FRAGCOLOR", "GL_POSITION", "POSITION", "TEXCOORD", "COLOR",
    "VEC2", "VEC3", "VEC4", "MAT2", "MAT3", "MAT4", "FLOAT",
    "LOWP", "MEDIUMP", "HIGHP", "TRUE", "FALSE", "NULL",
    "BGFX_SHADER_LANGUAGE_HLSL", "BGFX_SHADER_LANGUAGE_GLSL",
    "BGFX_CONFIG_MAX_BONES", "BGFX_SHADER_H_HEADER_GUARD",
    "SAMPLER2D", "SAMPLER2D_AUTOREG", "SAMPLERCUBE", "SAMPLER3D",
}


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def collect_defines(text: str) -> dict[str, str]:
    """收集源码里 #define 的宏（含被注释掉的，值记为空）。"""
    out: dict[str, str] = {}
    for raw in text.splitlines():
        m = DEFINE_RE.match(raw)
        if not m:
            continue
        out.setdefault(m.group(2), (m.group(3) or "").strip())
    return out


def collect_identifiers(text: str) -> set[str]:
    out: set[str] = set()
    # token-pasting 的前缀片段（`NL_CLOUD2##x##STEPS`）不是真宏，先剔除，
    # 否则会被误报成"引用了但未定义"。
    paste_prefixes: set[str] = set()
    for m in re.finditer(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*##", text):
        paste_prefixes.add(m.group(1))
    for raw in text.splitlines():
        line = raw.split("//", 1)[0]
        # 预处理行里的宏引用同样要算（#if / #ifdef / #elif 都在用宏），
        # 只跳过 #include 本身（它是路径，不是标识符）。
        if INCLUDE_RE.match(raw):
            continue
        for ident in IDENT_RE.findall(line):
            if ident in SKIP_IDENTS or ident in paste_prefixes:
                continue
            out.add(ident)
    return out


IFDEF_RE = re.compile(
    r"^\s*#\s*(?:ifdef|ifndef)\s+([A-Za-z_][A-Za-z0-9_]*)"
)
DEFINED_RE = re.compile(r"defined\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)")


def collect_gate_only_uses(text: str) -> tuple[set[str], set[str]]:
    """区分某个宏是"被当成开关"还是"被当成数值"使用。

    返回 (gate_macros, value_macros)：
      - gate_macros  : 只出现在 #ifdef / #ifndef / defined() 里的宏 —— 只看"有没有定义"；
      - value_macros : 出现在普通表达式里的宏（数值缩放、比较等）。

    例如 `#ifdef FEATURE_X` + `FEATURE_X * strength` 就是
    "两者都有"的典型：既是开关又参与数值 —— 这种最容易被误判成纯开关。
    """
    gate: set[str] = set()
    value: set[str] = set()
    for raw in text.splitlines():
        line = raw.split("//", 1)[0]
        stripped = line.lstrip()
        is_preproc = stripped.startswith("#")
        if INCLUDE_RE.match(raw):
            continue
        if is_preproc:
            for m in IFDEF_RE.finditer(line):
                gate.add(m.group(1))
            for m in DEFINED_RE.finditer(line):
                gate.add(m.group(1))
            # #if / #elif 里除了 defined() 之外出现的标识符算数值比较
            for ident in IDENT_RE.findall(line):
                if ident in gate:
                    continue
                if ident in SKIP_IDENTS:
                    continue
                value.add(ident)
            continue
        for ident in IDENT_RE.findall(line):
            if ident in SKIP_IDENTS:
                continue
            value.add(ident)
    return gate, value


def resolve_include(name: str, current: Path, roots: list[Path]) -> Path | None:
    candidates = [current.parent / name]
    for root in roots:
        candidates.append(root / name)
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


def walk_includes(start: Path, roots: list[Path], seen: set[Path]) -> str:
    """深度优先展开 #include 链，返回拼起来的源码文本。"""
    if start in seen or not start.is_file():
        return ""
    seen.add(start)
    text = read_text(start)
    parts = [text]
    for raw in text.splitlines():
        m = INCLUDE_RE.match(raw)
        if not m:
            continue
        resolved = resolve_include(m.group(1), start, roots)
        if resolved is not None:
            parts.append(walk_includes(resolved, roots, seen))
    return "\n".join(parts)


def load_options(pack: Path) -> tuple[set[str], dict[str, str]]:
    """返回 (选项宏集合, 选项默认值)。"""
    macros: set[str] = set()
    defaults: dict[str, str] = {}

    options_file = pack / "options.json"
    if options_file.is_file():
        try:
            data = json.loads(read_text(options_file))
            for option in data.get("options", []):
                if option.get("kind") == "uniform":
                    continue
                macro = option.get("macro") or option.get("name")
                if macro:
                    macros.add(macro)
                    defaults[macro] = str(option.get("default", ""))
        except Exception as exc:  # noqa: BLE001
            print(f"  ! options.json 解析失败: {exc}")

    props = pack / "shaders.properties"
    if props.is_file():
        for raw in read_text(props).splitlines():
            line = raw.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key = line.split("=", 1)[0].strip()
            if key.startswith("option."):
                macros.add(key[len("option."):])
    return macros, defaults


def material_roots(pack: Path) -> list[Path]:
    roots = [pack]
    for extra in ("include", "src", "materials"):
        d = pack / extra
        if d.is_dir():
            roots.append(d)
    return roots


def source_files(pack: Path) -> list[Path]:
    """找出所有材质着色器入口（.sc / .glsl / .sh / .h）。"""
    out: list[Path] = []
    for pattern in ("*.sc", "*.glsl", "*.sh", "*.h"):
        out.extend(sorted(pack.rglob(pattern)))
    # vanilla/ 是合并源，不是这个包自己的代码，跳过
    return [p for p in out if "vanilla" not in p.parts]


def _is_off_by_default(value: str) -> bool:
    """默认值是「关」吗：空 / False / OFF / 0 都算关。"""
    v = (value or "").strip().lower()
    return v in ("", "false", "off", "0", "0.0")



def validate(pack: Path, label: str) -> int:
    print(f"=== 校验 {label} ===")
    if not pack.is_dir():
        print("  ! 需要目录形式（zip 请先解压）")
        return 2

    option_macros, option_defaults = load_options(pack)
    roots = material_roots(pack)
    files = source_files(pack)
    print(f"  选项宏 {len(option_macros)} 个；扫描 {len(files)} 个源码文件")

    # 1) 源码里所有 #define（含 config.h 等头文件）
    source_defines: dict[str, str] = {}
    for f in files:
        for macro, value in collect_defines(read_text(f)).items():
            source_defines.setdefault(macro, value)

    # 2) 收集实际被引用的标识符。
    #    两种取法合起来：材质入口的 include 链（保证只看真用到的代码），
    #    加上整棵源码树（选项可能通过 pack 自己的头文件间接进入，比如
    #    部分仓库式包的宏定义在 src/include/lib/*.glsl，由项目结构注入）。
    referenced: set[str] = set()
    # 逐文件判定"这个宏在本文件里是被当开关、还是被当数值用"。
    # 跨文件合并会带来大量误报（A 文件 `#ifdef`、B 文件当数值用是常态），
    # 所以只在**同一个文件里同时出现两种用法**时才判定为"既当开关又当数值"。
    gate_macros: set[str] = set()
    value_macros: set[str] = set()
    both_macros: set[str] = set()

    def merge(text: str, is_config: bool) -> None:
        nonlocal referenced
        nonlocal gate_macros, value_macros, both_macros
        referenced |= collect_identifiers(text)
        if is_config:
            return
        g, v = collect_gate_only_uses(text)
        gate_macros |= g
        value_macros |= v
        both_macros |= (g & v)

    for f in files:
        if f.suffix not in (".sc", ".glsl"):
            continue
        if f.name not in ("fragment.sc", "vertex.sc", "compute.sc") and not f.name.endswith(".glsl"):
            continue
        merge(walk_includes(f, roots, set()), False)
    for f in files:
        merge(read_text(f), f.name == "config.h")

    # 3) 只看这个包**自己声明的选项宏**（有的包使用 NL_* 前缀，有的使用裸宏名，
    #    所以不能只靠前缀过滤）。再额外收一批 NL_*/RDL_* 方便看缺定义。
    interesting = {m for m in referenced if m in option_macros}
    interesting |= {m for m in referenced if m.startswith(("NL_", "RDL_"))}
    defined = set(source_defines) | option_macros
    missing = sorted(m for m in interesting if m not in defined)
    unused = sorted(m for m in option_macros if m not in interesting)

    problems = 0
    if missing:
        problems += len(missing)
        print(f"\n  [危险] 源码引用了但没人定义的宏（{len(missing)} 个）：")
        for macro in missing:
            print(f"    - {macro}")
        print("    这些宏会导致编译报 X3004 undeclared identifier，")
        print("    或在 #ifdef 里恒为假、整段功能静默失效。")
    else:
        print("\n  [OK] 源码引用的自定义宏都有定义")

    if unused:
        print(f"\n  [提醒] 声明了但源码没引用的选项（{len(unused)} 个）：")
        for macro in unused:
            print(f"    - {macro}  (默认 {option_defaults.get(macro, '')!r})")

    # 4) 只被 #ifdef 当开关用的选项：滑条拉到 0 会"关不掉"，
    #    因为 #ifdef 只看是否存在 —— 这类要么改成 toggle，要么像摆动那样
    #    由适配层把 config.h 里的默认定义注释掉。
    # "只当开关"：源码里出现过 #ifdef 形式的门控，但**没有任何文件**把它当数值用。
    # 这种滑条拉 0 必然关不掉（#ifdef 只看存在性）。
    gate_only = sorted(
        m for m in option_macros
        if m in gate_macros and m not in value_macros
    )
    if gate_only:
        # 分清两种情况：
        #   - 声明成 toggle 且默认关（源码里是 //#define）→ 完全正确，滑条/开关都能关；
        #   - 声明成 slider 且默认是数值           → 有问题，调 0 关不掉。
        toggle_ok = [m for m in gate_only if _is_off_by_default(option_defaults.get(m, ""))]
        slider_bad = [m for m in gate_only if m not in toggle_ok]
        if toggle_ok:
            print(f"\n  [OK] 用 #ifdef 当开关、默认关闭的布尔项（{len(toggle_ok)} 个，写法正确）：")
            for macro in toggle_ok:
                print(f"    - {macro}")
        if slider_bad:
            print(f"\n  [重点] 只被 #ifdef 当开关的滑条选项（{len(slider_bad)} 个）：")
            for macro in slider_bad:
                print(f"    - {macro}  (默认 {option_defaults.get(macro, '')!r})")
            print("    这类选项调 0 不会关闭，需要改成 toggle，或把源码里的无条件")
            print("    #define 注释掉（见适配器的配置归一化步骤）。")

    # 5) 既是开关又参与数值计算的选项：最容易出现"调 0 变成很小值"。
    both = sorted(m for m in option_macros if m in both_macros)
    if both:
        # 这一类数量通常很多（滑条基本都是"有值就用、没值就判存在"），
        # 只列前几个做参考，重点是提醒它们对"调 0"的敏感性。
        print(f"\n  [参考] 既当开关又参与数值的选项（{len(both)} 个，仅列前 8 个）：")
        for macro in both[:8]:
            print(f"    - {macro}  (默认 {option_defaults.get(macro, '')!r})")
        print("    这类选项注入 0 会真的把效果乘成 0（正确行为）；")
        print("    但若源码里还有无条件 #define，玩家值就会被覆盖（见下面的清单）。")

    # 6) 源码里**无条件** #define 的选项：会覆盖玩家注入的值
    #    （某些包最初就是在 config.h 里无条件定义可调宏）。
    #    config.user.h 是适配层有意写的默认值，带 #ifndef 守卫时没问题，
    #    所以这里以**守卫缺失**为准，而不是"出现过 #define"。
    # 逐文件记录"无条件定义"的宏；只要还有文件是无条件定义的，就认为可能覆盖玩家值。
    unguarded_in: dict[str, list[str]] = {}
    config_h_defines: set[str] = set()
    for f in files:
        if f.name == "config.user.h":
            continue  # 适配层有意写的默认值，带守卫，不算覆盖
        if f.name == "config.h":
            # 包的主配置：适配层（adapt_stage8.py）已经会处理其中"开关型"宏。
            # 这里不重复刷屏，只在最后汇总一句。
            config_h_defines |= set(collect_defines(read_text(f)))
            continue
        text = read_text(f)
        pending_guard: str | None = None
        for raw in text.splitlines():
            stripped = raw.strip()
            m = re.match(r"^#\s*ifndef\s+([A-Za-z_][A-Za-z0-9_]*)$", stripped)
            if m:
                pending_guard = m.group(1)
                continue
            m = DEFINE_RE.match(raw)
            if m and not m.group(1) and pending_guard != m.group(2):
                unguarded_in.setdefault(m.group(2), []).append(f.name)
            if stripped.startswith("#endif"):
                pending_guard = None
    shadowed = sorted(m for m in option_macros if m in unguarded_in)
    if shadowed:
        print(f"\n  [重点] config.h 之外的源码里无条件 #define 的选项（{len(shadowed)} 个）：")
        for macro in shadowed:
            where = ", ".join(sorted(set(unguarded_in[macro]))[:3])
            print(f"    - {macro}  (定义于 {where})")
        print("    这些会覆盖玩家注入的值，需要像摆动那样在适配层注释掉。")

    covered_by_config_h = sorted(m for m in option_macros if m in config_h_defines)
    if covered_by_config_h:
        print(
            f"\n  [说明] 另有 {len(covered_by_config_h)} 个选项的默认值写在 config.h 里。"
            "其中开关型的会由适配器的配置归一化步骤处理，"
            "其余是正常默认值。"
        )

    print(f"\n  源码 #define: {len(source_defines)} 个；选项: {len(option_macros)} 个；"
          f"源码引用: {len(interesting)} 个")
    return 1 if problems else 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("pack", type=Path, help="包目录，或 .zip（会临时解压）")
    args = parser.parse_args()

    target = args.pack
    tmp: Path | None = None
    label  = target.name
    if target.is_file() and target.suffix.lower() == ".zip":
        label = target.stem
        # 解压到工作区里，避免沙箱对系统临时目录写权限的限制。
        tmp = Path.cwd() / "build" / "validate_tmp"
        shutil.rmtree(tmp, ignore_errors=True)
        tmp.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(target) as zf:
            zf.extractall(tmp)
        target = tmp

    try:
        return validate(target, label)
    finally:
        if tmp is not None:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
