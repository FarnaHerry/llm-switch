// test_usage.cpp — llmswitch.usage 领域层测试（无框架：CHECK 失败计数，非零即败）。
// 全程隔离在临时目录：LLMSWITCH_DATA_DIR 与各 LLMSWITCH_*_DIR 分别指向
// temp_directory_path()/llmswitch-test-usage-<pid>。
//
// 覆盖重点全是「数字会不会算错」：
//   * Claude 同一条 message.id 的流式重复追加 → 按字段取 max（不是最后一次）；
//   * input 是否含缓存各家不同（claude/pi 不含，codex/qwen/zcode 含）→ 归一化后
//     总量必须等于各自日志里的 total；
//   * Codex 的累计值事件（token_count.info.total_token_usage）不得被计入；
//   * sync 幂等：同一批文件重扫不新增；追加半行不消费；新增完整行才入库；
//   * snapshot 的 today 口径与 byAgent 汇总。
#include <cstdio>  // stderr（std 模块不导出 stdout/stderr 宏）

#include "test_env.h"  // testenv::setenv/unsetenv：MSVC 无 setenv，路径直传版处理宽窄字符

import std;
import llmswitch.config;
import llmswitch.usage;

namespace {

int g_failures = 0;

#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            std::println(stderr, "FAIL {}: {}", __LINE__, #cond); \
            ++g_failures;                                        \
        }                                                        \
    } while (0)

void writeFile(const std::filesystem::path& path, std::string_view content) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

void appendFile(const std::filesystem::path& path, std::string_view content) {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    out << content;
}

using usage::UsageRecord;

// 每个测试用独立的数据目录：LLMSWITCH_DATA_DIR 优先级最高（见 cfg::dataDir()），
// 否则几个扫描测试会共用同一份目录互相污染。
std::filesystem::path g_base;

void useDataDir(std::string_view name) {
    const auto dir = g_base / name;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    testenv::setenv("LLMSWITCH_DATA_DIR", dir);
}

const UsageRecord* find(const std::vector<UsageRecord>& v, std::string_view key) {
    for (const auto& r : v) {
        if (r.key == key) return &r;
    }
    return nullptr;
}

void testIsoParsing() {
    // 1970-01-01T00:00:00Z = 0
    CHECK(usage::IsoToMillis("1970-01-01T00:00:00Z") == 0);
    // 2026-09-12T09:19:16.475Z —— 与真实 codex 记录一致
    const auto t = usage::IsoToMillis("2026-09-12T09:19:16.475Z");
    CHECK(t == 1789204756475LL);
    CHECK(usage::IsoToMillis("2026-09-12T09:19:16.475Z") ==
          usage::IsoToMillis("2026-09-12T09:19:16.475+00:00"));
    CHECK(usage::IsoToMillis("") == 0);
    CHECK(usage::IsoToMillis("not-a-time") == 0);
}

void testClaude() {
    // 同一条 message.id 被流式追加三次：input 递增、最后一次 output 反而更小，
    // 按字段取 max 后应取 {input=3067, output=120, cacheRead=33664, cacheWrite=10}。
    const std::string jsonl =
        R"({"type":"assistant","timestamp":"2026-09-12T09:19:16.475Z","message":{"id":"msg_A","model":"claude-opus-4-8","usage":{"input_tokens":100,"output_tokens":5,"cache_read_input_tokens":33664,"cache_creation_input_tokens":10}}})"
        "\n"
        R"({"type":"assistant","timestamp":"2026-09-12T09:19:17.000Z","message":{"id":"msg_A","model":"claude-opus-4-8","usage":{"input_tokens":3067,"output_tokens":120,"cache_read_input_tokens":33664,"cache_creation_input_tokens":10}}})"
        "\n"
        R"({"type":"assistant","timestamp":"2026-09-12T09:19:18.000Z","message":{"id":"msg_A","model":"claude-opus-4-8","usage":{"input_tokens":3067,"output_tokens":40,"cache_read_input_tokens":33664,"cache_creation_input_tokens":10}}})"
        "\n"
        // 坏行与缺 usage 的行不能影响结果
        "\n{ not json\n"
        R"({"type":"user","message":{"role":"user"}})"
        "\n";
    const auto records = usage::ParseClaude(jsonl);
    CHECK(records.size() == 1);
    const auto* r = find(records, "claude-code:msg_A");
    CHECK(r != nullptr);
    if (r != nullptr) {
        CHECK(r->inputTokens == 3067);   // Anthropic：input 不含缓存
        CHECK(r->outputTokens == 120);   // 取 max，不是最后一次的 40
        CHECK(r->cacheReadTokens == 33664);
        CHECK(r->cacheWriteTokens == 10);
        CHECK(r->model == "claude-opus-4-8");
        CHECK(r->tsMillis == usage::IsoToMillis("2026-09-12T09:19:18.000Z"));
        // 归一化总量 = input + output + cacheRead + cacheWrite
        CHECK(r->inputTokens + r->outputTokens + r->cacheReadTokens +
                  r->cacheWriteTokens ==
              36861);
    }
}

