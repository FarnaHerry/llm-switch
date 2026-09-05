// app.cpp — 应用壳（岛屿架构 + 自定义标题栏 + 系统托盘，对齐 Clash-Flux 壳风）：
//   标题栏：应用名 + 拖拽区，框架在其右侧渲染窗口按钮；收窄为 24px 高、去背景
//     直接融入窗口底色。主题为极简 AI 黑白风（MinimalDark/MinimalLightThemeSpec，
//     与 Clash-Flux 同配色：深色近纯黑 + 纯白主色；浅色海面白 + 近黑主色）。
//   下方：左侧顶级图标侧边栏（Agent 管理 / 设置，无岛屿包裹，直接落在窗口背景
//   上）｜内容区（Agent 管理页内再分二级工具栏 + 页面自己的一级岛屿——
//   PageScaffold，外壳不再套岛）。根节点刷整窗海面底色
//   （rootSpec.colors.background——AppRoot 在主题 provider 之上，UseTheme 只能
//   拿到默认浅色 spec，须按 dark 自选；子树在 provider 之下 UseTheme 正常）。
//
// 托盘：菜单按工具分组列出各组供应商（勾选当前项），点击直接 store.switchTo
// 切换；另有 显示主窗口 / 退出。菜单随全局 revision 变更重建。
#include <huxerui/huxerui.h>

#include <array>
#include <chrono>
#include <string>
#include <vector>

#include "ui.h"
#include "app.h"
#include "app_resources.h"

import llmswitch.config;
import llmswitch.models;
import llmswitch.store;

namespace llmswitch::ui {

namespace pages {

enum PageIndex : std::size_t {
    kAgents = 0,
    kSettings = 1,
};

} // namespace pages

namespace {

// 极简 AI 黑白风主题（对齐 Clash-Flux 配色）：
// 深色 = 近纯黑底 + 纯白主色（主色控件白底黑字）；浅色 = 近白海面 + 近黑主色。
// 文本/描边只用地道中灰，状态色仅 error 保留柔和红。
huxerui::ThemeSpec MinimalDarkThemeSpec() {
    huxerui::ThemeSpec spec = huxerui::MaterialDarkThemeSpec();
    spec.typography = huxerui::TypographyScheme{
        .body_large = 16.0F,
        .body_medium = font_size::kBody,
        .body_small = font_size::kChip,
        .label_large = font_size::kBody,
        .title_large = font_size::kTitle,
        .headline_small = 24.0F,
    };
    spec.colors.primary = huxerui::Color::Rgb(255, 255, 255);      // 纯白主色
    spec.colors.on_primary = huxerui::Color::Rgb(10, 10, 12);      // 白底上翻黑
    spec.colors.secondary = huxerui::Color::Rgb(214, 214, 217);
    spec.colors.on_secondary = huxerui::Color::Rgb(10, 10, 12);
    spec.colors.secondary_container = huxerui::Color::Rgb(30, 30, 35);
    spec.colors.on_secondary_container = huxerui::Color::Rgb(242, 242, 242);
    spec.colors.background = huxerui::Color::Rgb(10, 10, 12);      // #0A0A0C 近纯黑（海面）
    spec.colors.surface = huxerui::Color::Rgb(14, 14, 17);
    spec.colors.surface_container_low = huxerui::Color::Rgb(19, 19, 22);
    spec.colors.surface_container = huxerui::Color::Rgb(24, 24, 28);
    spec.colors.surface_container_high = huxerui::Color::Rgb(30, 30, 35);
    spec.colors.surface_container_highest = huxerui::Color::Rgb(37, 37, 43);
    spec.colors.on_surface = huxerui::Color::Rgb(242, 242, 242);   // 0.95 白
    spec.colors.on_surface_variant = huxerui::Color::Rgb(148, 148, 153); // 0.58 灰
    spec.colors.outline = huxerui::Color::Rgb(46, 46, 52);
    spec.colors.inverse_surface = huxerui::Color::Rgb(242, 242, 242);
    spec.colors.inverse_on_surface = huxerui::Color::Rgb(10, 10, 12);
    spec.colors.error = huxerui::Color::Rgb(235, 122, 112);        // 柔和红
    return spec;
}

huxerui::ThemeSpec MinimalLightThemeSpec() {
    huxerui::ThemeSpec spec = huxerui::MaterialLightThemeSpec();
    spec.typography = huxerui::TypographyScheme{
        .body_large = 16.0F,
        .body_medium = font_size::kBody,
        .body_small = font_size::kChip,
        .label_large = font_size::kBody,
        .title_large = font_size::kTitle,
        .headline_small = 24.0F,
    };
    // 冷中性灰白：保留柔和层级，去掉米白中过强的黄/棕分量。
    spec.colors.primary = huxerui::Color::Rgb(37, 40, 45);         // #25282D
    spec.colors.on_primary = huxerui::Color::Rgb(250, 250, 251);   // #FAFAFB
    spec.colors.secondary = huxerui::Color::Rgb(104, 112, 124);    // #68707C
    spec.colors.on_secondary = huxerui::Color::Rgb(250, 250, 251);
    spec.colors.secondary_container = huxerui::Color::Rgb(231, 234, 240);
    spec.colors.on_secondary_container = huxerui::Color::Rgb(37, 40, 45);
    spec.colors.background = huxerui::Color::Rgb(243, 244, 246);   // #F3F4F6 海面
    spec.colors.surface = huxerui::Color::Rgb(250, 250, 251);      // #FAFAFB
    spec.colors.surface_container_low = huxerui::Color::Rgb(248, 249, 250);
    spec.colors.surface_container = huxerui::Color::Rgb(241, 243, 245);
    spec.colors.surface_container_high = huxerui::Color::Rgb(231, 234, 238);
    spec.colors.surface_container_highest = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.on_surface = huxerui::Color::Rgb(36, 39, 44);      // #24272C
    spec.colors.on_surface_variant = huxerui::Color::Rgb(107, 114, 128); // #6B7280
    spec.colors.outline = huxerui::Color::Rgb(216, 220, 226);      // #D8DCE2
    spec.colors.inverse_surface = huxerui::Color::Rgb(36, 39, 44);
    spec.colors.inverse_on_surface = huxerui::Color::Rgb(250, 250, 251);
    spec.colors.error = huxerui::Color::Rgb(204, 64, 51);
    return spec;
}

// 主题边界：MaterialThemeDefinition(spec) 之上用 typed style 覆盖组件样式——
// 按钮/分段按钮/菜单圆角统一 8px（M3 默认全圆胶囊），叠加层用 on_surface
// 半透明（深色下黑叠黑、浅色黑底上白叠加不可见，故不用 M3 ripple）。
huxerui::View MinimalThemed(bool dark, huxerui::View content) {
    const huxerui::ThemeSpec spec = dark ? MinimalDarkThemeSpec() : MinimalLightThemeSpec();
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

    huxerui::SegmentedButtonStyle segments; // Default()：corner_radius=8
    segments.background = spec.colors.surface;
    segments.selected_background = spec.colors.primary;
    segments.label_style = huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                              spec.colors.on_surface};
    segments.selected_label = spec.colors.on_primary;
    segments.border = spec.colors.outline;
    segments.selected_border = spec.colors.primary;
    definition.Set(segments);

