// app.cpp — 应用壳（岛屿架构 + 自定义标题栏 + 系统托盘，对齐 Clash-Flux 壳风）：
//   标题栏：应用名与莲花标志在左，五瓣莲花导航锚点精确居中，两侧以圆点、
//     菱形和细线装饰；悬停时整组高亮，并在屏幕中央展开环形顶级页面图标。
//     框架在右侧渲染窗口按钮；标题栏
//     收窄为 24px 高、去背景直接融入窗口底色。主题为冷调石板风：
//     深色「石墨」= 中性石墨底 + 青蓝强调色；浅色「晴石」= 冷白底 + 天蓝强调色；
//     环境层是一层程序化环境光，error 为冷调语义红。
//   下方：内容区独占整行（Agent 管理页内再分二级工具栏 + 页面自己的
//   一级岛屿——PageScaffold，外壳不再套岛）。根节点刷整窗海面底色
//   （rootSpec.colors.background——AppRoot 在主题 provider 之上，UseTheme 只能
//   拿到默认浅色 spec，须按 dark 自选；子树在 provider 之下 UseTheme 正常）。
//
// 托盘：菜单每个工具一个顶层条目（「显示名 — 当前生效名」，托盘菜单图标
// 只接受位图故不带工具图标），hover 展开二级菜单选供应商（有官方厂商的组
// 首位「官方」条目 = restoreOfficial），点击直接切换；另有本地路由开关 /
// 显示主窗口 / 退出。菜单随全局 revision 变更重建。
// 关闭行为由设置页配置：询问、最小化到托盘或直接关闭；托盘菜单「退出」经
// application.Quit() 绕过关闭处理器正常终止。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "ui.h"
#include "app.h"
#include "app_resources.h"
#include "pet.h"
#include "single_instance.h"

import llmswitch.config;
import llmswitch.models;
import llmswitch.store;
import llmswitch.agents;
import llmswitch.agents.bridge;
import llmswitch.agents.hook_manager;

namespace llmswitch::ui {

namespace {

// 主题边界：MaterialThemeDefinition(spec) 之上用 typed style 覆盖组件样式——
// 按钮/分段按钮/菜单圆角统一 8px（M3 默认全圆胶囊），叠加层用 on_surface
// 半透明（深色下黑叠黑、浅色黑底上白叠加不可见，故不用 M3 ripple）。
huxerui::View AppThemed(bool dark, huxerui::View content) {
    const huxerui::ThemeSpec spec = dark ? AppDarkThemeSpec() : AppLightThemeSpec();
    huxerui::ThemeDefinition definition = huxerui::MaterialThemeDefinition(spec);

    const auto withAlpha = [](huxerui::Color c, float a) {
        c.alpha = a;
        return c;
    };

    huxerui::ButtonStyle buttons; // Default()：corner_radius=8、padding Symmetric(14,8)
    buttons.background = spec.colors.primary;
    buttons.label_style = huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                             spec.colors.on_primary};
    definition.Set(buttons);

    // 功能图标资源只保存白色 alpha-mask；这里是唯一的主题着色入口。
    // 深色映射冷白、浅色映射深石板蓝，资源本身无需维护主题分叉。
    huxerui::IconButtonStyle iconButtons = huxerui::IconButtonStyle::Default();
    iconButtons.foreground = spec.colors.on_surface;
    iconButtons.disabled_foreground = withAlpha(spec.colors.on_surface_variant, 0.38F);
    definition.Set(iconButtons);

    huxerui::SegmentedButtonStyle segments; // Default()：corner_radius=8
    segments.background = spec.colors.surface;
    segments.selected_background = spec.colors.primary;
    segments.label_style = huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                              spec.colors.on_surface};
    segments.selected_label = spec.colors.on_primary;
    segments.border = huxerui::Border{spec.colors.outline, 1.0F};
    segments.selected_border = huxerui::Border{spec.colors.primary, 1.0F};
    definition.Set(segments);

    // 内置确认框跟随主题（DialogStyle 是 Environment 值，经 ThemeDefinition::Set
    // 全局覆盖）；Default() 基线是白底浅色配色，逐字段换色。底色取
    // surface_container_highest——与 DialogCard 的 overlay 同源，两套弹窗底色一致。
    huxerui::DialogStyle dialogs = huxerui::DialogStyle::Default();
    dialogs.background = spec.colors.surface_container_highest;
    dialogs.title_style = huxerui::TextStyle{
        huxerui::Font::System(font_size::kTitle).WithWeight(huxerui::FontWeight::Bold),
        spec.colors.on_surface};
    dialogs.message_style = huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                               spec.colors.on_surface};
    dialogs.positive_action_style = huxerui::TextStyle{
        huxerui::Font::System(font_size::kBody), spec.colors.on_primary};
    dialogs.positive_action_background = spec.colors.primary;
    dialogs.positive_action_indication = huxerui::Indication{
        .hover = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_primary, 0.10F)},
        .press = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_primary, 0.18F)},
    };
    dialogs.negative_action_style = huxerui::TextStyle{
        huxerui::Font::System(font_size::kBody), spec.colors.on_surface};
    dialogs.negative_action_indication = huxerui::Indication{
        .hover = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_surface, 0.06F)},
        .press = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_surface, 0.12F)},
    };
    dialogs.action_separator_color = spec.colors.outline;
    definition.Set(dialogs);

    // 菜单类弹层（右键菜单、托盘菜单）统一 8px 圆角、同表面同阴影。
    huxerui::MenuStyle menus = huxerui::MenuStyle::Default();
    menus.background = spec.colors.surface_container;
    menus.foreground = spec.colors.on_surface;
    menus.icon_tint = spec.colors.on_surface_variant;
    menus.separator_color = spec.colors.outline;
    menus.shadow = huxerui::Shadow{huxerui::Color::Rgb(0, 0, 0, 0.24F), {}, 8.0F, 0.0F};
    menus.corner_radii = huxerui::CornerRadii{spec.shapes.small};
    menus.item_indication = huxerui::Indication{
        .hover = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_surface, 0.08F)},
        .press = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_surface, 0.12F)},
    };
    definition.Set(menus);

    return huxerui::Theme(std::move(definition), content);
}

// 托盘菜单：每个工具一个顶层条目（工具图标 + 「显示名 — 当前生效项」），
// hover 展开二级菜单选供应商（官方原生状态条目 = restoreOfficial 在首位，
// 勾选当前项）。随后是本地路由开关（勾选 = 运行中，点击 start/stop），
// 底部固定 显示主窗口 / 退出。
std::vector<huxerui::MenuEntry> BuildTrayMenu(
    huxerui::ApplicationHandle application,
    const std::shared_ptr<MainWindowController>& mainWindow,
    const std::shared_ptr<PetController>& pet,
    const std::shared_ptr<TrayPresentationController>& trayPresentation) {
    std::vector<huxerui::MenuEntry> entries;
    const std::weak_ptr<TrayPresentationController> weakPresentation =
        trayPresentation;
    auto& st = providerStore();
    // 按注册表列出全部工具组，覆盖当前登记的八个 Agent。
    for (const auto& spec : models::toolRegistry()) {
        const std::string tool(spec.id);
        const auto& g = st.group(tool);
        const std::string_view official = models::officialVendorName(tool);
        std::string currentName;
        for (const auto& p : g.providers) {
            if (p.id == g.current) currentName = p.name;
        }
        if (currentName.empty()) {
            currentName = official.empty() ? "未设置" : std::string(official);
        }
        // 二级菜单：官方原生状态条目与普通供应商同列同交互（勾选 = 当前）。
        std::vector<huxerui::MenuEntry> children;
        if (!official.empty()) {
            children.push_back(
                huxerui::MenuItem(std::string(official),
                                  [tool, mainWindow, weakPresentation] {
                    try {
                        providerStore().restoreOfficial(tool);
                        mainWindow->ShowMessage(std::format(
                            "{} 已恢复官方原生状态", ToolName(tool)));
                    } catch (const std::exception& e) {
                        mainWindow->ShowMessage(e.what(), true);
                    }
                    if (const auto presentation = weakPresentation.lock()) {
                        presentation->NotifyConfigurationChanged();
                    }
                }).Checked(g.current.empty()));
        }
        if (g.providers.empty()) {
            children.push_back(
                huxerui::MenuItem("（无供应商）", [] {}).Enabled(false));
        }
        for (const auto& p : g.providers) {
            const std::string id = p.id;
            const std::string name = p.name;
            children.push_back(
                huxerui::MenuItem(name, [tool, id, name, mainWindow,
                                         weakPresentation] {
                    try {
                        providerStore().switchTo(tool, id);
                        mainWindow->ShowMessage(std::format(
                            "{} 已切换到 {}", ToolName(tool), name));
                    } catch (const std::exception& e) {
                        mainWindow->ShowMessage(e.what(), true);
                    }
                    if (const auto presentation = weakPresentation.lock()) {
                        presentation->NotifyConfigurationChanged();
                    }
                }).Checked(g.current == p.id));
        }
        // 顶层条目（hover 展开二级菜单）。托盘菜单图标只接受位图
        // ImageAsset，工具图标是 SVG——顶层只留文字标签。
        entries.push_back(huxerui::MenuItem(
            std::format("{} — {}", spec.displayName, currentName),
            std::move(children)));
    }
    entries.push_back(huxerui::MenuSection{});
    // 本地路由开关：勾选 = 运行中；点击按 config 端口 start / stop。
    const int routerPort = st.config().routerPort;
    entries.push_back(
        huxerui::MenuItem(
            std::format("本地路由（127.0.0.1:{}）", routerPort),
            [mainWindow, weakPresentation] {
                try {
                    if (routerInstance().running()) {
                        routerInstance().stop();
                        mainWindow->ShowMessage("本地路由已停止");
                    } else {
                        routerInstance().start(providerStore().config().routerPort);
                        mainWindow->ShowMessage(std::format(
                            "本地路由已启动（127.0.0.1:{}）",
                            providerStore().config().routerPort));
                    }
                } catch (const std::exception& e) {
                    mainWindow->ShowMessage(e.what(), true);
                }
                if (const auto presentation = weakPresentation.lock()) {
                    presentation->NotifyConfigurationChanged();
                }
            })
            .Checked(routerInstance().running()));
    // AI 桌宠入口：勾选 = 窗口存活；点击同时更新启动入口偏好。
    entries.push_back(
        huxerui::MenuItem("AI 桌宠",
                          [pet, application, mainWindow, weakPresentation] {
                              try {
                                  if (pet->pet) {
                                      providerStore().setStartupMode("control-center");
                                      if (const auto presentation = weakPresentation.lock()) {
                                          presentation->NotifyConfigurationChanged(1U);
                                      }
                                      pet->Retire();
                                      mainWindow->Activate();
                                  } else {
                                      providerStore().setStartupMode("pet");
                                      if (const auto presentation = weakPresentation.lock()) {
                                          presentation->NotifyConfigurationChanged(0U);
                                      }
                                      pet->Show(application);
                                      if (!application.SystemTray().IsAvailable() ||
                                          !providerStore().config().trayEnabled) {
                                          mainWindow->Activate();
                                      } else {
                                          mainWindow->Hide();
                                      }
                                  }
                              } catch (const std::exception& e) {
                                  mainWindow->ShowMessage(e.what(), true);
                              }
                              if (const auto presentation = weakPresentation.lock()) {
                                  presentation->NotifyConfigurationChanged();
                              }
                          })
                            .Checked(pet->pet.has_value()));
    entries.push_back(huxerui::MenuSection{});
    entries.push_back(huxerui::MenuItem("显示主窗口", [mainWindow] {
        mainWindow->Activate();
    }));
    entries.push_back(huxerui::MenuSection{});
    entries.push_back(
        huxerui::MenuItem("退出", [application] { application.Quit(); }));
    return entries;
}

