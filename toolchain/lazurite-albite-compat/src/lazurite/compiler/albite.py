import subprocess

from lazurite.material.stage import ShaderStage
from lazurite.material.platform import ShaderPlatform
from .albite_container import AlbiteResult, parse_stage
from .macro_define import MacroDefine


class AlbiteCompiler:
    albite_path: str

    def __init__(self, albite_paths: list[str] | str | None = None) -> None:
        if albite_paths is None:
            albite_paths = ["./albite", "albite"]
        elif isinstance(albite_paths, str):
            albite_paths = [albite_paths]

        self.albite_path = ""

        for path in albite_paths:
            try:
                result = subprocess.run([path, "-v"], capture_output=True)
            except FileNotFoundError:
                pass
            else:
                self.albite_path = path
                break

        if not self.albite_path:
            raise Exception(
                f"Error! No valid ALBITE compiler was found in the list {albite_paths}"
            )

        if result.returncode:
            print(result.stdout.decode())
            print(result.stderr.decode())
            result.check_returncode()

    def compile(
        self,
        file: str,
        platform: ShaderPlatform = ShaderPlatform.ESSL_310,
        stage: ShaderStage = ShaderStage.Fragment,
        include: list[str] = None,
        defines: list[MacroDefine] = None,
        options: list[str] = None,
    ) -> AlbiteResult:
        profile = {
            ShaderPlatform.ESSL_310: "310_es",
            ShaderPlatform.Metal: "msl",
            ShaderPlatform.Direct3D_SM60: "s_6_0",
            ShaderPlatform.Direct3D_SM65: "s_6_5",
        }.get(platform)
        if profile is None:
            raise Exception(
                f"{platform.name} shaders cannot be compiled with Albite compiler!"
            )

        stage_type = {
            ShaderStage.Vertex: "v",
            ShaderStage.Fragment: "f",
            ShaderStage.Compute: "c",
        }.get(stage)
        if stage_type is None:
            raise Exception(
                f"{stage.name} shaders cannot be compiled with Albite compiler!"
            )

        args = [self.albite_path, "-f", file, "-t", stage_type, "-p", profile]

        if include:
            for item in include:
                args.extend(("-i", item))

        if defines:
            args.extend(("--define", ";".join(d.format_bgfx() for d in defines)))

        if options:
            args.extend(options)

        # Albite writes the compiled container to stdout when no -o is given.
        result = subprocess.run(args, capture_output=True)

        # stdout is the binary payload, so only stderr is treated as log output.
        log = []
        if result.stderr:
            log.append(result.stderr.decode(errors="replace"))

        has_log = bool(log)
        log = "\n\n".join([""] + log + ["Command: " + " ".join(args)])

        if result.returncode:
            raise Exception(log)

        if has_log:
            print(log)

        data = result.stdout

        return parse_stage(data, platform, stage)
