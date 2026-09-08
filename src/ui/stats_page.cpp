// stats_page.cpp — 使用统计页（PageScaffold「使用统计」）。
//
//   汇总卡：今日请求 / 总请求 / 成功率 / 平均延迟 / 输入 Token / 输出 Token
//     六个大数字格子（routerInstance().snapshot()）。
//   按供应商卡：perProvider 列表（名称 + 请求数 + 占比条形 + 百分比）。
//   操作（页头 actions）：「刷新」手动重取快照；「清空统计」内置确认对话框
//     → clearStats()（清内存统计 + 删 JSONL 日志文件）。
//   页面可见期间每 5s 自动刷新（Lifecycle + TaskScope 轮询，卸载自动取消；
//   State 只在 UI 线程写）。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

#include "ui.h"

import llmswitch.router;

namespace llmswitch::ui {
namespace {

[[huxerui::composable]] huxerui::View SectionTitle(const std::string& title) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Text(title).Style(huxerui::TextStyle{
        huxerui::Font::System(font_size::kChip).WithWeight(huxerui::FontWeight::Bold),
        theme.colors.primary});
}

[[huxerui::composable]] huxerui::View HintText(const std::string& text) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Text(text).Style(huxerui::TextStyle{
        huxerui::Font::System(font_size::kCaption),
        theme.colors.on_surface_variant});
}

// 大数字格子：overlay 表面 + 8pt 圆角，横向弹性均分。
[[huxerui::composable]] huxerui::View StatCell(std::string label,
                                               std::string value) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    return huxerui::Column {
        huxerui::Text(value).Style(huxerui::TextStyle{
            huxerui::Font::System(22.0F).WithWeight(huxerui::FontWeight::Bold),
            theme.colors.on_surface}),
        huxerui::Text(label).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kCaption),
            theme.colors.on_surface_variant}),
    }.With(huxerui::Spacing(2.0F),
           huxerui::Padding(huxerui::EdgeInsets::Symmetric(12.0F, 10.0F)),
           huxerui::Background(islands.overlay),
           huxerui::CornerRadius(islands.nested_radius),
           huxerui::Grow(1.0F));
}

