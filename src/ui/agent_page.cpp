// agent_page.cpp — Agent 管理页：顶部 NavigationBar 与 action group 共用一行，
// Pager 只切换下方 page；切换工具不会销毁供应商页的表单、列表和卡片局部状态。
#include <huxerui/huxerui.h>

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "ui.h"
#include "app_resources.h"

import llmswitch.models;

namespace llmswitch::ui {

[[huxerui::composable]] huxerui::View AgentPage(huxerui::State<int> revision) {
    const auto& registry = models::toolRegistry();
    if (registry.empty()) {
        return huxerui::Text("没有可用的 Agent 工具");
    }

    auto selectedTool = huxerui::UseState<std::size_t>(0);
    auto usageCache = huxerui::UseState<std::map<std::string, std::string>>({});
    auto addProviderRequest = huxerui::UseState<std::string>({});
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;

    std::vector<huxerui::NavigationItem> navigationItems;
    std::vector<huxerui::View> pages;
    navigationItems.reserve(registry.size());
    pages.reserve(registry.size());
    for (std::size_t index = 0; index < registry.size(); ++index) {
        const auto& spec = registry[index];
        const std::string id(spec.id);
        navigationItems.emplace_back(
            ToolIcon(spec.iconName), std::string(spec.displayName));
        pages.push_back(ProvidersPage(
                            id, revision, usageCache, addProviderRequest,
                            index == 0)
                            .Key("agent-providers:" + id)
                            .With(huxerui::Grow(1.0F)));
    }

    auto selectTool = [selectedTool](std::size_t index) {
        selectedTool = index;
    };
    auto requestAddProvider = [selectedTool, addProviderRequest] {
        const auto& currentRegistry = models::toolRegistry();
        const std::size_t index = selectedTool.Get();
        if (index < currentRegistry.size()) {
            addProviderRequest = std::string(currentRegistry[index].id);
        }
    };
    return huxerui::Column {
        huxerui::Row {
            huxerui::NavigationBar(std::move(navigationItems), selectedTool)
                .OnChanged(selectTool)
                .With(huxerui::Grow(1.0F)),
            huxerui::Row {
                huxerui::IconButton(app::images::add, "新增供应商")
                    .OnClick(requestAddProvider)
                    .With(huxerui::Tooltip("新增供应商")),
            }.With(huxerui::Spacing(theme.spacing.small),
                   huxerui::CrossAlign(
                       huxerui::CrossAxisAlignment::Center)),
        }.With(huxerui::Spacing(theme.spacing.small),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Pager(std::move(pages), selectedTool)
            .ScrollAxis(huxerui::Axis::Horizontal)
            .DragEnabled(false)
            .OnChanged(selectTool)
            .With(huxerui::Grow(1.0F)),
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

} // namespace llmswitch::ui
