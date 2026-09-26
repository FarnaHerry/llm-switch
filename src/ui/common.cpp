// common.cpp — 岛屿语义层（ResolveIslandTheme）、页面骨架/
// 卡片（二级岛）/ 弹窗卡片等跨页通用部件，以及全局 ProviderStore 持有点。
#include <huxerui/huxerui.h>

#include <cmath>
#include <string>
#include <string_view>

#include "ui.h"
#include "app_resources.h"

import llmswitch.config;
import llmswitch.models;
import llmswitch.store;

namespace llmswitch::ui {

store::ProviderStore& providerStore() {
    // 首次访问即加载（含首次导入收编）；进程内唯一实例，UI 线程独占。
    static store::ProviderStore store = store::ProviderStore::load();
    return store;
}

huxerui::ImageResource ToolIcon(std::string_view iconName) {
    if (iconName == "claudecode") {
        return app::images::claudecode;
    }
    if (iconName == "claude") {
        return app::images::claude;
    }
    if (iconName == "codex") {
        return app::images::codex;
    }
    if (iconName == "opencode") {
        return app::images::opencode;
    }
    if (iconName == "pi") {
        return app::images::pi;
    }
    if (iconName == "dsh") {
        return app::images::dsh;
    }
    if (iconName == "hermes") {
        return app::images::hermes;
    }
    if (iconName == "gemini") {
        return app::images::gemini;
    }
    if (iconName == "qwen") {
        return app::images::qwen;
    }
    if (iconName == "zcode") {
        return app::images::zcode;
    }
    return app::images::agents;
}

std::string_view ToolName(std::string_view tool) {
    // 展示名以注册表为准；未注册（不应发生）原样回显 id。
    if (const auto* spec = models::findTool(tool)) return spec->displayName;
    return tool;
}

IslandTheme ResolveIslandTheme(const huxerui::ThemeSpec& theme) {
    // 轻岛屿：页面更接近连续宣纸/玄墨画布，实体卡片仍保留足够承托；
    // overlay 保持近不透明，确保弹层在全景水墨上可读。
    const auto translucent = [](huxerui::Color c, float a) {
        c.alpha = a;
        return c;
    };
    // 状态色深浅分档：ThemeSpec 无 success/warning 槽位，按海面亮度（WCAG
    // 相对亮度）选一组——浅色用深一档的绿/琥珀保证作文字 ≥4.5:1，深色维持
    // 在深底上对比度本就充足的亮色。
    const auto luminance = [](huxerui::Color c) {
        const auto channel = [](float v) {
            return v <= 0.04045F ? v / 12.92F
                                 : std::pow((v + 0.055F) / 1.055F, 2.4F);
        };
        return 0.2126F * channel(c.red) + 0.7152F * channel(c.green) +
               0.0722F * channel(c.blue);
    };
    const bool dark = luminance(theme.colors.background) < 0.5F;
    return IslandTheme{
        .page_gap = theme.spacing.extra_small,
        .island_padding = theme.spacing.medium,
        .island_radius = 10.0F,
        .nested_radius = 6.0F,
        .ocean = theme.colors.background,
        .base = translucent(theme.colors.surface_container_low, 0.48F),
        .raised = translucent(theme.colors.surface_container, 0.78F),
        .overlay = translucent(theme.colors.surface_container_highest, 0.95F),
        .outline_soft = translucent(theme.colors.outline, 0.62F),
        .success = dark ? huxerui::Color::Rgb(22, 163, 74)   // #16A34A
                        : huxerui::Color::Rgb(21, 128, 61),  // #15803D
        .on_success = dark ? huxerui::Color::Rgb(6, 34, 49)  // 白字仅 3.3:1，翻墨青
                           : huxerui::Color::Rgb(255, 255, 255),
        .warning = dark ? huxerui::Color::Rgb(202, 138, 4)   // #CA8A04
                        : huxerui::Color::Rgb(180, 84, 10),  // #B4540A
    };
}

[[huxerui::composable]] huxerui::View PageScaffold(const std::string& title,
                                                   huxerui::View actions,
                                                   huxerui::View content) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    // 响应式：Compact(<600) 收窄页面内边距。
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    // 顶级页面不再有自己的卡片：页面与标题栏是同一块窗口表面，靠 AmbientGlow
    // 的环境光与下面分层的内容（PageSection / Card / QuietCard）组织信息。
    // 页面只负责内边距与滚动；左右边距同时是壳层标题栏的左右边距
    // （app.cpp 的 shellInset），应用名与页面标题因此共享同一条左边线。
    // 顶部不留内边距（top = 0）且壳层也不留 Spacing：页面标题紧接标题栏下沿，
    // 两者之间没有额外高度；标题栏自身的高度由平台解析，页面排在它之后贴合。
    const float inset = compact ? theme.spacing.medium : theme.spacing.large;
    huxerui::View body = content;
    return huxerui::Column {
        huxerui::Row {
            huxerui::Text(title, huxerui::TextRole::Title),
            huxerui::Spacer(),
            std::move(actions),
        }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        std::move(body).With(huxerui::Grow(1.0F)),
    }.With(huxerui::Padding(huxerui::EdgeInsets{.top = 0.0F,
                                                .right = inset,
                                                .bottom = inset,
                                                .left = inset}),
           huxerui::Spacing(theme.spacing.medium),
           huxerui::ClipChildren(),
           huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View Card(huxerui::View content) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    huxerui::View card = content;
    return std::move(card).With(
        huxerui::Background(islands.raised),
        huxerui::CornerRadius(islands.nested_radius),
        huxerui::Border(islands.outline_soft, 1.0F),
        huxerui::Padding(islands.island_padding));
}

// 列表条目卡：与 Card 同为 raised 表面 + 6pt 圆角，但去掉描边——列表里
// 逐项画框会退成「一串格子」，条目间靠表面色差与缝隙分层即可。用于
// 供应商/技能/MCP/会话等重复条目；独立分区仍用 Card。
[[huxerui::composable]] huxerui::View QuietCard(huxerui::View content) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    huxerui::View card = content;
    return std::move(card).With(
        huxerui::Background(islands.raised),
        huxerui::CornerRadius(islands.nested_radius),
        huxerui::Padding(islands.island_padding));
}