// 供应商行：名称 + 计数/百分比 + 占比条形（权重截到 [1, 99] 保证两端 Grow
// 恒为正）。
[[huxerui::composable]] huxerui::View ProviderStatRow(std::string name,
                                                      std::int64_t count,
                                                      double pct) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const float fill = static_cast<float>(std::clamp(pct, 1.0, 99.0));
    return huxerui::Column {
        huxerui::Row {
            huxerui::Text(name).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kChip), theme.colors.on_surface}),
            huxerui::Spacer(),
            huxerui::Text(std::format("{} 次 · {:.1f}%", count, pct))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.on_surface_variant}),
        },
        huxerui::Row {
            huxerui::Spacer().With(huxerui::Grow(fill),
                                   huxerui::Background(theme.colors.primary),
                                   huxerui::Frame{.height = 6.0F},
                                   huxerui::CornerRadius(3.0F)),
            huxerui::Spacer().With(huxerui::Grow(100.0F - fill)),
        },
    }.With(huxerui::Spacing(4.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

struct ProviderStat {
    std::string name;
    std::int64_t count = 0;
    double percentage = 0.0;

    bool operator==(const ProviderStat&) const = default;
};

struct StatsSummary {
    std::int64_t todayRequests = 0;
    std::int64_t totalRequests = 0;
    std::int64_t successCount = 0;
    double avgLatencyMs = 0.0;
    std::int64_t totalPromptTokens = 0;
    std::int64_t totalCompletionTokens = 0;
};

void ApplySnapshot(const router::StatsSnapshot& snapshot,
                   const huxerui::State<StatsSummary>& summary,
                   const huxerui::StateList<ProviderStat>& providers) {
    summary = StatsSummary{
        snapshot.todayRequests,
        snapshot.totalRequests,
        snapshot.successCount,
        snapshot.avgLatencyMs,
        snapshot.totalPromptTokens,
        snapshot.totalCompletionTokens,
    };

    const std::size_t shared =
        std::min(providers.Size(), snapshot.perProvider.size());
    for (std::size_t i = 0; i < shared; ++i) {
        const auto& [name, count] = snapshot.perProvider[i];
        const double percentage =
            snapshot.totalRequests > 0
                ? 100.0 * static_cast<double>(count) /
                      static_cast<double>(snapshot.totalRequests)
                : 0.0;
        providers.Set(i, ProviderStat{name, count, percentage});
    }
    while (providers.Size() > snapshot.perProvider.size()) {
        providers.PopBack();
    }
    for (std::size_t i = shared; i < snapshot.perProvider.size(); ++i) {
        const auto& [name, count] = snapshot.perProvider[i];
        const double percentage =
            snapshot.totalRequests > 0
                ? 100.0 * static_cast<double>(count) /
                      static_cast<double>(snapshot.totalRequests)
                : 0.0;
        providers.PushBack(ProviderStat{name, count, percentage});
    }
}

} // namespace

[[huxerui::composable]] huxerui::View StatsPage() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto dialog = huxerui::UseDialog();
    auto summary = huxerui::UseState(StatsSummary{});
    auto providerStats = huxerui::UseStateList<ProviderStat>();

    // 首组合加载 + 可见期间每 5s 自动刷新（卸载自动取消轮询）。
    huxerui::Lifecycle(
        [=] {
            ApplySnapshot(routerInstance().snapshot(), summary, providerStats);
            tasks.Launch([=]() -> huxerui::Task<void> {
                while (true) {
                    co_await huxerui::Delay(std::chrono::seconds{5});
                    ApplySnapshot(routerInstance().snapshot(), summary,
                                  providerStats);
                }
            });
            return [] {};
        },
        0);

    // 清空统计：弹窗会卸载点击路径上的节点，推迟出指针事件路径再弹。
    auto confirmClear = [=] {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            dialog.Show(
                "清空统计", "确定清空全部请求统计与日志？此操作不可撤销。",
                "清空", "取消",
                [=] {
                    routerInstance().clearStats();
                    ApplySnapshot(routerInstance().snapshot(), summary,
                                  providerStats);
                    toast.Show("统计已清空");
                });
        });
    };

    const StatsSummary s = summary.Get();
    const double successRate =
        s.totalRequests > 0
            ? 100.0 * static_cast<double>(s.successCount) /
                  static_cast<double>(s.totalRequests)
            : 0.0;

    const std::size_t providerCount = providerStats.Size();
    const float providerListHeight =
        std::min(320.0F, static_cast<float>(providerCount) * 64.0F);
    const auto buildProviderRow = [providerStats](std::size_t index) {
        const auto& provider = providerStats.At(index);
        return ProviderStatRow(provider.name, provider.count,
                               provider.percentage)
            .Key(provider.name);
    };

    return PageScaffold(
        "使用统计",
        huxerui::Row {
            huxerui::Button("刷新").OnClick([=] {
                ApplySnapshot(routerInstance().snapshot(), summary,
                              providerStats);
            }),
            huxerui::Button("清空统计").OnClick([=] { confirmClear(); }),
        }.With(huxerui::Spacing(8.0F)),
        huxerui::ScrollView(
            huxerui::Column {
                Card(huxerui::Column {
                    SectionTitle("汇总"),
                    huxerui::Row {
                        StatCell("今日请求", std::to_string(s.todayRequests)),
                        StatCell("总请求", std::to_string(s.totalRequests)),
                        StatCell("成功率",
                                 s.totalRequests > 0
                                     ? std::format("{:.1f}%", successRate)
                                     : "—"),
                    }.With(huxerui::Spacing(8.0F)),
                    huxerui::Row {
                        StatCell("平均延迟",
                                 std::format("{:.0f} ms", s.avgLatencyMs)),
                        StatCell("输入 Token",
                                 std::to_string(s.totalPromptTokens)),
                        StatCell("输出 Token",
                                 std::to_string(s.totalCompletionTokens)),
                    }.With(huxerui::Spacing(8.0F)),
                }.With(huxerui::Spacing(10.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),

                Card(huxerui::Column {
                    SectionTitle("按供应商"),
                    providerCount == 0
                        ? huxerui::View{HintText("暂无数据")}
                        : huxerui::View{
                              huxerui::VirtualList(providerCount, buildProviderRow)
                                  .EstimatedItemExtent(64.0F)
                                  .CacheExtent(192.0F)
                                  .With(huxerui::Frame{.height = providerListHeight})},
                }.With(huxerui::Spacing(8.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),
            }.With(huxerui::Spacing(12.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
            .With(huxerui::Grow(1.0F)));
}

} // namespace llmswitch::ui
