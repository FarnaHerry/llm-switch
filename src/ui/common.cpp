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

huxerui::ImageResource ToolIcon(std::string_view iconName, bool dark) {
    // Agent 身份图标使用透明标记（64px + @2x/@3x），不走 SVG tint。单色 mark
    // 提供白色 _dark 变体：浅色主题用黑色 base，深色主题用白色 _dark；彩色
    // 图标（Claude/DeepSeek/Oh My Pi）共用同一份。未知 iconName 回退错误图标。
    if (iconName == "claudecode" || iconName == "claude") {
        return app::images::agent_claude;
    }
    if (iconName == "codex") {
        return dark ? app::images::agent_codex_dark
                    : app::images::agent_codex;
    }
    if (iconName == "opencode") {
        return dark ? app::images::agent_opencode_dark
                    : app::images::agent_opencode;
    }
    if (iconName == "pi") {
        return dark ? app::images::agent_pi_dark : app::images::agent_pi;
    }
    if (iconName == "omp") {
        return app::images::agent_omp;
    }
    if (iconName == "deepseek") {
        return app::images::agent_dsh;
    }
    if (iconName == "hermes") {
        return dark ? app::images::agent_hermes_dark
                    : app::images::agent_hermes;
    }
    if (iconName == "zcode") {
        return dark ? app::images::agent_zcode_dark
                    : app::images::agent_zcode;
    }
    if (iconName == "kimi") {
        return dark ? app::images::agent_kimi_dark : app::images::agent_kimi;
    }
    return app::images::error;
}

huxerui::ImageResource PresetIcon(std::string_view providerName) {
    const std::string_view iconName =
        models::builtinPresetIconName(providerName);
    if (iconName == "packycode") return app::images::packycode;
    if (iconName == "aicodemirror") return app::images::aicodemirror;
    if (iconName == "code88") return app::images::code88;
    if (iconName == "duckcoding") return app::images::duckcoding;
    if (iconName == "kimi") return app::images::kimi;
    if (iconName == "volcengine") return app::images::volcengine;
    if (iconName == "baidu") return app::images::baidu;
    if (iconName == "qwen") return app::images::qwen;
    if (iconName == "tencent") return app::images::tencent;
    if (iconName == "xiaomi") return app::images::xiaomi;
    if (iconName == "deepseek") return app::images::deepseek;
    if (iconName == "glm") return app::images::glm;
    if (iconName == "zdotai") return app::images::zdotai;
    if (iconName == "minimax") return app::images::minimax;
    if (iconName == "stepfun") return app::images::stepfun;
    if (iconName == "doubao") return app::images::doubao;
    if (iconName == "siliconflow") return app::images::siliconflow;
    if (iconName == "meituan") return app::images::meituan;
    if (iconName == "antgroup") return app::images::antgroup;
    if (iconName == "modelscope") return app::images::modelscope;
    if (iconName == "ppio") return app::images::ppio;
    if (iconName == "openrouter") return app::images::openrouter;
    if (iconName == "aihubmix") return app::images::aihubmix;
    if (iconName == "shengsuanyun") return app::images::shengsuanyun;
    if (iconName == "qiniu") return app::images::qiniu;
    if (iconName == "aicoding") return app::images::aicoding;
    if (iconName == "subrouter") return app::images::subrouter;
    if (iconName == "cherryin") return app::images::cherryin;
    // Keep an unknown mapping visibly distinct so omissions cannot pass as a vendor icon.
    return app::images::error;
}

std::string_view ToolName(std::string_view tool) {
    // 展示名以注册表为准；未注册（不应发生）原样回显 id。
    if (const auto* spec = models::findTool(tool)) return spec->displayName;
    return tool;
}

std::size_t ToolRegistryIndex(std::string_view tool) {
    const auto registry = models::toolRegistry();
    for (std::size_t i = 0; i < registry.size(); ++i) {
        if (registry[i].id == tool) return i;
    }
    return 0;
}

IslandTheme ResolveIslandTheme(const huxerui::ThemeSpec& theme) {
    // 纯色面板：海面、页面岛、强调卡片、浮层全部同为纯色表面，区域区分
    // 靠描边线条（outline_soft），不再靠明度差。
    return IslandTheme{
        .island_padding = theme.spacing.medium,
        .island_radius = 10.0F,
        .nested_radius = 6.0F,
        .raised = theme.colors.surface,
        .base = theme.colors.surface,
        .overlay = theme.colors.surface,
        .outline_soft = theme.colors.outline,
        .success = theme.colors.on_surface,
        .on_success = theme.colors.surface,
        .warning = theme.colors.on_surface,
    };
}

// SDK 浮层样式没有边框字段，用无偏移、无模糊的 1pt 外扩表达普通描边。
huxerui::Shadow FloatingOutline(const huxerui::ThemeSpec& theme) {
    return huxerui::Shadow{theme.colors.outline, {}, 0.0F, 1.0F};
}

namespace {

// 观察原生控件的完整交互态，不替换点击/悬停回调，也不自行识别按压手势。
struct InteractionSink {
    class Extension;
    huxerui::State<huxerui::InteractionState> target;
};

class InteractionSink::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode&, const InteractionSink& spec) : target_(spec.target) {}

    void Update(huxerui::ViewNode&, const InteractionSink& spec) {
        target_ = spec.target;
    }

    void OnInteraction(huxerui::ViewNode&, const huxerui::InteractionState& state,
                       const std::optional<huxerui::InteractionEvent>&) override {
        target_ = state;
    }