    // 内置确认框跟随主题（DialogStyle 是 Environment 值，经 ThemeDefinition::Set
    // 全局覆盖）；Default() 基线是白底浅色配色，逐字段换色。
    huxerui::DialogStyle dialogs = huxerui::DialogStyle::Default();
    dialogs.background = spec.colors.surface_container_high;
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
    menus.corner_radius = spec.shapes.small;
    menus.item_indication = huxerui::Indication{
        .hover = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_surface, 0.08F)},
        .press = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_surface, 0.12F)},
    };
    definition.Set(menus);

    return huxerui::Theme(std::move(definition), content);
}

// 托盘菜单：按工具分组列出供应商（勾选当前项，点击直接切换），
// 底部固定 显示主窗口 / 退出。
std::vector<huxerui::MenuEntry> BuildTrayMenu(huxerui::WindowHandle window,
                                              huxerui::ApplicationHandle application,
                                              huxerui::ToastHandle toast,
                                              huxerui::State<int> revision) {
    std::vector<huxerui::MenuEntry> entries;
    auto& st = providerStore();
    // 按注册表列出全部工具组（阶段A 通用代码，自然覆盖 5 个工具）。
    for (const auto& spec : models::toolRegistry()) {
        const std::string tool(spec.id);
        entries.push_back(
            huxerui::MenuItem(std::string(spec.displayName), [] {}).Enabled(false));
        const auto& g = st.group(tool);
        if (g.providers.empty()) {
            entries.push_back(huxerui::MenuItem("（无供应商）", [] {}).Enabled(false));
        }
        for (const auto& p : g.providers) {
            const std::string id = p.id;
            const std::string name = p.name;
            entries.push_back(
                huxerui::MenuItem(name, [tool, id, name, toast, revision] {
                    try {
                        providerStore().switchTo(tool, id);
                        toast.Show(std::format("{} 已切换到 {}", ToolName(tool), name));
                    } catch (const std::exception& e) {
                        toast.Show(e.what());
                    }
                    revision = revision.Get() + 1;
                }).Checked(g.current == p.id));
        }
        entries.push_back(huxerui::MenuSection{});
    }
    entries.push_back(huxerui::MenuItem("显示主窗口", [window] { window.Activate(); }));
    entries.push_back(huxerui::MenuSection{});
    entries.push_back(
        huxerui::MenuItem("退出", [application] { application.Quit(); }));
    return entries;
}

