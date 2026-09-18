// app.cpp — 应用壳（岛屿架构 + 自定义标题栏 + 系统托盘，对齐 Clash-Flux 壳风）：
//   标题栏：应用名与莲花标志在左，五瓣莲花导航锚点精确居中，两侧以圆点、
//     菱形和细线装饰；悬停时整组高亮，并在屏幕中央展开环形顶级页面图标。
//     框架在右侧渲染窗口按钮；标题栏
//     收窄为 24px 高、去背景直接融入窗口底色。主题为冷调石板风：
//     深色「深海」= 海军蓝底 + 青蓝强调色；浅色「晴石」= 冷白底 + 天蓝强调色；
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

// 冷调主题：深色「深海」= 海军蓝海面（#101923）+ 青蓝强调色；浅色「晴石」=
// 冷白纸面（#FDFDFD）+ 天蓝强调色。文本用冷调石板蓝-灰阶，交互态（按钮/
// 选中/开关/标题栏 hover）统一由 primary 承担；error 保留语义红（冷调）。
// 强调色比参考图实测的亮蓝（#4595F7）略深一档，换取白字在其上的对比度
// （≈4.2:1），按钮标签因此仍然可读。
huxerui::ThemeSpec AppDarkThemeSpec() {
    huxerui::ThemeSpec spec = huxerui::MaterialDarkThemeSpec();
    spec.typography = huxerui::TypographyScheme{
        .body_large = 16.0F,
        .body_medium = font_size::kBody,
        .body_small = font_size::kChip,
        .label_large = font_size::kBody,
        .title_large = font_size::kTitle,
        .headline_small = 24.0F,
    };
    spec.colors.primary = huxerui::Color::Rgb(56, 189, 248);       // 青蓝强调 #38BDF8
    spec.colors.on_primary = huxerui::Color::Rgb(6, 34, 49);       // 强调色上翻深墨青
    spec.colors.primary_container = huxerui::Color::Rgb(27, 58, 80);
    spec.colors.on_primary_container = huxerui::Color::Rgb(190, 231, 251);
    spec.colors.secondary = huxerui::Color::Rgb(159, 176, 194);
    spec.colors.on_secondary = huxerui::Color::Rgb(16, 25, 35);
    spec.colors.secondary_container = huxerui::Color::Rgb(34, 48, 63);   // hover / 选中底块
    spec.colors.on_secondary_container = huxerui::Color::Rgb(232, 238, 246);
    spec.colors.tertiary_container = huxerui::Color::Rgb(30, 58, 74);
    spec.colors.on_tertiary_container = huxerui::Color::Rgb(185, 217, 232);
    spec.colors.background = huxerui::Color::Rgb(16, 25, 35);      // 海军蓝海面 #101923
    spec.colors.surface = huxerui::Color::Rgb(23, 35, 48);
    spec.colors.surface_container_low = huxerui::Color::Rgb(20, 30, 42);
    spec.colors.surface_container = huxerui::Color::Rgb(26, 36, 49);      // 二级岛卡片
    spec.colors.surface_container_high = huxerui::Color::Rgb(32, 43, 57); // hover 面 / 弹窗
    spec.colors.surface_container_highest = huxerui::Color::Rgb(38, 49, 63);
    spec.colors.on_surface = huxerui::Color::Rgb(232, 238, 246);   // 石板冷白正文
    spec.colors.on_surface_variant = huxerui::Color::Rgb(147, 163, 182); // 次要冷灰
    spec.colors.outline = huxerui::Color::Rgb(44, 58, 73);
    spec.colors.inverse_surface = huxerui::Color::Rgb(232, 238, 246);
    spec.colors.inverse_on_surface = huxerui::Color::Rgb(16, 25, 35);
    spec.colors.scrim = huxerui::Color::Rgb(4, 10, 18, 0.55F);
    spec.colors.error = huxerui::Color::Rgb(240, 115, 108);        // 语义红 #F0736C
    return spec;
}

