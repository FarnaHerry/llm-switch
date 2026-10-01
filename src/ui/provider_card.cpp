// provider_card.cpp — 供应商列表卡片与卡片操作.
#include <huxerui/huxerui.h>

#include <format>
#include <string>

#include "app_resources.h"
#include "providers_internal.h"

import llmswitch.models;
import llmswitch.store;

namespace llmswitch::ui {

using provider_detail::FetchLatency;
using provider_detail::FetchUsageText;
using provider_detail::WriteUsageCache;

// 切换提示后缀：注册表标记 needsRestart 的工具追加一句重启提示，其余为空。
std::string RestartHintSuffix(const std::string& tool) {
    const auto* spec = models::findTool(tool);
    if (spec == nullptr || !spec->needsRestart) return {};
    return std::format("，重启 {} 客户端后生效", spec->displayName);
}

namespace {

// 卡片中间状态行是三段式里的一段固定尺寸内容：Row 的非 Grow 子项按 intrinsic
// 宽度参与布局，Grow 的信息列只能吃剩下的宽度。所以状态文本必须自带上限——
// 用量失败文本带 URL 与响应体摘要，一条就能宽到 1000+pt，把信息列挤到 0 宽、
// 把右侧操作组整个顶出卡片（切换/编辑/删除全部点不到）。显示层统一「取首行 +
// 预览长度上限」，再叠 Frame.max_width 兜底：无论缓存里的状态文本多长，中间
// 这段的宽度都有硬上限，卡片布局不再受影响。被截掉的内容仍可看：悬停出 Tooltip。
constexpr std::size_t kStatusPreviewColumns = 26;
constexpr float kStatusTextMaxWidth = 150.0F;

// 状态文本的显示形态：preview 是首行截断后的预览，truncated = 预览丢了内容
// （多行或超长）。完整文本由调用方按引用传入——它就是缓存里的那条原文，
// 不必在这里再存一份。
struct StatusPreview {
    std::string preview;
    bool truncated = false;
};

// 预览预算按「显示列」计，不按字节也不按字符：按字节算中文只剩三分之一预算，
// 按字符算中文又能占到两倍宽度。26 列 ≈ 13 个汉字 ≈ 143pt（kCaption 11pt），
// 于是不论中英混排，预览恒为单行且不超 kStatusTextMaxWidth。
std::size_t PreviewCut(const std::string& line) {
    std::size_t columns = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const auto lead = static_cast<unsigned char>(line[i]);
        // 续字节：列数已在多字节序列的首字节计入，跳过。
        if ((lead & 0xC0U) == 0x80U) continue;
        const std::size_t width = lead >= 0xE0U ? 2U : 1U;  // 3/4 字节 = 全角
        if (columns + width > kStatusPreviewColumns) return i;
        columns += width;
    }
    return line.size();
}

// 取首行 + 按 UTF-8 字符边界截断；截断后追加省略号。
StatusPreview PreviewStatus(const std::string& text) {
    std::string line = text.substr(0, text.find('\n'));
    const std::size_t cut = PreviewCut(line);
    if (cut == line.size()) {
        return {std::move(line), line.size() != text.size()};
    }
    line.resize(cut);
    return {line + "…", true};
}

// 中间状态行的一条状态文本（延迟 / 用量）：宽度上限 + 被截断时的完整文本
// Tooltip。预览与完整文本等价的（普通延迟、正常用量值）不挂 Tooltip——那只会
// 多一个不提供新信息的浮层；用量失败文本是两行，完整原因（URL 与响应体摘要）
// 只在 Tooltip 里出现。
huxerui::View StatusText(const StatusPreview& preview, const std::string& full,
                         huxerui::Color color) {
    huxerui::View view =
        huxerui::Text(preview.preview)
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
                                      color})
            .With(huxerui::Frame{.max_width = kStatusTextMaxWidth});
    if (preview.truncated && !full.empty()) {
        view = std::move(view).With(huxerui::Tooltip(full));
    }
    return view;
}

} // namespace

