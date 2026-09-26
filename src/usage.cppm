// usage.cppm — llmswitch.usage：从各 agent 自己的会话/账本日志导入 token 用量。
//
// 为什么读日志而不是只靠代理：cc-switch v3.13 起把用量做成两个来源——代理请求
// 日志，以及直接解析 CLI 会话日志。本项目对齐后者：各 agent 本来就把每次调用的
// token 写进了自己的日志，读出来即可，用户不需要开本地路由。
//
// 各 agent 的格式完全不同（见 usage.cpp 的 parser），但共同的坑有三个：
//   1) 同一次调用会被重复写多行（Claude Code 流式追加），必须按去重键合并；
//   2) 合并规则是**按字段取最大值**——实测「最后一次覆盖」会少算（有 4% 的
//      message.id 最后一次反而更小），取 max 永不漏；
//   3) Codex 另有 total_token_usage 这样的**累计值**字段，按行累加会严重虚高，
//      只取逐条记录。
//
// 本模块只做**纯计算**：解析各 agent 日志、按字节位点增量扫描、聚合口径。
// 持久化不在这里——账本落在 SQLite（HuxerUI/Lib-SQLite），由 UI 层持有并异步
// 编排（见 src/ui/stats_page.cpp 的 usage 段）。这样本模块保持无 huxerui 依赖，
// 可以照常被轻量单测直接链接。
export module llmswitch.usage;

import std;

namespace usage {

// 一条用量记录 = 一次上游调用（去重后的最小单位）。
export struct UsageRecord {
    std::string agent;  // claude-code / codex / qwen / pi / zcode
    std::string model;
    std::int64_t tsMillis = 0;
    std::int64_t inputTokens = 0;
    std::int64_t outputTokens = 0;
    std::int64_t cacheReadTokens = 0;
    std::int64_t cacheWriteTokens = 0;
    std::string key;  // 去重键：agent + ":" + 各 agent 的原生 id

    bool operator==(const UsageRecord&) const = default;
};

export struct UsageTotals {
    std::int64_t requests = 0;
    std::int64_t inputTokens = 0;
    std::int64_t outputTokens = 0;
    std::int64_t cacheReadTokens = 0;
    std::int64_t cacheWriteTokens = 0;

    // 「真实消耗」归一化总量：输入 + 输出 + 缓存创建 + 缓存读取，与
    // cc-switch v3.15 的口径一致（缓存读写单独成列，不重复计入输入）。
    [[nodiscard]] constexpr std::int64_t TotalTokens() const {
        return inputTokens + outputTokens + cacheReadTokens + cacheWriteTokens;
    }

    bool operator==(const UsageTotals&) const = default;
};

export struct AgentUsage {
    std::string agent;
    UsageTotals totals;
    bool operator==(const AgentUsage&) const = default;
};

export struct UsageSnapshot {
    UsageTotals total;
    std::vector<AgentUsage> byAgent;  // 按归一化总量降序
    std::int64_t records = 0;
    std::int64_t todayRequests = 0;
    std::int64_t todayTokens = 0;
    bool operator==(const UsageSnapshot&) const = default;
};

export struct SyncReport {
    std::size_t scannedFiles = 0;  // 本轮实际读取的文件数（未变的文件跳过）
    std::size_t skippedFiles = 0;  // size 与上次一致、直接跳过的文件数
    std::size_t parsedRecords = 0;  // 解析出的记录数（合并前）
};

// 每个源文件已消费到的字节位点（持久化在 SQLite 的 scan_state 表）。
export struct FileState {
    std::uintmax_t offset = 0;
    std::uintmax_t size = 0;
    bool operator==(const FileState&) const = default;
};
export using ScanState = std::map<std::string, FileState>;

// 一轮扫描的结果。调用方负责把这三样落库（同一次事务里）：
// records 按去重键字段级 max 合并后写入 usage_records；advances 推进位点；
// removed 清掉已消失文件的位点。跨轮次的 max 合并由数据库的 upsert 保证。
export struct UsageScan {
    std::vector<UsageRecord> records;
    std::vector<std::pair<std::string, FileState>> advances;
    std::vector<std::string> removed;
    SyncReport report;
};

// 扫描各 agent 日志的新增部分。纯文件 IO + 解析，不做持久化；会读大量文件，
// 调用方负责放到 worker 线程。
export UsageScan ScanUsageLogs(const ScanState& state);

// 本地当天 0 点的 epoch 毫秒（聚合「今日」口径用）。
export std::int64_t TodayStartMillis();

// ---- 各 agent 的解析入口（公开以便单测直接喂文本）----
// 输入是一段 JSONL（通常是文件的新增片段），输出已按去重键做字段级 max
// 合并后的记录；坏行跳过，缺字段按 0 处理。
export std::vector<UsageRecord> ParseClaude(std::string_view jsonl);
export std::vector<UsageRecord> ParseCodex(std::string_view jsonl);
export std::vector<UsageRecord> ParseQwen(std::string_view jsonl);
export std::vector<UsageRecord> ParsePi(std::string_view jsonl);
export std::vector<UsageRecord> ParseZcode(std::string_view jsonl);

// ISO 8601（YYYY-MM-DDTHH:MM:SS[.fff][Z]）→ epoch 毫秒。自己实现而不用
// std::chrono::parse：后者在 libstdc++/libc++/MSVC 上的可用性与时区行为
// 不一致，跨平台 CI 不可控。无法识别时返回 0。
export std::int64_t IsoToMillis(std::string_view iso);

} // namespace usage
