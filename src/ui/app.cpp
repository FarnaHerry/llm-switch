// app.cpp — 应用壳（岛屿架构 + 自定义标题栏 + 系统托盘，对齐 Clash-Flux 壳风）：
//   标题栏：应用名在左，太极导航锚点精确居中；悬停时以太极为中心向两侧
//     展开 8 个顶级页面图标，点击直接切页。框架在右侧渲染窗口按钮；标题栏
//     收窄为 24px 高、去背景直接融入窗口底色。主题为太极水墨风：
//     深色「玄墨」= 暖调近黑底 + 宣纸白主色；浅色「宣纸」= 米白纸面 + 浓墨主色；
//     状态色仅 error 保留朱砂红。
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

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "ui.h"
#include "app.h"
#include "app_resources.h"
#include "single_instance.h"

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

// 关闭确认弹窗使用自定义内容，保证弹层卡片、正文和操作按钮都走应用主题。
// 托盘宿主可能不存在（例如 Linux 桌面未提供 StatusNotifierHost），此时仍显示
// 「最小化到托盘」选项但明确禁用，避免用户误以为点击后应用会安全地隐藏。
[[huxerui::composable]] huxerui::View CloseConfirmationContent(
    huxerui::DialogContext context, huxerui::ApplicationHandle application,
    huxerui::SystemTrayHandle tray, huxerui::WindowHandle window,
    huxerui::ToastHandle toast) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const huxerui::TaskScope tasks = huxerui::UseTaskScope();
    const bool trayAvailable = tray.IsAvailable();
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
        std::move(trayHint),
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

// 标题栏中央导航：静止时只显示太极，悬停时以它为中心对称展开
// 全部 8 个顶级页面图标。图标使用标题栏局部的紧凑 IconButtonStyle，不改变
// 正文内按钮的 40pt 交互尺寸；选中态仍以语义色承载底块表达。
[[huxerui::composable]] huxerui::View TitleBarNavigation(
    huxerui::State<std::size_t> navPage) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    auto expanded = huxerui::UseState(false);
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

    const auto makeButton = [navPage, &islands, &theme](const Item& item) {
        const std::size_t page = item.page;
        huxerui::View button =
            huxerui::IconButton(item.icon, item.tooltip)
                .OnClick([navPage, page] { navPage = page; })
                .With(huxerui::Tooltip(item.tooltip))
                .Key(std::string("title-nav:") + item.tooltip);
        if (navPage.Get() == page) {
            button = std::move(button).With(
                huxerui::Background(theme.colors.secondary_container),
                huxerui::CornerRadius(islands.nested_radius));
        }
        return button;
    };
    huxerui::View taiji = huxerui::Stack {
        huxerui::Image(app::images::taiji)
            .With(huxerui::Frame{.width = 16.0F, .height = 16.0F}),
    }
        .With(huxerui::Frame{.width = kTitleBarContentHeight,
                             .height = kTitleBarContentHeight},
              huxerui::Align(huxerui::HorizontalAlignment::Center,
                             huxerui::VerticalAlignment::Center),
              huxerui::Semantics{.role = huxerui::SemanticRole::Image,
                                  .label = "页面导航"},
              huxerui::Tooltip("悬停展开页面导航"))
        .Key("title-nav:taiji");

    std::vector<huxerui::View> children;
    children.reserve(expanded.Get() ? items.size() + 1 : 1);
    if (expanded.Get()) {
        for (std::size_t i = 0; i < items.size() / 2; ++i) {
            children.push_back(makeButton(items[i]));
        }
    }
    children.push_back(taiji);
    if (expanded.Get()) {
        for (std::size_t i = items.size() / 2; i < items.size(); ++i) {
            children.push_back(makeButton(items[i]));
        }
    }

    huxerui::View navigation = huxerui::Row(std::move(children))
        .With(huxerui::Frame{.height = kTitleBarContentHeight},
              huxerui::Spacing(2.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
        .On<huxerui::ViewEvents::Hover>(
            [expanded](const huxerui::HoverEvent& event) {
                expanded = event.type != huxerui::HoverEventType::Leave;
            });
    if (expanded.Get()) {
        navigation = std::move(navigation).With(
            huxerui::Background(islands.overlay),
            huxerui::CornerRadius(kTitleBarContentHeight * 0.5F),
            huxerui::Border(islands.outline_soft, 0.75F));
    }

    huxerui::IconButtonStyle titleIcons = huxerui::IconButtonStyle::Default();
    titleIcons.foreground = theme.colors.on_surface;
    titleIcons.disabled_foreground = theme.colors.on_surface_variant;
    titleIcons.icon_size = 16.0F;
    titleIcons.minimum_interactive_size = kTitleBarContentHeight;
    titleIcons.state_layer_size = 22.0F;
    titleIcons.corner_radius = islands.nested_radius;
    huxerui::ThemeDefinition overrides;
    overrides.Set(titleIcons);
    return huxerui::Theme(std::move(overrides), navigation);
}

// 页面宿主单独订阅导航状态。IndexedPages 保留全部页面及其局部状态，
// 页面切换的重组范围被限制在页面宿主，不再让 AppRoot 重组整套窗口壳。
[[huxerui::composable]] huxerui::View TopLevelPageHost(
    huxerui::State<std::size_t> navPage,
    const std::shared_ptr<std::vector<huxerui::View>>& cachedPages) {
    return huxerui::IndexedPages(*cachedPages, navPage.Get())
        .With(huxerui::Grow(1.0F));
}

// 顶级页面宿主作用域：只创建一次固定的页面声明缓存。标题栏导航与
// TopLevelPageHost 各自订阅 navPage，切页不会向上冒泡到 AppRoot 或重建背景。
[[huxerui::composable]] huxerui::View TopLevelNavigation(
    huxerui::State<std::size_t> navPage, huxerui::State<int> revision,
    huxerui::State<int> themeMode) {
    auto pageCache =
        huxerui::UseState<std::shared_ptr<std::vector<huxerui::View>>>({});
    std::shared_ptr<std::vector<huxerui::View>> cachedPages = pageCache.Get();
    if (!cachedPages || cachedPages->size() != 8) {
        auto nextPages = std::make_shared<std::vector<huxerui::View>>();
        nextPages->reserve(8);
        nextPages->push_back(
            AgentPage(revision, navPage).Key("agents").With(huxerui::Grow(1.0F)));
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
        nextPages->push_back(SettingsPage(themeMode, revision)
                                 .Key("settings")
                                 .With(huxerui::Grow(1.0F)));
        nextPages->push_back(
            AboutPage().Key("about").With(huxerui::Grow(1.0F)));
        pageCache = nextPages;
        cachedPages = std::move(nextPages);
    }

    return TopLevelPageHost(navPage, cachedPages);
}

} // namespace