// 平铺分区：标题 + 内容直接落在一级岛表面，不包 raised 卡——一页连排多张
// 盒子正是「卡片滥用」的来源；分区之间由 SectionDivider 的发丝线 + 页面
// 缝隙划分。仅需要强调的独立块（如关于页头部）保留 Card。
[[huxerui::composable]] huxerui::View PageSection(huxerui::View title,
                                                  huxerui::View content) {
    huxerui::View section_title = title;
    huxerui::View section_content = content;
    return huxerui::Column {
        std::move(section_title),
        std::move(section_content),
    }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 分区之间的发丝分隔线（主题 Divider 样式），上下留小缝呼吸。
[[huxerui::composable]] huxerui::View SectionDivider() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Divider()
        .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(
                  theme.spacing.small, 0.0F)));
}

// 加载/刷新指示：无限自转的刷新图标。写法照 HuxerUI 自带示例
// examples/ui_gallery 的 OrbitCanvasPreview——先用 SnapSpec 落在 0°，挂载后
// 再翻到 AnimateTo(-360°) + 无限线性迭代。这一步不能省：Rotation 扩展在挂载
// 时是把值直接 Set 到目标的，之后只有修饰符再次变化才会走 AnimateTo，直接写
// 死 AnimateTo(-360°) 的结果是停在 360°（视觉上完全不动）。
// reduced_motion 下保持静止（示例同款判断），不自作主张强行转。
[[huxerui::composable]] huxerui::View SpinningRefreshIcon(float size,
                                                          huxerui::Color tint,
                                                          bool active) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto started = huxerui::UseState(false);
    huxerui::Lifecycle([started] { started = true; });
    const bool spinning = active && started.Get() && !theme.motion.reduced_motion;
    return huxerui::Image(app::images::refresh)
        .Tint(tint)
        .With(huxerui::Frame{.width = size, .height = size},
              huxerui::Rotation(
                  spinning
                      ? huxerui::AnimateTo(
                            -360.0F,
                            huxerui::TweenSpec{1.0, huxerui::Easing::Linear},
                            huxerui::AnimationPlayback{.iterations = std::nullopt})
                      : huxerui::AnimateTo(0.0F, huxerui::SnapSpec{})));
}

[[huxerui::composable]] huxerui::View DialogCard(huxerui::View content) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    huxerui::View card = content;
    return std::move(card).With(
        huxerui::Shadow{huxerui::Color::Rgb(0, 0, 0, 0.24F), {}, 24.0F, 0.0F},
        huxerui::Background(islands.overlay),
        huxerui::CornerRadius(islands.island_radius),
        huxerui::Border(islands.outline_soft, 1.0F),
        huxerui::ClipChildren(),
        huxerui::Padding(islands.island_padding));
}

} // namespace llmswitch::ui
