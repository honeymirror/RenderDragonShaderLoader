#pragma once

#include <string>

// 绘制后端抽象。UI 核心（ui::Screen）只依赖这个接口：
//   - 游戏内：GameCanvas 包 MinecraftUIRenderContext（见 ShaderManagerScreen.cpp）；
//   - 其他宿主（例如自己写的离线预览器）：实现同样的四个方法即可。
// 接口刻意保持最小（矩形 + 一行文字 + 一次 flush），这样两边都容易实现。
namespace renderdragon_shader_loader::ui {

struct Color {
    float r = 1.f, g = 1.f, b = 1.f, a = 1.f;
};

enum class Align { Left, Center, Right };

class Canvas {
public:
    virtual ~Canvas() = default;

    // 实心矩形。alpha 是整体不透明度（0..1），与 color.a 相乘由实现决定。
    virtual void fill(float x, float y, float w, float h, Color color, float alpha) = 0;

    // 在 [x, x + boxWidth] 内按 align 画一行文字。size = 字号倍数（1.0 = 14 GUI 单位高）。
    virtual void text(float x, float y, float boxWidth, std::string const& value, Color color, Align align,
                      float size) = 0;

    // 量一行文字的宽度（GUI 单位）。滑块拖动时那个"跟着鼠标走的名字浮层"要靠它定宽，
    // 所以宿主必须给准确值；默认实现只是按等宽估算，仅供没有字体的场合兜底。
    [[nodiscard]] virtual float textWidth(std::string const& value, float size) const {
        return (float)value.size() * 6.f * size;
    }

    // 一次绘制批次收尾（游戏里对应 flushImages/flushText；GDI 里是空操作）。
    virtual void flush() {}
};

} // namespace renderdragon_shader_loader::ui