void testCodex() {
    // 真实 codex 记录：total = input + output，input 含 cached；
    // 另有一条累计值事件（token_count）必须被忽略。
    const std::string jsonl =
        R"({"type":"token_usage_record","timestamp":"2026-09-12T09:19:16.475Z","payload":{"response_id":"resp_1","usage":{"input_tokens":22718,"cached_input_tokens":16128,"cache_write_input_tokens":0,"output_tokens":425}}})"
        "\n"
        R"({"type":"event_msg","timestamp":"2026-09-12T09:19:17.000Z","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":999999,"output_tokens":999999},"last_token_usage":{"input_tokens":22718,"output_tokens":425}}}})"
        "\n";
    const auto records = usage::ParseCodex(jsonl);
    CHECK(records.size() == 1);
    const auto* r = find(records, "codex:resp_1");
    CHECK(r != nullptr);
    if (r != nullptr) {
        CHECK(r->inputTokens == 22718 - 16128);  // 减掉缓存，避免重复计数
        CHECK(r->cacheReadTokens == 16128);
        CHECK(r->outputTokens == 425);
        // 归一化总量必须等于记录里的 total_tokens（23143）
        CHECK(r->inputTokens + r->outputTokens + r->cacheReadTokens +
                  r->cacheWriteTokens ==
              23143);
    }
}

void testQwen() {
    // 账本口径：totalTokens = inputTokens + outputTokens ⇒ input 含 cached。
    const std::string jsonl =
        R"({"schemaVersion":1,"id":"q1","timestamp":"2026-09-02T14:03:14.379Z","model":"qwen3.6-flash","inputTokens":12585,"outputTokens":340,"cachedTokens":12165,"thoughtsTokens":173,"totalTokens":12925})"
        "\n";
    const auto records = usage::ParseQwen(jsonl);
    CHECK(records.size() == 1);
    const auto* r = find(records, "qwen:q1");
    CHECK(r != nullptr);
    if (r != nullptr) {
        CHECK(r->inputTokens == 12585 - 12165);
        CHECK(r->cacheReadTokens == 12165);
        CHECK(r->outputTokens == 340);
        CHECK(r->model == "qwen3.6-flash");
        CHECK(r->inputTokens + r->outputTokens + r->cacheReadTokens +
                  r->cacheWriteTokens ==
              12925);  // == totalTokens
    }
}

void testPi() {
    // pi 口径：totalTokens = input + cacheWrite + cacheRead + output ⇒ input 不含缓存。
    const std::string jsonl =
        R"({"type":"message","id":"p1","timestamp":"2026-09-02T03:25:37.844Z","message":{"role":"assistant","model":"muse-spark","usage":{"input":535,"output":126,"cacheRead":7665,"cacheWrite":0,"totalTokens":8326,"cost":{"input":0}}}})"
        "\n"
        R"({"type":"message","id":"p2","timestamp":"2026-09-02T03:26:00.000Z","message":{"role":"user"}})"
        "\n";
    const auto records = usage::ParsePi(jsonl);
    CHECK(records.size() == 1);
    const auto* r = find(records, "pi:p1");
    CHECK(r != nullptr);
    if (r != nullptr) {
        CHECK(r->inputTokens == 535);
        CHECK(r->cacheReadTokens == 7665);
        CHECK(r->outputTokens == 126);
        CHECK(r->inputTokens + r->outputTokens + r->cacheReadTokens +
                  r->cacheWriteTokens ==
              8326);
    }
}

void testZcode() {
    // zcode 口径：totalTokens = inputTokens + outputTokens ⇒ input 含 cacheReadTokens。
    const std::string jsonl =
        R"({"completedAt":"2026-09-19T13:12:32.094Z","durationMs":16374,"requestId":"req_1","model":{"modelId":"glm-5.3","providerId":"llmswitch:x"},"response":{"usage":{"inputTokens":187081,"outputTokens":253,"totalTokens":187334,"cacheReadTokens":184768,"reasoningTokens":127}}})"
        "\n";
    const auto records = usage::ParseZcode(jsonl);
    CHECK(records.size() == 1);
    const auto* r = find(records, "zcode:req_1");
    CHECK(r != nullptr);
    if (r != nullptr) {
        CHECK(r->inputTokens == 187081 - 184768);
        CHECK(r->cacheReadTokens == 184768);
        CHECK(r->outputTokens == 253);
        CHECK(r->model == "glm-5.3");
        CHECK(r->inputTokens + r->outputTokens + r->cacheReadTokens +
                  r->cacheWriteTokens ==
              187334);
    }
}

