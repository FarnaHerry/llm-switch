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

// 黑白双色主题：中性色阶参考 Clash-Flux 的 FluxDarkColors / FluxLightColors，
// 用稳定的明度差区分海面、卡片和浮层；除状态语义色（error/success/warning）
// 外，只有主要操作按钮、选中态与焦点使用主题蓝。背景、卡片、描边、图标和
// 装饰一律走黑白灰。主题蓝在深浅两态都取 #2563EB：白字对比度 5.17:1，在
// 浅底上作控件填充也保持 4.9:1；深色下不作小号正文色，只用于填充、边框和图形。
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
    spec.colors.primary_container = huxerui::Color::Rgb(35, 36, 37);
    spec.colors.on_primary_container = huxerui::Color::Rgb(241, 241, 241);
    spec.colors.secondary = huxerui::Color::Rgb(185, 185, 185);    // 中性灰 #B9B9B9
    spec.colors.on_secondary = huxerui::Color::Rgb(32, 32, 32);
    spec.colors.secondary_container = huxerui::Color::Rgb(43, 43, 43);   // hover / 选中底块
    spec.colors.on_secondary_container = huxerui::Color::Rgb(226, 226, 226);
    spec.colors.tertiary_container = huxerui::Color::Rgb(36, 36, 36);
    spec.colors.on_tertiary_container = huxerui::Color::Rgb(222, 222, 222);
    spec.colors.background = huxerui::Color::Rgb(26, 27, 28);       // 海面 #1A1B1C
    spec.colors.surface = huxerui::Color::Rgb(22, 23, 24);         // #161718
    spec.colors.surface_container_low = huxerui::Color::Rgb(20, 21, 22);  // 页面岛 #141516
    spec.colors.surface_container = huxerui::Color::Rgb(30, 31, 32);      // 强调卡片 #1E1F20
    spec.colors.surface_container_high = huxerui::Color::Rgb(36, 37, 38); // hover / 普通按钮
    spec.colors.surface_container_highest = huxerui::Color::Rgb(42, 43, 44); // 浮层 #2A2B2C
    spec.colors.on_surface = huxerui::Color::Rgb(241, 241, 241);    // 近白正文
    spec.colors.on_surface_variant = huxerui::Color::Rgb(176, 176, 176); // 次要灰
    spec.colors.outline = huxerui::Color::Rgb(65, 65, 65);          // 中性描边 #414141
    spec.colors.inverse_surface = huxerui::Color::Rgb(232, 232, 232);
    spec.colors.inverse_on_surface = huxerui::Color::Rgb(32, 32, 32);
    spec.colors.scrim = huxerui::Color::Rgb(7, 7, 7, 0.66F);
    spec.colors.error = huxerui::Color::Rgb(255, 144, 153);         // 语义红 #FF9099
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
    spec.colors.on_primary_container = huxerui::Color::Rgb(36, 38, 41);
    spec.colors.secondary = huxerui::Color::Rgb(94, 104, 114);      // #5E6872
    spec.colors.on_secondary = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.secondary_container = huxerui::Color::Rgb(227, 231, 235); // hover / 选中底块
    spec.colors.on_secondary_container = huxerui::Color::Rgb(48, 56, 64);
    spec.colors.tertiary_container = huxerui::Color::Rgb(236, 238, 240);
    spec.colors.on_tertiary_container = huxerui::Color::Rgb(63, 70, 77);
    spec.colors.background = huxerui::Color::Rgb(250, 250, 251);    // 海面 #FAFAFB
    spec.colors.surface = huxerui::Color::Rgb(254, 254, 255);       // #FEFEFF
    spec.colors.surface_container_low = huxerui::Color::Rgb(241, 242, 244); // 页面岛 #F1F2F4
    spec.colors.surface_container = huxerui::Color::Rgb(254, 254, 255);     // 强调卡片
    spec.colors.surface_container_high = huxerui::Color::Rgb(248, 249, 251); // hover / 浮层
    spec.colors.surface_container_highest = huxerui::Color::Rgb(254, 254, 255);
    spec.colors.on_surface = huxerui::Color::Rgb(36, 38, 41);       // 近黑正文 #242629
    spec.colors.on_surface_variant = huxerui::Color::Rgb(104, 108, 114); // 次要灰 #686C72
    spec.colors.outline = huxerui::Color::Rgb(211, 215, 221);       // #D3D7DD
    spec.colors.inverse_surface = huxerui::Color::Rgb(38, 40, 44);
    spec.colors.inverse_on_surface = huxerui::Color::Rgb(244, 246, 249);
    spec.colors.scrim = huxerui::Color::Rgb(27, 31, 38, 0.34F);
    spec.colors.error = huxerui::Color::Rgb(180, 35, 50);           // 语义红 #B42332
    spec.interactions.focus_ring.color = spec.colors.primary;
    return spec;
}

// 桌宠透明窗口上的玻璃药丸/状态条：颜色必须从当前主题派生，不能固定写死
// 深色底。深色主题下是深灰玻璃，浅色主题下是近白玻璃；文字、描边同步翻转。
inline huxerui::Color GlassFill(const huxerui::ThemeSpec& theme, float alpha = 0.86F) {
    huxerui::Color color = theme.colors.surface_container_highest;
    color.alpha = alpha;
    return color;
}

inline huxerui::Color GlassBorder(const huxerui::ThemeSpec& theme,
                                  float alpha = 0.32F) {
    huxerui::Color color = theme.colors.outline;
    color.alpha = alpha;
    return color;
}

} // namespace llmswitch::ui
