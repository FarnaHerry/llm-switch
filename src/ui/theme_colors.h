// theme_colors.h — llm-switch 配色唯一入口（架构对齐 Clash-Flux 的
// src/ui/theme_colors.h）：深浅两态 ThemeSpec 与字号阶梯集中在此，主应用与
// Windows 安装器壳子共享。安装器是独立小 exe（不链接 llmswitch 模块），经
// platform/windows/huxerui.cmake 的只读 include 复用本头文件；改色只改这里。
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

// 黑白双色主题：深色「墨」= 近黑海面 + 中性石墨卡片；浅色「纸」= 纯白海面 +
// 中性浅灰卡片。除状态语义色（error/success/warning）外，只有主要操作按钮、
// 选中态与焦点使用主题蓝；背景、卡片、描边、图标和装饰一律走黑白灰。
// 主题蓝在深浅两态都取 #2563EB：白字对比度 5.17:1，在浅底上作控件填充
// 也保持 4.9:1；深色下不作小号正文色，只用于填充、边框和图形。
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
    spec.colors.primary = huxerui::Color::Rgb(37, 99, 235);        // 主题蓝 #2563EB
    spec.colors.on_primary = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.primary_container = huxerui::Color::Rgb(31, 31, 31);
    spec.colors.on_primary_container = huxerui::Color::Rgb(245, 245, 245);
    spec.colors.secondary = huxerui::Color::Rgb(163, 163, 163);    // 中性灰 #A3A3A3
    spec.colors.on_secondary = huxerui::Color::Rgb(10, 10, 10);
    spec.colors.secondary_container = huxerui::Color::Rgb(38, 38, 38);   // hover / 选中底块
    spec.colors.on_secondary_container = huxerui::Color::Rgb(245, 245, 245);
    spec.colors.tertiary_container = huxerui::Color::Rgb(32, 32, 32);
    spec.colors.on_tertiary_container = huxerui::Color::Rgb(212, 212, 212);
    spec.colors.background = huxerui::Color::Rgb(10, 10, 10);       // 近黑海面 #0A0A0A
    spec.colors.surface = huxerui::Color::Rgb(17, 17, 17);
    spec.colors.surface_container_low = huxerui::Color::Rgb(20, 20, 20);
    spec.colors.surface_container = huxerui::Color::Rgb(24, 24, 24);      // 二级卡片
    spec.colors.surface_container_high = huxerui::Color::Rgb(34, 34, 34); // hover / 普通按钮
    spec.colors.surface_container_highest = huxerui::Color::Rgb(42, 42, 42);
    spec.colors.on_surface = huxerui::Color::Rgb(245, 245, 245);    // 近白正文
    spec.colors.on_surface_variant = huxerui::Color::Rgb(163, 163, 163); // 次要灰
    spec.colors.outline = huxerui::Color::Rgb(51, 51, 51);          // 中性描边 #333333
    spec.colors.inverse_surface = huxerui::Color::Rgb(245, 245, 245);
    spec.colors.inverse_on_surface = huxerui::Color::Rgb(10, 10, 10);
    spec.colors.scrim = huxerui::Color::Rgb(0, 0, 0, 0.62F);
    spec.colors.error = huxerui::Color::Rgb(240, 115, 108);        // 语义红 #F0736C
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
    spec.colors.primary = huxerui::Color::Rgb(37, 99, 235);        // 主题蓝 #2563EB
    spec.colors.on_primary = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.primary_container = huxerui::Color::Rgb(229, 229, 229);
    spec.colors.on_primary_container = huxerui::Color::Rgb(17, 17, 17);
    spec.colors.secondary = huxerui::Color::Rgb(82, 82, 82);        // 中性灰 #525252
    spec.colors.on_secondary = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.secondary_container = huxerui::Color::Rgb(239, 239, 239); // hover / 选中底块
    spec.colors.on_secondary_container = huxerui::Color::Rgb(17, 17, 17);
    spec.colors.tertiary_container = huxerui::Color::Rgb(244, 244, 244);
    spec.colors.on_tertiary_container = huxerui::Color::Rgb(38, 38, 38);
    spec.colors.background = huxerui::Color::Rgb(255, 255, 255);    // 纯白海面 #FFFFFF
    spec.colors.surface = huxerui::Color::Rgb(250, 250, 250);
    spec.colors.surface_container_low = huxerui::Color::Rgb(247, 247, 247);
    spec.colors.surface_container = huxerui::Color::Rgb(242, 242, 242);  // 二级卡片
    spec.colors.surface_container_high = huxerui::Color::Rgb(232, 232, 232); // hover / 普通按钮
    spec.colors.surface_container_highest = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.on_surface = huxerui::Color::Rgb(17, 17, 17);       // 近黑正文
    spec.colors.on_surface_variant = huxerui::Color::Rgb(107, 107, 107); // 次要灰
    spec.colors.outline = huxerui::Color::Rgb(212, 212, 212);       // 中性描边 #D4D4D4
    spec.colors.inverse_surface = huxerui::Color::Rgb(17, 17, 17);
    spec.colors.inverse_on_surface = huxerui::Color::Rgb(250, 250, 250);
    spec.colors.scrim = huxerui::Color::Rgb(0, 0, 0, 0.38F);
    spec.colors.error = huxerui::Color::Rgb(198, 58, 58);           // 语义红 #C63A3A
    spec.interactions.focus_ring.color = spec.colors.primary;
    return spec;
}

} // namespace llmswitch::ui