huxerui::ThemeSpec AppLightThemeSpec() {
    huxerui::ThemeSpec spec = huxerui::MaterialLightThemeSpec();
    spec.typography = huxerui::TypographyScheme{
        .body_large = 16.0F,
        .body_medium = font_size::kBody,
        .body_small = font_size::kChip,
        .label_large = font_size::kBody,
        .title_large = font_size::kTitle,
        .headline_small = 24.0F,
    };
    // 冷白石面：海面近纯白，岛屿与其只差一档冷灰；卡片走浅蓝灰玻璃面 +
    // 冷灰描边，hover 面提亮为淡蓝。
    spec.colors.primary = huxerui::Color::Rgb(47, 123, 230);       // 天蓝强调 #2F7BE6
    spec.colors.on_primary = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.primary_container = huxerui::Color::Rgb(220, 234, 253);
    spec.colors.on_primary_container = huxerui::Color::Rgb(18, 58, 107);
    spec.colors.secondary = huxerui::Color::Rgb(91, 108, 129);     // 石板灰 #5B6C81
    spec.colors.on_secondary = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.secondary_container = huxerui::Color::Rgb(233, 240, 248); // hover / 选中底块
    spec.colors.on_secondary_container = huxerui::Color::Rgb(30, 42, 58);
    spec.colors.tertiary_container = huxerui::Color::Rgb(228, 236, 245);
    spec.colors.on_tertiary_container = huxerui::Color::Rgb(43, 59, 78);
    spec.colors.background = huxerui::Color::Rgb(253, 253, 253);   // 冷白海面 #FDFDFD
    spec.colors.surface = huxerui::Color::Rgb(246, 249, 252);
    spec.colors.surface_container_low = huxerui::Color::Rgb(243, 247, 251);
    spec.colors.surface_container = huxerui::Color::Rgb(241, 245, 250);  // 二级岛卡片
    spec.colors.surface_container_high = huxerui::Color::Rgb(233, 240, 248); // hover 面 / 弹窗
    spec.colors.surface_container_highest = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.on_surface = huxerui::Color::Rgb(30, 42, 58);      // 深石板蓝正文 #1E2A3A
    spec.colors.on_surface_variant = huxerui::Color::Rgb(85, 103, 125); // 次要冷灰 #55677D
    spec.colors.outline = huxerui::Color::Rgb(220, 227, 235);      // 冷灰描边 #DCE3EB
    spec.colors.inverse_surface = huxerui::Color::Rgb(30, 42, 58);
    spec.colors.inverse_on_surface = huxerui::Color::Rgb(245, 248, 252);
    spec.colors.scrim = huxerui::Color::Rgb(9, 18, 30, 0.38F);
    spec.colors.error = huxerui::Color::Rgb(214, 69, 69);          // 语义红 #D64545
    return spec;
}

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

// 中央圆盘的同心环、八向连线与节点均由 Canvas 按主题色绘制；它们只是环境
// 装饰，不参与命中，页面动作仍由标准 IconButton 承担。
huxerui::View RadialNavigationArtwork(huxerui::Color color) {
    color.alpha = 0.34F;
    return huxerui::Canvas([color](huxerui::PaintContext& paint,
                                   huxerui::Size size) {
        const huxerui::Point center{size.width * 0.5F, size.height * 0.5F};
        const huxerui::StrokeStyle fine{
            .width = 0.8F,
            .cap = huxerui::StrokeCap::Round,
            .join = huxerui::StrokeJoin::Round,
        };
        const huxerui::StrokeStyle strong{
            .width = 1.15F,
            .cap = huxerui::StrokeCap::Round,
            .join = huxerui::StrokeJoin::Round,
        };

        paint.DrawArc(center, 78.0F, 0.0F, 360.0F, color, fine);
        paint.DrawArc(center, 112.0F, 0.0F, 360.0F, color, fine);
        paint.DrawArc(center, 144.0F, 0.0F, 360.0F, color, strong);

        constexpr std::array<huxerui::Point, 8> directions{
            huxerui::Point{0.0F, -1.0F}, huxerui::Point{0.707F, -0.707F},
            huxerui::Point{1.0F, 0.0F}, huxerui::Point{0.707F, 0.707F},
            huxerui::Point{0.0F, 1.0F}, huxerui::Point{-0.707F, 0.707F},
            huxerui::Point{-1.0F, 0.0F}, huxerui::Point{-0.707F, -0.707F},
        };
        for (const auto& direction : directions) {
            const huxerui::Point start{center.x + direction.x * 52.0F,
                                        center.y + direction.y * 52.0F};
            const huxerui::Point end{center.x + direction.x * 146.0F,
                                      center.y + direction.y * 146.0F};
            paint.DrawLine(start, end, color, fine);
            paint.DrawCircle(huxerui::Point{center.x + direction.x * 112.0F,
                                             center.y + direction.y * 112.0F},
                             2.2F, color);
        }
    }).With(huxerui::Frame{.width = 360.0F, .height = 360.0F});
}

