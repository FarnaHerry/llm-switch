// common.cpp — 轻岛屿原语（ResolveIslandTheme/IslandSurface）、页面骨架（一级岛）/
// 卡片（二级岛）/ 弹窗卡片等跨页通用部件，以及全局 ProviderStore 持有点。
#include <huxerui/huxerui.h>

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

IconPair ToolIcon(std::string_view iconName) {
    if (iconName == "claudecode") {
        return {app::images::claudecode, app::images::claudecode_selected};
    }
    if (iconName == "claude") {
        return {app::images::claude, app::images::claude_selected};
    }
    if (iconName == "codex") {
        return {app::images::codex, app::images::codex_selected};
    }
    if (iconName == "opencode") {
        return {app::images::opencode, app::images::opencode_selected};
    }
    if (iconName == "pi") {
        return {app::images::pi, app::images::pi_selected};
    }
    return {app::images::agents, app::images::agents_selected};
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
    // 二级岛本身“落墨”：不再用规整矢量边框模拟水墨。底层墨框由断续、
    // 不等宽笔触和角部淡晕构成，内容仍按常规约束排版，避免造型损害可用性。
    return huxerui::Stack {
        huxerui::Image(app::images::ink_card_frame)
            .Fit(huxerui::ImageFit::Fill),
        huxerui::Column { std::move(content) }
            .With(huxerui::Padding(islands.island_padding),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
    }.With(huxerui::Background(islands.raised),
           huxerui::CornerRadius(3.0F),
           huxerui::ClipChildren(),
           huxerui::Align(huxerui::HorizontalAlignment::Stretch,
                          huxerui::VerticalAlignment::Stretch));
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
