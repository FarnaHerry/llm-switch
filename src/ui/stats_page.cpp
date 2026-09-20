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
import llmswitch.usage;

namespace llmswitch::ui {
namespace {

[[huxerui::composable]] huxerui::View SectionTitle(const std::string& title) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    // 分组标签走次要文本色，强调色只留给可交互状态。
    return huxerui::Text(title).Style(huxerui::TextStyle{
        huxerui::Font::System(font_size::kChip).WithWeight(huxerui::FontWeight::Bold),
        theme.colors.on_surface_variant});
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

[[huxerui::composable]] huxerui::View StatsPage(huxerui::State<std::size_t> navPage) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto dialog = huxerui::UseDialog();
    auto summary = huxerui::UseState(StatsSummary{});
    auto providerStats = huxerui::UseStateList<ProviderStat>();

    // 首组合加载 + 可见期间每 5s 自动刷新（卸载自动取消轮询）。
    huxerui::Lifecycle(
        [=] {
            if (navPage.Get() != 2) return std::function<void()>{[] {}};
            ApplySnapshot(routerInstance().snapshot(), summary, providerStats);
            const auto polling = tasks.Launch([=]() -> huxerui::Task<void> {
                while (true) {
                    co_await huxerui::Delay(std::chrono::seconds{5});
                    ApplySnapshot(routerInstance().snapshot(), summary,
                                  providerStats);
                }
            });
            return std::function<void()>{[polling] { polling.Cancel(); }};
        },
        navPage.Get() == 2);

    // 清空统计：弹窗会卸载点击路径上的节点，经事件队列推迟出指针事件
    // 路径再弹（不走帧调度）。
    // 注意 lambda 必须有 co_return：返回类型是协程 Task，没有 co_ 关键字的
    // 普通函数会「有返回值却没有 return」直接流出函数尾（UB），Launch 拿到
    // 的是未初始化的 Task —— 点一下「清空统计」就是段错误。
    auto confirmClear = [=] {
        tasks.Launch([=]() -> huxerui::Task<void> {
            dialog.Show(
                "清空统计", "确定清空全部请求统计与日志？此操作不可撤销。",
                "清空", "取消",
                [=] {
                    routerInstance().clearStats();
                    ApplySnapshot(routerInstance().snapshot(), summary,
                                  providerStats);
                    toast.Show("统计已清空");
                });
            co_return;
        });
    };

    // 会话日志导入：与代理统计是两个独立来源（对齐 cc-switch v3.13 的双数据源）。
    // sync() 要读几百个会话文件，必须走 RunWorker 离开 UI 线程；代次用于丢弃
    // 上一次延迟返回的结果。
    auto importedUsage = huxerui::UseState(usage::UsageSnapshot{});
    auto usageSyncing = huxerui::UseState(false);
    auto usageGeneration = huxerui::UseState(0);
    const bool statsVisible = navPage.Get() == 2;

    auto syncUsage = [=] {
        const int request = usageGeneration.Get() + 1;
        usageGeneration = request;
        usageSyncing = true;
        tasks.Launch([=]() -> huxerui::Task<void> {
            try {
                co_await huxerui::RunWorker([] {
                    usageStore().sync();
                    return 0;
                });
            } catch (const std::exception& e) {
                if (usageGeneration.Get() != request) co_return;
                usageSyncing = false;
                toast.Show(std::format("同步会话用量失败：{}", e.what()));
                co_return;
            }
            if (usageGeneration.Get() != request) co_return;
            importedUsage = usageStore().snapshot();
            usageSyncing = false;
        });
    };

    // 进入统计页时增量同步一次（scan-state 记住每个文件读到的偏移，没变的文件
    // 只 stat 不读，代价很小）。
    huxerui::Lifecycle(
        [=] {
            if (navPage.Get() != 2) return std::function<void()>{[] {}};
            syncUsage();
            return std::function<void()>{[] {}};
        },
        statsVisible);

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