// 切页时序：轮盘与旧页面**一同**缩回屏幕中心那颗莲花 → 在不可见时换页 →
// 新页面从同一中心展开。收起段的时长不能省：AnimateTo 是从当前值补间，
// 目标必须真正走到收起态再换页，否则新页面只会从 ~1.0 抖一下，看不出展开。
// 收起比展开快一档。
constexpr double kCollapseSeconds = 0.18;

// 根级径向导航：中轴区域贯穿标题栏到圆盘，保证 Hover 能平滑交接；圆盘本身
// 保持 8 个既有顶级页面，按顺时针方向均匀排布并复用 navPage。
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
        huxerui::Point{0.0F, -144.0F}, huxerui::Point{102.0F, -102.0F},
        huxerui::Point{144.0F, 0.0F}, huxerui::Point{102.0F, 102.0F},
        huxerui::Point{0.0F, 144.0F}, huxerui::Point{-102.0F, 102.0F},
        huxerui::Point{-144.0F, 0.0F}, huxerui::Point{-102.0F, -102.0F},
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
    radialChildren.push_back(RadialNavigationArtwork(theme.colors.primary));
    for (std::size_t i = 0; i < items.size(); ++i) {
        radialChildren.push_back(makeButton(items[i], positions[i]));
    }
    radialChildren.push_back(
        huxerui::Stack {
            huxerui::Image(app::images::lotus_bud)
                .Tint(theme.colors.on_surface)
                .With(huxerui::Frame{.width = 36.0F, .height = 36.0F},
                      huxerui::Opacity(huxerui::AnimateTo(
                          revealed.Get() ? 0.0F : 1.0F, motion)),
                      huxerui::Scale(huxerui::AnimateTo(
                          revealed.Get() ? 0.78F : 1.0F, motion))),
            huxerui::Image(app::images::lotus_bloom)
                .Tint(theme.colors.on_surface)
                .With(huxerui::Frame{.width = 40.0F, .height = 40.0F},
                      huxerui::Opacity(huxerui::AnimateTo(
                          revealed.Get() ? 1.0F : 0.0F, motion)),
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
        std::move(radial),
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
            std::move(hoverCorridor),
            huxerui::Spacer(),
        }.With(huxerui::Grow(1.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));

    return huxerui::Stack {
        huxerui::Row {}.With(
            huxerui::Grow(1.0F),
            huxerui::Background(huxerui::Color::Rgb(12, 18, 24, 0.46F)),
            huxerui::Opacity(huxerui::AnimateTo(
                revealed.Get() ? 1.0F : 0.0F, motion))),
        std::move(navigation),
    }.With(huxerui::Grow(1.0F),
           huxerui::Align(huxerui::HorizontalAlignment::Stretch,
                          huxerui::VerticalAlignment::Stretch));
}

// 托盘跟随应用展示状态：前台/可见时使用盛放莲花，后台（最小化或隐藏）时
// 使用闭合花苞。生命周期读取封装在独立组合边界内，避免状态切换重组窗口正文。
[[huxerui::composable]] huxerui::View SystemTrayPresentation(
    huxerui::ApplicationHandle application, huxerui::SystemTrayHandle tray,
    huxerui::WindowHandle window, huxerui::ToastHandle toast,
    huxerui::State<int> revision) {
    const bool minimized = application.LifecycleState() ==
                           huxerui::ApplicationLifecycleState::Background;

    tray.OnActivate([window] { window.Activate(); });
    huxerui::Lifecycle(
        [application, tray, window, toast, revision, minimized] {
            tray.Show(minimized ? app::images::lotus_tray_bud
                                : app::images::lotus_tray_bloom,
                      huxerui::SystemTrayOptions{
                          .tooltip = "llm-switch",
                          .menu = BuildTrayMenu(window, application, toast,
                                                revision)});
            return [tray] { tray.Hide(); };
        },
        revision, minimized);

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
              .duration = revealed ? 0.30 : kCollapseSeconds,
              .easing = huxerui::Easing::EaseOut}};
    const std::size_t current = navPage.Get();
    // 切页变换只套在**当前可见页**上。宿主里 8 个页面全部保持挂载（各自保留
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
    huxerui::State<std::size_t> navPage, huxerui::State<int> revision,
    huxerui::State<int> themeMode, huxerui::State<bool> pageReveal) {
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

    return TopLevelPageHost(navPage, cachedPages, pageReveal);
}

