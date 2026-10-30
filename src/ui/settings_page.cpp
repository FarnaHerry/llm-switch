// settings_page.cpp — 设置页：外观主题用三枚普通图标（深色/浅色/跟随系统）
// 单选，存 AppConfig.themeMode 的 system/dark/light 并即时生效；Claude Code
// 安装检查、live 配置文件路径展示、导入/导出。版本与项目信息在关于页，
// 用量查询开关与刷新间隔在各供应商的独立用量配置页维护。
//
// 导入/导出优先走 FilePicker 系统文件对话框（SaveFileAsync/OpenFileAsync）；
// 平台不可用（CanSaveFiles/CanOpenFiles 为 false，如无 xdg-desktop-portal）时
// 回落：导出到 dataDir()/exports/ 并展示路径，导入提供路径输入框。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <filesystem>
#include <string_view>
#include <string>
#include <vector>

#include "ui.h"
#include "app_resources.h"
#include "pet.h"

import llmswitch.config;
import llmswitch.models;
import llmswitch.store;
import llmswitch.agents;
import llmswitch.agents.bridge;
import llmswitch.agents.hook_manager;

namespace llmswitch::ui {
namespace {

// 主题存值仍是 system/dark/light；选择器只显示图标，不显示文字。
const std::vector<std::string> kThemeModes{"system", "dark", "light"};

std::size_t CloseBehaviorIndex(std::string_view behavior) {
    if (behavior == "tray") return 1;
    if (behavior == "quit") return 2;
    return 0;
}

::agents::hooks::HookStatus ReadHookStatus(std::string_view tool) noexcept {
    try {
        return ::agents::hooks::Inspect(tool);
    } catch (...) {
        return {};
    }
}

[[huxerui::composable]] huxerui::View ThemeModeSelector(
    huxerui::State<int> themeMode) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool systemDark =
        huxerui::UseSystemThemeBrightness() == huxerui::ThemeBrightness::Dark;
    const auto sceneTransition = huxerui::UseSceneTransition();
    const int mode = themeMode.Get();

    huxerui::IconButtonStyle iconOn = huxerui::IconButtonStyle::Default();
    iconOn.foreground = theme.colors.primary;
    iconOn.icon_size = 20.0F;
    iconOn.minimum_interactive_size = 36.0F;
    iconOn.state_layer_size = 30.0F;
    iconOn.corner_radius = 18.0F;
    huxerui::IconButtonStyle iconOff = iconOn;
    iconOff.foreground = theme.colors.on_surface_variant;
    huxerui::ThemeDefinition iconOnTheme;
    iconOnTheme.Set(iconOn);
    huxerui::ThemeDefinition iconOffTheme;
    iconOffTheme.Set(iconOff);

    const auto select = [sceneTransition, themeMode, systemDark](int next) {
        if (next == themeMode.Get()) return;
        const auto resolvesToDark = [systemDark](int selected) {
            return selected == 1 || (selected == 0 && systemDark);
        };
        huxerui::TransitionSpec transition{
            huxerui::CircularRevealTransition{},
            huxerui::TweenSpec{.duration = 0.36},
        };
        if (resolvesToDark(themeMode.Get()) && !resolvesToDark(next)) {
            transition = transition.Reversed();
        }
        sceneTransition.RunFromCurrentInteraction(
            std::move(transition), [themeMode, next] {
                themeMode = next;
                providerStore().setThemeMode(kThemeModes[next]);
            });
    };

    const auto button = [&theme, &iconOnTheme, &iconOffTheme, mode, select](
                            huxerui::ImageResource resource, int value,
                            std::string_view label) {
        const bool selected = mode == value;
        huxerui::View button =
            huxerui::IconButton(std::move(resource), std::string(label))
                .OnClick([select, value] { select(value); })
                .With(huxerui::Tooltip(std::string(label)));
        // 背景必须加在 Theme 内部的按钮上；Theme 是 Environment 节点，
        // 不能给 Environment 节点声明 Background 等视图行为。
        if (selected) {
            button = std::move(button).With(
                huxerui::Background(theme.colors.surface_container_high),
                huxerui::CornerRadius(18.0F));
        }
        return huxerui::Theme(selected ? iconOnTheme : iconOffTheme,
                              std::move(button));
    };

