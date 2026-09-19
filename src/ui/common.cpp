// common.cpp — 轻岛屿原语（ResolveIslandTheme/IslandSurface）、页面骨架（一级岛）/
// 卡片（二级岛）/ 弹窗卡片等跨页通用部件，以及全局 ProviderStore 持有点。
#include <huxerui/huxerui.h>

#include <cmath>
#include <string>
#include <string_view>

#include "ui.h"
#include "app_resources.h"

import llmswitch.config;
import llmswitch.models;
import llmswitch.store;

namespace llmswitch::ui {

store::ProviderStore& providerStore() {
    // 首次访问即加载（含首次导入收编）；进程内唯一实例，UI 线程独占。
    static store::ProviderStore store = store::ProviderStore::load();
    return store;
}

huxerui::ImageResource ToolIcon(std::string_view iconName) {
    if (iconName == "claudecode") {
        return app::images::claudecode;
    }
    if (iconName == "claude") {
        return app::images::claude;
    }
    if (iconName == "codex") {
        return app::images::codex;
    }
    if (iconName == "opencode") {
        return app::images::opencode;
    }
    if (iconName == "pi") {
        return app::images::pi;
    }
    if (iconName == "dsh") {
        return app::images::dsh;
    }
    if (iconName == "hermes") {
        return app::images::hermes;
    }
    if (iconName == "gemini") {
        return app::images::gemini;
    }
    if (iconName == "qwen") {
        return app::images::qwen;
    }
    if (iconName == "zcode") {
        return app::images::zcode;
    }
    return app::images::agents;
}

std::string_view ToolName(std::string_view tool) {
    // 展示名以注册表为准；未注册（不应发生）原样回显 id。
    if (const auto* spec = models::findTool(tool)) return spec->displayName;
    return tool;
}

IslandTheme ResolveIslandTheme(const huxerui::ThemeSpec& theme) {
    // 轻岛屿：页面更接近连续宣纸/玄墨画布，实体卡片仍保留足够承托；
    // overlay 保持近不透明，确保弹层在全景水墨上可读。
    const auto translucent = [](huxerui::Color c, float a) {
        c.alpha = a;
        return c;
    };
    // 状态色深浅分档：ThemeSpec 无 success/warning 槽位，按海面亮度（WCAG
    // 相对亮度）选一组——浅色用深一档的绿/琥珀保证作文字 ≥4.5:1，深色维持
    // 在深底上对比度本就充足的亮色。
    const auto luminance = [](huxerui::Color c) {
        const auto channel = [](float v) {
            return v <= 0.04045F ? v / 12.92F
                                 : std::pow((v + 0.055F) / 1.055F, 2.4F);
        };
        return 0.2126F * channel(c.red) + 0.7152F * channel(c.green) +
               0.0722F * channel(c.blue);
    };
    const bool dark = luminance(theme.colors.background) < 0.5F;
    return IslandTheme{
        .page_gap = theme.spacing.extra_small,
        .island_padding = theme.spacing.medium,
        .island_radius = 10.0F,
        .nested_radius = 6.0F,
        .ocean = theme.colors.background,
        .base = translucent(theme.colors.surface_container_low, 0.48F),
        .raised = translucent(theme.colors.surface_container, 0.78F),
        .overlay = translucent(theme.colors.surface_container_highest, 0.95F),
        .outline_soft = translucent(theme.colors.outline, 0.62F),
        .success = dark ? huxerui::Color::Rgb(22, 163, 74)   // #16A34A
                        : huxerui::Color::Rgb(21, 128, 61),  // #15803D
        .on_success = dark ? huxerui::Color::Rgb(6, 34, 49)  // 白字仅 3.3:1，翻墨青
                           : huxerui::Color::Rgb(255, 255, 255),
        .warning = dark ? huxerui::Color::Rgb(202, 138, 4)   // #CA8A04
                        : huxerui::Color::Rgb(180, 84, 10),  // #B4540A
    };
}

namespace {

huxerui::Color IslandColor(const IslandTheme& islands, IslandLevel level) {
    switch (level) {
        case IslandLevel::Base: return islands.base;
        case IslandLevel::Raised: return islands.raised;
        case IslandLevel::Overlay: return islands.overlay;
    }
    return islands.base;
}

} // namespace

[[huxerui::composable]] huxerui::View IslandSurface(huxerui::View content,
                                                    IslandLevel level) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    // composable 形参被 codegen 固定为 const：拷贝到局部再走右值 With 链。
    huxerui::View surface = content;
    return std::move(surface).With(huxerui::Background(IslandColor(islands, level)),
                                   huxerui::CornerRadius(islands.island_radius),
                                   huxerui::Padding(islands.island_padding));
}

[[huxerui::composable]] huxerui::View PageScaffold(const std::string& title,
                                                   huxerui::View actions,
                                                   huxerui::View content) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    // 响应式：Compact(<600) 收窄一级岛内边距。
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    // 一级轻岛：Grow + Stretch 占满页面区块，低对比半透明表面让环境水墨
    // 隐约透出；内容在岛内部滚动。
    huxerui::View body = content;
    return huxerui::Column {
        huxerui::Row {
            huxerui::Text(title, huxerui::TextRole::Title),
            huxerui::Spacer(),
            std::move(actions),
        }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        std::move(body).With(huxerui::Grow(1.0F)),
    }.With(huxerui::Padding(compact ? theme.spacing.medium
                                    : theme.spacing.large),
           huxerui::Spacing(theme.spacing.medium),
           huxerui::Background(islands.base),
           huxerui::CornerRadius(islands.island_radius),
           huxerui::Border(islands.outline_soft, 0.75F),
           huxerui::ClipChildren(),
           huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View Card(huxerui::View content) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    huxerui::View card = content;
    // 冷调主题下卡片与页面只差一档冷灰（半透明表面叠在环境光上），单靠底色
    // 不足以划出卡片边界，补一条 1pt 主题描边——仍是单层轻量样式，不引入
    // SVG 边框或裁剪层。
    return std::move(card).With(
        huxerui::Background(islands.raised),
        huxerui::CornerRadius(islands.nested_radius),
        huxerui::Border(islands.outline_soft, 1.0F),
        huxerui::Padding(islands.island_padding));
}

[[huxerui::composable]] huxerui::View DialogCard(huxerui::View content) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    huxerui::View card = content;
    return std::move(card).With(
        huxerui::Shadow{huxerui::Color::Rgb(0, 0, 0, 0.24F), {}, 24.0F, 0.0F},
        huxerui::Background(islands.overlay),
        huxerui::CornerRadius(islands.island_radius),
        huxerui::Border(islands.outline_soft, 1.0F),
        huxerui::ClipChildren(),
        huxerui::Padding(islands.island_padding));
}

} // namespace llmswitch::ui
