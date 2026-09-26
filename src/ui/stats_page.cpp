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
#include <huxerui/sqlite.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "ui.h"

import llmswitch.config;
import llmswitch.router;
import llmswitch.usage;

namespace llmswitch::ui {
namespace {

// ---- 用量账本：SQLite（HuxerUI/Lib-SQLite）----------------------------------
//
// 会话日志导入的记录落在 cfg::usageDir()/usage.db，表结构见下面的 kUsageRecords
// / kUsageScanState。老的 usage.jsonl + scan-state.json 已废弃、不再读取（不兼容
// 旧数据，首次运行从空库开始）。
//
// 为什么账本在 UI 层而不是 llmswitch.usage 里：Lib-SQLite 的公开 API 只有异步
// （Database::*Async 返回 Task，同步入口只存在于 Transaction 回调内），而
// llmswitch.usage 是无 huxerui 依赖的纯模块（只 import std）。让域模块持有数据库
// 就得把 huxerui 头文件塞进它的全局模块片段，跨 GCC/Clang/MSVC 不稳。所以域模块
// 只负责解析/扫描/口径，异步编排与持久化留在这里。
const huxerui::sqlite::Table<usage::UsageRecord> kUsageRecords{
    "usage_records",
    huxerui::sqlite::Column<&usage::UsageRecord::key>{"key",
                                                      huxerui::sqlite::PrimaryKey{}},
    huxerui::sqlite::Column<&usage::UsageRecord::agent>{"agent"},
    huxerui::sqlite::Column<&usage::UsageRecord::model>{"model"},
    huxerui::sqlite::Column<&usage::UsageRecord::tsMillis>{"ts_millis"},
    huxerui::sqlite::Column<&usage::UsageRecord::inputTokens>{"input_tokens"},
    huxerui::sqlite::Column<&usage::UsageRecord::outputTokens>{"output_tokens"},
    huxerui::sqlite::Column<&usage::UsageRecord::cacheReadTokens>{"cache_read_tokens"},
    huxerui::sqlite::Column<&usage::UsageRecord::cacheWriteTokens>{"cache_write_tokens"},
    huxerui::sqlite::Index<&usage::UsageRecord::agent, &usage::UsageRecord::tsMillis>{
        "idx_usage_records_agent_ts"},
};

// 扫描位点：每个源文件已消费到的字节偏移（取代原来的 scan-state.json）。
// 列名用 byte_offset/byte_size，避开 SQL 的 OFFSET 关键字。
struct ScanStateRow {
    std::string file;
    std::int64_t byteOffset = 0;
    std::int64_t byteSize = 0;
};
const huxerui::sqlite::Table<ScanStateRow> kUsageScanState{
    "scan_state",
    huxerui::sqlite::Column<&ScanStateRow::file>{"file", huxerui::sqlite::PrimaryKey{}},
    huxerui::sqlite::Column<&ScanStateRow::byteOffset>{"byte_offset"},
    huxerui::sqlite::Column<&ScanStateRow::byteSize>{"byte_size"},
};

const huxerui::sqlite::Schema kUsageSchema{1, kUsageRecords, kUsageScanState};
const huxerui::sqlite::Migrations kUsageMigrations{};  // v1：新库直接建到当前版本

huxerui::Task<huxerui::sqlite::Result<huxerui::sqlite::Database>> OpenUsageDatabase() {
    const std::filesystem::path file = cfg::usageDir() / "usage.db";
    huxerui::sqlite::OpenOptions options{
        .journal_mode = huxerui::sqlite::JournalMode::Wal,
        .busy_timeout = std::chrono::seconds{5},
        .create_parent_directories = true,
    };
    co_return co_await huxerui::sqlite::Database::OpenAsync(
        huxerui::File(file.string()), kUsageSchema, kUsageMigrations, options);
}

// 一条记录一行；命中主键时**逐字段取 max**——流式追加会把同一次调用写多行，
// 且实测「最后一次覆盖」会少算（见 usage.cppm 的说明），取 max 永不漏。
constexpr std::string_view kUpsertRecord =
    "INSERT INTO usage_records(key,agent,model,ts_millis,input_tokens,output_tokens,"
    "cache_read_tokens,cache_write_tokens) VALUES(?,?,?,?,?,?,?,?) "
    "ON CONFLICT(key) DO UPDATE SET "
    "agent=excluded.agent, model=excluded.model, "
    "ts_millis=max(ts_millis,excluded.ts_millis), "
    "input_tokens=max(input_tokens,excluded.input_tokens), "
    "output_tokens=max(output_tokens,excluded.output_tokens), "
    "cache_read_tokens=max(cache_read_tokens,excluded.cache_read_tokens), "
    "cache_write_tokens=max(cache_write_tokens,excluded.cache_write_tokens)";

constexpr std::string_view kUpsertScanState =
    "INSERT INTO scan_state(file,byte_offset,byte_size) VALUES(?,?,?) "
    "ON CONFLICT(file) DO UPDATE SET byte_offset=excluded.byte_offset, "
    "byte_size=excluded.byte_size";

// 汇总查询：总量 + 今日。今日阈值由 usage::TodayStartMillis() 传入，口径与重建前
// 的 C++ 聚合一致（只算 tsMillis > 0 且 >= 当天 0 点的记录）。
constexpr std::string_view kSnapshotTotal =
    "SELECT COUNT(*),"
    " COALESCE(SUM(input_tokens),0), COALESCE(SUM(output_tokens),0),"
    " COALESCE(SUM(cache_read_tokens),0), COALESCE(SUM(cache_write_tokens),0),"
    " COALESCE(SUM(CASE WHEN ts_millis>0 AND ts_millis>=? THEN 1 ELSE 0 END),0),"
    " COALESCE(SUM(CASE WHEN ts_millis>0 AND ts_millis>=? THEN"
    "   input_tokens+output_tokens+cache_read_tokens+cache_write_tokens"
    "   ELSE 0 END),0) "
    "FROM usage_records";

constexpr std::string_view kSnapshotByAgent =
    "SELECT agent, COUNT(*),"
    " COALESCE(SUM(input_tokens),0), COALESCE(SUM(output_tokens),0),"
    " COALESCE(SUM(cache_read_tokens),0), COALESCE(SUM(cache_write_tokens),0) "
    "FROM usage_records GROUP BY agent";

huxerui::Task<huxerui::sqlite::Result<usage::UsageSnapshot>> QueryUsageSnapshot(
    huxerui::sqlite::Database database) {
    using huxerui::sqlite::Result;
    using huxerui::sqlite::RowView;

    const std::int64_t todayStart = usage::TodayStartMillis();
    auto totals = co_await database.QueryAsync<std::int64_t>(
        std::string(kSnapshotTotal),
        [](const RowView& row) -> Result<std::int64_t> {
            return row.Get<std::int64_t>(0);
        },
        todayStart, todayStart);
    if (!totals) co_return totals.Error();

    usage::UsageSnapshot snapshot;
    if (!totals->empty()) {
        const auto& row = *totals;
        snapshot.records = row[0];
        snapshot.total.requests = row[0];
        snapshot.total.inputTokens = row[1];
        snapshot.total.outputTokens = row[2];
        snapshot.total.cacheReadTokens = row[3];
        snapshot.total.cacheWriteTokens = row[4];
        snapshot.todayRequests = row[5];
        snapshot.todayTokens = row[6];
    }

    struct AgentRow {
        std::string agent;
        std::int64_t requests = 0;
        std::int64_t input = 0;
        std::int64_t output = 0;
        std::int64_t cacheRead = 0;
        std::int64_t cacheWrite = 0;
    };
    auto agents = co_await database.QueryAsync<AgentRow>(
        std::string(kSnapshotByAgent),
        [](const RowView& row) -> Result<AgentRow> {
            auto agent = row.Get<std::string>(0);
            if (!agent) return agent.Error();
            AgentRow out;
            out.agent = *agent;
            const auto read = [&row](std::size_t index,
                                     std::int64_t& target) -> Result<void> {
                auto value = row.Get<std::int64_t>(index);
                if (!value) return value.Error();
                target = *value;
                return {};
            };
            if (auto ok = read(1, out.requests); !ok) return ok.Error();
            if (auto ok = read(2, out.input); !ok) return ok.Error();
            if (auto ok = read(3, out.output); !ok) return ok.Error();
            if (auto ok = read(4, out.cacheRead); !ok) return ok.Error();
            if (auto ok = read(5, out.cacheWrite); !ok) return ok.Error();
            return out;
        });
    if (!agents) co_return agents.Error();

    snapshot.byAgent.reserve(agents->size());
    for (const auto& row : *agents) {
        usage::UsageTotals totalsForAgent;
        totalsForAgent.requests = row.requests;
        totalsForAgent.inputTokens = row.input;
        totalsForAgent.outputTokens = row.output;
        totalsForAgent.cacheReadTokens = row.cacheRead;
        totalsForAgent.cacheWriteTokens = row.cacheWrite;
        snapshot.byAgent.push_back(usage::AgentUsage{row.agent, totalsForAgent});
    }
    std::sort(snapshot.byAgent.begin(), snapshot.byAgent.end(),
              [](const usage::AgentUsage& a, const usage::AgentUsage& b) {
                  if (a.totals.TotalTokens() != b.totals.TotalTokens()) {
                      return a.totals.TotalTokens() > b.totals.TotalTokens();
                  }
                  return a.agent < b.agent;
              });
    co_return snapshot;
}

// 一轮同步：读位点 → worker 线程扫文件 → 一次事务里 upsert 记录/位点 → 查快照。
huxerui::Task<huxerui::sqlite::Result<usage::UsageSnapshot>> SyncUsageDatabase(
    huxerui::sqlite::Database database) {
    // 位点很小（几十 KB），直接查出来交给 worker。
    auto rows = co_await database.Select(kUsageScanState).AllAsync();
    if (!rows) co_return rows.Error();
    usage::ScanState state;
    for (const auto& row : *rows) {
        if (row.byteOffset < 0 || row.byteSize < 0) continue;
        state.emplace(row.file,
                      usage::FileState{static_cast<std::uintmax_t>(row.byteOffset),
                                       static_cast<std::uintmax_t>(row.byteSize)});
    }

    // 文件 IO + 解析全程在 worker 线程。
    usage::UsageScan scan = co_await huxerui::RunWorker(
        [state = std::move(state)] { return usage::ScanUsageLogs(state); });

    auto applied = co_await database.TransactionAsync(
        [&scan](huxerui::sqlite::Transaction& transaction)
            -> huxerui::sqlite::Result<void> {
            for (const auto& record : scan.records) {
                auto result = transaction.Execute(
                    std::string(kUpsertRecord), record.key, record.agent,
                    record.model, record.tsMillis, record.inputTokens,
                    record.outputTokens, record.cacheReadTokens,
                    record.cacheWriteTokens);
                if (!result) return result.Error();
            }
            for (const auto& [file, fileState] : scan.advances) {
                auto result = transaction.Execute(
                    std::string(kUpsertScanState), file,
                    static_cast<std::int64_t>(fileState.offset),
                    static_cast<std::int64_t>(fileState.size));
                if (!result) return result.Error();
            }
            for (const auto& file : scan.removed) {
                auto result = transaction.Execute(
                    "DELETE FROM scan_state WHERE file = ?", file);
                if (!result) return result.Error();
            }
            return {};
        });
    if (!applied) co_return applied.Error();

    co_return co_await QueryUsageSnapshot(database);
}

huxerui::Task<huxerui::sqlite::Result<usage::UsageSnapshot>> ClearUsageDatabase(
    huxerui::sqlite::Database database) {
    auto cleared = co_await database.TransactionAsync(
        [](huxerui::sqlite::Transaction& transaction)
            -> huxerui::sqlite::Result<void> {
            auto records = transaction.Execute("DELETE FROM usage_records");
            if (!records) return records.Error();
            auto state = transaction.Execute("DELETE FROM scan_state");
            if (!state) return state.Error();
            return {};
        });
    if (!cleared) co_return cleared.Error();
    co_return co_await QueryUsageSnapshot(database);
}

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
    // 会话日志导入：与代理统计是两个独立来源（对齐 cc-switch v3.13 的双数据源）。
    // 扫描读几百个会话文件（走 RunWorker），账本落 SQLite（走库的串行 worker），
    // 两者都不占 UI 线程；代次用于丢弃上一次延迟返回的结果。
    auto importedUsage = huxerui::UseState(usage::UsageSnapshot{});
    auto usageSyncing = huxerui::UseState(false);
    auto usageGeneration = huxerui::UseState(0);
    // 数据库句柄跟着本组合的生命周期（库是可拷贝句柄，共享一个连接与 worker）。
    auto usageDatabase =
        huxerui::UseState(std::optional<huxerui::sqlite::Database>{});
    const bool statsVisible = navPage.Get() == 2;

