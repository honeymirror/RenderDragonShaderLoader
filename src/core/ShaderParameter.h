#pragma once

#include <algorithm>
#include <cctype>
#include <iterator>
#include <map>
#include <sstream>
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
    std::string langKey;
    std::string group;
    std::string icon;
    std::string kind = "source"; // source / profile / uniform
    ParameterType type = ParameterType::Bool;

    // kind == "uniform": patch an existing inline vec4 uniform after compilation.
    std::string material;
    std::string uniform;
    std::string uniformType = "vec4";
    std::vector<int> components;
    std::string defaultValue;

    // Slider
    double min     = 0;
    double max     = 1;
    double def     = 0;
    bool   integer = false;

    // Enum / Bool 的候选值
    std::vector<std::string> values;
    // select 的 label -> macro numeric value；为空时按 values 下标编译。
    std::map<std::string, double> macroMap;

    // Color
    double r = 0, g = 0, b = 0;

    std::string current; // 当前值（字符串形式，交给编译/运行时）
};

// 把参数转成传给编译器/hook 的编译宏 "NAME=value"。
// Iris / OptiFine 风格的参数都是编译期宏；颜色参数暂不参与编译。
inline std::string toCompileMacro(ShaderParameter const& p) {
    if (p.kind == "uniform" || p.kind == "profile") return {};
    switch (p.type) {
    case ParameterType::Bool: {
        std::string v = p.current;
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        bool on = (v == "on" || v == "true" || v == "yes" || v == "1");
        // 开 -> 注入 NAME=1；关 -> 不注入（保持未定义），这样源码里的 #ifdef NAME
        // 和 #if NAME（未定义按 0 处理）两种写法都正确。
        return on ? p.macro + "=1" : std::string{};
    }
    case ParameterType::Slider: {
        // 滑条一律按实际值注入（包括 0）。曾尝试"值为 0 就不注入宏"，是错的：
        // 源码里 `float wave = NL_PLANTS_WAVE * windStrength;` 属于数值缩放，
        // 宏缺失会回落到 config.h 里的作者默认值（0.05），于是"调 0"反而变成
        // "很慢的摆动"。显式注入 0.0 才是真正的关闭。
        // 适配层已把这些开关宏在 config.h 里的定义注释掉
        // （由适配器的配置归一化步骤处理），这里的值即唯一真相。
        return p.macro + "=" + (p.current.empty() ? std::string{"0"} : p.current);
    }
    case ParameterType::Enum: {
        if (auto it = p.macroMap.find(p.current); it != p.macroMap.end()) {
            std::ostringstream out;
            out << it->second;
            return p.macro + "=" + out.str();
        }
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
