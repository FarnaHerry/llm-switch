// agent_page.cpp — Agent 管理页：顶部 Agent 工具栏与 action group 共用一行，
// Pager 只切换下方 page；切换工具不会销毁供应商页的表单、列表和卡片局部状态。
#include <huxerui/huxerui.h>

#include <cstddef>
#include <map>
#include <memory>
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

    std::vector<huxerui::View> toolButtons;
    toolButtons.reserve(registry.size());
    auto providerPageCache =
        huxerui::UseState<std::shared_ptr<std::vector<huxerui::View>>>({});
    std::shared_ptr<std::vector<huxerui::View>> cachedPages =
        providerPageCache.Get();
    if (!cachedPages || cachedPages->size() != registry.size()) {
        auto nextPages = std::make_shared<std::vector<huxerui::View>>();
        nextPages->reserve(registry.size());
        for (std::size_t index = 0; index < registry.size(); ++index) {
            const auto& spec = registry[index];
            const std::string id(spec.id);
            nextPages->push_back(
                ProvidersPage(id, revision, usageCache, addProviderRequest,
                              index == 0)
                    .Key("agent-providers:" + id)
                    .With(huxerui::Grow(1.0F)));
        }
        providerPageCache = nextPages;
        cachedPages = std::move(nextPages);
    }

    for (std::size_t index = 0; index < registry.size(); ++index) {
        const auto& spec = registry[index];
        const std::string label(spec.displayName);
        huxerui::View toolButton =
            huxerui::IconButton(ToolIcon(spec.iconName), label)
                .OnClick([selectedTool, index] { selectedTool = index; })
                .With(huxerui::Tooltip(label));
        if (selectedTool.Get() == index) {
            toolButton = std::move(toolButton).With(
                huxerui::Background(islands.overlay),
                huxerui::CornerRadius(islands.nested_radius));
        }
        toolButtons.push_back(std::move(toolButton));
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

    huxerui::View navigationContainer = huxerui::Row {
        huxerui::Row(std::move(toolButtons))
            .With(huxerui::Spacing(theme.spacing.extra_small),
                  huxerui::MainAlign(huxerui::MainAxisAlignment::Start),
                  huxerui::CrossAlign(
                      huxerui::CrossAxisAlignment::Center)),
    }.With(huxerui::Padding(theme.spacing.extra_small),
           huxerui::Background(islands.raised),
           huxerui::CornerRadius(islands.nested_radius),
           huxerui::ClipChildren(),
           huxerui::MainAlign(huxerui::MainAxisAlignment::Start),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    return huxerui::Column {
        huxerui::Row {
            std::move(navigationContainer),
            huxerui::Row {
                huxerui::IconButton(app::images::add, "新增供应商")
                    .OnClick(requestAddProvider)
                    .With(huxerui::Tooltip("新增供应商")),
            }.With(huxerui::Spacing(theme.spacing.small),
                   huxerui::CrossAlign(
                       huxerui::CrossAxisAlignment::Center)),
        }.With(huxerui::Spacing(theme.spacing.small),
               huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Pager(*cachedPages, selectedTool)
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
