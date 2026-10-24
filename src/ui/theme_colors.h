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

// 冷调主题：深色「石墨」= 中性石墨海面（#101214）+ 青蓝强调色；浅色「晴石」=
// 冷白纸面（#FDFDFD）+ 天蓝强调色。文本用冷灰-石板阶，交互态（按钮/
// 选中/开关/标题栏 hover）统一由 primary 承担；error 保留语义红（冷调）。
// 浅色强调色比参考图实测的亮蓝（#4595F7）深一档到 #2870D6：白字对比度
// ≈4.8:1、作描边 ≈4.6:1，双双过 AA 正文线（#2F7BE6 时白字仅 4.12:1）。
// 石墨主题（中性冷灰阶）：表面不再发蓝，青蓝强调色成为唯一彩色源，在
// 石墨底上更跳。对比度全项过 AA 且余量优于旧海军蓝（正文 15.3:1）。
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
    spec.colors.primary = huxerui::Color::Rgb(56, 189, 248);       // 青蓝强调 #38BDF8
    spec.colors.on_primary = huxerui::Color::Rgb(6, 34, 49);       // 强调色上翻深墨青
    spec.colors.primary_container = huxerui::Color::Rgb(27, 58, 80);
    spec.colors.on_primary_container = huxerui::Color::Rgb(190, 231, 251);
    spec.colors.secondary = huxerui::Color::Rgb(154, 161, 170);    // 中性石板 #9AA1AA
    spec.colors.on_secondary = huxerui::Color::Rgb(16, 18, 20);
    spec.colors.secondary_container = huxerui::Color::Rgb(35, 40, 46);   // hover / 选中底块
    spec.colors.on_secondary_container = huxerui::Color::Rgb(232, 234, 237);
    spec.colors.tertiary_container = huxerui::Color::Rgb(33, 38, 45);
    spec.colors.on_tertiary_container = huxerui::Color::Rgb(190, 199, 208);
    spec.colors.background = huxerui::Color::Rgb(16, 18, 20);      // 石墨海面 #101214
    spec.colors.surface = huxerui::Color::Rgb(23, 26, 29);
    spec.colors.surface_container_low = huxerui::Color::Rgb(20, 23, 26);
    spec.colors.surface_container = huxerui::Color::Rgb(26, 30, 34);      // 二级岛卡片
    spec.colors.surface_container_high = huxerui::Color::Rgb(33, 37, 41); // hover 面 / 弹窗
    spec.colors.surface_container_highest = huxerui::Color::Rgb(39, 44, 49);
    spec.colors.on_surface = huxerui::Color::Rgb(232, 234, 237);   // 冷白正文 #E8EAED
    spec.colors.on_surface_variant = huxerui::Color::Rgb(154, 161, 170); // 次要冷灰
    spec.colors.outline = huxerui::Color::Rgb(45, 51, 58);         // 石墨描边 #2D333A
    spec.colors.inverse_surface = huxerui::Color::Rgb(232, 234, 237);
    spec.colors.inverse_on_surface = huxerui::Color::Rgb(16, 18, 20);
    spec.colors.scrim = huxerui::Color::Rgb(5, 7, 10, 0.55F);
    spec.colors.error = huxerui::Color::Rgb(240, 115, 108);        // 语义红 #F0736C
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
    // 冷白石面：海面近纯白，岛屿与其只差一档冷灰；卡片走浅蓝灰玻璃面 +
    // 冷灰描边，hover 面提亮为淡蓝。
    spec.colors.primary = huxerui::Color::Rgb(40, 112, 214);       // 天蓝强调 #2870D6
    spec.colors.on_primary = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.primary_container = huxerui::Color::Rgb(220, 234, 253);
    spec.colors.on_primary_container = huxerui::Color::Rgb(18, 58, 107);
    spec.colors.secondary = huxerui::Color::Rgb(91, 108, 129);     // 石板灰 #5B6C81
    spec.colors.on_secondary = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.secondary_container = huxerui::Color::Rgb(233, 240, 248); // hover / 选中底块
    spec.colors.on_secondary_container = huxerui::Color::Rgb(30, 42, 58);
    spec.colors.tertiary_container = huxerui::Color::Rgb(228, 236, 245);
    spec.colors.on_tertiary_container = huxerui::Color::Rgb(43, 59, 78);
    spec.colors.background = huxerui::Color::Rgb(253, 253, 253);   // 冷白海面 #FDFDFD
    spec.colors.surface = huxerui::Color::Rgb(246, 249, 252);
    spec.colors.surface_container_low = huxerui::Color::Rgb(243, 247, 251);
    spec.colors.surface_container = huxerui::Color::Rgb(241, 245, 250);  // 二级岛卡片
    spec.colors.surface_container_high = huxerui::Color::Rgb(233, 240, 248); // hover 面 / 弹窗
    spec.colors.surface_container_highest = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.on_surface = huxerui::Color::Rgb(30, 42, 58);      // 深石板蓝正文 #1E2A3A
    spec.colors.on_surface_variant = huxerui::Color::Rgb(85, 103, 125); // 次要冷灰 #55677D
    spec.colors.outline = huxerui::Color::Rgb(220, 227, 235);      // 冷灰描边 #DCE3EB
    spec.colors.inverse_surface = huxerui::Color::Rgb(30, 42, 58);
    spec.colors.inverse_on_surface = huxerui::Color::Rgb(245, 248, 252);
    spec.colors.scrim = huxerui::Color::Rgb(9, 18, 30, 0.38F);
    spec.colors.error = huxerui::Color::Rgb(198, 58, 58);          // 语义红 #C63A3A（作文字 ≈4.9:1 过 AA）
    return spec;
}

} // namespace llmswitch::ui
