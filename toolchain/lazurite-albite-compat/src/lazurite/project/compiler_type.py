import enum


class CompilerType(enum.Enum):
    SHADERC = enum.auto()
    DXC = enum.auto()
    ALBITE = enum.auto()

    @classmethod
    def from_name(cls, name: str):
        if name == "shaderc":
            return cls.SHADERC
        if name == "dxc":
            return cls.DXC
        if name == "albite":
            return cls.ALBITE
