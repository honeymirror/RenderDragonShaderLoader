#pragma once

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string>
#include <vector>

namespace renderdragon_shader_loader {

// 着色器参数类型（对齐 OptiFine / Iris 的 define / slider / enum / color）。
enum class ParameterType {
    Bool,   // 布尔开关 -> 编译宏 0/1
    Slider, // 数值范围 -> 编译宏（整数）或运行时 uniform（浮点）
    Enum,   // 枚举 -> 编译宏（序号）
    Color,  // RGB 颜色 -> 运行时 uniform（vec3）
};

// 一个可调参数，声明在 shaders.properties 中。
struct ShaderParameter {
    std::string name;
    std::string macro; // 对应编译宏名，默认等于 name
    std::string label;
    ParameterType type = ParameterType::Bool;

    // Slider
    double min     = 0;
    double max     = 1;
    double def     = 0;
    bool   integer = false;

    // Enum / Bool 的候选值
    std::vector<std::string> values;

    // Color
    double r = 0, g = 0, b = 0;

    std::string current; // 当前值（字符串形式，交给编译/运行时）
};

// 把参数转成传给编译器/hook 的编译宏 "NAME=value"。
// 只对结构性参数（布尔 / 整数 slider / 枚举）生效；浮点 slider 与颜色走运行时
// uniform（二期），这里返回空串表示不参与编译宏。
inline std::string toCompileMacro(ShaderParameter const& p) {
    switch (p.type) {
    case ParameterType::Bool: {
        std::string v = p.current;
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        bool on = (v == "on" || v == "true" || v == "yes" || v == "1");
        // 开 -> 注入 NAME=1；关 -> 不注入（保持未定义），这样源码里的 #ifdef NAME
        // 和 #if NAME（未定义按 0 处理）两种写法都正确。
        return on ? p.macro + "=1" : std::string{};
    }
    case ParameterType::Slider:
        return p.integer ? p.macro + "=" + p.current : std::string{};
    case ParameterType::Enum: {
        auto it = std::find(p.values.begin(), p.values.end(), p.current);
        if (it == p.values.end()) return {};
        return p.macro + "=" + std::to_string(std::distance(p.values.begin(), it));
    }
    case ParameterType::Color:
        return {};
    }
    return {};
}

} // namespace renderdragon_shader_loader
