// agent_page.cpp — Agent 管理页：左侧二级图标侧栏（遍历 models::toolRegistry()
// 渲染 5 个 agent 工具）+ 右侧该工具的供应商管理页（ProvidersPage 按工具参数化
// 复用）。二级栏比顶级侧栏窄、以竖 Divider 分隔，视觉层级低于顶级栏。
#include <huxerui/huxerui.h>

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include "ui.h"
#include "app_resources.h"

import llmswitch.config;
import llmswitch.models;

namespace llmswitch::ui {
namespace {

// ToolSpec.iconName → 图标资源对（普通半透明 / 选中实心，resources/README.md
// 有来源与许可表）。未知名回退 agents 图标（不应发生）。
struct IconPair {
    huxerui::ImageResource normal;
    huxerui::ImageResource selected;
};

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

} // namespace

[[huxerui::composable]] huxerui::View AgentPage(huxerui::State<int> revision) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    auto tasks = huxerui::UseTaskScope();
    // 当前工具 id：默认注册表第一个（claude-code）。State 持有字符串，
    // 切工具只重渲染右侧内容区，二级栏按钮不卸载。
    auto currentTool = huxerui::UseState<std::string>(
        std::string(models::toolRegistry().front().id));
    const std::string tool = currentTool.Get();

    // 二级图标栏：选中态用实心变体，悬停显示工具名。
    std::vector<huxerui::View> buttons;
    for (const auto& spec : models::toolRegistry()) {
        const std::string id(spec.id);
        const IconPair icons = ToolIcon(spec.iconName);
        const std::string displayName(spec.displayName);
        buttons.push_back(
            huxerui::IconButton(tool == id ? icons.selected : icons.normal,
                                displayName)
                .OnClick([currentTool, id] { currentTool = id; })
                .With(huxerui::Tooltip(displayName)));
    }

    // Claude Desktop 仅 macOS / Windows：图标照常显示，页面顶部加提示条
    // （providers 的增删改仍可用，只是本平台无法切换生效）。
    const bool claudeUnsupported =
        tool == "claude" && cfg::claudeDesktopDir().empty();

    std::vector<huxerui::View> contentItems;
    if (claudeUnsupported) {
        contentItems.push_back(
            huxerui::Text("Claude Desktop 仅支持 macOS / "
                          "Windows，当前平台切换不可用")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.on_surface_variant})
                .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 6.0F)),
                      huxerui::Background(islands.raised),
                      huxerui::CornerRadius(islands.nested_radius)));
    }
    contentItems.push_back(
        ProvidersPage(tool, revision).Key(tool).With(huxerui::Grow(1.0F)));

    return huxerui::Row {
        huxerui::Column(std::move(buttons))
            .With(huxerui::Padding(theme.spacing.small),
                  huxerui::Spacing(theme.spacing.small),
                  huxerui::Frame{.width = 48.0F},
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Divider(huxerui::Axis::Vertical),
        huxerui::Column(std::move(contentItems))
            .With(huxerui::Spacing(theme.spacing.small),
                  huxerui::Padding(huxerui::EdgeInsets::Symmetric(
                      theme.spacing.small, 0.0F)),
                  huxerui::Grow(1.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
    }.With(huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace llmswitch::ui
