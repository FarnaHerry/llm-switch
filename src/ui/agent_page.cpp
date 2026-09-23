// agent_page.cpp — Agent 管理页：顶部 Agent 工具栏与 action group 共用一行，
// 下方 Pager 支持左右拖动；各工具页保留供应商页表单、列表和卡片局部状态。
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

[[huxerui::composable]] huxerui::View AgentToolButton(
    std::string iconName, std::string label,
    huxerui::State<std::size_t> selectedTool, std::size_t toolIndex) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    huxerui::View toolButton =
        huxerui::IconButton(ToolIcon(iconName), label)
            .OnClick([selectedTool, toolIndex] { selectedTool = toolIndex; })
            .With(huxerui::Tooltip(label));
    if (selectedTool.Get() == toolIndex) {
        toolButton = std::move(toolButton).With(
            huxerui::Background(islands.overlay),
            huxerui::CornerRadius(islands.nested_radius));
    }
    return toolButton;
}

[[huxerui::composable]] huxerui::View AgentPager(
    std::shared_ptr<std::vector<huxerui::View>> pages,
    huxerui::State<std::size_t> selectedTool, std::size_t pageCount) {
    auto selectTool = [selectedTool, pageCount](std::size_t index) {
        if (index < pageCount) {
            selectedTool = index;
        }
    };
    return huxerui::Pager(*pages, selectedTool)
        .ScrollAxis(huxerui::Axis::Horizontal)
        .DragEnabled(true)
        .OnChanged(selectTool);
}

[[huxerui::composable]] huxerui::View AgentPage(huxerui::State<int> revision, huxerui::State<std::size_t> navPage) {
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

    auto toolButtonCache =
        huxerui::UseState<std::shared_ptr<std::vector<huxerui::View>>>({});
    std::shared_ptr<std::vector<huxerui::View>> cachedButtons =
        toolButtonCache.Get();
    if (!cachedButtons || cachedButtons->size() != registry.size()) {
        auto nextButtons = std::make_shared<std::vector<huxerui::View>>();
        nextButtons->reserve(registry.size());
        for (std::size_t index = 0; index < registry.size(); ++index) {
            const auto& spec = registry[index];
            const std::string id(spec.id);
            nextButtons->push_back(
                AgentToolButton(std::string(spec.iconName),
                                std::string(spec.displayName), selectedTool, index)
                    .Key("agent-tool:" + id));
        }
        toolButtonCache = nextButtons;
        cachedButtons = std::move(nextButtons);
    }

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
                              navPage, selectedTool, index)
                    .Key("agent-providers:" + id)
                    .With(huxerui::Grow(1.0F)));
        }
        providerPageCache = nextPages;
        cachedPages = std::move(nextPages);
    }

    auto requestAddProvider = [selectedTool, addProviderRequest] {
        const auto& currentRegistry = models::toolRegistry();
        const std::size_t index = selectedTool.Get();
        if (index < currentRegistry.size()) {
            addProviderRequest = std::string(currentRegistry[index].id);
        }
    };

    huxerui::View navigationContainer = huxerui::Row {
        huxerui::Row(*cachedButtons)
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
    // 与 PageScaffold 同一套壳层约束：顶部不留内边距（top = 0，壳层也不留
    // Spacing），Agent 工具栏紧接标题栏下沿；左右边距是壳层标题栏的同一个
    // shellInset，应用名与工具栏左对齐。
    const float inset = compact ? theme.spacing.medium : theme.spacing.large;
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
        AgentPager(cachedPages, selectedTool, registry.size())
            .With(huxerui::Grow(1.0F)),
    }.With(huxerui::Padding(huxerui::EdgeInsets{.top = 0.0F,
                                               .right = inset,
                                               .bottom = inset,
                                               .left = inset}),
           huxerui::Spacing(theme.spacing.medium),
           huxerui::ClipChildren(),
           huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace llmswitch::ui