// 关闭确认弹窗使用自定义内容，保证弹层卡片、正文和操作按钮都走应用主题。
// 托盘宿主可能不存在（例如 Linux 桌面未提供 StatusNotifierHost），此时仍显示
// 「最小化到托盘」选项但明确禁用，避免用户误以为点击后应用会安全地隐藏。
[[huxerui::composable]] huxerui::View CloseConfirmationContent(
    huxerui::DialogContext context, huxerui::ApplicationHandle application,
    huxerui::SystemTrayHandle tray, huxerui::WindowHandle window,
    huxerui::ToastHandle toast, bool trayEnabled) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const huxerui::TaskScope tasks = huxerui::UseTaskScope();
    const bool trayAvailable = tray.IsAvailable() && trayEnabled;
    const huxerui::TextStyle hintStyle{
        huxerui::Font::System(font_size::kCaption),
        theme.colors.on_surface_variant};

    const std::string trayHintText =
        trayAvailable ? "最小化到托盘后，应用会继续在后台运行。"
                      : "系统托盘当前不可用，暂时无法最小化到托盘。";
    huxerui::View trayHint =
        huxerui::Text(trayHintText).Style(hintStyle);

    return DialogCard(huxerui::Column {
        huxerui::Text("关闭 llm-switch", huxerui::TextRole::Title),
        huxerui::Text("确定要退出应用吗？"),
        trayHint,
        huxerui::Row {
            huxerui::Spacer(),
            huxerui::Button("取消").OnClick([context] { context.Dismiss(); }),
            huxerui::Button("最小化到托盘")
                .OnClick([context, tray, window, toast, tasks] {
                    // 托盘宿主可能在弹窗打开后才消失，点击时再次确认最新状态。
                    if (!tray.IsAvailable()) {
                        toast.Show("系统托盘当前不可用，无法最小化到托盘");
                        return;
                    }
                    // Hide 会清空当前窗口的 pointer session；必须等本次 PointerUp
                    // 派发完成，否则运行时随后擦除该 session 时会使用失效迭代器。
                    tasks.Post([context, window] {
                        context.Dismiss();
                        window.Hide();
                    });
                })
                .With(huxerui::Enabled(trayAvailable)),
            huxerui::Button("退出").OnClick([application] { application.Quit(); }),
        }.With(huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
    }.With(huxerui::Spacing(12.0F),
           huxerui::Frame{.width = 420.0F},
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

::agents::hooks::HookStatus ReadStartupHookStatus(std::string_view tool) noexcept {
    try {
        return ::agents::hooks::Inspect(tool);
    } catch (...) {
        return {};
    }
}

// 新安装简短设置：只配置桌宠外观、登录启动、托盘与当前可用的活动来源。
[[huxerui::composable]] huxerui::View StartupSetupContent(
    huxerui::DialogContext context, huxerui::WindowHandle window,
    huxerui::ApplicationHandle application, huxerui::SystemTrayHandle tray,
    const std::shared_ptr<PetController>& pet,
    huxerui::State<std::size_t> startupMode,
    huxerui::State<bool> setupCompleted,
    huxerui::State<bool> trayEnabled,
    huxerui::TaskScope appTasks, huxerui::ToastHandle toast) {
    const auto live2d = huxerui::UseState(providerStore().config().petLive2D);
    const auto topmost = huxerui::UseState(providerStore().config().petTopmost);
    const auto launchAtLogin =
        huxerui::UseState(providerStore().config().launchAtLogin);
    const auto showTray = huxerui::UseState(
        providerStore().config().trayEnabled && tray.IsAvailable());
    const auto routerEnabled =
        huxerui::UseState(providerStore().config().routerEnabled);
    const auto activityBridge = huxerui::UseService<::agents::ActivityBridge>();
    const auto claudeHookInitial = ReadStartupHookStatus("claude-code");
    const auto codexHookInitial = ReadStartupHookStatus("codex");
    auto claudeHooksEnabled = huxerui::UseState(claudeHookInitial.installed);
    auto codexHooksEnabled = huxerui::UseState(codexHookInitial.installed);
    std::error_code helperPathError;
    const bool nativeHooksAvailable = activityBridge->Running() &&
        std::filesystem::is_regular_file(cfg::agentHookExecutable(), helperPathError);
    const bool live2dAvailable = PetLive2DAvailable();
    const bool trayAvailable = tray.IsAvailable();
    huxerui::View appearance =
        huxerui::Text("莲花球（内建皮肤；当前构建未启用 Live2D）");
    if (live2dAvailable) {
        appearance = huxerui::SegmentedButton(
                         {"莲花球", "Live2D"}, live2d.Get() ? 1U : 0U)
                         .OnChanged([live2d](std::size_t index) {
                             if (index < 2) live2d = index == 1;
                         });
    }

    huxerui::View setup = huxerui::Column {
        huxerui::Text("设置你的桌面伙伴", huxerui::TextRole::Title),
        huxerui::Text("选择几项偏好后启动桌宠；之后都可以在设置中更改。"),
        huxerui::Row {
            huxerui::Text("桌宠外观", huxerui::TextRole::Label),
            huxerui::Spacer(),
            appearance,
        }.With(huxerui::Spacing(10.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Row {
            huxerui::Switch("保持置顶", topmost.Get())
                .OnChanged([topmost](bool enabled) { topmost = enabled; }),
            huxerui::Spacer(),
            huxerui::Switch("登录时启动", launchAtLogin.Get())
                .OnChanged([launchAtLogin](bool enabled) {
                    launchAtLogin = enabled;
                }),
        }.With(huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Row {
            huxerui::Switch("显示系统托盘", showTray.Get())
                .OnChanged([showTray](bool enabled) { showTray = enabled; })
                .With(huxerui::Enabled(trayAvailable)),
            huxerui::Spacer(),
            huxerui::Text(trayAvailable ? "用于唤回与退出"
                                        : "当前环境不可用"),
        }.With(huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Text("活动来源", huxerui::TextRole::Label),
        huxerui::Switch("启动本地路由并读取其活动状态",
                        routerEnabled.Get())
            .OnChanged([routerEnabled](bool enabled) {
                routerEnabled = enabled;
            }),
        huxerui::Text(std::format(
            "把 Agent 配置到 127.0.0.1:{} 后才能看到请求。",
            providerStore().config().routerPort)),
        huxerui::Switch("接收 Claude Code 活动", claudeHooksEnabled.Get())
            .OnChanged([claudeHooksEnabled](bool enabled) {
                claudeHooksEnabled = enabled;
            })
            .With(huxerui::Enabled(nativeHooksAvailable || claudeHooksEnabled.Get())),
        huxerui::Text(claudeHookInitial.blockedByUser
            ? "Claude Code 已关闭 disableAllHooks；需先在 Claude Code 自身设置中恢复。"
            : "仅接收会话生命周期状态，不读取提示词、回复或工具内容。"),
        huxerui::Switch("接收 Codex 活动", codexHooksEnabled.Get())
            .OnChanged([codexHooksEnabled](bool enabled) {
                codexHooksEnabled = enabled;
            })
            .With(huxerui::Enabled(nativeHooksAvailable || codexHooksEnabled.Get())),
        huxerui::Text(codexHookInitial.blockedByUser
            ? "Codex 已关闭 hooks；需先在 Codex 自身配置中恢复。"
            : "Codex 首次安装后需在 /hooks 审查并信任；信任前活动保持未知。"),
        huxerui::Text(nativeHooksAvailable
            ? "原生来源只在本机传递状态元数据。"
            : "原生 hook helper 或本地接收桥不可用；可稍后在设置中检查。"),
        huxerui::Row {
            huxerui::Spacer(),
            huxerui::Button("完成并启动桌宠")
                .OnClick([context, window, application, tray, pet, startupMode,
                          setupCompleted, trayEnabled, appTasks, toast, live2d,
                          topmost, launchAtLogin, showTray, routerEnabled,
                          claudeHooksEnabled, codexHooksEnabled,
                          activityBridge,
                          claudeInitiallyInstalled = claudeHookInitial.installed,
                          codexInitiallyInstalled = codexHookInitial.installed] {
                    const bool selectedLive2D = live2d.Get();
                    const bool selectedTopmost = topmost.Get();
                    const bool selectedLaunchAtLogin = launchAtLogin.Get();
                    const bool selectedTray = showTray.Get();
                    const bool selectedRouter = routerEnabled.Get();
                    const bool selectedClaudeHooks = claudeHooksEnabled.Get();
                    const bool selectedCodexHooks = codexHooksEnabled.Get();
                    appTasks.Post([context, window, application, tray, pet,
                                   startupMode, setupCompleted, trayEnabled,
                                   toast, selectedLive2D, selectedTopmost,
                                   selectedLaunchAtLogin, selectedTray,
                                   selectedRouter, selectedClaudeHooks,
                                   selectedCodexHooks, claudeInitiallyInstalled,
                                   codexInitiallyInstalled, appTasks, activityBridge] {
                        const bool previousLaunchAtLogin =
                            providerStore().config().launchAtLogin;
                        try {
                            if (selectedLaunchAtLogin !=
                                previousLaunchAtLogin) {
                                SetLaunchAtLogin(selectedLaunchAtLogin);
                            }
                            try {
                                providerStore().completeFirstRunSetup(
                                    selectedLive2D, selectedTopmost,
                                    selectedLaunchAtLogin, selectedTray,
                                    selectedRouter);
                            } catch (...) {
                                if (selectedLaunchAtLogin !=
                                    previousLaunchAtLogin) {
                                    try {
                                        SetLaunchAtLogin(
                                            previousLaunchAtLogin);
                                    } catch (...) {
                                    }
                                }
                                throw;
                            }
                        } catch (const std::exception& e) {
                            toast.Show(std::format("设置未能保存：{}", e.what()));
                            return;
                        }

                        context.Dismiss();
                        pet->live2d = selectedLive2D;
                        pet->topmost = selectedTopmost;
                        pet->trayEnabled = selectedTray;
                        trayEnabled = selectedTray;
                        setupCompleted = true;

                        if (selectedRouter && !routerInstance().running()) {
                            try {
                                routerInstance().start(
                                    providerStore().config().routerPort);
                            } catch (const std::exception& e) {
                                toast.Show(std::format(
                                    "本地路由启动失败：{}", e.what()));
                            }
                        }
                        appTasks.Launch([selectedClaudeHooks, selectedCodexHooks,
                                        claudeInitiallyInstalled,
                                        codexInitiallyInstalled,
                                        activityBridge, toast]()
                                           -> huxerui::Task<void> {
                            const std::array<std::tuple<std::string, bool, bool>, 2>
                                sources{{{"claude-code", selectedClaudeHooks,
                                          selectedClaudeHooks != claudeInitiallyInstalled},
                                         {"codex", selectedCodexHooks,
                                          selectedCodexHooks != codexInitiallyInstalled}}};
                            for (const auto& [tool, enabled, changed] : sources) {
                                if (!changed) continue;
                                try {
                                    const auto status = co_await huxerui::RunWorker(
                                        [tool, enabled] {
                                            ::agents::hooks::Configure(
                                                tool, cfg::agentHookExecutable(), enabled);
                                            return ::agents::hooks::Inspect(tool);
                                        });
                                    const auto health = !status.installed
                                        ? ::agents::SourceHealth::disabled
                                        : status.blockedByUser || !activityBridge->Running()
                                            ? ::agents::SourceHealth::unavailable
                                            : tool == "codex"
                                                ? ::agents::SourceHealth::needsTrust
                                                : ::agents::SourceHealth::awaitingEvent;
                                    activityBridge->SetSourceHealth(tool, health);
                                    if (enabled && tool == "codex") {
                                        toast.Show("Codex hook 已安装；请在 /hooks 中审查并信任，信任前活动保持未知。");
                                    } else if (enabled) {
                                        toast.Show("Claude Code hook 已安装；新会话触发事件后会显示活动。");
                                    }
                                } catch (const std::exception& e) {
                                    toast.Show(std::format("{} 活动来源设置失败：{}",
                                        tool == "codex" ? "Codex" : "Claude Code", e.what()));
                                }
                            }
                        });
                        try {
                            if (startupMode.Get() == 0) {
                                pet->Show(application);
                                window.Hide();
                            } else {
                                window.Show();
                            }
                        } catch (const std::exception& e) {
                            window.Show();
                            toast.Show(std::format(
                                "桌宠启动失败，已打开控制中心：{}", e.what()));
                        }
                    });
                }),
        }.With(huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
    }.With(huxerui::Spacing(8.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    return DialogCard(
        huxerui::ScrollView(setup)
            .ScrollAxis(huxerui::Axis::Vertical)
            .With(huxerui::Frame{.width = 500.0F, .max_height = 480.0F}));
}

// 标题栏装饰随可用宽度伸缩：莲花左右保持对称细线，圆点和菱形作为节奏节点。
// 这是纯环境绘制层，不参与命中；Hover 仍只由中央莲花锚点触发。
huxerui::View TitleBarOrnamentArtwork(huxerui::Color color, bool glow) {
    return huxerui::Canvas([color, glow](huxerui::PaintContext& paint,
                                         huxerui::Size size) mutable {
        // 画布没有固有尺寸（NodeKind::Canvas 不产生内容尺寸），由外层 Stack 居中，
        // 因此画布局部原点就是莲花中心：几何一律用固定常量推导，不再读 size。
        const huxerui::Point center{size.width * 0.5F, size.height * 0.5F};

        // 自莲花中心量起：第一等分点 = 菱形，第二等分点 = 圆点（即最外点），
        // 之后向两侧延伸一小段尾线，用 alpha 渐隐收尾。相比参考图去掉了最外
        // 那一段（左右各少一段），菱形与圆点是原第一、第二等分点的位置。
        constexpr float kDiamondAt = 30.0F;
        constexpr float kEndDotAt = 60.0F;
        constexpr float kTail = 22.0F;
        // 第一段（0 .. 30）被莲花锚点覆盖：线从锚点外缘起画，不画到中心。
        constexpr float kBadgeRadius = 12.0F;

        const huxerui::StrokeStyle lineStyle{
            .width = glow ? 1.15F : 0.7F,
            .cap = huxerui::StrokeCap::Round,
            .join = huxerui::StrokeJoin::Round,
        };

        color.alpha = glow ? 0.86F : 0.42F;
        const auto drawDiamond = [&paint, color](float x, float y, float radius) {
            huxerui::Path diamond;
            diamond.MoveTo({x, y - radius})
                .LineTo({x + radius, y})
                .LineTo({x, y + radius})
                .LineTo({x - radius, y})
                .Close();
            paint.FillPath(diamond, color);
        };

        const auto drawSide = [&](float sign) {
            const float from = center.x + sign * kBadgeRadius;
            const float to = center.x + sign * kEndDotAt;

            if (glow) {
                // 悬停横线带一层宽而淡的底色 —— 多层低透明度描边叠加出「发光」
                // 观感（SDK 无模糊滤镜，辉光一律由分层描边/阴影表达）。
                huxerui::Color halo = color;
                halo.alpha = 0.20F;
                const huxerui::StrokeStyle haloStyle{
                    .width = 5.0F,
                    .cap = huxerui::StrokeCap::Round,
                    .join = huxerui::StrokeJoin::Round,
                };
                paint.DrawLine({from, center.y}, {to, center.y}, halo, haloStyle);
            }
            paint.DrawLine({from, center.y}, {to, center.y}, color, lineStyle);

            // 尾线：只改可见度不改颜色，用同色 alpha 渐变从有线渐隐到全透明。
            huxerui::Color tailStart = color;
            tailStart.alpha *= 0.9F;
            huxerui::Color tailEnd = color;
            tailEnd.alpha = 0.0F;
            const float tailFrom = to;
            const float tailTo = center.x + sign * (kEndDotAt + kTail);
            huxerui::Path tail;
            tail.MoveTo({tailFrom, center.y}).LineTo({tailTo, center.y});
            const bool rightward = sign > 0.0F;
            paint.StrokePath(
                tail,
                huxerui::LinearGradient{
                    .start = rightward ? huxerui::Point{0.0F, 0.5F}
                                       : huxerui::Point{1.0F, 0.5F},
                    .end = rightward ? huxerui::Point{1.0F, 0.5F}
                                     : huxerui::Point{0.0F, 0.5F},
                    .stops = {{0.0F, tailStart}, {1.0F, tailEnd}},
                },
                huxerui::Rect{std::min(tailFrom, tailTo), center.y - 1.0F, kTail,
                              2.0F},
                lineStyle);

            drawDiamond(center.x + sign * kDiamondAt, center.y,
                        glow ? 3.2F : 2.6F);
            paint.DrawCircle({center.x + sign * kEndDotAt, center.y},
                             glow ? 1.5F : 1.1F, color);
        };
        drawSide(-1.0F);
        drawSide(1.0F);

        // 莲花外侧的一圈线：两层细环由内向外变淡，得到轻微虚化的外扩感；
        // 悬停时整体加强，与横线的辉光呼应。
        constexpr float kFullCircle = 6.2831853F;
        huxerui::Color ring = color;
        ring.alpha = glow ? 0.34F : 0.16F;
        const huxerui::StrokeStyle ringStyle{
            .width = glow ? 0.8F : 0.6F,
            .cap = huxerui::StrokeCap::Round,
            .join = huxerui::StrokeJoin::Round,
        };
        paint.DrawArc(center, kBadgeRadius + 2.5F, 0.0F, kFullCircle, ring,
                      ringStyle);
        ring.alpha = glow ? 0.18F : 0.09F;
        paint.DrawArc(center, kBadgeRadius + 4.5F, 0.0F, kFullCircle, ring,
                      ringStyle);
    });
}

// Hover 进入后由根级浮层在屏幕中央承接导航。离开事件交给浮层的中轴 Hover
// 区域处理，避免鼠标从标题栏移向圆盘时提前收起。
[[huxerui::composable]] huxerui::View TitleBarNavigationTrigger(
    huxerui::State<bool> navigationOpen) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    const bool open = navigationOpen.Get();
    const huxerui::AnimationSpec bloomMotion = theme.motion.reduced_motion
        ? huxerui::AnimationSpec{huxerui::SnapSpec{}}
        : huxerui::AnimationSpec{huxerui::TweenSpec{
              .duration = 0.18,
              .easing = huxerui::Easing::EaseOut}};
    huxerui::Color glowShadow = theme.colors.primary;
    glowShadow.alpha = open ? 0.50F : 0.0F;

    // SVG 的可见轮廓中心比 24×24 viewBox 几何中心高约 0.6 DIP。圆环必须继续
    // 与标题栏严格同心，因此只下移花瓣图形，不偏移锚点的外框与装饰横线。
    huxerui::View lotus =
        huxerui::Stack {
            // 花瓣本体两种状态都是正文墨色（浅色=深石板蓝、深色=冷白），
            // 悬停时由承载圆底 + 描边 + 外辉光表达强调，花瓣本身不换成强调色。
            huxerui::Image(app::images::lotus_bloom)
                .Tint(theme.colors.on_surface)
                .With(huxerui::Frame{.width = 18.0F, .height = 18.0F},
                      huxerui::Offset(huxerui::Point{0.0F, 0.6F})),
        }
            .With(huxerui::Frame{.width = kTitleBarContentHeight,
                                 .height = kTitleBarContentHeight},
                  huxerui::Align(huxerui::HorizontalAlignment::Center,
                                 huxerui::VerticalAlignment::Center),
                  huxerui::Background(open ? theme.colors.secondary_container
                                            : huxerui::Color::Transparent()),
                  huxerui::CornerRadius(kTitleBarContentHeight * 0.5F),
                  huxerui::Border(open ? theme.colors.primary
                                        : islands.outline_soft,
                                   open ? 1.15F : 0.75F),
                  huxerui::Shadow{glowShadow, {}, open ? 14.0F : 0.0F, 0.0F},
                  huxerui::Scale(huxerui::AnimateTo(
                      open ? 1.08F : 1.0F, bloomMotion)),
                  huxerui::Semantics{.role = huxerui::SemanticRole::Image,
                                      .label = "页面导航"},
                  huxerui::Tooltip("悬停展开页面导航"))
            .On<huxerui::ViewEvents::Hover>(
                [navigationOpen](const huxerui::HoverEvent& event) {
                    if (event.type != huxerui::HoverEventType::Leave) {
                        navigationOpen = true;
                    }
                })
            .Key("title-nav:lotus-anchor");

    // 装饰线默认完全不展开：整组缩在莲花锚点范围内且全透明，悬停时才一起
    // 向外展开并淡入。缩放原点用修饰符默认值（节点中心），而画布没有固有
    // 尺寸、由 Stack 居中，所以原点正好是莲花中心——线条读起来就是从莲花
    // 向外长出来的。收起态的 scale 不为 0，避免退化变换。
    constexpr float kCollapsedScale = 0.15F;
    return huxerui::Stack {
        TitleBarOrnamentArtwork(theme.colors.primary, true)
            .With(huxerui::Opacity(huxerui::AnimateTo(open ? 1.0F : 0.0F,
                                                      bloomMotion)),
                  huxerui::Scale(huxerui::AnimateTo(
                      open ? 1.0F : kCollapsedScale, bloomMotion))),
        lotus,
    }.With(huxerui::Frame{.height = kTitleBarContentHeight},
              huxerui::Align(huxerui::HorizontalAlignment::Center,
                             huxerui::VerticalAlignment::Center),
              // 根级覆盖层不经过 WindowTitleBar 的平台标题区校正。实机标题按钮
              // 中心比 y=0 起算的 24 DIP 内容框低约 3 DIP，整组下移与其对齐。
              huxerui::Offset(huxerui::Point{0.0F, 3.0F}))
        .Key("title-nav:ornament");
}

// 中央圆盘的同心环、八向连线与节点是可跟随主题 tint 的静态 SVG 装饰；它们
// 不参与命中，页面动作仍由标准 IconButton 承担。
huxerui::View RadialNavigationArtwork(huxerui::Color color) {
    return huxerui::Image(app::images::radial_navigation_artwork)
        .Tint(color)
        .With(huxerui::Frame{.width = 360.0F, .height = 360.0F});
}

// 切页时序：轮盘与旧页面**一同**缩回屏幕中心那颗莲花 → 在不可见时换页 →
// 新页面从同一中心展开。收起段的时长不能省：AnimateTo 是从当前值补间，
// 目标必须真正走到收起态再换页，否则新页面只会从 ~1.0 抖一下，看不出展开。
// 收起比展开快一档。GTK 后端动画期间全窗内容逐帧重光栅化（无跨帧缓存），
// 时序每缩短 1ms 就少一帧昂贵重绘——收合 0.12s / 展开 0.18s 是当前折中。
constexpr double kCollapseSeconds = 0.12;

// 根级径向导航：中轴区域贯穿标题栏到圆盘，保证 Hover 能平滑交接；圆盘本身
// 保持全部八个顶级页面，按顺时针方向均匀排布并复用 navPage。
[[huxerui::composable]] huxerui::View RadialNavigationOverlay(
    huxerui::State<std::size_t> navPage,
    huxerui::State<bool> navigationOpen,
    huxerui::State<bool> pageReveal, huxerui::TaskScope ownerTasks) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    const bool open = navigationOpen.Get();
    auto revealed = huxerui::UseState(false);
    // 收起后要继续挂载到回收动画播完——否则盘在关闭的一瞬间就卸载，看不到回收。
    auto mounted = huxerui::UseState(false);
    auto tasks = huxerui::UseTaskScope();

    huxerui::Lifecycle(
        [revealed, mounted, tasks, navigationOpen, open] {
            if (open) {
                mounted = true;
                revealed = true;
            } else {
                revealed = false;
                tasks.Launch([mounted, navigationOpen]() -> huxerui::Task<void> {
                    co_await huxerui::Delay(std::chrono::duration<double>{
                        kCollapseSeconds});
                    // 这段时间内又被悬停打开时不卸载（open 变化会重跑本 Lifecycle）。
                    if (!navigationOpen.Get()) {
                        mounted = false;
                    }
                    co_return;
                });
            }
            return [] {};
        },
        open);

    if (!mounted.Get()) return huxerui::Row {};

    struct Item {
        huxerui::ImageResource icon;
        const char* tooltip;
        std::size_t page;
    };
    const std::array<Item, 8> items{
        Item{app::images::agents, "Agent 管理", pages::kAgents},
        Item{app::images::router, "本地路由", pages::kRouter},
        Item{app::images::stats, "使用统计", pages::kStats},
        Item{app::images::mcp, "MCP 服务器", pages::kMcp},
        Item{app::images::skills, "Skills", pages::kSkills},
        Item{app::images::sessions, "会话", pages::kSessions},
        Item{app::images::settings, "设置", pages::kSettings},
        Item{app::images::about, "关于", pages::kAbout},
    };

    constexpr std::array<huxerui::Point, 8> positions{
        huxerui::Point{0.0F, -144.0F}, huxerui::Point{101.8F, -101.8F},
        huxerui::Point{144.0F, 0.0F}, huxerui::Point{101.8F, 101.8F},
        huxerui::Point{0.0F, 144.0F}, huxerui::Point{-101.8F, 101.8F},
        huxerui::Point{-144.0F, 0.0F}, huxerui::Point{-101.8F, -101.8F},
    };
    // 展开稍慢、回收更快：回收是新页面展开的前置动作，要干脆。
    const huxerui::AnimationSpec motion = theme.motion.reduced_motion
        ? huxerui::AnimationSpec{huxerui::SnapSpec{}}
        : huxerui::AnimationSpec{huxerui::TweenSpec{
              .duration = open ? 0.22 : kCollapseSeconds,
              .easing = huxerui::Easing::EaseOut}};

    const auto makeButton = [navPage, navigationOpen, pageReveal, ownerTasks,
                             revealed, &islands, &theme,
                             &motion](const Item& item, huxerui::Point position) {
        const std::size_t page = item.page;
        huxerui::View button =
            huxerui::IconButton(item.icon, item.tooltip)
                .OnClick([navPage, navigationOpen, pageReveal, ownerTasks, page] {
                    // 两段式切页：先让选择盘快速回收到中心，再换页并从中心展开
                    // 新页面。收起盘会卸载被点击的图标本身，所以整条时序都要推迟
                    // 到事件派发之后（见 CLAUDE.md 线程契约），并且跑在 AppRoot 的
                    // 作用域里——选择盘卸载后它依然存活。开关在点击时现读配置。
                    const bool autoClose =
                        providerStore().config().radialNavAutoClose;
                    ownerTasks.Launch(
                        [navPage, navigationOpen, pageReveal, page,
                         autoClose]() -> huxerui::Task<void> {
                            // 轮盘（连着八个选项）与旧页面同时开始缩回屏幕中心，
                            // 视觉上是"一起被吸进那颗莲花"。
                            if (autoClose) {
                                navigationOpen = false;
                            }
                            pageReveal = false;
                            co_await huxerui::Delay(
                                std::chrono::duration<double>{kCollapseSeconds});
                            // 此刻两者都已缩到不可见：换页不会被看到，新页面
                            // 于是从同一个中心展开。
                            navPage = page;
                            co_await huxerui::Delay(
                                std::chrono::duration<double>{0});
                            pageReveal = true;
                            co_return;
                        });
                })
                .With(huxerui::Tooltip(item.tooltip),
                      huxerui::Background(islands.overlay),
                      huxerui::CornerRadius(26.0F),
                      huxerui::Border(islands.outline_soft, 0.8F),
                      huxerui::Shadow{huxerui::Color::Rgb(0, 0, 0, 0.18F),
                                      {}, 12.0F, 0.0F},
                      huxerui::Offset(huxerui::AnimateTo(
                          revealed.Get() ? position : huxerui::Point{}, motion)),
                      huxerui::Opacity(huxerui::AnimateTo(
                          revealed.Get() ? 1.0F : 0.0F, motion)),
                      huxerui::Scale(huxerui::AnimateTo(
                          revealed.Get() ? 1.0F : 0.72F, motion)))
                .Key(std::string("radial-nav:") + item.tooltip);
        if (navPage.Get() == page) {
            button = std::move(button).With(
                huxerui::Background(theme.colors.secondary_container),
                huxerui::Border(theme.colors.primary, 1.2F));
        }
        return button;
    };

    std::vector<huxerui::View> radialChildren;
    radialChildren.reserve(items.size() + 2);
    // 同心环与八向连线也随盘收放：它们是纯环境装饰（不参与命中、没有逐件
    // 布局），整组向中心缩放不会带来交互件「连同间距一起塌缩」的问题，读
    // 起来正是线条被收进莲花——与图标各自沿半径飞回同走一条时间线。
    radialChildren.push_back(
        RadialNavigationArtwork(theme.colors.primary)
            .With(huxerui::Opacity(huxerui::AnimateTo(
                      revealed.Get() ? 1.0F : 0.0F, motion)),
                  huxerui::Scale(huxerui::AnimateTo(
                      revealed.Get() ? 1.0F : 0.55F, motion))));
    for (std::size_t i = 0; i < items.size(); ++i) {
        radialChildren.push_back(makeButton(items[i], positions[i]));
    }
    // 盘心只有盛放莲花一种形态（含苞态已整体废弃）：盘收合时它随盘一起
    // 淡出、缩到 0.72，展开时放大回 1.0——"莲花开、线条长出来"的读法不变。
    radialChildren.push_back(
        huxerui::Stack {
            huxerui::Image(app::images::lotus_bloom)
                .Tint(theme.colors.on_surface)
                .With(huxerui::Frame{.width = 40.0F, .height = 40.0F},
                      huxerui::Scale(huxerui::AnimateTo(
                          revealed.Get() ? 1.0F : 0.72F, motion))),
        }
            .With(huxerui::Frame{.width = 82.0F, .height = 82.0F},
                  huxerui::Align(huxerui::HorizontalAlignment::Center,
                                 huxerui::VerticalAlignment::Center),
                  huxerui::Background(islands.overlay),
                  huxerui::CornerRadius(41.0F),
                  huxerui::Border(theme.colors.primary, 1.5F),
                  huxerui::Shadow{theme.colors.primary, {}, 18.0F, 0.0F},
                  huxerui::Scale(huxerui::AnimateTo(
                      revealed.Get() ? 1.0F : 0.82F, motion)),
                  huxerui::Opacity(huxerui::AnimateTo(
                      revealed.Get() ? 1.0F : 0.0F, motion)),
                  huxerui::Semantics{.role = huxerui::SemanticRole::Image,
                                      .label = "页面导航中心"})
            .Key("radial-nav:lotus"));

    huxerui::View radial = huxerui::Stack(std::move(radialChildren))
        .With(huxerui::Frame{.width = 420.0F, .height = 420.0F},
              huxerui::Align(huxerui::HorizontalAlignment::Center,
                             huxerui::VerticalAlignment::Center));

    huxerui::View hoverCorridor = huxerui::Column {
        huxerui::Spacer(),
        radial,
        huxerui::Spacer(),
    }
        .With(huxerui::Frame{.width = 420.0F},
              huxerui::Grow(1.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
        // 收起就是展开的逆过程：每个 icon 沿同一条路径飞回中心（各自的
        // Offset/Opacity/Scale 反向），不做整组缩放——整组缩会让四周的图标
        // 连同间距一起塌缩，而不是"回收进去"。
        .On<huxerui::ViewEvents::Hover>(
            [navigationOpen](const huxerui::HoverEvent& event) {
                // 中轴区只负责「保持展开」：把 Hover 从标题栏莲花平滑交接给圆盘。
                // 打开只由莲花的 Hover 负责——收起动画还在播时指针往往仍在本区
                // 内，若这里也接受非 Leave 事件，一次轻微移动就会把盘重新打开，
                // 那正是「设置了点击关闭却偶尔不关」的来源。
                if (!navigationOpen.Get()) {
                    return;
                }
                navigationOpen = event.type != huxerui::HoverEventType::Leave;
            });

    huxerui::IconButtonStyle radialIcons = huxerui::IconButtonStyle::Default();
    radialIcons.foreground = theme.colors.on_surface;
    radialIcons.disabled_foreground = theme.colors.on_surface_variant;
    radialIcons.icon_size = 24.0F;
    radialIcons.minimum_interactive_size = 52.0F;
    radialIcons.state_layer_size = 48.0F;
    radialIcons.corner_radius = 26.0F;
    huxerui::ThemeDefinition overrides;
    overrides.Set(radialIcons);

    huxerui::View navigation = huxerui::Theme(
        std::move(overrides),
        huxerui::Row {
            huxerui::Spacer(),
            hoverCorridor,
            huxerui::Spacer(),
        }.With(huxerui::Grow(1.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));

    return huxerui::Stack {
        huxerui::Row {}.With(
            huxerui::Grow(1.0F),
            huxerui::Background(huxerui::Color::Rgb(12, 18, 24, 0.46F)),
            huxerui::Opacity(huxerui::AnimateTo(
                revealed.Get() ? 1.0F : 0.0F, motion))),
        navigation,
    }.With(huxerui::Grow(1.0F),
           huxerui::Align(huxerui::HorizontalAlignment::Stretch,
                          huxerui::VerticalAlignment::Stretch));
}

// 托盘图标固定使用盛放莲花：曾经按应用展示状态在前台用盛放、后台（最小化或
// 隐藏）用含苞，双态已整体废弃，所以这里也不再订阅生命周期状态。
// 生命周期读取封装在独立组合边界内，避免状态切换重组窗口正文。
[[huxerui::composable]] huxerui::View SystemTrayPresentation(
    const std::shared_ptr<TrayPresentationController>& trayPresentation,
    bool trayAvailable, huxerui::State<int> revision,
    huxerui::State<std::size_t> startupMode,
    huxerui::State<bool> setupCompleted,
    huxerui::State<bool> trayEnabled) {
    const std::shared_ptr<PetController> pet = huxerui::UseService<PetController>();
    huxerui::Lifecycle(
        [trayPresentation] {
            trayPresentation->Refresh();
            return [] {};
        },
        trayAvailable, revision, pet->epoch, startupMode, setupCompleted,
        trayEnabled);

    return huxerui::Row {};
}

// 页面宿主单独订阅导航状态。IndexedPages 保留全部页面及其局部状态，
// 页面切换的重组范围被限制在页面宿主，不再让 AppRoot 重组整套窗口壳。
[[huxerui::composable]] huxerui::View TopLevelPageHost(
    huxerui::State<std::size_t> navPage,
    const std::shared_ptr<std::vector<huxerui::View>>& cachedPages,
    huxerui::State<bool> pageReveal) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool revealed = pageReveal.Get();
    const huxerui::AnimationSpec motion = theme.motion.reduced_motion
        ? huxerui::AnimationSpec{huxerui::SnapSpec{}}
        : huxerui::AnimationSpec{huxerui::TweenSpec{
              .duration = revealed ? 0.18 : kCollapseSeconds,
              .easing = huxerui::Easing::EaseOut}};
    const std::size_t current = navPage.Get();
    // 切页变换只套在**当前可见页**上。宿主里工作台与八个原页面全部保持挂载（各自保留
    // 列表/表单状态），若把 Opacity/Scale 套在整个宿主外层，每帧都要把全部
    // 页面做一次离屏合成——页面有真实数据时非常卡。缓存的是 View 值，这里
    // .With() 产出新声明并现读动画目标，既不会冻结目标值也不重建页面子树。
    std::vector<huxerui::View> pages = *cachedPages;
    if (current < pages.size()) {
        pages[current] = std::move(pages[current]).With(
            huxerui::Opacity(
                huxerui::AnimateTo(revealed ? 1.0F : 0.0F, motion)),
            huxerui::Scale(
                huxerui::AnimateTo(revealed ? 1.0F : 0.12F, motion)));
    }
    return huxerui::IndexedPages(std::move(pages), current)
        .With(huxerui::Grow(1.0F));
}

// 顶级页面宿主作用域：只创建一次固定的页面声明缓存。标题栏导航与
// TopLevelPageHost 各自订阅 navPage，切页不会向上冒泡到 AppRoot 或重建背景。
[[huxerui::composable]] huxerui::View TopLevelNavigation(
    huxerui::State<std::size_t> navPage,
    huxerui::State<std::size_t> workspacePage,
    huxerui::State<int> revision,
    huxerui::State<int> themeMode, huxerui::State<bool> pageReveal,
    huxerui::State<std::size_t> startupMode,
    huxerui::State<bool> trayEnabled,
    huxerui::State<std::size_t> selectedAgentTool,
    huxerui::State<std::string> addProviderRequest) {
    auto pageCache =
        huxerui::UseState<std::shared_ptr<std::vector<huxerui::View>>>({});
    std::shared_ptr<std::vector<huxerui::View>> cachedPages = pageCache.Get();
    if (!cachedPages || cachedPages->size() != 9) {
        auto nextPages = std::make_shared<std::vector<huxerui::View>>();
        nextPages->reserve(9);
        nextPages->push_back(
            PetWorkspacePage(navPage, workspacePage).Key("pet-workspace")
                .With(huxerui::Grow(1.0F)));
        nextPages->push_back(
            AgentPage(revision, navPage, selectedAgentTool, addProviderRequest)
                .Key("agents")
                .With(huxerui::Grow(1.0F)));
        nextPages->push_back(
            RouterPage(navPage).Key("router").With(huxerui::Grow(1.0F)));
        nextPages->push_back(
            StatsPage(navPage).Key("stats").With(huxerui::Grow(1.0F)));
        nextPages->push_back(
            McpPage().Key("mcp").With(huxerui::Grow(1.0F)));
        nextPages->push_back(
            SkillsPage().Key("skills").With(huxerui::Grow(1.0F)));
        nextPages->push_back(
            SessionsPage().Key("sessions").With(huxerui::Grow(1.0F)));
        nextPages->push_back(SettingsPage(themeMode, revision, startupMode,
                                          trayEnabled)
                                 .Key("settings")
                                 .With(huxerui::Grow(1.0F)));
        nextPages->push_back(
            AboutPage().Key("about").With(huxerui::Grow(1.0F)));
        pageCache = nextPages;
        cachedPages = std::move(nextPages);
    }

    return TopLevelPageHost(navPage, cachedPages, pageReveal);
}

// 环境光：冷调主题的窗口底纹。深色是石墨底上一层柔和青蓝辉光（偏向画面
// 上方，对齐参考图深色半边的柔和光斑），浅色是冷白底顶部一层极淡天蓝洗色；
// 两侧背景都保持干净——旧的全景水墨画卷与程序化泼墨已随水墨身份退役。
// Canvas 直接铺满宿主（不要包 Align/Frame：Align 会把画布按父约束撑满，
// 画布本就是全幅），光斑按画布尺寸取比例，窗口缩放时不失真。只把光斑
// 实际覆盖的顶部画进 cairo 批次（深 72% / 浅 56% 窗高；渐变中心/半径按
// 裁剪矩形归一化，且纵向半径恰好在裁剪线上衰减到 0，无接缝）——GTK
// 后端动画期间全窗内容逐帧重光栅化，环境光是最大的单件，裁掉恒为零的
// 尾部直接减少每帧光栅面积。
huxerui::View AmbientGlow(bool dark) {
    return huxerui::Canvas(
        [dark](huxerui::PaintContext& paint, huxerui::Size size) {
            huxerui::Color inner =
                dark ? huxerui::Color::Rgb(56, 189, 248, 0.10F)
                     : huxerui::Color::Rgb(40, 112, 214, 0.06F);
            huxerui::Color outer = inner;
            outer.alpha = 0.0F;
            // 光斑中心按窗口高度定位（深色 0.30 / 浅色 0.10），换算进裁剪
            // 矩形；纵向半径取「到裁剪线的剩余高度」保证边缘 alpha=0。
            const float crop = dark ? 0.72F : 0.56F;
            const float centerY = (dark ? 0.30F : 0.10F) / crop;
            paint.DrawRect(huxerui::Rect{0.0F, 0.0F, size.width,
                                         size.height * crop},
                           huxerui::RadialGradient{
                               .center = {0.5F, centerY},
                               .radius = {0.66F, 1.0F - centerY},
                               .stops = {{0.0F, inner}, {1.0F, outer}},
                           });
        });
}

} // namespace

huxerui::WindowOptions ControlCenterWindowOptions() {
    return {
        .title = "llm-switch",
        .initial_size = {1080.0F, 720.0F},
        .minimum_size = huxerui::Size{800.0F, 600.0F},
        .chrome_mode = huxerui::WindowChromeMode::Custom,
        .title_bar_height = 24.0F,
    };
}

std::uint64_t MainWindowController::Bind(huxerui::WindowHandle window,
                                         huxerui::ToastHandle toast) {
    window_ = std::move(window);
    toast_ = std::move(toast);
    return ++generation_;
}

void MainWindowController::Unbind(std::uint64_t generation) noexcept {
    if (generation_ != generation) return;
    window_.reset();
    toast_.reset();
}

void MainWindowController::Activate() {
    if (!window_) window_ = application_.OpenWindow(options_);
    window_->Show();
    window_->Activate();
}

void MainWindowController::Hide() const {
    if (window_) window_->Hide();
}

void MainWindowController::ShowMessage(std::string message,
                                       bool openIfNeeded) {
    if (toast_) {
        if (openIfNeeded) Activate();
        toast_->Show(std::move(message));
    } else if (openIfNeeded) {
        pendingMessage_ = std::move(message);
        Activate();
    }
}

std::optional<std::string> MainWindowController::TakePendingMessage() {
    auto message = std::move(pendingMessage_);
    pendingMessage_.reset();
    return message;
}

std::uint64_t TrayPresentationController::BindUiState(
    huxerui::State<int> revision,
    huxerui::State<std::size_t> startupMode) {
    revision_ = std::move(revision);
    startupMode_ = std::move(startupMode);
    return ++uiStateGeneration_;
}

void TrayPresentationController::UnbindUiState(
    std::uint64_t generation) noexcept {
    if (uiStateGeneration_ != generation) return;
    revision_.reset();
    startupMode_.reset();
}

void TrayPresentationController::NotifyConfigurationChanged(
    std::optional<std::size_t> startupMode) {
    if (startupMode && startupMode_) *startupMode_ = *startupMode;
    if (revision_) *revision_ = revision_->Get() + 1;
    Refresh();
}

void TrayPresentationController::Refresh() {
    const auto& config = providerStore().config();
    if (!config.firstRunSetupCompleted || !config.trayEnabled ||
        !tray_.IsAvailable()) {
        tray_.Hide();
        return;
    }
    tray_.Show(
        app::images::lotus_tray_bloom,
        huxerui::SystemTrayOptions{
            .tooltip = "llm-switch",
            .menu = BuildTrayMenu(application_, mainWindow_, pet_,
                                  shared_from_this()),
        });
}

void EnsureRuntimeServices(huxerui::ToastHandle toast) {
    const auto& config = providerStore().config();
    if (!config.firstRunSetupCompleted) return;
    if (config.launchAtLogin) {
        try {
            SetLaunchAtLogin(true);
        } catch (const std::exception& e) {
            toast.Show(std::format("登录启动项更新失败：{}", e.what()));
        }
    }
    if (config.routerEnabled && !routerInstance().running()) {
        try {
            routerInstance().start(config.routerPort);
        } catch (const std::exception& e) {
            toast.Show(e.what());
        }
    }
}

void InstallApplication(huxerui::ApplicationContext& context) {
    // 托盘激活处理器是 Runtime 生命周期的一次性注册，只能在 ApplicationHook
    // 装一次。主窗口控制器负责窗口尚未创建时的按需打开。
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    const huxerui::SystemTrayHandle tray = application.SystemTray();
    const huxerui::TaskScope appTasks = huxerui::UseApplicationTaskScope();

    auto mainWindow = std::make_shared<MainWindowController>(
        application, ControlCenterWindowOptions());
    context.Provide(mainWindow);

    // 主窗口页面导航目标（桌宠信息卡「跳页 + 激活主窗」用）：与
    // MainWindowController 同一模式，主窗口组合期绑定 navPage。
    context.Provide(std::make_shared<MainNavTarget>());

    // 原生 Agent hooks 由应用生命周期桥接；桌宠窗口可以隐藏/退役，接收端仍
    // 在后台工作。事件只保存在 bridge 内存中，描述符仅含 loopback 端口和令牌。
    auto activityBridge = std::make_shared<::agents::ActivityBridge>();
    const bool bridgeStarted = activityBridge->Start(cfg::agentBridgeFile());
    for (const std::string_view tool : {"claude-code", "codex"}) {
        ::agents::SourceHealth health = ::agents::SourceHealth::disabled;
        try {
            const ::agents::hooks::HookStatus hook = ::agents::hooks::Inspect(tool);
            if (hook.installed) {
                health = !bridgeStarted || hook.blockedByUser
                    ? ::agents::SourceHealth::unavailable
                    : tool == "codex" ? ::agents::SourceHealth::needsTrust
                                       : ::agents::SourceHealth::awaitingEvent;
            }
        } catch (...) {
            health = ::agents::SourceHealth::unavailable;
        }
        activityBridge->SetSourceHealth(std::string(tool), health);
    }
    context.Provide(activityBridge);

    // AI 桌宠控制器注册为应用级服务（桌宠窗口 / 设置页 / 托盘菜单共用）。
    auto pet = std::make_shared<PetController>();
    pet->readNativeActivity = [activityBridge](std::int64_t nowMillis) {
        const ::agents::ActivitySnapshot snapshot =
            activityBridge->SnapshotAt(nowMillis);
        PetNativeActivitySnapshot result;
        result.activeTools = snapshot.activeTools;
        result.sources.reserve(snapshot.sources.size());
        for (const auto& source : snapshot.sources) {
            PetNativeSourceSnapshot row{
                .tool = source.tool,
                .lastEventMillis = source.lastEventMillis,
                .enabled = source.health != ::agents::SourceHealth::disabled,
                .unknown = source.unknown};
            switch (source.state) {
                case ::agents::ActivityState::unknown:
                case ::agents::ActivityState::sessionEnded:
                    row.state = PetNativeState::unknown;
                    break;
                case ::agents::ActivityState::idle: row.state = PetNativeState::idle; break;
                case ::agents::ActivityState::working: row.state = PetNativeState::working; break;
                case ::agents::ActivityState::waiting: row.state = PetNativeState::waiting; break;
                case ::agents::ActivityState::completed: row.state = PetNativeState::completed; break;
                case ::agents::ActivityState::failed: row.state = PetNativeState::failed; break;
                case ::agents::ActivityState::cancelled: row.state = PetNativeState::cancelled; break;
            }
            result.sources.push_back(std::move(row));
        }
        result.recentOutcomes.reserve(snapshot.recentOutcomes.size());
        for (const auto& outcome : snapshot.recentOutcomes) {
            PetNativeState state = PetNativeState::unknown;
            switch (outcome.state) {
                case ::agents::ActivityState::completed:
                    state = PetNativeState::completed;
                    break;
                case ::agents::ActivityState::failed:
                    state = PetNativeState::failed;
                    break;
                case ::agents::ActivityState::cancelled:
                    state = PetNativeState::cancelled;
                    break;
                default:
                    continue;
            }
            result.recentOutcomes.push_back(PetNativeOutcomeSnapshot{
                .tool = outcome.tool,
                .state = state,
                .occurredAtMillis = outcome.occurredAtMillis,
            });
        }
        return result;
    };
    pet->topmost = providerStore().config().petTopmost;
    pet->lightweight = providerStore().config().petLightweight;
    pet->compact = providerStore().config().petCompact;
    pet->live2d = providerStore().config().petLive2D;
    pet->trayEnabled = providerStore().config().trayEnabled;
    {
        const std::string& mode = providerStore().config().themeMode;
        pet->darkTheme =
            mode == "dark" || (mode != "light" && cfg::systemPrefersDark());
    }
    context.Provide(pet);
    auto trayPresentation = std::make_shared<TrayPresentationController>(
        application, tray, mainWindow, pet);
    context.Provide(trayPresentation);
    const std::weak_ptr<TrayPresentationController> weakTrayPresentation =
        trayPresentation;
    pet->refreshTray = [weakTrayPresentation] {
        if (const auto presentation = weakTrayPresentation.lock()) {
            presentation->Refresh();
        }
    };

    // 绑定生产路由上游。ApplicationHook 在无主窗口启动时仍会运行，且
    // HuxerUI HttpClient/应用 TaskScope 在此已可用。
    BindRouterUpstreamSession(huxerui::UseService<huxerui::HttpClient>(),
                              appTasks);

    tray.OnActivate([mainWindow, pet, application] {
        const auto& config = providerStore().config();
        if (config.startupMode == "pet") {
            try {
                pet->Show(application);
            } catch (const std::exception&) {
                mainWindow->ShowMessage("桌宠启动失败，已打开控制中心。", true);
            }
        } else {
            mainWindow->Activate();
        }
    });

    // 二次启动唤回由应用级处理器承担，即使控制中心从未创建也可按启动模式
    // 打开桌宠或按需创建控制中心。
    single_instance::SetActivationHandler(
        [appTasks, mainWindow, pet, application] {
            appTasks.Post([mainWindow, pet, application] {
                const auto& config = providerStore().config();
                if (config.startupMode == "pet") {
                    try {
                        pet->Show(application);
                        return;
                    } catch (const std::exception&) {
                        // 失败时创建可见控制中心作为恢复入口。
                        mainWindow->ShowMessage(
                            "桌宠启动失败，已打开控制中心。", true);
                        return;
                    }
                }
                mainWindow->Activate();
            });
        });

    appTasks.Post([mainWindow, pet, application, trayPresentation] {
        const auto& config = providerStore().config();
        const bool controlCenterStartup =
            config.startupMode == "control-center";

        if (controlCenterStartup) {
            mainWindow->Activate();
        } else {
            try {
                pet->Show(application);
            } catch (const std::exception&) {
                mainWindow->ShowMessage("桌宠启动失败，已打开控制中心。", true);
                return;
            }
        }
        trayPresentation->Refresh();
    });
}

[[huxerui::composable]] huxerui::View AppContent() {
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    const huxerui::WindowHandle window = huxerui::UseWindow();
    const huxerui::SystemTrayHandle tray = application.SystemTray();
    const bool trayAvailable = tray.IsAvailable();
    auto toast = huxerui::UseToast();
    auto tasks = huxerui::UseTaskScope();
    const auto appTasks = huxerui::UseApplicationTaskScope();
    const auto pet = huxerui::UseService<PetController>();
    const auto mainWindow = huxerui::UseService<MainWindowController>();
    const auto trayPresentation =
        huxerui::UseService<TrayPresentationController>();
    const auto& initialConfig = providerStore().config();
    const std::string startupModeValue = initialConfig.startupMode;
    // 初始值在 UseState 之前算好（组合体内不写 State）：
    // 主题模式 0=跟随系统 1=深色 2=浅色；配置里的 themeMode 字符串映射。
    int initialThemeMode = 0;
    {
        const std::string& saved = providerStore().config().themeMode;
        if (saved == "dark") initialThemeMode = 1;
        if (saved == "light") initialThemeMode = 2;
    }
    auto themeMode = huxerui::UseState<int>(std::move(initialThemeMode));
    auto startupMode = huxerui::UseState<std::size_t>(
        startupModeValue == "control-center" ? 1U : 0U);
    auto setupCompleted =
        huxerui::UseState(initialConfig.firstRunSetupCompleted);
    auto trayEnabled = huxerui::UseState(initialConfig.trayEnabled);
    // 顶级页面状态同时供标题栏导航与保持挂载的 IndexedPages 宿主订阅。
    // AppRoot 本身不读取它，切页只重组这两个局部子树。
    auto navPage = huxerui::UseState<std::size_t>(pages::kWorkspace);
    auto workspacePage =
        huxerui::UseState<std::size_t>(std::size_t{workspace_pages::kHome});
    auto selectedAgentTool = huxerui::UseState<std::size_t>(0);
    // 把页面与 Agent 选择绑进应用级导航目标：桌宠信息卡等窗口外入口可跳到
    // 指定 Agent 并激活主窗口（主窗口卸载时解绑）。
    {
        const auto nav = huxerui::UseService<MainNavTarget>();
        huxerui::Lifecycle([nav, navPage, selectedAgentTool] {
            nav->Bind(navPage, selectedAgentTool);
            return [nav] { nav->Unbind(); };
        });
    }
    auto addProviderRequest = huxerui::UseState<std::string>({});
    // 标题栏莲花与根级径向浮层共享开合状态；AppRoot 不读取它，避免 Hover
    // 让整套窗口内容重组。
    auto navigationOpen = huxerui::UseState(false);
    // 全局变更计数：任何写库操作（含托盘切换）后 +1，驱动托盘菜单重建
    // （Lifecycle 依赖）与页面重读。
    auto revision = huxerui::UseState<int>(0);
    // 页面展开进度：切页时由径向导航的时序先置 false、换页后再置 true，
    // 让新页面从中心缩放+淡入展开（见 RadialNavigationOverlay 的点击时序）。
    auto pageReveal = huxerui::UseState(true);

    // 主窗口按需创建；打开后把当前附件与应用级控制器绑定，退役时安全解绑。
    huxerui::Lifecycle([mainWindow, window, toast] {
        const std::uint64_t generation = mainWindow->Bind(window, toast);
        if (const auto message = mainWindow->TakePendingMessage()) {
            toast.Show(*message);
        }
        return [mainWindow, generation] { mainWindow->Unbind(generation); };
    });

    huxerui::Lifecycle([trayPresentation, revision, startupMode] {
        const std::uint64_t generation =
            trayPresentation->BindUiState(revision, startupMode);
        return [trayPresentation, generation] {
            trayPresentation->UnbindUiState(generation);
        };
    });

    // 托盘宿主消失时恢复窗口，避免已经隐藏的窗口失去可见入口。
    huxerui::Lifecycle(
        [window, trayAvailable, trayEnabled] {
            if (!trayAvailable || !trayEnabled.Get()) window.Show();
        },
        trayAvailable, trayEnabled);

    huxerui::Lifecycle([toast] {
        EnsureRuntimeServices(toast);
        return [] {};
    }, 0);

    const bool dark =
        themeMode.Get() == 1 || (themeMode.Get() == 0 && cfg::systemPrefersDark());
    const huxerui::ThemeSpec rootSpec = dark ? AppDarkThemeSpec() : AppLightThemeSpec();

    // 顶级页面不再有自己的卡片，页面与标题栏共用窗口表面；两者的左右边距
    // 必须取同一个值，否则应用名和页面标题会差出一段缩进，看起来仍是两层。
    // 该值必须与 PageScaffold / AgentPage 的内边距保持一致。
    const bool compactShell =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    const float shellInset =
        compactShell ? rootSpec.spacing.medium : rootSpec.spacing.large;

    // 叠放根：环境层只有一层冷调环境光，内容仍以轻岛屿组织并透出环境；
    // 最外层仍刷海面底色，保证极端宽高比下背景稳定。
    huxerui::View content = huxerui::Stack {
        AmbientGlow(dark),
        huxerui::Column {
            // 标题栏只负责应用名、拖拽区和系统按钮预留；莲花锚点在下方根级
            // 覆盖层按整窗宽度居中，避免被右侧最小化/最大化/关闭按钮推偏。
            huxerui::WindowTitleBar {
                huxerui::Image(app::images::lotus_bloom)
                    .Tint(rootSpec.colors.on_surface)
                    .With(huxerui::Frame{.width = 16.0F, .height = 16.0F}),
                huxerui::Text("llm-switch")
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kChip)
                            .WithWeight(huxerui::FontWeight::Bold),
                        rootSpec.colors.on_surface}),
                huxerui::Button("工作台")
                    .OnClick([navPage, workspacePage] {
                        navPage = pages::kWorkspace;
                        workspacePage = workspace_pages::kHome;
                    }),
                huxerui::Spacer(),
            }
                .With(huxerui::Padding(
                    huxerui::EdgeInsets::Symmetric(shellInset, 0.0F))),
            // 内容区独占标题栏之外的整行，不再为左侧顶级导航预留宽度。
            // 左右边距由页面自身（PageScaffold / AgentPage）提供，这里不再
            // 额外缩进——页面卡片已去掉，外面再套一层就会与标题栏错位。
            // 纵向同理：这里不声明 Spacing（默认 0），页面顶部也不留内边距，
            // 页面内容紧接标题栏下沿。标题栏的实际高度由平台解析（Linux 把
            // 声明的 24 抬到 32 的最小值，Windows 取 max(24, 系统标题高度)），
            // WindowTitleBar 按解析值测量，页面排在它后面自然贴合——这正是
            // 不能用硬编码高度去"对齐"的原因。
            // 页面容器承担切页展开：缩放轴心取容器中心，配合淡入即"从中心展开"。
            TopLevelNavigation(navPage, workspacePage, revision, themeMode, pageReveal,
                               startupMode, trayEnabled, selectedAgentTool,
                               addProviderRequest)
                .With(huxerui::Grow(1.0F)),
        }
            .With(huxerui::Padding(huxerui::EdgeInsets{.bottom =
                                                           rootSpec.spacing.small}),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        // 与整窗而非 WindowTitleBar 的可用内容区对齐，保证莲花位于几何中心。
        huxerui::Column {
            huxerui::Row {
                TitleBarNavigationTrigger(navigationOpen)
                    .With(huxerui::Grow(1.0F)),
            }.With(huxerui::Frame{.height = kTitleBarContentHeight},
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
            huxerui::Spacer(),
        }.With(huxerui::Grow(1.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        RadialNavigationOverlay(navPage, navigationOpen, pageReveal, tasks),
    }
        .With(// 窗口整体海面底色刷满根节点：岛间缝隙透出底色与环境画卷。
              huxerui::Background(rootSpec.colors.background),
              // Stack 以自身对齐摆放所有子项（子项自带 Align 仅作用于图片
              // 内容），双向 Stretch 让水墨长卷与内容列都铺满窗口，长卷内容
              // 再经 Image.Align(Center, End) 钉在底部。
              huxerui::Align(huxerui::HorizontalAlignment::Stretch,
                             huxerui::VerticalAlignment::Stretch),
              // 圆角 + 裁剪：底层长卷是矩形绘制，须随窗口圆角收口。
              huxerui::CornerRadius(12.0F), huxerui::ClipChildren(),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    // Stack 的 Stretch 只拉伸子项，不会让 Stack 自身从自然尺寸扩展到窗口。
    // 用一个有 Grow 子项的 Column 把完整窗口约束传入 Stack，保证四边都能
    // 随窗口拉伸，也让内容区获得稳定的有限滚动视口。
    // DialogHandle 必须在 AppThemed 的子作用域内获取，否则关闭确认框会捕获
    // HuxerUI 默认 Environment，无法使用应用的主体色和对话框样式。
    huxerui::View windowBehavior = huxerui::Scope(
        [application, tray, window, toast, startupMode,
         setupCompleted, trayEnabled, pet, appTasks] {
            const auto dialog = huxerui::UseDialog();
            const auto tasks = huxerui::UseTaskScope();
            huxerui::Lifecycle(
                [dialog, application, tray, window, pet,
                 startupMode, setupCompleted, trayEnabled, appTasks, toast] {
                    if (!setupCompleted.Get()) {
                        dialog.Show(
                            [application, tray, window, pet, startupMode,
                             setupCompleted, trayEnabled,
                             appTasks, toast](huxerui::DialogContext context)
                                -> huxerui::View {
                                return StartupSetupContent(
                                    context, window, application, tray, pet,
                                    startupMode, setupCompleted, trayEnabled,
                                    appTasks, toast);
                            },
                            huxerui::DialogOptions{
                                .dismiss_on_outside_press = false,
                                .dismiss_on_cancel = false,
                            });
                    }
                    return [] {};
                },
                setupCompleted);
            const auto showCloseConfirmation =
                [application, tray, window, toast, dialog, trayEnabled] {
                    dialog.Show(
                        [application, tray, window,
                         toast, trayEnabled](huxerui::DialogContext context)
                            -> huxerui::View {
                            return CloseConfirmationContent(
                                context, application, tray, window, toast,
                                trayEnabled.Get());
                        },
                        huxerui::DialogOptions{});
                };

            // 「最小化到托盘」同时覆盖标题栏最小化按钮和系统最小化请求。
            // 其他关闭策略保留平台原生最小化语义；托盘不可用时不吞掉请求，
            // 让窗口仍能普通最小化，并给出原因提示。
            window.OnMinimizeRequest([tray, trayEnabled, window, toast, tasks] {
                if (providerStore().config().closeBehavior != "tray") return false;
                if (!tray.IsAvailable() || !trayEnabled.Get()) {
                    toast.Show("系统托盘当前不可用，已保留普通最小化");
                    return false;
                }
                // 标题栏按钮仍处于 PointerUp 派发中；延后隐藏，避免同步清空
                // pointer session 后事件收尾继续访问失效迭代器。
                tasks.Post([window] { window.Hide(); });
                return true;
            });

            window.OnCloseRequest(
                [application, tray, trayEnabled, window,
                 showCloseConfirmation, tasks] {
                const std::string& behavior = providerStore().config().closeBehavior;
                if (behavior == "tray") {
                    // 托盘不可用时不能返回 false，否则原生关闭路径会直接退出
                    // 应用；改为询问弹窗，用户仍可取消关闭。
                    if (!tray.IsAvailable() || !trayEnabled.Get()) {
                        showCloseConfirmation();
                        return true;
                    }
                    tasks.Post([window] { window.Hide(); });
                    return true;
                }
                if (behavior == "quit") {
                    application.Quit();
                    return true;
                }

                showCloseConfirmation();
                return true;
            });
            // 保留一个实际挂载的空布局节点；默认构造 View 没有 ViewSpec，Scope
            // 可能被运行时省略，导致关闭处理器没有机会注册。
            return huxerui::Row {};
        });

    huxerui::View filledContent = huxerui::Column {
        SystemTrayPresentation(trayPresentation, trayAvailable, revision,
                               startupMode, setupCompleted, trayEnabled),
        windowBehavior,
        std::move(content).With(huxerui::Grow(1.0F)),
    }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
           huxerui::Grow(1.0F));

    return AppThemed(dark, filledContent);
}

huxerui::View AppRoot() {
    // 多窗口分发：主窗口（key 为空）走应用壳；桌宠窗口（key=="pet"，透明
    // Utility）走 PetContent；信息卡（key=="pet-card"，桌宠的 owner 窗口）走
    // PetCardContent。WindowIdentity 由框架按窗口种入 Environment。
    return huxerui::Scope([] -> huxerui::View {
        const huxerui::WindowIdentity identity =
            huxerui::UseEnvironment<huxerui::WindowIdentity>();
        if (identity.key == llmswitch::ui::kPetWindowKey) {
            return llmswitch::ui::PetContent();
        }
        if (identity.key == llmswitch::ui::kPetCardWindowKey) {
            return llmswitch::ui::PetCardContent();
        }
        return llmswitch::ui::AppContent();
    });
}

} // namespace llmswitch::ui