// 环境光：冷调主题的窗口底纹。深色是海军蓝底上一层柔和青蓝辉光（偏向画面
// 上方，对齐参考图深色半边的柔和光斑），浅色是冷白底顶部一层极淡天蓝洗色；
// 两侧背景都保持干净——旧的全景水墨画卷与程序化泼墨已随水墨身份退役。
// Canvas 直接铺满宿主（不要包 Align/Frame：Align 会把画布按父约束撑满，
// 画布本就是全幅），光斑按画布尺寸取比例，窗口缩放时不失真。
huxerui::View AmbientGlow(bool dark) {
    return huxerui::Canvas(
        [dark](huxerui::PaintContext& paint, huxerui::Size size) {
            huxerui::Color inner =
                dark ? huxerui::Color::Rgb(56, 189, 248, 0.10F)
                     : huxerui::Color::Rgb(47, 123, 230, 0.06F);
            huxerui::Color outer = inner;
            outer.alpha = 0.0F;
            paint.DrawRect(huxerui::Rect{0.0F, 0.0F, size.width, size.height},
                           huxerui::RadialGradient{
                               .center = {0.5F, dark ? 0.30F : 0.10F},
                               .radius = {0.66F, dark ? 0.60F : 0.45F},
                               .stops = {{0.0F, inner}, {1.0F, outer}},
                           });
        });
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
    // 标题栏莲花与根级径向浮层共享开合状态；AppRoot 不读取它，避免 Hover
    // 让整套窗口内容重组。
    auto navigationOpen = huxerui::UseState(false);
    // 全局变更计数：任何写库操作（含托盘切换）后 +1，驱动托盘菜单重建
    // （Lifecycle 依赖）与页面重读。
    auto revision = huxerui::UseState<int>(0);
    // 页面展开进度：切页时由径向导航的时序先置 false、换页后再置 true，
    // 让新页面从中心缩放+淡入展开（见 RadialNavigationOverlay 的点击时序）。
    auto pageReveal = huxerui::UseState(true);

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
    const huxerui::ThemeSpec rootSpec = dark ? AppDarkThemeSpec() : AppLightThemeSpec();

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
                huxerui::Spacer(),
            }
                .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(
                          rootSpec.spacing.small, 0.0F))),
            // 内容区独占标题栏之外的整行，不再为左侧顶级导航预留宽度。
            // 页面容器承担切页展开：缩放轴心取容器中心，配合淡入即"从中心展开"。
            TopLevelNavigation(navPage, revision, themeMode, pageReveal)
                .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(
                          rootSpec.spacing.small, 0.0F)),
                      huxerui::Grow(1.0F)),
        }
            .With(huxerui::Spacing(rootSpec.spacing.extra_small),
                  huxerui::Padding(huxerui::EdgeInsets{.bottom =
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
        SystemTrayPresentation(application, tray, window, toast, revision),
        std::move(windowBehavior),
        std::move(content).With(huxerui::Grow(1.0F)),
    }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
           huxerui::Grow(1.0F));

    return AppThemed(dark, std::move(filledContent));
}

} // namespace llmswitch::ui
