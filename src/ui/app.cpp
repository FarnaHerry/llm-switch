// app.cpp — 应用壳（岛屿架构 + 自定义标题栏 + 系统托盘，对齐 Clash-Flux 壳风）：
//   标题栏：太极标 + 应用名 + 拖拽区，框架在其右侧渲染窗口按钮；收窄为 24px 高、
//     去背景直接融入窗口底色。主题为太极水墨风（InkDark/InkLightThemeSpec）：
//     深色「玄墨」= 暖调近黑底 + 宣纸白主色；浅色「宣纸」= 米白纸面 + 浓墨主色；
//     状态色仅 error 保留朱砂红。
//   下方：左侧顶级图标侧边栏（Agent 管理 / 本地路由 / 使用统计 / MCP 服务器 /
//   Skills / 会话 / 设置 / 关于，无岛屿包裹，直接落在窗口背景上）｜内容区
//   （Agent 管理页内再分二级工具栏 + 页面自己的一级岛屿——
//   PageScaffold，外壳不再套岛）。根节点刷整窗海面底色
//   （rootSpec.colors.background——AppRoot 在主题 provider 之上，UseTheme 只能
//   拿到默认浅色 spec，须按 dark 自选；子树在 provider 之下 UseTheme 正常）。
//
// 托盘：菜单每个工具一个顶层条目（「显示名 — 当前生效名」，托盘菜单图标
// 只接受位图故不带工具图标），hover 展开二级菜单选供应商（有官方厂商的组
// 首位「官方」条目 = restoreOfficial），点击直接切换；另有本地路由开关 /
// 显示主窗口 / 退出。菜单随全局 revision 变更重建。
// 关闭按钮在托盘可用时只隐藏窗口（OnCloseRequest 消费请求），「退出」经
// application.Quit() 绕过关闭处理器正常终止。
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
    kRouter = 1,
    kStats = 2,
    kMcp = 3,
    kSkills = 4,
    kSessions = 5,
    kSettings = 6,
    kAbout = 7,
};

} // namespace pages