    // 会话日志导入的按 Agent 分布（占比按归一化 token 总量算）。
    const usage::UsageSnapshot imported = importedUsage.Get();
    const std::size_t importedAgents = imported.byAgent.size();
    const float importedListHeight =
        std::min(320.0F, static_cast<float>(importedAgents) * 64.0F);
    const auto buildAgentRow = [imported](std::size_t index) {
        const auto& entry = imported.byAgent[index];
        const double share =
            imported.total.TotalTokens() > 0
                ? 100.0 *
                      static_cast<double>(entry.totals.TotalTokens()) /
                      static_cast<double>(imported.total.TotalTokens())
                : 0.0;
        return ProviderStatRow(std::string(ToolName(entry.agent)),
                               entry.totals.requests, share)
            .Key(entry.agent);
    };

    return PageScaffold(
        "使用统计",
        huxerui::Row {
            huxerui::Button("刷新").OnClick([=] {
                ApplySnapshot(routerInstance().snapshot(), summary,
                              providerStats);
                importedUsage = usageStore().snapshot();
            }),
            huxerui::Button("同步会话用量").OnClick([syncUsage] { syncUsage(); })
                .With(huxerui::Enabled(!usageSyncing.Get())),
            huxerui::Button("清空统计").OnClick([=] { confirmClear(); }),
        }.With(huxerui::Spacing(8.0F)),
        huxerui::ScrollView(
            huxerui::Column {
                PageSection(SectionTitle("本地路由请求 · 汇总"),
                            huxerui::Column {
                    s.totalRequests == 0
                        ? huxerui::View{HintText(
                              "这里只统计经过本地路由的请求。到「本地路由」页开启总开关与"
                              "逐 Agent 开关，再把 CLI 的 base URL 指向接入地址即可；"
                              "不想过路由的用量见下方「会话日志导入」。")}
                        : huxerui::View{huxerui::Row{}},
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

                SectionDivider(),

                PageSection(SectionTitle("本地路由请求 · 按供应商"),
                            huxerui::Column {
                    providerCount == 0
                        ? huxerui::View{HintText("暂无数据")}
                        : huxerui::View{
                              huxerui::VirtualList(providerCount, buildProviderRow)
                                  .EstimatedItemExtent(64.0F)
                                  .CacheExtent(192.0F)
                                  .With(huxerui::Frame{.height = providerListHeight})},
                }.With(huxerui::Spacing(8.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),

                SectionDivider(),

                // 第二个数据源：从各 agent 自己的会话日志导入，不需要开代理。
                PageSection(SectionTitle("会话日志导入 · 汇总"),
                            huxerui::Column {
                    imported.records == 0
                        ? huxerui::View{HintText(
                              usageSyncing.Get()
                                  ? "正在扫描会话日志…"
                                  : "还没有导入到用量。各 Agent 会把自己每次调用的 "
                                    "token 写进会话日志，用过对应 Agent 后点右上角"
                                    "「同步会话用量」即可（不需要开本地路由）。")}
                        : huxerui::View{huxerui::Column {
                              huxerui::Row {
                                  StatCell("请求",
                                           std::to_string(imported.total.requests)),
                                  StatCell("输入 Token",
                                           std::to_string(imported.total.inputTokens)),
                                  StatCell("输出 Token",
                                           std::to_string(imported.total.outputTokens)),
                                  StatCell("缓存读",
                                           std::to_string(imported.total.cacheReadTokens)),
                              }.With(huxerui::Spacing(8.0F)),
                              huxerui::Row {
                                  StatCell("真实消耗",
                                           std::to_string(imported.total.TotalTokens())),
                                  StatCell("今日请求",
                                           std::to_string(imported.todayRequests)),
                                  StatCell("今日 Token",
                                           std::to_string(imported.todayTokens)),
                              }.With(huxerui::Spacing(8.0F)),
                          }.With(huxerui::Spacing(10.0F),
                                 huxerui::CrossAlign(
                                     huxerui::CrossAxisAlignment::Stretch))},
                }.With(huxerui::Spacing(8.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),

                PageSection(SectionTitle("会话日志导入 · 按 Agent"),
                            huxerui::Column {
                    importedAgents == 0
                        ? huxerui::View{HintText("暂无数据")}
                        : huxerui::View{
                              huxerui::VirtualList(importedAgents, buildAgentRow)
                                  .EstimatedItemExtent(64.0F)
                                  .CacheExtent(192.0F)
                                  .With(huxerui::Frame{.height = importedListHeight})},
                }.With(huxerui::Spacing(8.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),
            }.With(huxerui::Spacing(12.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
            .With(huxerui::Grow(1.0F)));
}

} // namespace llmswitch::ui