// 官方常驻卡：有官方厂商的工具（models::officialVendorName 非空）固定在// 供应商列表第一位；切换 = store.restoreOfficial 还原厂商原生状态（与
// 普通卡同样的切换语义，无确认框）。active = 组 current 与 detectCurrent
// 均为空（即当前生效的就是厂商原生状态）。
[[huxerui::composable]] huxerui::View OfficialCard(std::string tool, bool active,
                                                   huxerui::ToastHandle toast,
                                                   huxerui::State<int> revision) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    return QuietCard(huxerui::Row {
        huxerui::Column {
            huxerui::Row {
                huxerui::Text(std::string(models::officialVendorName(tool)))
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kBody)
                            .WithWeight(huxerui::FontWeight::SemiBold),
                        theme.colors.on_surface}),
                active
                    ? huxerui::View{
                          huxerui::Text("使用中").Style(huxerui::TextStyle{
                              huxerui::Font::System(font_size::kCaption),
                              theme.colors.on_primary})}
                          .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(
                                    8.0F, 2.0F)),
                                huxerui::Background(theme.colors.primary),
                                huxerui::CornerRadius(islands.nested_radius))
                    : huxerui::View{huxerui::Row{}},
            }.With(huxerui::Spacing(6.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
            huxerui::Text("厂商原生状态（撤掉第三方配置覆盖，回到官方登录）")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.on_surface_variant}),
        }.With(huxerui::Spacing(6.0F),
               huxerui::Grow(1.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start)),
        // 操作组右对齐：图标 + Tooltip（active 时禁用）。
        huxerui::Row {
            huxerui::IconButton(app::images::swap, "切换")
                .OnClick([toast, tool, revision] {
                    try {
                        providerStore().restoreOfficial(tool);
                        toast.Show("已切换到官方原生状态" +
                                   RestartHintSuffix(tool));
                    } catch (const std::exception& e) {
                        toast.Show(e.what());
                    }
                    revision = revision.Get() + 1;
                })
                .With(huxerui::Enabled(!active),
                      huxerui::Tooltip(active ? "当前使用"
                                              : "切换到官方原生状态")),
        }.With(huxerui::Spacing(4.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
    }.With(huxerui::Spacing(8.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)))
        .Key("official");
}