    return huxerui::Row {
        button(app::images::theme_dark, 1, "深色模式"),
        button(app::images::theme_light, 2, "浅色模式"),
        button(app::images::theme_system, 0, "跟随系统"),
    }.With(huxerui::Spacing(2.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
           sceneTransition.Anchor());
}

// 版本号编译期常量由顶层 CMake 注入（hcg 不支持 composable 内条件编译，
// 字符串在文件作用域先拼好）。
[[huxerui::composable]] huxerui::View SettingRow(const std::string& label,
                                                 const std::string& hint,
                                                 huxerui::View control) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Row {
        huxerui::Column {
            huxerui::Text(label).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody), theme.colors.on_surface}),
            hint.empty()
                ? huxerui::View{huxerui::Row{}}
                : huxerui::View{huxerui::Text(hint).Style(huxerui::TextStyle{
                      huxerui::Font::System(font_size::kCaption),
                      theme.colors.on_surface_variant})},
        }.With(huxerui::Spacing(2.0F)),
        huxerui::Spacer(),
        control,
    }.With(huxerui::Spacing(12.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

// 本地环境检查的一行：工具图标 + 名称 + 检测结果。未检测到且该工具有可核实的
// 官方安装命令时，给一个「复制安装命令」按钮（走应用剪贴板服务）。
[[huxerui::composable]] huxerui::View ToolStatusRow(
    const models::ToolSpec& spec, const std::string& detectedPath,
    std::shared_ptr<huxerui::Clipboard> clipboard, huxerui::ToastHandle toast) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool installed = !detectedPath.empty();
    const std::string command(spec.installCommand);
    return huxerui::Row {
        huxerui::Image(ToolIcon(spec.iconName))
            .Tint(theme.colors.on_surface_variant)
            .With(huxerui::Frame{.width = 18.0F, .height = 18.0F}),
        huxerui::Column {
            huxerui::Text(std::string(spec.displayName))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kBody),
                    theme.colors.on_surface}),
            huxerui::Text(installed ? detectedPath
                                    : (command.empty()
                                           ? "未检测到（由系统包管理器提供）"
                                           : "未检测到"))
                .Style(huxerui::TextStyle{
                    huxerui::Font::Monospace(font_size::kCaption),
                    theme.colors.on_surface_variant}),
        }.With(huxerui::Spacing(2.0F), huxerui::Grow(1.0F)),
        (installed || command.empty())
            ? huxerui::View{huxerui::Row{}}
            : huxerui::View{
                  huxerui::Button("复制安装命令")
                      .OnClick([clipboard, toast, command] {
                          if (!clipboard || !clipboard->IsAvailable()) {
                              toast.Show(std::format("剪贴板不可用，请手动执行：{}",
                                                     command));
                              return;
                          }
                          if (clipboard->WriteText(command)) {
                              toast.Show("安装命令已复制到剪贴板");
                          } else {
                              toast.Show(std::format("复制失败，请手动执行：{}",
                                                     command));
                          }
                      })},
    }.With(huxerui::Spacing(10.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

[[huxerui::composable]] huxerui::View SectionTitle(const std::string& title) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    // 分组标签走次要文本色：强调色（primary）只留给可交互状态，
    // 标题不再随主题变成蓝色。
    return huxerui::Text(title).Style(huxerui::TextStyle{
        huxerui::Font::System(font_size::kChip).WithWeight(huxerui::FontWeight::Bold),
        theme.colors.on_surface_variant});
}

// 配置文件路径行。每次展示都重新 stat；不存在时显示「未创建」且不可点击，
// 避免把尚未生成的 live 文件伪装成可编辑项。
[[huxerui::composable]] huxerui::View PathRow(const std::string& label,
                                              const std::string& path,
                                              bool openable = false) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto toast = huxerui::UseToast();
    std::error_code existsError;
    const bool exists =
        !path.empty() &&
        std::filesystem::exists(std::filesystem::path(path), existsError);
    const bool canOpen = openable && exists;
    huxerui::View row = huxerui::Column {
        huxerui::Text(label).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kChip), theme.colors.on_surface}),
        huxerui::Text(exists ? path : path + "  （未创建）")
            .Style(huxerui::TextStyle{
                huxerui::Font::Monospace(font_size::kCaption),
                canOpen ? theme.colors.primary
                        : (exists ? theme.colors.on_surface_variant
                                  : theme.colors.outline)}),
    }.With(huxerui::Spacing(2.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    if (!canOpen) return row;
    return std::move(row)
        .OnClick([path, toast] {
            if (!OpenInSystemEditor(path)) {
                toast.Show("无法用系统编辑器打开：" + path);
            }
        })
        .With(huxerui::Tooltip("点击用系统默认编辑器打开"));
}

} // namespace