    // 打开库（首次）→ 返回句柄；失败时已经把错误交给调用方处理。
    auto ensureUsageDatabase =
        [](huxerui::State<std::optional<huxerui::sqlite::Database>> databaseState,
           huxerui::ToastHandle toastHandle) -> huxerui::Task<bool> {
        if (databaseState.Get().has_value()) co_return true;
        auto opened = co_await OpenUsageDatabase();
        if (!opened) {
            toastHandle.Show(std::format("打开用量账本失败：{}", opened.Error().Message()));
            co_return false;
        }
        databaseState = *opened;
        co_return true;
    };

    auto syncUsage = [=] {
        const int request = usageGeneration.Get() + 1;
        usageGeneration = request;
        usageSyncing = true;
        tasks.Launch([=]() -> huxerui::Task<void> {
            if (!co_await ensureUsageDatabase(usageDatabase, toast)) {
                if (usageGeneration.Get() != request) co_return;
                usageSyncing = false;
                co_return;
            }
            auto synced = co_await SyncUsageDatabase(*usageDatabase.Get());
            if (usageGeneration.Get() != request) co_return;
            usageSyncing = false;
            if (!synced) {
                toast.Show(std::format("同步会话用量失败：{}", synced.Error().Message()));
                co_return;
            }
            importedUsage = *synced;
        });
    };