// 左列：图标侧边栏（无岛屿包裹，选中态用实心图标变体，悬停显示文字提示）。
[[huxerui::composable]] huxerui::View SideShell(huxerui::State<std::size_t> navPage) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    // 响应式：Compact(<600) 收窄侧栏宽度与内边距。
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    struct Item {
        huxerui::ImageResource icon;
        huxerui::ImageResource icon_selected;
        const char* tooltip;
        std::size_t page;
    };
    // 顶级侧栏：Agent 管理（内嵌二级工具栏）+ 设置。
    const std::array<Item, 2> items{
        Item{app::images::agents, app::images::agents_selected, "Agent 管理",
             pages::kAgents},
        Item{app::images::settings, app::images::settings_selected, "设置",
             pages::kSettings},
    };

    std::vector<huxerui::View> buttons;
    for (const Item& item : items) {
        const std::size_t page = item.page;
        const huxerui::ImageResource& icon =
            navPage.Get() == page ? item.icon_selected : item.icon;
        buttons.push_back(
            huxerui::IconButton(icon, item.tooltip)
                .OnClick([tasks, navPage, page] {
                    // 切页会卸载内容子树：推迟出指针事件路径
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        co_await huxerui::Delay(std::chrono::duration<double>{0});
                        navPage = page;
                    });
                })
                .With(huxerui::Tooltip(item.tooltip)));
    }
    return huxerui::Column(std::move(buttons))
        .With(huxerui::Padding(compact ? theme.spacing.small
                                       : theme.spacing.medium),
              huxerui::Spacing(theme.spacing.small),
              huxerui::Frame{.width = compact ? 44.0F : 56.0F},
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

} // namespace

[[huxerui::composable]] huxerui::View AppRoot() {
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    const huxerui::WindowHandle window = huxerui::UseWindow();
    const huxerui::SystemTrayHandle tray = application.SystemTray();
    const bool trayAvailable = tray.IsAvailable();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();

    // 初始值在 UseState 之前算好（组合体内不写 State）：
    // 主题模式 0=跟随系统 1=深色 2=浅色；配置里的 themeMode 字符串映射。
    int initialThemeMode = 0;
    {
        const std::string& saved = providerStore().config().themeMode;
        if (saved == "dark") initialThemeMode = 1;
        if (saved == "light") initialThemeMode = 2;
    }
    auto themeMode = huxerui::UseState<int>(std::move(initialThemeMode));
    auto navPage = huxerui::UseState<std::size_t>(pages::kAgents);
    // 全局变更计数：任何写库操作（含托盘切换）后 +1，驱动托盘菜单重建
    // （Lifecycle 依赖）与页面重读。
    auto revision = huxerui::UseState<int>(0);

    // 托盘：图标 + 菜单；点击托盘图标激活主窗口。仅在可用时注册。
    if (trayAvailable) {
        tray.OnActivate([window] { window.Activate(); });
        huxerui::Lifecycle(
            [tray, window, application, toast, revision] {
                tray.Show(app::images::tray,
                          huxerui::SystemTrayOptions{
                              .tooltip = "llm-switch",
                              .menu = BuildTrayMenu(window, application, toast,
                                                    revision)});
                return [tray] { tray.Hide(); };
            },
            revision);
    }

    const bool dark =
        themeMode.Get() == 1 || (themeMode.Get() == 0 && cfg::systemPrefersDark());
    const huxerui::ThemeSpec rootSpec = dark ? MinimalDarkThemeSpec() : MinimalLightThemeSpec();
    const IslandTheme rootIslands = ResolveIslandTheme(rootSpec);

    std::vector<huxerui::View> pages;
    pages.push_back(AgentPage(revision).Key("agents").With(huxerui::Grow(1.0F)));
    pages.push_back(SettingsPage(themeMode, revision)
                        .Key("settings").With(huxerui::Grow(1.0F)));

    huxerui::View content = huxerui::Column {
        // 自定义标题栏：应用名 + 拖拽区（框架在其右侧渲染窗口按钮）。收窄 +
        // 去背景：直接融入窗口海面底色；垂直零内边距，内容本身 24pt 高，
        // 与 title_bar_height 对齐。
        huxerui::WindowTitleBar {
            huxerui::Text("llm-switch")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kChip)
                        .WithWeight(huxerui::FontWeight::Bold),
                    rootSpec.colors.on_surface})
                .With(huxerui::WindowDragRegion{}),
            huxerui::Spacer{}.With(huxerui::Grow(1.0F), huxerui::WindowDragRegion{}),
        }
            .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(
                      rootSpec.spacing.small, 0.0F)),
                  huxerui::Spacing(rootSpec.spacing.small)),
        // 主行：图标侧栏（无岛屿包裹）+ 内容区；Grow 吃满标题栏之外剩余高度。
        // 内容区不再套外壳岛：区域划分由各页面自己的一级岛（PageScaffold）承担。
        huxerui::Row {
            SideShell(navPage),
            huxerui::IndexedPages(std::move(pages), navPage.Get())
                .With(huxerui::Grow(1.0F)),
        }
            .With(huxerui::Spacing(rootIslands.page_gap),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                  huxerui::Grow(1.0F)),
    }
        .With(huxerui::Spacing(rootSpec.spacing.extra_small),
              // 窗口整体海面底色刷满根节点：岛间缝隙透出底色形成层次。
              huxerui::Background(rootSpec.colors.background),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    return MinimalThemed(dark, std::move(content));
}

} // namespace llmswitch::ui