// 增量扫描：真实目录 + 位点推进 + 半行不消费。纯计算，不涉及持久化。
void testScanIncremental() {
    useDataDir("scan");
    const auto root = std::filesystem::temp_directory_path() /
                      ("llmswitch-usage-scan-" + std::to_string(testenv::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    testenv::setenv("LLMSWITCH_CLAUDE_PROJECTS", (root / "claude"));
    testenv::setenv("LLMSWITCH_CODEX_SESSIONS", (root / "codex"));
    testenv::setenv("LLMSWITCH_QWEN_USAGE", (root / "qwen"));
    testenv::setenv("LLMSWITCH_PI_SESSIONS", (root / "pi"));
    testenv::setenv("LLMSWITCH_ZCODE_ROLLOUT", (root / "zcode"));

    const auto claudeFile = root / "claude" / "proj" / "s1.jsonl";
    const std::string lineA =
        R"({"type":"assistant","timestamp":"2026-09-12T09:19:16.475Z","message":{"id":"m1","model":"opus","usage":{"input_tokens":100,"output_tokens":7,"cache_read_input_tokens":0,"cache_creation_input_tokens":0}}})"
        "\n";
    const std::string lineA2 =
        R"({"type":"assistant","timestamp":"2026-09-12T09:19:17.000Z","message":{"id":"m1","model":"opus","usage":{"input_tokens":100,"output_tokens":9,"cache_read_input_tokens":0,"cache_creation_input_tokens":0}}})"
        "\n";
    writeFile(claudeFile, lineA);

    // 首轮：读到一条，位点推进到文件尾。
    const usage::UsageScan first = usage::ScanUsageLogs({});
    CHECK(first.report.scannedFiles == 1);
    CHECK(first.report.skippedFiles == 0);
    CHECK(first.records.size() == 1);
    CHECK(first.records.front().key == "claude-code:m1");
    CHECK(first.records.front().outputTokens == 7);
    CHECK(first.advances.size() == 1);
    CHECK(first.advances.front().second.size == std::filesystem::file_size(claudeFile));

    // 位点回灌：文件没变 → 只 stat 不读，不产出记录。
    usage::ScanState state;
    for (const auto& [file, fs] : first.advances) state.emplace(file, fs);
    const usage::UsageScan again = usage::ScanUsageLogs(state);
    CHECK(again.report.scannedFiles == 0);
    CHECK(again.report.skippedFiles == 1);
    CHECK(again.records.empty());
    CHECK(again.advances.empty());

    // 追加同一 message.id 的更大值：同一片段内按字段 max 合并，仍是同一条记录
    // （去重键相同，不会变成两条）。
    appendFile(claudeFile, lineA2);
    const usage::UsageScan third = usage::ScanUsageLogs(state);
    CHECK(third.report.scannedFiles == 1);
    CHECK(third.records.size() == 1);
    CHECK(third.records.front().key == "claude-code:m1");
    CHECK(third.records.front().outputTokens == 9);   // 取 max，不是最后一次覆盖
    CHECK(third.records.front().inputTokens == 100);

    // 末尾半行不消费：写半行扫不出记录，补齐后才产出。
    {
        usage::ScanState fresh;
        for (const auto& [file, fs] : third.advances) fresh.emplace(file, fs);
        appendFile(claudeFile,
                   R"({"type":"assistant","timestamp":"2026-09-12T09:20:00.000Z","message":{"id":"m2","usage":{"input_tokens":5,"output_tokens":1}})");
        const usage::UsageScan half = usage::ScanUsageLogs(fresh);
        CHECK(half.records.empty());
        appendFile(claudeFile, "}\n");
        const usage::UsageScan full = usage::ScanUsageLogs(fresh);
        CHECK(full.records.size() == 1);
        CHECK(full.records.front().key == "claude-code:m2");
    }

    std::filesystem::remove_all(root, ec);
    testenv::unsetenv("LLMSWITCH_CLAUDE_PROJECTS");
    testenv::unsetenv("LLMSWITCH_CODEX_SESSIONS");
    testenv::unsetenv("LLMSWITCH_QWEN_USAGE");
    testenv::unsetenv("LLMSWITCH_PI_SESSIONS");
    testenv::unsetenv("LLMSWITCH_ZCODE_ROLLOUT");
}

} // namespace

int main() {
    g_base = std::filesystem::temp_directory_path() /
             ("llmswitch-test-usage-" + std::to_string(testenv::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(g_base, ec);
    std::filesystem::create_directories(g_base, ec);
    // 隔离到临时数据目录，别污染真实 ~/.local/share/llm-switch。
    useDataDir("default");

    testIsoParsing();
    testClaude();
    testCodex();
    testQwen();
    testPi();
    testZcode();
    testScanIncremental();

    std::filesystem::remove_all(g_base, ec);
    testenv::unsetenv("LLMSWITCH_DATA_DIR");
    if (g_failures != 0) {
        std::println(stderr, "test_usage: {} check(s) failed", g_failures);
        return 1;
    }
    std::println("test_usage: all checks passed");
    return 0;
}
