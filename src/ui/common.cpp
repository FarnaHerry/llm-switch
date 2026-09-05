// common.cpp — 岛屿原语（ResolveIslandTheme/IslandSurface）、页面骨架（一级岛）/
// 卡片（二级岛）/ 弹窗卡片等跨页通用部件，以及全局 ProviderStore 持有点。
#include <huxerui/huxerui.h>

#include <string>
#include <string_view>

#include "ui.h"

import llmswitch.config;
import llmswitch.models;
import llmswitch.store;

namespace llmswitch::ui {

store::ProviderStore& providerStore() {
    // 首次访问即加载（含首次导入收编）；进程内唯一实例，UI 线程独占。
    static store::ProviderStore store = store::ProviderStore::load();
    return store;
}

std::string MaskedApiKey(const std::string& key) {
    if (key.empty()) return "未设置";
    if (key.size() < 9) return "****";
    return std::format("{}…{}…{}", key.substr(0, 4), std::string(6, '*'),
                       key.substr(key.size() - 4));
}

std::string_view ToolName(std::string_view tool) {
    // 展示名以注册表为准；未注册（不应发生）原样回显 id。
    if (const auto* spec = models::findTool(tool)) return spec->displayName;
    return tool;
}

IslandTheme ResolveIslandTheme(const huxerui::ThemeSpec& theme) {
    return IslandTheme{
        .page_gap = theme.spacing.small,
        .island_padding = theme.spacing.medium,
        .island_radius = 16.0F,
        .nested_radius = 8.0F,
        .ocean = theme.colors.background,
        .base = theme.colors.surface_container_low,
        .raised = theme.colors.surface_container,
        .overlay = theme.colors.surface_container_highest,
        .outline_soft = theme.colors.outline,
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
    // 一级岛：页面根本身是岛（Grow + Stretch 占满页面区块，圆角 16pt，
    // base 表面），内容在岛内部滚动；海面底色经岛间缝隙透出。
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
           huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View Card(huxerui::View content) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    // 二级岛：raised 表面（比一级岛高一层级）+ 8pt 同心圆角。
    return huxerui::Column { std::move(content) }
        .With(huxerui::Padding(islands.island_padding),
              huxerui::Background(islands.raised),
              huxerui::CornerRadius(islands.nested_radius),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
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
