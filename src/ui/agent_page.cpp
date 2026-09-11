// agent_page.cpp — Agent 管理页：NavigationBar 与 Pager 共用一个受控索引。
// NavigationBar 固定在页面底部，Pager 保留各工具页；切换工具不会销毁供应商
// 页的表单、列表和卡片局部状态。
#include <huxerui/huxerui.h>

#include <map>
#include <string>
#include <vector>

#include "ui.h"

import llmswitch.models;

namespace llmswitch::ui {

[[huxerui::composable]] huxerui::View AgentPage(huxerui::State<int> revision) {
    const auto& registry = models::toolRegistry();
    if (registry.empty()) {
        return huxerui::Text("没有可用的 Agent 工具");
    }

    auto selectedTool = huxerui::UseState<std::size_t>(0);
    auto usageCache = huxerui::UseState<std::map<std::string, std::string>>({});

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
                            id, revision, usageCache, index == 0)
                            .Key("agent-providers:" + id)
                            .With(huxerui::Grow(1.0F)));
    }

    auto selectTool = [selectedTool](std::size_t index) {
        selectedTool = index;
    };
    return huxerui::Column {
        huxerui::Pager(std::move(pages), selectedTool)
            .ScrollAxis(huxerui::Axis::Horizontal)
            .DragEnabled(false)
            .OnChanged(selectTool)
            .With(huxerui::Grow(1.0F)),
        huxerui::NavigationBar(std::move(navigationItems), selectedTool)
            .OnChanged(selectTool),
    }.With(huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace llmswitch::ui