namespace {

// 太极水墨风主题：深色「玄墨」= 暖调近黑海面（玄）+ 宣纸白主色；
// 浅色「宣纸」= 米白纸面 + 浓墨主色。文本/描边只用暖调墨色阶
// （浓墨/淡墨），状态色仅 error 保留朱砂红（印泥）。
huxerui::ThemeSpec InkDarkThemeSpec() {
    huxerui::ThemeSpec spec = huxerui::MaterialDarkThemeSpec();
    spec.typography = huxerui::TypographyScheme{
        .body_large = 16.0F,
        .body_medium = font_size::kBody,
        .body_small = font_size::kChip,
        .label_large = font_size::kBody,
        .title_large = font_size::kTitle,
        .headline_small = 24.0F,
    };
    spec.colors.primary = huxerui::Color::Rgb(230, 224, 210);      // 宣纸白主色 #E6E0D2
    spec.colors.on_primary = huxerui::Color::Rgb(38, 35, 30);      // 主色上翻浓墨
    spec.colors.secondary = huxerui::Color::Rgb(179, 172, 156);
    spec.colors.on_secondary = huxerui::Color::Rgb(38, 35, 30);
    spec.colors.secondary_container = huxerui::Color::Rgb(58, 54, 45);
    spec.colors.on_secondary_container = huxerui::Color::Rgb(230, 224, 210);
    spec.colors.background = huxerui::Color::Rgb(22, 20, 17);      // 玄 #161411 暖调近黑（海面）
    spec.colors.surface = huxerui::Color::Rgb(28, 26, 22);
    spec.colors.surface_container_low = huxerui::Color::Rgb(33, 30, 26);
    spec.colors.surface_container = huxerui::Color::Rgb(40, 37, 31);
    spec.colors.surface_container_high = huxerui::Color::Rgb(47, 44, 37);
    spec.colors.surface_container_highest = huxerui::Color::Rgb(56, 52, 44);
    spec.colors.on_surface = huxerui::Color::Rgb(214, 208, 192);   // 宣纸灰正文
    spec.colors.on_surface_variant = huxerui::Color::Rgb(163, 156, 139); // 淡墨
    spec.colors.outline = huxerui::Color::Rgb(76, 71, 60);
    spec.colors.inverse_surface = huxerui::Color::Rgb(214, 208, 192);
    spec.colors.inverse_on_surface = huxerui::Color::Rgb(38, 35, 30);
    spec.colors.error = huxerui::Color::Rgb(223, 114, 86);         // 朱砂（浅）#DF7256
    return spec;
}

huxerui::ThemeSpec InkLightThemeSpec() {
    huxerui::ThemeSpec spec = huxerui::MaterialLightThemeSpec();
    spec.typography = huxerui::TypographyScheme{
        .body_large = 16.0F,
        .body_medium = font_size::kBody,
        .body_small = font_size::kChip,
        .label_large = font_size::kBody,
        .title_large = font_size::kTitle,
        .headline_small = 24.0F,
    };
    // 宣纸色：暖调米白纸面（更亮一档，对齐墨韵参考），卡片近白 + 细墨边，
    // 主色浓墨而非纯黑。
    spec.colors.primary = huxerui::Color::Rgb(43, 40, 35);         // 浓墨主色 #2B2823
    spec.colors.on_primary = huxerui::Color::Rgb(246, 243, 234);
    spec.colors.secondary = huxerui::Color::Rgb(110, 105, 92);     // 淡墨 #6E695C
    spec.colors.on_secondary = huxerui::Color::Rgb(248, 245, 238);
    spec.colors.secondary_container = huxerui::Color::Rgb(227, 221, 203);
    spec.colors.on_secondary_container = huxerui::Color::Rgb(43, 40, 35);
    spec.colors.background = huxerui::Color::Rgb(239, 234, 224);   // 宣纸海面 #EFEAE0
    spec.colors.surface = huxerui::Color::Rgb(247, 244, 236);
    spec.colors.surface_container_low = huxerui::Color::Rgb(242, 238, 228);
    spec.colors.surface_container = huxerui::Color::Rgb(248, 245, 236);  // 二级岛近白
    spec.colors.surface_container_high = huxerui::Color::Rgb(230, 225, 211);
    spec.colors.surface_container_highest = huxerui::Color::Rgb(252, 250, 243);
    spec.colors.on_surface = huxerui::Color::Rgb(46, 43, 37);      // 浓墨正文 #2E2B25
    spec.colors.on_surface_variant = huxerui::Color::Rgb(110, 105, 92); // 淡墨
    spec.colors.outline = huxerui::Color::Rgb(216, 210, 194);      // 细墨边
    spec.colors.inverse_surface = huxerui::Color::Rgb(46, 43, 37);
    spec.colors.inverse_on_surface = huxerui::Color::Rgb(246, 243, 234);
    spec.colors.error = huxerui::Color::Rgb(181, 70, 46);          // 朱砂 #B5462E
    return spec;
}

// 主题边界：MaterialThemeDefinition(spec) 之上用 typed style 覆盖组件样式——
// 按钮/分段按钮/菜单圆角统一 8px（M3 默认全圆胶囊），叠加层用 on_surface
// 半透明（深色下黑叠黑、浅色黑底上白叠加不可见，故不用 M3 ripple）。
huxerui::View InkThemed(bool dark, huxerui::View content) {
    const huxerui::ThemeSpec spec = dark ? InkDarkThemeSpec() : InkLightThemeSpec();
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
    // 深色映射宣纸白、浅色映射浓墨，资源本身无需维护主题分叉。
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
std::vector<huxerui::MenuEntry> BuildTrayMenu(huxerui::WindowHandle window,
                                              huxerui::ApplicationHandle application,
                                              huxerui::ToastHandle toast,
                                              huxerui::State<int> revision) {
    std::vector<huxerui::MenuEntry> entries;
    auto& st = providerStore();
    // 按注册表列出全部工具组（阶段A 通用代码，自然覆盖 5 个工具）。
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
                huxerui::MenuItem(std::string(official), [tool, toast, revision] {
                    try {
                        providerStore().restoreOfficial(tool);
                        toast.Show(std::format("{} 已恢复官方原生状态",
                                               ToolName(tool)));
                    } catch (const std::exception& e) {
                        toast.Show(e.what());
                    }
                    revision = revision.Get() + 1;
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
            [toast, revision] {
                try {
                    if (routerInstance().running()) {
                        routerInstance().stop();
                        toast.Show("本地路由已停止");
                    } else {
                        routerInstance().start(providerStore().config().routerPort);
                        toast.Show(std::format(
                            "本地路由已启动（127.0.0.1:{}）",
                            providerStore().config().routerPort));
                    }
                } catch (const std::exception& e) {
                    toast.Show(e.what());
                }
                revision = revision.Get() + 1;
            })
            .Checked(routerInstance().running()));
    entries.push_back(huxerui::MenuSection{});
    entries.push_back(huxerui::MenuItem("显示主窗口", [window] { window.Activate(); }));
    entries.push_back(huxerui::MenuSection{});
    entries.push_back(
        huxerui::MenuItem("退出", [application] { application.Quit(); }));
    return entries;
}

// 左列：图标侧边栏（无岛屿包裹，单套无色图标由主题 tint 自适应；
// 选中态用承载底块表达，悬停显示文字提示）。
[[huxerui::composable]] huxerui::View SideShell(huxerui::State<std::size_t> navPage) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    // 响应式：Compact(<600) 收窄侧栏宽度与内边距。
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    const IslandTheme islands = ResolveIslandTheme(theme);
    struct Item {
        huxerui::ImageResource icon;
        const char* tooltip;
        std::size_t page;
    };
    // 顶级侧栏（8 区块）：Agent 管理（内嵌二级工具栏）/ 本地路由 / 使用统计 /
    // MCP 服务器 / Skills / 会话 / 设置 / 关于。
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

    std::vector<huxerui::View> buttons;
    for (const Item& item : items) {
        const std::size_t page = item.page;
        huxerui::View button =
            huxerui::IconButton(item.icon, item.tooltip)
                .OnClick([tasks, navPage, page] {
                    // 切页会卸载内容子树：推迟出指针事件路径
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        co_await huxerui::Delay(std::chrono::duration<double>{0});
                        navPage = page;
                    });
                })
                .With(huxerui::Tooltip(item.tooltip));
        if (navPage.Get() == page) {
            button = std::move(button).With(
                huxerui::Background(islands.raised),
                huxerui::CornerRadius(islands.nested_radius));
        }
        buttons.push_back(std::move(button));
    }
    // 栏底留白：水墨长卷在整窗背景底部横带上露出（见下方 Background
    // ImageFill），侧栏不再单独挂装饰。
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

    // 关闭最小化到托盘：托盘可用时关闭请求只隐藏窗口（托盘菜单「显示主窗口」
    // 经 Activate 召回；「退出」走 application.Quit()，绕过本处理器正常终止）。
    // 托盘不可用时返回 false 继续平台默认关闭，避免进程藏死。
    window.OnCloseRequest([tray, window] {
        if (!tray.IsAvailable()) return false;
        window.Hide();
        return true;
    });

    // 路由自动启动（任务G）：首组合一次。config().routerEnabled 且路由器未运行
    // 时按 config().routerPort 启动；start 失败抛 std::runtime_error，toast 提示。
    huxerui::Lifecycle(
        [toast] {
            const auto& config = providerStore().config();
            if (config.routerEnabled && !routerInstance().running()) {
                try {
                    routerInstance().start(config.routerPort);
                } catch (const std::exception& e) {
                    toast.Show(e.what());
                }
            }
            return [] {};
        },
        0);

    const bool dark =
        themeMode.Get() == 1 || (themeMode.Get() == 0 && cfg::systemPrefersDark());
    const huxerui::ThemeSpec rootSpec = dark ? InkDarkThemeSpec() : InkLightThemeSpec();
    const IslandTheme rootIslands = ResolveIslandTheme(rootSpec);

    std::vector<huxerui::View> pages;
    pages.push_back(AgentPage(revision).Key("agents").With(huxerui::Grow(1.0F)));
    pages.push_back(RouterPage().Key("router").With(huxerui::Grow(1.0F)));
    pages.push_back(StatsPage().Key("stats").With(huxerui::Grow(1.0F)));
    pages.push_back(McpPage().Key("mcp").With(huxerui::Grow(1.0F)));
    pages.push_back(SkillsPage().Key("skills").With(huxerui::Grow(1.0F)));
    pages.push_back(SessionsPage().Key("sessions").With(huxerui::Grow(1.0F)));
    pages.push_back(SettingsPage(themeMode, revision)
                        .Key("settings").With(huxerui::Grow(1.0F)));
    pages.push_back(AboutPage().Key("about").With(huxerui::Grow(1.0F)));

    // 叠放根：全景水墨从页脚装饰升级为环境层。深浅主题分别使用低对比度画卷，
    // Cover 铺满窗口、中央刻意净空；轻岛屿让顶部远山和四角近景隐约透出。
    // 最外层仍刷海面底色，保证图片加载前和极端宽高比下背景稳定。
    huxerui::View content = huxerui::Stack {
        huxerui::Image(dark ? app::images::ink_backdrop_dark
                            : app::images::ink_backdrop_light)
            .Fit(huxerui::ImageFit::Cover)
            .Align(huxerui::HorizontalAlignment::Center,
                   huxerui::VerticalAlignment::Center),
        huxerui::Column {
            // 自定义标题栏：太极标 + 应用名 + 拖拽区（框架在其右侧渲染窗口
            // 按钮）。收窄 + 去背景：直接融入窗口海面底色；垂直零内边距，
            // 内容本身 24pt 高，与 title_bar_height 对齐。
            huxerui::WindowTitleBar {
                huxerui::Image(app::images::taiji)
                    .With(huxerui::Frame{.width = 16.0F, .height = 16.0F},
                          huxerui::WindowDragRegion{}),
                huxerui::Text("llm-switch")
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kChip)
                            .WithWeight(huxerui::FontWeight::Bold),
                        rootSpec.colors.on_surface})
                    .With(huxerui::WindowDragRegion{}),
                // 朱砂印章「易」（取其「变易/切换」意，应太极八卦题）：深浅
                // 主题都用固定印泥红 + 宣纸白字。
                huxerui::Text("易")
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(9.0F)
                            .WithWeight(huxerui::FontWeight::Bold),
                        huxerui::Color::Rgb(248, 245, 236)})
                    .With(huxerui::Background(huxerui::Color::Rgb(178, 58, 42)),
                          huxerui::CornerRadius(2.5F),
                          huxerui::Padding(
                              huxerui::EdgeInsets::Symmetric(3.5F, 1.5F)),
                          huxerui::WindowDragRegion{}),
                huxerui::Spacer{}.With(huxerui::Grow(1.0F),
                                       huxerui::WindowDragRegion{}),
            }
                .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(
                          rootSpec.spacing.small, 0.0F)),
                      huxerui::Spacing(rootSpec.spacing.small)),
            // 主行：图标侧栏（无岛屿包裹）+ 内容区；Grow 吃满标题栏之外剩余
            // 高度。内容区不再套外壳岛：区域划分由各页面自己的一级岛承担。
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
                  huxerui::Padding(huxerui::EdgeInsets{.bottom =
                                                           rootSpec.spacing.small}),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
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

    return InkThemed(dark, std::move(content));
}

} // namespace llmswitch::ui
