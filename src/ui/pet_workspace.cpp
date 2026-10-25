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
            .Tint(theme.colors.primary)
            .With(huxerui::Frame{.width = 24.0F, .height = 24.0F}),
        huxerui::Column {
            huxerui::Text(module.title, huxerui::TextRole::Label)
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kBody)
                        .WithWeight(huxerui::FontWeight::SemiBold),
                    theme.colors.on_surface}),
            huxerui::Text(module.description)
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.on_surface_variant}),
        }.With(huxerui::Spacing(3.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start),
               huxerui::Grow(1.0F)),
    }.With(huxerui::Spacing(12.0F),
           huxerui::Padding(huxerui::EdgeInsets::Symmetric(16.0F, 14.0F)),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
           huxerui::Grow(1.0F)))
        .With(huxerui::Frame{.min_height = 84.0F},
              huxerui::PointerCursor(huxerui::PointerCursorKind::Hand),
              huxerui::Tooltip("打开此管理模块"))
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

std::string WorkspaceActivityLabel(const PetController& pet) {
    if (pet.waitingForInput.Get()) return "等待处理";
    if (pet.streaming.Get() || pet.agentWorking.Get()) return "进行中";
    if (pet.failure.Get()) return "最近活动失败";
    if (pet.activityUnknown.Get()) return "活动状态未知";
    return "已观察为空闲";
}

std::string WorkspaceActivityDetail(const PetController& pet) {
    const std::vector<std::string> tools = pet.activeTools.Get();
    if (tools.empty()) {
        if (pet.activityUnknown.Get()) {
            return "尚无足够事件确认活动状态；未接入来源不会被当作空闲。";
        }
        return "当前没有观察到活跃 Agent。";
    }

    std::string detail = "活跃 Agent：";
    for (std::size_t i = 0; i < tools.size(); ++i) {
        if (i != 0) detail += "、";
        detail += ToolName(tools[i]);
    }
    return detail;
}

} // namespace

[[huxerui::composable]] huxerui::View WorkspaceHomePage(
    huxerui::State<std::size_t> navPage,
    huxerui::State<std::size_t> workspacePage) {
    if (navPage.Get() != pages::kWorkspace ||
        workspacePage.Get() != workspace_pages::kHome) {
        return huxerui::View{};
    }
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const auto pet = huxerui::UseService<PetController>();
    const std::string activityLabel = WorkspaceActivityLabel(*pet);
    const std::string activityDetail = WorkspaceActivityDetail(*pet);
    const std::int64_t todayRequests = pet->todayRequests.Get();

    const std::array<WorkspaceModuleInfo, 8> modules{
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
                            "管理代理开关、端口和请求日志", pages::kRouter},
        WorkspaceModuleInfo{app::images::mcp, "MCP 服务器",
                            "管理本机 MCP 清单与工具配置", pages::kMcp},
        WorkspaceModuleInfo{app::images::skills, "Skills 技能库",
                            "管理中央技能库和工具侧同步", pages::kSkills},
        WorkspaceModuleInfo{app::images::settings, "桌宠与应用设置",
                            "调整启动、外观、托盘和活动来源", pages::kSettings},
        WorkspaceModuleInfo{app::images::about, "关于 llm-switch",
                            "查看版本、技术栈与许可信息", pages::kAbout},
    };

    std::vector<huxerui::View> moduleRows;
    moduleRows.reserve(modules.size() / 2);
    for (std::size_t i = 0; i < modules.size(); i += 2) {
        moduleRows.push_back(huxerui::Row {
            WorkspaceShortcut(modules[i], navPage, workspacePage),
            WorkspaceShortcut(modules[i + 1], navPage, workspacePage),
        }.With(huxerui::Spacing(theme.spacing.medium),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
    }

    huxerui::View activity = Card(huxerui::Row {
        huxerui::Column {
            huxerui::Text("桌宠活动")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.on_surface_variant}),
            huxerui::Text(activityLabel, huxerui::TextRole::Title)
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kTitle)
                        .WithWeight(huxerui::FontWeight::Bold),
                    theme.colors.on_surface}),
            huxerui::Text(activityDetail)
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kBody),
                    theme.colors.on_surface_variant}),
        }.With(huxerui::Spacing(6.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start),
               huxerui::Grow(1.0F)),
        huxerui::Column {
            huxerui::Text(std::format("{}", todayRequests))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kTitle)
                        .WithWeight(huxerui::FontWeight::Bold),
                    theme.colors.on_surface}),
            huxerui::Text("本地路由今日请求")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.on_surface_variant}),
        }.With(huxerui::Spacing(2.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::End)),
    }.With(huxerui::Spacing(theme.spacing.large),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));

    huxerui::View modulesContent = huxerui::Column{std::move(moduleRows)}
        .With(huxerui::Spacing(theme.spacing.medium),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    return PageScaffold(
        "桌宠工作台", huxerui::View{},
        huxerui::ScrollView {
            huxerui::Column {
                std::move(activity),
                PageSection(
                    huxerui::Text("管理模块", huxerui::TextRole::Label),
                    std::move(modulesContent)),
                huxerui::Text(
                    "Agent 供应商、会话与统计已接入工作台；其余模块暂沿用原页面。"
                    "顶部莲花菜单仍可直接打开全部原管理页面。")
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
    huxerui::State<std::size_t> selectedTool) {
    auto addProviderRequest = huxerui::UseState<std::string>({});
    const std::size_t current = workspacePage.Get();
    return huxerui::IndexedPages(
               std::vector<huxerui::View>{
                   WorkspaceHomePage(navPage, workspacePage)
                       .Key("workspace-home"),
                   WorkspaceSessionsPage(navPage, workspacePage)
                       .Key("workspace-sessions"),
                   WorkspaceStatsPage(navPage, workspacePage)
                       .Key("workspace-stats"),
                   WorkspaceAgentPage(revision, navPage, workspacePage,
                                      selectedTool, addProviderRequest)
                       .Key("workspace-agents"),
               },
               current)
        .With(huxerui::Grow(1.0F));
}

} // namespace llmswitch::ui
