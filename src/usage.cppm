// usage.cppm — llmswitch.usage：从各 agent 自己的会话/账本日志导入 token 用量。
//
// 为什么读日志而不是只靠代理：cc-switch v3.13 起把用量做成两个来源——代理请求
// 日志，以及直接解析 CLI 会话日志。本项目对齐后者：各 agent 本来就把每次调用的
// token 写进了自己的日志，读出来即可，用户不需要开本地路由。
//
// 各 agent 的格式完全不同（见 usage.cpp 的 collector），但共同的坑有三个：
//   1) 同一次调用会被重复写多行（Claude Code 流式追加），必须按去重键合并；
//   2) 合并规则是**按字段取最大值**——实测「最后一次覆盖」会少算（有 4% 的
//      message.id 最后一次反而更小），取 max 永不漏；
//   3) Codex 另有 total_token_usage 这样的**累计值**字段，按行累加会严重虚高，
//      只取逐条记录。
//
// 落库：cfg::usageDir()/usage.jsonl（append-only，装载时按去重键 max 合并）
// 与 scan-state.json（每个文件已消费到的字节偏移，保证增量与幂等）。
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
    std::size_t storedRecords = 0;  // 值有变化、追加进账本的条数
};

// 增量扫描 + 幂等落库。sync() 会做文件 IO，调用方负责放到 worker 线程。
export class UsageStore {
public:
    UsageStore();
    ~UsageStore();
    UsageStore(const UsageStore&) = delete;
    UsageStore& operator=(const UsageStore&) = delete;

    void load();
    SyncReport sync();
    void clear();

    [[nodiscard]] UsageSnapshot snapshot() const;
    [[nodiscard]] std::int64_t recordCount() const;
    [[nodiscard]] std::filesystem::path ledgerPath() const;

    // ---- 各 agent 的解析入口（公开以便单测直接喂文本）----
    // 输入是一段 JSONL（通常是文件的新增片段），输出已按去重键做字段级 max
    // 合并后的记录；坏行跳过，缺字段按 0 处理。
    static std::vector<UsageRecord> ParseClaude(std::string_view jsonl);
    static std::vector<UsageRecord> ParseCodex(std::string_view jsonl);
    static std::vector<UsageRecord> ParseQwen(std::string_view jsonl);
    static std::vector<UsageRecord> ParsePi(std::string_view jsonl);
    static std::vector<UsageRecord> ParseZcode(std::string_view jsonl);

    // ISO 8601（YYYY-MM-DDTHH:MM:SS[.fff][Z]）→ epoch 毫秒。自己实现而不用
    // std::chrono::parse：后者在 libstdc++/libc++/MSVC 上的可用性与时区行为
    // 不一致，跨平台 CI 不可控。无法识别时返回 0。
    static std::int64_t IsoToMillis(std::string_view iso);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace usage