private:
    huxerui::State<huxerui::InteractionState> target_;
};

[[huxerui::composable]] huxerui::View ButtonAction(huxerui::View action, bool primary) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto interaction = huxerui::UseState(huxerui::InteractionState{});
    const auto current = interaction.Get();
    const bool active = current.enabled &&
        (current.hovered || current.pressed || current.focus_visible);
    // 交互态统一趋向主题强调色，已填充的主按钮保持底色，不再次反转。
    const bool filled = primary || active;
    huxerui::ButtonStyle style = huxerui::UseEnvironment<huxerui::ButtonStyle>();
    style.background = filled ? huxerui::VisualFill{theme.colors.primary}
                              : huxerui::VisualFill{huxerui::Color::Transparent()};
    style.label_style.foreground = filled ? theme.colors.on_primary : theme.colors.on_surface;
    // 实心按钮的描边与文字同色，hover/按压/焦点在保持底色时仍有反馈。
    style.indication = MonochromeIndication(style.label_style.foreground);
    huxerui::ThemeDefinition definition;
    definition.Set(style);
    huxerui::View button = action;
    button = std::move(button).With(
        huxerui::CornerRadius(style.corner_radii),
        huxerui::Border(theme.colors.outline, 1.0F),
        InteractionSink{interaction});
    return huxerui::Theme(std::move(definition), std::move(button));
}

} // namespace

[[huxerui::composable]] huxerui::View OutlinedAction(huxerui::View action) {
    return ButtonAction(action, false);
}

[[huxerui::composable]] huxerui::View PrimaryAction(huxerui::View action) {
    return ButtonAction(action, true);
}

[[huxerui::composable]] huxerui::View InvertingIconAction(huxerui::View action) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto interaction = huxerui::UseState(huxerui::InteractionState{});
    const auto current = interaction.Get();
    const bool active = current.enabled &&
        (current.hovered || current.pressed || current.focus_visible);
    huxerui::IconButtonStyle style = huxerui::UseEnvironment<huxerui::IconButtonStyle>();
    if (active) style.foreground = theme.colors.on_primary;
    style.indication = MonochromeIndication(style.foreground);
    huxerui::ThemeDefinition definition;
    definition.Set(style);
    huxerui::View button = action;
    if (active) {
        button = std::move(button).With(huxerui::Background(theme.colors.primary));
    }
    button = std::move(button).With(InteractionSink{interaction});
    return huxerui::Theme(std::move(definition), std::move(button));
}

[[huxerui::composable]] huxerui::View PageScaffoldWithHeader(huxerui::View header,
                                                             huxerui::View content) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    // 响应式：Compact(<600) 收窄页面内边距。
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    // 顶级页面与标题栏共用纯色窗口表面，靠分区、间距与描边组织信息。
    // 页面只负责内边距与滚动；左右边距同时是壳层标题栏的左右边距
    // （app.cpp 的 shellInset），应用名与页面标题因此共享同一条左边线。
    // 顶部不留内边距（top = 0）且壳层也不留 Spacing：页面标题紧接标题栏下沿，
    // 两者之间没有额外高度；标题栏自身的高度由平台解析，页面排在它之后贴合。
    const float inset = compact ? theme.spacing.medium : theme.spacing.large;
    // 形参在 hcg 生成的组合体里按值捕获（const），链式修饰符是右值限定，
    // 所以先落成局部变量再 move。
    huxerui::View headerView = header;
    huxerui::View body = content;
    return huxerui::Column {
        std::move(headerView)
            .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
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

[[huxerui::composable]] huxerui::View PageScaffold(const std::string& title,
                                                   huxerui::View actions,
                                                   huxerui::View content) {
    return PageScaffoldWithHeader(
        huxerui::Row {
            huxerui::Text(title, huxerui::TextRole::Title),
            huxerui::Spacer(),
            actions,
        }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        std::move(content));
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

// 列表条目卡：纯色表面 + 1pt 描边线条区分区域（纯色下表面色差消失，
// 线条是唯一的分区手段）。用于供应商/技能/MCP/会话等重复条目；独立强调
// 分区仍用 Card。
[[huxerui::composable]] huxerui::View QuietCard(huxerui::View content) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    huxerui::View card = content;
    return std::move(card).With(
        huxerui::Background(islands.base),
        huxerui::CornerRadius(islands.nested_radius),
        huxerui::Border(islands.outline_soft, 1.0F),
        huxerui::Padding(islands.island_padding));
}

// 平铺分区：标题 + 内容直接落在一级岛表面，不包 raised 卡——一页连排多张
// 盒子正是「卡片滥用」的来源；分区之间由 SectionDivider 的发丝线 + 页面
// 缝隙划分。仅需要强调的独立块（如关于页头部）保留 Card。
[[huxerui::composable]] huxerui::View PageSection(huxerui::View title,
                                                  huxerui::View content) {
    return huxerui::Column {
        title,
        content,
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
        FloatingOutline(theme),
        huxerui::Background(islands.overlay),
        huxerui::CornerRadius(islands.island_radius),
        huxerui::Border(islands.outline_soft, 1.0F),
        huxerui::ClipChildren(),
        huxerui::Padding(islands.island_padding));
}

} // namespace llmswitch::ui