    // 「刷新」只重查快照（不重扫文件）。
    auto refreshUsage = [=] {
        const int request = usageGeneration.Get() + 1;
        usageGeneration = request;
        tasks.Launch([=]() -> huxerui::Task<void> {
            if (!co_await ensureUsageDatabase(usageDatabase, toast)) co_return;
            auto snapshot = co_await QueryUsageSnapshot(*usageDatabase.Get());
            if (usageGeneration.Get() != request) co_return;
            if (!snapshot) {
                toast.Show(std::format("读取用量账本失败：{}", snapshot.Error().Message()));
                co_return;
            }
            importedUsage = *snapshot;
        });
    };

    // 「清空统计」同时清两个数据源：本地路由的内存统计 + JSONL 日志，以及会话
    // 日志导入的用量账本（usage.db 的两张表）。用量可从会话日志幂等重建
    // （scan_state 一并清空 → 下轮全量重扫），所以清掉是可恢复的。
    auto confirmClear = [=] {
        tasks.Launch([=]() -> huxerui::Task<void> {
            dialog.Show(
                "清空统计", "确定清空全部请求统计与日志？此操作不可撤销。",
                "清空", "取消",
                [=] {
                    routerInstance().clearStats();
                    ApplySnapshot(routerInstance().snapshot(), summary,
                                  providerStats);
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        if (!co_await ensureUsageDatabase(usageDatabase, toast)) {
                            co_return;
                        }
                        auto cleared =
                            co_await ClearUsageDatabase(*usageDatabase.Get());
                        if (!cleared) {
                            toast.Show(std::format("清空用量账本失败：{}",
                                                   cleared.Error().Message()));
                            co_return;
                        }
                        importedUsage = *cleared;
                        toast.Show("统计已清空");
                    });
                });
            co_return;
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
                refreshUsage();
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
