// agent_page.cpp — Agent 管理页：顶部 Agent 工具栏与 action group 共用一行，
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

    std::vector<huxerui::SegmentedButtonItem> navigationItems;
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

    // 工具栏使用普通按钮尺寸：继承当前 SegmentedButton 主题，只移除边框、
    // 缩小内边距与图标，并使用项目二级岛表面表达选中态。
    huxerui::SegmentedButtonStyle navigationStyle =
        huxerui::UseEnvironment<huxerui::SegmentedButtonStyle>();
    navigationStyle.background = huxerui::Color::Transparent();
    navigationStyle.selected_background = islands.overlay;
    navigationStyle.label_style.foreground = theme.colors.on_surface_variant;
    navigationStyle.selected_label = theme.colors.on_surface;
    navigationStyle.border = huxerui::Border{huxerui::Color::Transparent(), 0.0F};
    navigationStyle.selected_border =
        huxerui::Border{huxerui::Color::Transparent(), 0.0F};
    navigationStyle.padding = huxerui::EdgeInsets::Symmetric(
        theme.spacing.small, theme.spacing.extra_small);
    navigationStyle.icon_size = 18.0F;
    navigationStyle.icon_spacing = theme.spacing.extra_small;
    navigationStyle.minimum_segment_width = 48.0F;
    navigationStyle.minimum_height = 40.0F;
    navigationStyle.corner_radii = huxerui::CornerRadii{islands.nested_radius};
    huxerui::ThemeDefinition navigationTheme;
    navigationTheme.Set(navigationStyle);

    huxerui::View navigationBar = huxerui::SegmentedButton(
                                      std::move(navigationItems), selectedTool)
                                      .OnChanged(selectTool);
    huxerui::View navigationContainer = huxerui::Row {
        huxerui::Theme(std::move(navigationTheme),
                       std::move(navigationBar)),
    }.With(huxerui::Padding(theme.spacing.extra_small),
           huxerui::Background(islands.raised),
           huxerui::CornerRadius(islands.nested_radius),
           huxerui::ClipChildren(),
           huxerui::Grow(1.0F),
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