[[huxerui::composable]] huxerui::View AppRoot() {
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    const huxerui::WindowHandle window = huxerui::UseWindow();
    const huxerui::SystemTrayHandle tray = application.SystemTray();
    const bool trayAvailable = tray.IsAvailable();
    auto toast = huxerui::UseToast();
    auto tasks = huxerui::UseTaskScope();

    // A second Linux process forwards here; TaskScope::Post returns to the UI thread before activating the window.
    huxerui::Lifecycle(
        [window, tasks] {
            single_instance::SetActivationHandler([window, tasks] {
                tasks.Post([window] { window.Activate(); });
            });
            return [] { single_instance::ClearActivationHandler(); };
        });

    // 初始值在 UseState 之前算好（组合体内不写 State）：
    // 主题模式 0=跟随系统 1=深色 2=浅色；配置里的 themeMode 字符串映射。
    int initialThemeMode = 0;
    {
        const std::string& saved = providerStore().config().themeMode;
        if (saved == "dark") initialThemeMode = 1;
        if (saved == "light") initialThemeMode = 2;
    }
    auto themeMode = huxerui::UseState<int>(std::move(initialThemeMode));
    // 顶级页面状态同时供标题栏导航与保持挂载的 IndexedPages 宿主订阅。
    // AppRoot 本身不读取它，切页只重组这两个局部子树。
    auto navPage = huxerui::UseState<std::size_t>(pages::kAgents);
    // 全局变更计数：任何写库操作（含托盘切换）后 +1，驱动托盘菜单重建
    // （Lifecycle 依赖）与页面重读。
    auto revision = huxerui::UseState<int>(0);

    // 托盘：始终提交图标 + 菜单；点击托盘图标激活主窗口。Linux 的托盘宿主
    // 可能晚于应用出现，HuxerUI 会在不可用期间暂存展示，宿主恢复后自动显示。
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

    // 托盘宿主消失时恢复窗口，避免已经隐藏的窗口失去可见入口。
    huxerui::Lifecycle(
        [window, trayAvailable] {
            if (!trayAvailable) window.Show();
        },
        trayAvailable);

    // 路由上游会话绑定（首组合）：生产桥接 = HuxerUI 平台 HttpClient。UseService/
    // UseTaskScope 在组合体内求值；本调用早于下方自启与一切托盘/页面/事件
    // 调用点——routerInstance() 单例也在此首次构造。
    BindRouterUpstreamSession(huxerui::UseService<huxerui::HttpClient>(),
                              huxerui::UseTaskScope());

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

    // 叠放根：全景水墨从页脚装饰升级为环境层。深浅主题分别使用低对比度画卷，
    // Fill 铺满窗口、中央刻意净空；轻岛屿让顶部远山和四角近景隐约透出。
    // 最外层仍刷海面底色，保证图片加载前和极端宽高比下背景稳定。
    huxerui::View content = huxerui::Stack {
        huxerui::Image(dark ? app::images::ink_backdrop_dark
                            : app::images::ink_backdrop_light)
            // 极窄窗口下 Cover 的浮点裁剪源矩形可能比 SVG viewBox 多出
            // 极小误差，触发 HuxerUI 的边界校验。背景是装饰层，Fill 使用
            // 完整源矩形，优先保证窗口缩放始终安全。
            .Fit(huxerui::ImageFit::Fill)
            .Align(huxerui::HorizontalAlignment::Center,
                   huxerui::VerticalAlignment::Center),
        // Canvas+Path 程序化泼墨伪元素：环境层，叠在水墨画卷与内容之间，
        // 右上为主、左下淡些呼应；seed 固定，形态恒定不随重组抖动。
        // 不要包 Align/Frame：Align 节点会把画布按父约束撑满（画布本就是
        // 全幅），落点由 anchor 在画布内定位。
        InkSplash(0x51B7U, 1.0F, InkSplashAnchor::TopEnd),
        InkSplash(0x2F3DU, 0.65F, InkSplashAnchor::BottomStart),
        huxerui::Column {
            // Stack 把左侧应用名与精确居中的太极导航叠放，避免
            // 应用名宽度把太极推离中心；窗口按钮仍由框架在右侧渲染。
            huxerui::WindowTitleBar {
                huxerui::Stack {
                    huxerui::Row {
                        huxerui::Text("llm-switch")
                            .Style(huxerui::TextStyle{
                                huxerui::Font::System(font_size::kChip)
                                    .WithWeight(huxerui::FontWeight::Bold),
                                rootSpec.colors.on_surface}),
                        huxerui::Spacer(),
                    }.With(huxerui::CrossAlign(
                        huxerui::CrossAxisAlignment::Center)),
                    huxerui::Row {
                        huxerui::Spacer(),
                        TitleBarNavigation(navPage),
                        huxerui::Spacer(),
                    }.With(huxerui::CrossAlign(
                        huxerui::CrossAxisAlignment::Center)),
                }.With(huxerui::Grow(1.0F),
                       huxerui::Align(huxerui::HorizontalAlignment::Stretch,
                                      huxerui::VerticalAlignment::Stretch)),
            }
                .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(
                          rootSpec.spacing.small, 0.0F))),
            // 内容区独占标题栏之外的整行，不再为左侧顶级导航预留宽度。
            TopLevelNavigation(navPage, revision, themeMode)
                .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(
                          rootSpec.spacing.small, 0.0F)),
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

    // Stack 的 Stretch 只拉伸子项，不会让 Stack 自身从自然尺寸扩展到窗口。
    // 用一个有 Grow 子项的 Column 把完整窗口约束传入 Stack，保证四边都能
    // 随窗口拉伸，也让内容区获得稳定的有限滚动视口。
    // DialogHandle 必须在 InkThemed 的子作用域内获取，否则关闭确认框会捕获
    // HuxerUI 默认 Environment，无法使用应用的主体色和对话框样式。
    huxerui::View windowBehavior = huxerui::Scope(
        [application, tray, window, toast] {
            const auto dialog = huxerui::UseDialog();
            const auto tasks = huxerui::UseTaskScope();
            const auto showCloseConfirmation =
                [application, tray, window, toast, dialog] {
                    dialog.Show(
                        [application, tray, window,
                         toast](huxerui::DialogContext context) -> huxerui::View {
                            return CloseConfirmationContent(
                                context, application, tray, window, toast);
                        },
                        huxerui::DialogOptions{});
                };

            // 「最小化到托盘」同时覆盖标题栏最小化按钮和系统最小化请求。
            // 其他关闭策略保留平台原生最小化语义；托盘不可用时不吞掉请求，
            // 让窗口仍能普通最小化，并给出原因提示。
            window.OnMinimizeRequest([tray, window, toast, tasks] {
                if (providerStore().config().closeBehavior != "tray") return false;
                if (!tray.IsAvailable()) {
                    toast.Show("系统托盘当前不可用，已保留普通最小化");
                    return false;
                }
                // 标题栏按钮仍处于 PointerUp 派发中；延后隐藏，避免同步清空
                // pointer session 后事件收尾继续访问失效迭代器。
                tasks.Post([window] { window.Hide(); });
                return true;
            });

            window.OnCloseRequest([tray, window, showCloseConfirmation, tasks] {
                const std::string& behavior = providerStore().config().closeBehavior;
                if (behavior == "tray") {
                    // 托盘不可用时不能返回 false，否则原生关闭路径会直接退出
                    // 应用；改为询问弹窗，用户仍可取消关闭。
                    if (!tray.IsAvailable()) {
                        showCloseConfirmation();
                        return true;
                    }
                    tasks.Post([window] { window.Hide(); });
                    return true;
                }
                if (behavior == "quit") return false;

                showCloseConfirmation();
                return true;
            });
            // 保留一个实际挂载的空布局节点；默认构造 View 没有 ViewSpec，Scope
            // 可能被运行时省略，导致关闭处理器没有机会注册。
            return huxerui::Row {};
        });

    huxerui::View filledContent = huxerui::Column {
        std::move(windowBehavior),
        std::move(content).With(huxerui::Grow(1.0F)),
    }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
           huxerui::Grow(1.0F));

    return InkThemed(dark, std::move(filledContent));
}

} // namespace llmswitch::ui
