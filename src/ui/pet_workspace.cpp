// pet_workspace.cpp — 桌宠工作台首页与原管理页面的过渡导航。
#include <huxerui/huxerui.h>

#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "app.h"
#include "app_resources.h"
#include "pet.h"
#include "ui.h"

namespace llmswitch::ui {
namespace {

struct WorkspaceModuleInfo {
    huxerui::ImageResource icon;
    const char* title;
    const char* description;
    std::size_t page;
    std::size_t workspacePage = workspace_pages::kHome;
};

[[huxerui::composable]] huxerui::View WorkspaceShortcut(
    WorkspaceModuleInfo module, huxerui::State<std::size_t> navPage,
    huxerui::State<std::size_t> workspacePage) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return QuietCard(huxerui::Row {
        huxerui::Image(module.icon)
            .Tint(theme.colors.on_surface_variant)
            .With(huxerui::Frame{.width = 24.0F, .height = 24.0F}),
        huxerui::Text(module.title, huxerui::TextRole::Label)
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody)
                    .WithWeight(huxerui::FontWeight::SemiBold),
                theme.colors.on_surface})
            .With(huxerui::Grow(1.0F)),
    }.With(huxerui::Spacing(12.0F),
           huxerui::Padding(huxerui::EdgeInsets::Symmetric(12.0F, 8.0F)),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
           huxerui::Grow(1.0F)))
        .With(huxerui::Frame{.min_height = 48.0F},
              huxerui::PointerCursor(huxerui::PointerCursorKind::Hand),
              huxerui::Tooltip(module.description))
        .OnClick([navPage, workspacePage, module] {
            if (module.workspacePage != workspace_pages::kHome) {
                workspacePage = module.workspacePage;
                navPage = pages::kWorkspace;
            } else {
                workspacePage = workspace_pages::kHome;
                navPage = module.page;
            }
        })
        .Key(std::format("workspace:{}", module.title));
}

std::array<WorkspaceModuleInfo, 8> WorkspaceModules() {
    return {
        WorkspaceModuleInfo{app::images::agents, "Agent 与供应商",
                            "管理工具、供应商和模型映射", pages::kWorkspace,
                            workspace_pages::kAgents},
        WorkspaceModuleInfo{app::images::sessions, "会话历史",
                            "查看、搜索、导出或删除历史会话", pages::kWorkspace,
                            workspace_pages::kSessions},
        WorkspaceModuleInfo{app::images::stats, "使用统计",
                            "查看请求量、token 与供应商分布", pages::kWorkspace,
                            workspace_pages::kStats},
        WorkspaceModuleInfo{app::images::router, "本地路由",
                            "管理代理开关、端口和请求日志", pages::kWorkspace,
                            workspace_pages::kRouter},
        WorkspaceModuleInfo{app::images::mcp, "MCP 服务器",
                            "管理本机 MCP 清单与工具配置", pages::kWorkspace,
                            workspace_pages::kMcp},
        WorkspaceModuleInfo{app::images::skills, "Skills 技能库",
                            "管理中央技能库和工具侧同步", pages::kWorkspace,
                            workspace_pages::kSkills},
        WorkspaceModuleInfo{app::images::settings, "桌宠与应用设置",
                            "调整启动、外观、托盘和活动来源", pages::kWorkspace,
                            workspace_pages::kSettings},
        WorkspaceModuleInfo{app::images::about, "关于 llm-switch",
                            "查看版本、技术栈与许可信息", pages::kWorkspace,
                            workspace_pages::kAbout},
    };
}

[[huxerui::composable]] huxerui::View WorkspaceModuleSidebar(
    huxerui::State<std::size_t> navPage,
    huxerui::State<std::size_t> workspacePage) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const auto modules = WorkspaceModules();
    std::vector<huxerui::View> shortcuts;
    shortcuts.reserve(modules.size());
    for (const auto& module : modules) {
        shortcuts.push_back(WorkspaceShortcut(module, navPage, workspacePage));
    }
    return huxerui::Column {
        huxerui::Text("功能卡片", huxerui::TextRole::Label),
        huxerui::ScrollView {
            huxerui::Column{std::move(shortcuts)}
                .With(huxerui::Spacing(theme.spacing.small),
                      huxerui::CrossAlign(
                          huxerui::CrossAxisAlignment::Stretch)),
        }.With(huxerui::Grow(1.0F)),
    }.With(huxerui::Frame{.width = 228.0F},
           huxerui::Padding(huxerui::EdgeInsets::Symmetric(
               theme.spacing.medium, 0.0F)),
           huxerui::Spacing(theme.spacing.medium),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace

[[huxerui::composable]] huxerui::View WorkspaceHomePage(
    huxerui::State<std::size_t> navPage,
    huxerui::State<std::size_t> workspacePage,
    huxerui::State<std::size_t> selectedTool) {
    if (navPage.Get() != pages::kWorkspace ||
        workspacePage.Get() != workspace_pages::kHome) {
        return huxerui::View{};
    }
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();

    return PageScaffold(
        "桌宠信息卡", huxerui::View{},
        huxerui::ScrollView {
            huxerui::Column {
                PetInfoSummary(workspacePage, selectedTool),
                huxerui::Text(
                    "所有管理功能可从左侧卡片直接在此信息卡内使用；顶部莲花菜单保留原控制中心页面。")
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kCaption),
                        theme.colors.on_surface_variant}),
            }.With(huxerui::Spacing(theme.spacing.large),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        }.With(huxerui::Grow(1.0F)));
}

[[huxerui::composable]] huxerui::View PetWorkspacePage(
    huxerui::State<std::size_t> navPage,
    huxerui::State<std::size_t> workspacePage,
    huxerui::State<int> revision,
    huxerui::State<std::size_t> selectedTool,
    huxerui::State<int> themeMode,
    huxerui::State<std::size_t> startupMode,
    huxerui::State<bool> trayEnabled) {
    auto addProviderRequest = huxerui::UseState<std::string>({});
    const std::size_t current = workspacePage.Get();
    huxerui::View content = huxerui::IndexedPages(
               std::vector<huxerui::View>{
                   WorkspaceHomePage(navPage, workspacePage, selectedTool)
                       .Key("workspace-home"),
                   WorkspaceSessionsPage(navPage, workspacePage)
                       .Key("workspace-sessions"),
                   WorkspaceStatsPage(navPage, workspacePage)
                       .Key("workspace-stats"),
                   WorkspaceAgentPage(revision, navPage, workspacePage,
                                      selectedTool, addProviderRequest)
                       .Key("workspace-agents"),
                   WorkspaceRouterPage(navPage, workspacePage)
                       .Key("workspace-router"),
                   WorkspaceMcpPage(revision).Key("workspace-mcp"),
                   WorkspaceSkillsPage(revision).Key("workspace-skills"),
                   SettingsPage(themeMode, revision, startupMode, trayEnabled,
                                navPage, workspacePage, true)
                       .Key("workspace-settings"),
                   AboutPage().Key("workspace-about"),
               },
               current)
        .With(huxerui::Grow(1.0F));
    return huxerui::Row {
        WorkspaceModuleSidebar(navPage, workspacePage),
        std::move(content),
    }.With(huxerui::Spacing(8.0F), huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace llmswitch::ui