[[huxerui::composable]] huxerui::View ProviderCard(
    std::string tool, const models::Provider& provider, bool active, bool isCurrent,
    huxerui::TaskScope tasks, huxerui::ToastHandle toast,
    huxerui::State<int> revision, UsageCache usageCache,
    huxerui::State<std::size_t> formToolIndex,
    huxerui::State<std::string> formTarget) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    auto dialog = huxerui::UseDialog();
    const auto http = huxerui::UseService<huxerui::HttpClient>();
    const std::string id = provider.id;
    const std::string name = provider.name;
    const std::string accessUrl = models::effectiveBaseUrl(provider);

    // 连通检测状态（卡片级）：latency 空 = 未检测；检测中禁用按钮。
    // State 经 .Key(id) 随卡片保活，revision 重读不丢。
    auto checking = huxerui::UseState(false);
    auto latency = huxerui::UseState<std::string>({});
    // 用量查询进行中：为真时刷新图标自转（同样随 .Key(id) 保活）。
    auto usageRefreshing = huxerui::UseState(false);

    // 用量展示：启用开关打开且 usageUrl 非空才显示；缓存未命中显示占位。
    std::string usageText;
    bool usageError = false;
    const bool usageConfigured = provider.usageEnabled && !provider.usageUrl.empty();
    if (usageConfigured) {
        const auto& cache = usageCache.Get();
        if (const auto it = cache.find(id); it != cache.end()) {
            usageText = it->second;
            usageError = it->second.starts_with("查询失败");
        } else {
            usageText = "用量待查询";
        }
    }
    const StatusPreview usagePreview = PreviewStatus(usageText);
    const std::string latencyText =
        checking.Get() ? "连通检测中…" : latency.Get();
    const StatusPreview latencyPreview = PreviewStatus(latencyText);

    auto bump = [revision] { revision = revision.Get() + 1; };

    // 纯导航直接写 State；表单页由 AgentPage 换页挂载，卸载由后续帧完成。
    // 两个 State 一起写：formToolIndex 说明表单属于哪个 Agent 组（供应商 id
    // 只在组内唯一，所以目标组必须跟着入口一起确定）。
    auto showEdit = [formToolIndex, formTarget, tool, id] {
        formToolIndex = ToolRegistryIndex(tool);
        formTarget = id;
    };
    auto showUsage = [formToolIndex, formTarget, tool, id] {
        formToolIndex = ToolRegistryIndex(tool);
        formTarget = "usage:" + id;
    };

    // 删除：内置确认框（主题化 DialogStyle 见 app.cpp MinimalThemed）。
    auto showDeleteConfirm = [dialog, toast, tool, id, name, bump] {
        dialog.Show(
            "删除供应商",
            std::format("确定删除「{}」？此操作不可撤销。", name),
            "删除", "取消",
            [toast, tool, id, name, bump] {
                try {
                    providerStore().removeProvider(tool, id);
                    toast.Show(std::format("已删除 {}", name));
                } catch (const std::exception& e) {
                    toast.Show(e.what());
                }
                bump();
            },
            {});
    };

    // 用量刷新按钮的悬停状态层。IconButton 只接受 ImageVariant、没有暴露图标级
    // 修饰符，直接在按钮节点上加 Rotation 会把 32×32 的悬停状态层一起转起来，
    // 所以下面自己拼一个等价按钮：外层持有状态层/语义/光标/提示，内层是共享的
    // SpinningRefreshIcon（只转图标自己）。几何沿用 IconButton 的默认值
    // （state layer 32×32、圆角 shapes.extra_small），与同一排其它操作图标一致。
    huxerui::Indication refreshIndication = theme.interactions.indication;
    refreshIndication.geometry.layer_size = huxerui::Size{32.0F, 32.0F};
    refreshIndication.geometry.clip_corner_radii =
        huxerui::CornerRadii{theme.shapes.extra_small};

    // 三段式：左信息列（Grow 吃满剩余宽度）｜ 中间状态行（延迟 + 用量
    // 横向排列，垂直居中落在内容与操作组之间）｜ 右侧操作图标组（自绘
    // 图标 + Tooltip）。
    return QuietCard(huxerui::Row {
        huxerui::Column {
            huxerui::Row {
                huxerui::Text(name).Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kBody)
                        .WithWeight(huxerui::FontWeight::SemiBold),
                    theme.colors.on_surface}),
                active
                    ? huxerui::View{
                          huxerui::Text("使用中").Style(huxerui::TextStyle{
                              huxerui::Font::System(font_size::kCaption),
                              theme.colors.on_primary})}
                          .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(
                                    8.0F, 2.0F)),
                                huxerui::Background(theme.colors.primary),
                                huxerui::CornerRadius(islands.nested_radius))
                    : huxerui::View{huxerui::Row{}},
            }.With(huxerui::Spacing(6.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
            accessUrl.empty()
                ? huxerui::View{huxerui::Row{}}
                : huxerui::View{huxerui::Text(accessUrl)
                                    .Style(huxerui::TextStyle{
                                        huxerui::Font::Monospace(font_size::kChip),
                                        theme.colors.on_surface_variant})},
            provider.notes.empty()
                ? huxerui::View{huxerui::Row{}}
                : huxerui::View{huxerui::Text(provider.notes)
                                    .Style(huxerui::TextStyle{
                                        huxerui::Font::System(font_size::kCaption),
                                        theme.colors.on_surface_variant})},
        }.With(huxerui::Spacing(6.0F),
               huxerui::Grow(1.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start)),
        // 中间状态行：延迟与用量横向排列（延迟在前、用量在后，后续新增的
        // 状态项也在此行追加）。延迟：连通检测结果（未检测不显示；失败
        // error 色）；用量：启用开关打开且 usageUrl 非空才显示，带手动刷新
        // 图标。两者皆无时整行塌缩为零宽。文本一律经 StatusText 限宽（首行 +
        // 预览上限 + Frame.max_width），这条固定段因此永远吃不下信息列与
        // 右侧操作组；被截掉的完整文本进 Tooltip。
        huxerui::Row {
            (!checking.Get() && latency.Get().empty())
                ? huxerui::View{huxerui::Row{}}
                : StatusText(latencyPreview, latencyText,
                             latency.Get().starts_with("不可达")
                                 ? theme.colors.error
                                 : theme.colors.on_surface_variant),
            !usageConfigured
                ? huxerui::View{huxerui::Row{}}
                : StatusText(usagePreview, usageText,
                             usageError ? theme.colors.error
                                        : theme.colors.on_surface_variant),
            !usageConfigured
                ? huxerui::View{huxerui::Row{}}
                : huxerui::View{
                      huxerui::Stack {
                          SpinningRefreshIcon(20.0F, theme.colors.on_surface,
                                              usageRefreshing.Get()),
                      }
                          .OnClick([tasks, usageCache, provider, http,
                                    usageRefreshing] {
                              // 已在查询中就不再叠加请求，避免图标重启一轮。
                              if (usageRefreshing.Get()) return;
                              usageRefreshing = true;
                              // HuxerUI HTTP 异步请求完成后回 UI 线程写缓存 State。
                              tasks.Launch([usageCache, provider, http,
                                            usageRefreshing]() -> huxerui::Task<void> {
                                  WriteUsageCache(usageCache, provider.id,
                                                  co_await FetchUsageText(http, provider));
                                  usageRefreshing = false;
                              });
                          })
                          .With(huxerui::Frame{.width = 40.0F, .height = 40.0F},
                                huxerui::Align(
                                    huxerui::HorizontalAlignment::Center,
                                    huxerui::VerticalAlignment::Center),
                                huxerui::Focusable{true},
                                refreshIndication,
                                huxerui::Semantics{
                                    .role = huxerui::SemanticRole::Button,
                                    .label = "刷新"},
                                huxerui::Tooltip("重新查询用量"),
                                huxerui::PointerCursor(
                                    huxerui::PointerCursorKind::Hand))},
        }.With(huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Row {
            huxerui::IconButton(app::images::swap, "切换")
                .OnClick([toast, tool, id, name, bump] {
                    try {
                        providerStore().switchTo(tool, id);
                        toast.Show(std::format("已切换到 {}", name) +
                                   RestartHintSuffix(tool));
                    } catch (const std::exception& e) {
                        toast.Show(e.what());
                    }
                    bump();
                })
                .With(huxerui::Enabled(!isCurrent),
                      huxerui::Tooltip(isCurrent ? "当前使用" : "切换到此供应商")),
            // 联通检测：HuxerUI HttpClient 等待响应头，最长 10s，完成后回 UI
            // 线程写卡片 State。URL 空（codex 可留空）时禁用。
            huxerui::IconButton(app::images::activity, "联通检测")
                .OnClick([tasks, checking, latency, http,
                          url = accessUrl] {
                    checking = true;
                    latency = std::string{};
                    tasks.Launch([checking, latency,
                                  http, url]() -> huxerui::Task<void> {
                        try {
                            const double ms = co_await FetchLatency(http, url);
                            checking = false;
                            latency = std::format("延迟 {:.0f} ms", ms);
                        } catch (const std::exception& e) {
                            checking = false;
                            latency = std::format("不可达：{}", e.what());
                        }
                    });
                })
                .With(huxerui::Enabled(!checking.Get() &&
                                       !accessUrl.empty()),
                      huxerui::Tooltip(accessUrl.empty()
                                           ? "该供应商未设置 URL"
                                           : "检测实际访问 URL 的连通性与延迟")),
            huxerui::IconButton(app::images::edit, "编辑")
                .OnClick([showEdit] { showEdit(); })
                .With(huxerui::Tooltip("编辑")),
            huxerui::IconButton(app::images::gauge, "用量查询配置")
                .OnClick([showUsage] { showUsage(); })
                .With(huxerui::Tooltip("用量查询配置")),
            huxerui::IconButton(app::images::copy, "复制")
                .OnClick([toast, tool, id, bump] {
                    try {
                        const auto copy = providerStore().duplicateProvider(tool, id);
                        toast.Show(std::format("已复制为 {}", copy.name));
                    } catch (const std::exception& e) {
                        toast.Show(e.what());
                    }
                    bump();
                })
                .With(huxerui::Tooltip("复制")),
            huxerui::IconButton(app::images::trash, "删除")
                .OnClick([tasks, showDeleteConfirm] {
                    // 弹窗会卸载点击路径上的节点：推迟出指针事件路径。
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        showDeleteConfirm();  // 不经 Delay(0)（帧调度）
                        co_return;
                    });
                })
                .With(huxerui::Tooltip("删除")),
        }.With(huxerui::Spacing(4.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
    }.With(huxerui::Spacing(8.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)))
        .Key(id);
}

} // namespace llmswitch::ui
