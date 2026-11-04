// theme_colors.h — llm-switch 配色唯一入口（架构对齐 Clash-Flux 的
// src/ui/theme_colors.h）：深浅两态 ThemeSpec 与字号阶梯集中在此，主应用与
// Windows 安装器壳子共享。安装器是独立小 exe（不链接 llmswitch 模块），经
// platform/windows/huxerui.cmake 的只读 include 复用本头文件；改色只改这里。
//
// 纯色面板：深色模式海面/卡片/浮层全部纯黑（#000000），浅色模式全部纯白
// （#FFFFFF）；前景文字与线条为纯白/纯黑反色，不再有中性灰阶分层。区域
// 区分靠描边线条（outline）完成。主要操作按钮与选中/焦点使用反色纯色
// （深色下白底黑字、浅色下黑底白字）。语义色（error/success/warning）
// 同样使用黑白，通过文字与图标表达状态。
#pragma once

#include <huxerui/huxerui.h>

namespace llmswitch::ui {

// 全项目统一字号阶梯（pt）：控件/正文跟随 SDK 默认 14，不再散落硬编码字面量。
namespace font_size {
inline constexpr float kCaption = 11.0F;  // 徽标、状态小字
inline constexpr float kChip = 12.0F;     // 紧凑部件文字
inline constexpr float kBody = 14.0F;     // 正文/按钮/输入框（SDK 默认）
inline constexpr float kTitle = 20.0F;    // 页面/弹窗标题
} // namespace font_size

// 纯色面板：深色模式纯黑底 + 纯白字 + 纯白描边；浅色模式纯白底 + 纯黑字 +
// 纯黑描边。区域区分全部靠线条（outline）完成，不再有明度分层。主要操作
// （primary）与选中/焦点使用反色纯色：深色下白底黑字，浅色下黑底白字。
// 错误、成功与警告同样使用黑白。
inline huxerui::ThemeSpec AppDarkThemeSpec() {
    huxerui::ThemeSpec spec = huxerui::MaterialDarkThemeSpec();
    spec.typography = huxerui::TypographyScheme{
        .body_large = 16.0F,
        .body_medium = font_size::kBody,
        .body_small = font_size::kChip,
        .label_large = font_size::kBody,
        .title_large = font_size::kTitle,
        .headline_small = 24.0F,
    };
    spec.colors.primary = huxerui::Color::Rgb(255, 255, 255);        // 反色纯色
    spec.colors.on_primary = huxerui::Color::Rgb(0, 0, 0);
    spec.colors.primary_container = huxerui::Color::Rgb(0, 0, 0);
    spec.colors.on_primary_container = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.secondary = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.on_secondary = huxerui::Color::Rgb(0, 0, 0);
    spec.colors.secondary_container = huxerui::Color::Rgb(0, 0, 0);
    spec.colors.on_secondary_container = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.tertiary_container = huxerui::Color::Rgb(0, 0, 0);
    spec.colors.on_tertiary_container = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.background = huxerui::Color::Rgb(0, 0, 0);           // 纯黑
    spec.colors.surface = huxerui::Color::Rgb(0, 0, 0);
    spec.colors.surface_container_low = huxerui::Color::Rgb(0, 0, 0);
    spec.colors.surface_container = huxerui::Color::Rgb(0, 0, 0);
    spec.colors.surface_container_high = huxerui::Color::Rgb(0, 0, 0);
    spec.colors.surface_container_highest = huxerui::Color::Rgb(0, 0, 0);
    spec.colors.on_surface = huxerui::Color::Rgb(255, 255, 255);     // 纯白
    spec.colors.on_surface_variant = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.outline = huxerui::Color::Rgb(255, 255, 255);         // 线条纯白
    spec.colors.inverse_surface = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.inverse_on_surface = huxerui::Color::Rgb(0, 0, 0);
    spec.colors.scrim = huxerui::Color::Rgb(0, 0, 0, 0.66F);
    spec.colors.error = spec.colors.on_surface;
    spec.interactions.indication = huxerui::Indication{
        .hover = huxerui::IndicationLayer{.fill = huxerui::Color::Rgb(255, 255, 255, 0.08F)},
        .press = huxerui::IndicationLayer{.fill = huxerui::Color::Rgb(255, 255, 255, 0.12F)},
    };
    spec.interactions.focus_ring.color = spec.colors.primary;
    return spec;
}

inline huxerui::ThemeSpec AppLightThemeSpec() {
    huxerui::ThemeSpec spec = huxerui::MaterialLightThemeSpec();
    spec.typography = huxerui::TypographyScheme{
        .body_large = 16.0F,
        .body_medium = font_size::kBody,
        .body_small = font_size::kChip,
        .label_large = font_size::kBody,
        .title_large = font_size::kTitle,
        .headline_small = 24.0F,
    };
    spec.colors.primary = huxerui::Color::Rgb(0, 0, 0);               // 反色纯色
    spec.colors.on_primary = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.primary_container = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.on_primary_container = huxerui::Color::Rgb(0, 0, 0);
    spec.colors.secondary = huxerui::Color::Rgb(0, 0, 0);
    spec.colors.on_secondary = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.secondary_container = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.on_secondary_container = huxerui::Color::Rgb(0, 0, 0);
    spec.colors.tertiary_container = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.on_tertiary_container = huxerui::Color::Rgb(0, 0, 0);
    spec.colors.background = huxerui::Color::Rgb(255, 255, 255);     // 纯白
    spec.colors.surface = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.surface_container_low = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.surface_container = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.surface_container_high = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.surface_container_highest = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.on_surface = huxerui::Color::Rgb(0, 0, 0);           // 纯黑
    spec.colors.on_surface_variant = huxerui::Color::Rgb(0, 0, 0);
    spec.colors.outline = huxerui::Color::Rgb(0, 0, 0);               // 线条纯黑
    spec.colors.inverse_surface = huxerui::Color::Rgb(0, 0, 0);
    spec.colors.inverse_on_surface = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.scrim = huxerui::Color::Rgb(0, 0, 0, 0.34F);
    spec.colors.error = spec.colors.on_surface;
    spec.interactions.indication = huxerui::Indication{
        .hover = huxerui::IndicationLayer{.fill = huxerui::Color::Rgb(0, 0, 0, 0.08F)},
        .press = huxerui::IndicationLayer{.fill = huxerui::Color::Rgb(0, 0, 0, 0.12F)},
    };
    spec.interactions.focus_ring.color = spec.colors.primary;
    return spec;
}

// 桌宠透明窗口上的信息面板使用不透明纯色，避免桌面颜色透进背景。
inline huxerui::Color GlassFill(const huxerui::ThemeSpec& theme) {
    return theme.colors.surface;
}

inline huxerui::Color GlassBorder(const huxerui::ThemeSpec& theme) {
    return theme.colors.outline;
}

} // namespace llmswitch::ui
