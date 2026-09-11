// agent_page.cpp — Agent 管理页：工具选择与页面内容共用一个受控索引。
// 供应商岛屿内部保留原来的工具图标栏，Pager 只负责保留各工具页；切换工具
// 不会销毁供应商页的表单、列表和卡片局部状态。
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

    std::vector<huxerui::View> pages;
    pages.reserve(registry.size());
    for (std::size_t index = 0; index < registry.size(); ++index) {
        const auto& spec = registry[index];
        const std::string id(spec.id);
        pages.push_back(ProvidersPage(
                            id, selectedTool, revision, usageCache, index == 0)
                            .Key("agent-providers:" + id)
                            .With(huxerui::Grow(1.0F)));
    }

    return huxerui::Pager(std::move(pages), selectedTool)
            .ScrollAxis(huxerui::Axis::Horizontal)
            .DragEnabled(false)
            .OnChanged([selectedTool](std::size_t index) {
                selectedTool = index;
            })
            .With(huxerui::Grow(1.0F));
}

} // namespace llmswitch::ui