[[huxerui::composable]] huxerui::View SettingsPage(
    huxerui::State<int> themeMode, huxerui::State<int> revision,
    huxerui::State<std::size_t> startupMode,
    huxerui::State<bool> trayEnabled,
    huxerui::State<std::size_t> navPage,
    huxerui::State<std::size_t> workspacePage, bool inWorkspace) {
    // 订阅当前页：切回设置页时重新做一遍配置文件存在性检查，避免外部新建
    // 文件后仍显示旧的「未创建」状态。
    if (inWorkspace) {
        (void)workspacePage.Get();
    } else {
        (void)navPage.Get();
    }
    auto configRefresh = huxerui::UseState<int>(0);
    (void)configRefresh.Get();
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    const auto picker = huxerui::UseService<huxerui::FilePicker>();
    auto lastExport = huxerui::UseState<std::string>({});
    auto importPath = huxerui::UseState(huxerui::TextEditingValue{""});
    auto closeBehavior = huxerui::UseState<std::size_t>(
        CloseBehaviorIndex(providerStore().config().closeBehavior));
    auto claudeCodeSkipInstallationChecks =
        huxerui::UseState(providerStore().claudeCodeSkipInstallationChecks());
    auto radialNavAutoClose =
        huxerui::UseState(providerStore().config().radialNavAutoClose);
    const auto pet = huxerui::UseService<PetController>();
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    const bool petTrayAvailable = application.SystemTray().IsAvailable() &&
                                  trayEnabled.Get();
    auto launchAtLogin =
        huxerui::UseState(providerStore().config().launchAtLogin);
    auto petTopmost = huxerui::UseState(providerStore().config().petTopmost);
    auto petLightweight =
        huxerui::UseState(providerStore().config().petLightweight);
    auto petCompact = huxerui::UseState(providerStore().config().petCompact);
    auto petFollowDarkMode =
        huxerui::UseState(providerStore().config().petFollowDarkMode);
    const bool petLive2DBuilt = PetLive2DAvailable();
    const auto activityBridge = huxerui::UseService<::agents::ActivityBridge>();
    std::error_code helperPathError;
    const bool agentBridgeReady = activityBridge->Running() &&
        std::filesystem::is_regular_file(cfg::agentHookExecutable(), helperPathError);
    const auto claudeHookInitial = ReadHookStatus("claude-code");
    const auto codexHookInitial = ReadHookStatus("codex");
    auto claudeHooksEnabled = huxerui::UseState(claudeHookInitial.installed);
    auto claudeHooksBlocked = huxerui::UseState(claudeHookInitial.blockedByUser);
    auto codexHooksEnabled = huxerui::UseState(codexHookInitial.installed);
    auto codexHooksBlocked = huxerui::UseState(codexHookInitial.blockedByUser);
    auto claudeHooksBusy = huxerui::UseState(false);
    auto codexHooksBusy = huxerui::UseState(false);

    // Hook 配置改写与备份在 worker 中完成；返回 UI 线程后更新来源健康状态。
    auto configureHook = [tasks, toast, activityBridge](
        std::string tool, huxerui::State<bool> enabledState,
        huxerui::State<bool> blockedState, huxerui::State<bool> busyState,
        bool enabled) {
        if (busyState.Get()) return;
        busyState = true;
        tasks.Launch([tool = std::move(tool), enabledState, blockedState,
                      busyState, enabled, toast, activityBridge]() mutable
                         -> huxerui::Task<void> {
            try {
                const auto status = co_await huxerui::RunWorker(
                    [tool, enabled] {
                        ::agents::hooks::Configure(
                            tool, cfg::agentHookExecutable(), enabled);
                        return ::agents::hooks::Inspect(tool);
                    });
                enabledState = status.installed;
                blockedState = status.blockedByUser;
                const ::agents::SourceHealth health = !status.installed
                    ? ::agents::SourceHealth::disabled
                    : status.blockedByUser || !activityBridge->Running()
                        ? ::agents::SourceHealth::unavailable
                        : tool == "codex" ? ::agents::SourceHealth::needsTrust
                                           : ::agents::SourceHealth::awaitingEvent;
                activityBridge->SetSourceHealth(tool, health);
                if (enabled && tool == "codex") {
                    toast.Show("Codex hook 已安装；请在 Codex 的 /hooks 中审查并信任，信任前桌宠会显示未知。");
                } else if (enabled) {
                    toast.Show("Claude Code hook 已安装；新会话触发事件后桌宠开始显示活动。");
                } else {
                    toast.Show("已移除 llm-switch 管理的 hook；其它 hooks 保留。");
                }
            } catch (const std::exception& e) {
                toast.Show(e.what());
            }
            busyState = false;
        });
    };

    // ---- 本地环境检查 ----
    // 只做存在性检测：在 PATH 与几个已知安装目录里找可执行文件（纯文件系统，
    // 不起子进程），所以报「装没装 / 装在哪」，不报版本。进页面检测一次，
    // 「重新检测」按钮手动重跑。
    const auto clipboard = huxerui::UseApplication().Clipboard();
    using DetectionMap = std::map<std::string, std::string>;
    auto detectedTools = huxerui::UseState(DetectionMap{});
    auto detectTools = [detectedTools] {
        DetectionMap found;
        for (const auto& spec : models::toolRegistry()) {
            if (spec.binary.empty()) continue;  // 无 CLI 的工具（Claude Desktop）
            std::error_code ec;
            const auto path = cfg::findExecutable(spec.binary);
            if (!path.empty()) {
                found.emplace(std::string(spec.id), path.string());
            }
        }
        detectedTools = std::move(found);
    };
    huxerui::Lifecycle([detectTools] {
        detectTools();
        return [] {};
    });

    // 检查行先在这里拼好（列表必须在返回语句之前构造，不能在 View 树里塞
    // lambda）；跳过检查时留空，整段不渲染。
    std::vector<huxerui::View> installRows;
    if (!claudeCodeSkipInstallationChecks.Get()) {
        const DetectionMap& found = detectedTools.Get();
        for (const auto& spec : models::toolRegistry()) {
            if (spec.binary.empty()) continue;  // 无 CLI（Claude Desktop）
            const auto it = found.find(std::string(spec.id));
            installRows.push_back(ToolStatusRow(
                spec, it == found.end() ? std::string{} : it->second, clipboard,
                toast));
        }
    }

    // Agent 管理开关：关闭项存 AppConfig.disabledTools，其他页面按它过滤。
    // 图标本身即复选框：正常着色 = 启用，灰色 = 关闭；点击直接切换。
    auto disabledTools =
        huxerui::UseState(providerStore().config().disabledTools);
    huxerui::IconButtonStyle agentIconOn = huxerui::IconButtonStyle::Default();
    agentIconOn.foreground = theme.colors.on_surface;
    agentIconOn.icon_size = 26.0F;
    agentIconOn.minimum_interactive_size = 42.0F;
    agentIconOn.state_layer_size = 38.0F;
    agentIconOn.corner_radius = 19.0F;
    huxerui::IconButtonStyle agentIconOff = agentIconOn;
    agentIconOff.foreground = theme.colors.on_surface_variant;
    agentIconOff.foreground.alpha = 0.38F;
    huxerui::ThemeDefinition agentIconOnTheme;
    agentIconOnTheme.Set(agentIconOn);
    huxerui::ThemeDefinition agentIconOffTheme;
    agentIconOffTheme.Set(agentIconOff);

    std::vector<huxerui::View> managedAgentIcons;
    managedAgentIcons.reserve(models::toolRegistry().size());
    for (const auto& spec : models::toolRegistry()) {
        const std::string id(spec.id);
        const bool enabled =
            std::ranges::find(disabledTools.Get(), id) ==
            disabledTools.Get().end();
        const std::string label(spec.displayName);
        const std::string tip =
            enabled ? label + " · 已启用（点击关闭）"
                    : label + " · 已关闭（点击启用）";
        managedAgentIcons.push_back(
            huxerui::Theme(
                enabled ? agentIconOnTheme : agentIconOffTheme,
                huxerui::IconButton(ToolIcon(spec.iconName), label)
                    .OnClick([disabledTools, revision, toast, id] {
                        const bool turnOn =
                            std::ranges::find(disabledTools.Get(), id) !=
                            disabledTools.Get().end();
                        auto next = disabledTools.Get();
                        const auto it = std::ranges::find(next, id);
                        if (turnOn && it != next.end()) {
                            next.erase(it);
                        } else if (!turnOn && it == next.end()) {
                            next.push_back(id);
                        }
                        try {
                            providerStore().setToolEnabled(id, turnOn);
                            disabledTools = next;
                            revision = revision.Get() + 1;
                        } catch (const std::exception& e) {
                            toast.Show(e.what());
                        }
                    })
                    .With(huxerui::Tooltip(tip)))
                .Key("managed-agent:" + id));
    }
    huxerui::View managedAgents = huxerui::Column {
        huxerui::Text("点击图标切换是否管理该 Agent；灰色表示已关闭。")
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                theme.colors.on_surface_variant}),
        huxerui::Row(std::move(managedAgentIcons))
            .With(huxerui::Spacing(10.0F),
                  huxerui::CrossAlign(
                      huxerui::CrossAxisAlignment::Center)),
    }.With(huxerui::Spacing(10.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    const bool canSave = picker && picker->CanSaveFiles();
    const bool canOpen = picker && picker->CanOpenFiles();

    // 导出：有系统对话框走 SaveFileAsync（先落临时文件做源），否则固定导出到
    // dataDir()/exports/ 并在页面上展示路径。
    auto doExport = [=] {
        tasks.Launch([=]() -> huxerui::Task<void> {
            if (canSave) {
                const auto tmp = std::filesystem::temp_directory_path() /
                                 "llm-switch-export.json";
                try {
                    providerStore().exportTo(tmp);
                } catch (const std::exception& e) {
                    toast.Show(e.what());
                    co_return;
                }
                const bool ok = co_await picker->SaveFileAsync(
                    huxerui::File(tmp.string()),
                    huxerui::SaveFileOptions{
                        .suggested_name = "llm-switch-config.json",
                        .filter = {.name = "JSON", .extensions = {"json"}}});
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                if (ok) toast.Show("配置已导出");
            } else {
                const auto dest = cfg::dataDir() / "exports" /
                                  "llm-switch-config.json";
                try {
                    providerStore().exportTo(dest);
                    lastExport = dest.string();
                    toast.Show("已导出到 " + dest.string());
                } catch (const std::exception& e) {
                    toast.Show(e.what());
                }
            }
        });
    };

    // 导入：有系统对话框走 OpenFileAsync → 落到临时文件 → importFrom；
    // 否则用手动输入的路径。成功/失败都 toast；成功后 revision+1 刷新全界面。
    auto doImportPicked = [=] {
        tasks.Launch([=]() -> huxerui::Task<void> {
            const auto ref = co_await picker->OpenFileAsync(
                huxerui::FilePickerFilter{.name = "JSON", .extensions = {"json"}});
            if (!ref) co_return;
            const auto tmp = std::filesystem::temp_directory_path() /
                             "llm-switch-import.json";
            if (!co_await ref->ImportToAsync(huxerui::File(tmp.string()),
                                             true)) {
                toast.Show("无法读取所选文件");
                co_return;
            }
            try {
                providerStore().importFrom(tmp);
                toast.Show("导入完成");
                revision = revision.Get() + 1;
            } catch (const std::exception& e) {
                toast.Show(e.what());
            }
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
        });
    };
    auto doImportPath = [=] {
        const std::string path = importPath.Get().text;
        if (path.empty()) {
            toast.Show("请输入配置文件路径");
            return;
        }
        try {
            providerStore().importFrom(path);
            toast.Show("导入完成");
            revision = revision.Get() + 1;
        } catch (const std::exception& e) {
            toast.Show(e.what());
        }
    };

    return PageScaffold(
        "设置",
        huxerui::Row{},
        huxerui::ScrollView(
            huxerui::Column {
                PageSection(SectionTitle("通用"),
                            huxerui::Column {
                    SettingRow(
                        "主题", "深色 / 浅色 / 跟随系统",
                        ThemeModeSelector(themeMode)),
                    SettingRow(
                        "点击页面后收起导航盘",
                        "标题栏中心莲花阵：点选顶级页面后立即收起；关闭则保持展开，指针移出才收起",
                        huxerui::Switch(radialNavAutoClose.Get())
                            .OnChanged([radialNavAutoClose, revision, toast](bool on) {
                                try {
                                    providerStore().setRadialNavAutoClose(on);
                                    radialNavAutoClose = on;
                                    revision = revision.Get() + 1;
                                } catch (const std::exception& e) {
                                    toast.Show(e.what());
                                }
                            })),
                    SettingRow(
                        "关闭行为",
                        "系统关闭按钮和标题栏关闭按钮均使用此行为",
                        huxerui::SegmentedButton(
                            {"关闭时询问", "最小化到托盘", "直接关闭"},
                            closeBehavior)
                            .OnChanged([closeBehavior, toast](std::size_t index) {
                                static constexpr std::string_view behaviors[] = {
                                    "ask", "tray", "quit"};
                                if (index >= std::size(behaviors)) return;
                                try {
                                    providerStore().setCloseBehavior(
                                        std::string(behaviors[index]));
                                    closeBehavior = index;
                                } catch (const std::exception& e) {
                                    toast.Show(e.what());
                                }
                            })),
                    SettingRow(
                        "登录时启动 llm-switch",
                        "在当前用户下次登录时自动启动；更改会立即写入系统启动项",
                        huxerui::Switch(launchAtLogin.Get())
                            .OnChanged([launchAtLogin, toast](bool enabled) {
                                const bool previous =
                                    providerStore().config().launchAtLogin;
                                try {
                                    SetLaunchAtLogin(enabled);
                                    try {
                                        providerStore().setLaunchAtLogin(enabled);
                                    } catch (...) {
                                        try {
                                            SetLaunchAtLogin(previous);
                                        } catch (...) {
                                        }
                                        throw;
                                    }
                                    launchAtLogin = enabled;
                                } catch (const std::exception& e) {
                                    toast.Show(e.what());
                                }
                            })),
                    SettingRow(
                        "显示系统托盘",
                        "托盘用于唤回桌宠、打开控制中心和退出应用；关闭后保留可见窗口入口",
                        huxerui::Switch(trayEnabled.Get())
                            .OnChanged([pet, trayEnabled, toast](bool enabled) {
                                try {
                                    providerStore().setTrayEnabled(enabled);
                                    trayEnabled = enabled;
                                    pet->trayEnabled = enabled;
                                } catch (const std::exception& e) {
                                    toast.Show(e.what());
                                }
                            })),
                }.With(huxerui::Spacing(10.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),

                SectionDivider(),

                // AI 桌宠：启动入口可即时切换；置顶切换同步到现存窗口；
                // 轻量级模式下隐藏即退役、唤出重建。
                PageSection(SectionTitle("管理的 Agent"),
                            std::move(managedAgents)),
                PageSection(SectionTitle("AI 桌宠"),
                            huxerui::Column {
                    SettingRow(
                        "启动入口",
                        "桌宠优先会自动启动桌宠并收起控制中心；控制中心模式直接打开管理页面",
                        huxerui::SegmentedButton(
                            {"桌宠优先", "控制中心"}, startupMode)
                            .OnChanged([pet, application, startupMode, toast](std::size_t index) {
                                if (index > 1) return;
                                try {
                                    providerStore().setStartupMode(
                                        index == 0 ? "pet" : "control-center");
                                    startupMode = index;
                                    if (index == 0) {
                                        pet->Show(application);
                                    } else {
                                        pet->Close();
                                    }
                                } catch (const std::exception& e) {
                                    toast.Show(e.what());
                                }
                            })),
                    SettingRow(
                        "桌宠窗口置顶",
                        "保持在普通窗口之上显示（Wayland 等合成器可能忽略）",
                        huxerui::Switch(petTopmost.Get())
                            .OnChanged([pet, petTopmost, toast](bool on) {
                                try {
                                    providerStore().setPetTopmost(on);
                                    pet->SetTopmost(on);
                                    petTopmost = on;
                                } catch (const std::exception& e) {
                                    toast.Show(e.what());
                                }
                            })),
                    SettingRow(
                        "跟随深色模式",
                        "桌宠外观跟随应用深浅色变化；皮肤素材尚未接入，当前只保存偏好",
                        huxerui::Switch(petFollowDarkMode.Get())
                            .OnChanged([pet, petFollowDarkMode, toast](bool on) {
                                try {
                                    providerStore().setPetFollowDarkMode(on);
                                    pet->followDarkMode = on;
                                    petFollowDarkMode = on;
                                    // TODO(pet-theme): 素材就绪后在这里触发
                                    // 桌宠按新明暗主题重绘/换肤。
                                } catch (const std::exception& e) {
                                    toast.Show(e.what());
                                }
                            })),
                    // 轻量级模式（对齐 Clash-Flux 的同名托盘选项）：隐藏时
                    // 退役窗口释放资源，托盘/菜单唤出时重建。托盘不可用时
                    // 没有唤出入口，不允许开启（框架桌宠示例同款守卫）。
                    SettingRow(
                        "轻量级模式",
                        "隐藏桌宠时销毁窗口与界面，唤出时重新创建（位置与偏好保留）",
                        huxerui::Switch(petLightweight.Get())
                            .OnChanged(
                                [pet, petLightweight, toast,
                                 petTrayAvailable](bool on) {
                                    try {
                                        providerStore().setPetLightweight(on);
                                        pet->SetLightweight(on,
                                                            petTrayAvailable);
                                        petLightweight = on;
                                    } catch (const std::exception& e) {
                                        toast.Show(e.what());
                                    }
                                })
                            .With(huxerui::Enabled(petTrayAvailable))),
                    // 胶囊模式：信息量压缩到「状态点 + 今日次数」，点击同样
                    // 展开信息卡；切换即按新尺寸重建窗口（位置保留）。
                    SettingRow(
                        "胶囊模式",
                        "桌宠缩为小胶囊：状态点 + 今日请求数",
                        huxerui::Switch(petCompact.Get())
                            .OnChanged(
                                [pet, application, petCompact, toast](bool on) {
                                    try {
                                        providerStore().setPetCompact(on);
                                        pet->SetCompact(on, application);
                                        petCompact = on;
                                    } catch (const std::exception& e) {
                                        toast.Show(e.what());
                                    }
                                })),
                    SettingRow(
                        "桌宠皮肤",
                        std::string(petLive2DBuilt
                                        ? "Shimeji 用于 Q 版帧动画；Live2D 用于高阶模型与交互"
                                        : "Shimeji 用于 Q 版；此构建未启用 Live2D（需要 Cubism SDK）"),
                        huxerui::SegmentedButton(
                            {"莲花球", "Shimeji", "Live2D"},
                            static_cast<std::size_t>(pet->skin.Get()))
                            .OnChanged([pet, application, toast](std::size_t index) {
                                if (index > 2) return;
                                const auto skin = static_cast<PetSkin>(index);
                                try {
                                    providerStore().setPetSkin(
                                        std::string(PetSkinName(skin)));
                                    pet->SetSkin(skin, application);
                                } catch (const std::exception& e) {
                                    toast.Show(e.what());
                                }
                            })),
                    PetModelSettings(),
                }.With(huxerui::Spacing(10.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),

                SectionDivider(),

                PageSection(SectionTitle("Agent 活动来源"),
                            huxerui::Column {
                    SettingRow(
                        "Claude Code hooks",
                        claudeHooksBlocked.Get()
                            ? "Claude Code 已关闭 disableAllHooks；启用前请在 Claude Code 自身设置中恢复 hooks"
                            : "只提取 session_id、事件名和状态；不保存提示词、回复、工具内容或 transcript 路径",
                        huxerui::Switch(claudeHooksEnabled.Get())
                            .OnChanged([configureHook, claudeHooksEnabled,
                                        claudeHooksBlocked, claudeHooksBusy](bool on) {
                                configureHook("claude-code", claudeHooksEnabled,
                                              claudeHooksBlocked, claudeHooksBusy, on);
                            })
                            .With(huxerui::Enabled(agentBridgeReady ||
                                claudeHooksEnabled.Get()))),
                    SettingRow(
                        "Codex hooks",
                        codexHooksBlocked.Get()
                            ? "Codex config.toml 已关闭 hooks；启用前请在 Codex 自身设置中恢复"
                            : "hook 首次安装或变更需在 Codex 的 /hooks 中审查并信任；信任前活动保持未知",
                        huxerui::Switch(codexHooksEnabled.Get())
                            .OnChanged([configureHook, codexHooksEnabled,
                                        codexHooksBlocked, codexHooksBusy](bool on) {
                                configureHook("codex", codexHooksEnabled,
                                              codexHooksBlocked, codexHooksBusy, on);
                            })
                            .With(huxerui::Enabled(agentBridgeReady ||
                                codexHooksEnabled.Get()))),
                    huxerui::Text(agentBridgeReady
                        ? "活动只在本机通过受令牌保护的 loopback bridge 传递；应用退出时桥接描述符会删除。"
                        : "本地活动接收程序不可用；请使用随应用安装的 llm-switch-agent-hook。")
                        .Style(huxerui::TextStyle{
                            huxerui::Font::System(font_size::kCaption),
                            theme.colors.on_surface_variant}),
                }.With(huxerui::Spacing(10.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),

                SectionDivider(),

                // 本地环境检查（对齐 cc-switch 设置页的同名分区）：只做存在性
                // 检测 + 复制官方安装命令，不执行安装/升级——HuxerUI 没有子进程
                // API，跑不了 `claude --version` 这类探测，装/升留给用户自己跑。
                // 上方「跳过安装检查」打开时整段收起。
                PageSection(SectionTitle("本地环境检查"),
                            huxerui::Column {
                    SettingRow(
                        "跳过安装检查",
                        "不再列出各 CLI 的安装位置（仅适合自己管理安装）",
                        huxerui::Switch(claudeCodeSkipInstallationChecks.Get())
                            .OnChanged([claudeCodeSkipInstallationChecks, revision, toast](bool on) {
                                try {
                                    providerStore().setClaudeCodeSkipInstallationChecks(on);
                                    claudeCodeSkipInstallationChecks = on;
                                    revision = revision.Get() + 1;
                                } catch (const std::exception& e) {
                                    toast.Show(e.what());
                                }
                            })),
                    claudeCodeSkipInstallationChecks.Get()
                        ? huxerui::View{huxerui::Row{}}
                        : huxerui::View{huxerui::Column {
                              huxerui::Text("检测到的可直接用；未检测到的给出可复制的官方安装命令。"
                                            "只查本机 PATH 与常见安装目录，不联网、不执行安装。")
                                  .Style(huxerui::TextStyle{
                                      huxerui::Font::System(font_size::kCaption),
                                      theme.colors.on_surface_variant}),
                              huxerui::Column(std::move(installRows))
                                  .With(huxerui::Spacing(10.0F),
                                        huxerui::CrossAlign(
                                            huxerui::CrossAxisAlignment::Stretch)),
                          }.With(huxerui::Spacing(10.0F),
                                 huxerui::CrossAlign(
                                     huxerui::CrossAxisAlignment::Stretch))},
                }.With(huxerui::Spacing(10.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),

                SectionDivider(),

                PageSection(SectionTitle("配置"),
                            huxerui::Column {
                    huxerui::Row {
                        huxerui::Text("点击彩色配置路径，用系统默认编辑器打开；"
                                      "灰色「未创建」表示文件尚不存在。")
                            .Style(huxerui::TextStyle{
                                huxerui::Font::System(font_size::kCaption),
                                theme.colors.on_surface_variant})
                            .With(huxerui::Grow(1.0F)),
                        huxerui::Button("重新检查").OnClick([configRefresh] {
                            configRefresh = configRefresh.Get() + 1;
                        }),
                    }.With(huxerui::Spacing(8.0F),
                           huxerui::CrossAlign(
                               huxerui::CrossAxisAlignment::Center)),
                    PathRow("Claude Code 设置（live）",
                            cfg::claudeSettingsFile().string(), true),
                    PathRow("Codex 凭据（live）", cfg::codexAuthFile().string(),
                            true),
                    PathRow("Codex 主配置（live）",
                            cfg::codexConfigFile().string(), true),
                    PathRow("opencode 配置（live）",
                            cfg::opencodeConfigFile().string(), true),
                    PathRow("pi 模型注册表（live）",
                            cfg::piModelsFile().string(), true),
                    PathRow("pi 设置（live）",
                            cfg::piSettingsFile().string(), true),
                    PathRow("ZCode 配置（live）",
                            cfg::zcodeConfigFile().string(), true),
                    PathRow("Kimi Code 配置（live）",
                            cfg::kimiConfigFile().string(), true),
                    PathRow("dsh 设置（live）",
                            cfg::dshSettingsFile().string(), true),
                    PathRow("Hermes 配置（live）",
                            cfg::hermesConfigFile().string(), true),
                    cfg::claudeDesktopDir().empty()
                        ? huxerui::View{huxerui::Row{}}
                        : huxerui::View{PathRow(
                              "Claude Desktop 配置（live）",
                              (cfg::claudeDesktopDir() /
                               "claude_desktop_config.json")
                                  .string(),
                              true)},
                    PathRow("llm-switch 配置库", cfg::configFile().string()),
                    PathRow("用量模板（可选覆盖）",
                            cfg::usageTemplatesFile().string()),
                    PathRow("备份目录", cfg::backupsDir().string()),
                    huxerui::Text("导出整个配置库为 JSON；导入按供应商 id 合并"
                                  "（导入前自动备份当前配置到 backups/）。")
                        .Style(huxerui::TextStyle{
                            huxerui::Font::System(font_size::kCaption),
                            theme.colors.on_surface_variant}),
                    huxerui::Row {
                        huxerui::Button("导出配置").OnClick([doExport] {
                            doExport();
                        }),
                        canOpen
                            ? huxerui::View{huxerui::Button("导入配置")
                                                .OnClick([doImportPicked] {
                                                    doImportPicked();
                                                })}
                            : huxerui::View{huxerui::Row{}},
                    }.With(huxerui::Spacing(8.0F)),
                    // 无文件对话框平台：手动路径导入。
                    canOpen
                        ? huxerui::View{huxerui::Row{}}
                        : huxerui::View{
                              huxerui::Row {
                                  huxerui::TextField(importPath.Get())
                                      .Placeholder("配置文件路径")
                                      .Variant(huxerui::TextFieldVariant::Outlined)
                                      .OnChanged(
                                          [importPath](
                                              const huxerui::TextEditingValue& v) {
                                              importPath = v;
                                          })
                                      .With(huxerui::Grow(1.0F)),
                                  huxerui::Button("导入").OnClick([doImportPath] {
                                      doImportPath();
                                  }),
                              }.With(huxerui::Spacing(8.0F))},
                    lastExport.Get().empty()
                        ? huxerui::View{huxerui::Row{}}
                        : huxerui::View{huxerui::Text("上次导出：" +
                                                      lastExport.Get())
                                            .Style(huxerui::TextStyle{
                                                huxerui::Font::Monospace(
                                                    font_size::kCaption),
                                                theme.colors.on_surface_variant})},
                }.With(huxerui::Spacing(8.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),
            }.With(huxerui::Spacing(12.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
            .With(huxerui::Grow(1.0F)));
}

} // namespace llmswitch::ui
