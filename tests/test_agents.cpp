// test_agents.cpp — 原生 Agent hooks 的隐私白名单、事件映射与内存聚合测试。
#include <cstdio>
#include "test_env.h"

import std;
import nlohmann.json;
import llmswitch.config;
import llmswitch.agents;
import llmswitch.agents.bridge;
import llmswitch.agents.claude_code;
import llmswitch.agents.codex;
import llmswitch.agents.hook_manager;
import llmswitch.agents.hook_input;

namespace {

int g_failures = 0;

#define CHECK(cond)                                               \
    do {                                                          \
        if (!(cond)) {                                             \
            std::println(stderr, "FAIL {}: {}", __LINE__, #cond); \
            ++g_failures;                                         \
        }                                                         \
    } while (false)

template <class Function>
bool throwsRuntimeError(Function&& function) {
    try {
        std::invoke(std::forward<Function>(function));
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

void testClaudeCodeAdapter() {
    constexpr std::string_view secret = "prompt-must-not-leak-9f31";
    const std::string input = std::format(
        R"({{"session_id":"claude-session-1","hook_event_name":"UserPromptSubmit","prompt":"{}","tool_input":{{"command":"{}"}}}})",
        secret, secret);

    const auto event = agents::claude_code::ParseHookInput(input, 1000);
    CHECK(event.has_value());
    if (event) {
        CHECK(event->tool == "claude-code");
        CHECK(event->sessionKey == "claude-session-1");
        CHECK(event->state == agents::ActivityState::working);
        CHECK(event->occurredAtMillis == 1000);
        CHECK(event->tool.find(secret) == std::string::npos);
        CHECK(event->sessionKey.find(secret) == std::string::npos);
    }

    const auto waiting = agents::claude_code::ParseHookInput(
        R"({"session_id":"s1","hook_event_name":"PermissionRequest","tool_input":{"command":"private"}})",
        1100);
    CHECK(waiting && waiting->state == agents::ActivityState::waiting);

    const auto toolFailure = agents::claude_code::ParseHookInput(
        R"({"session_id":"s1","hook_event_name":"PostToolUseFailure","tool_response":"private"})",
        1150);
    CHECK(toolFailure && toolFailure->state == agents::ActivityState::working);

    const auto failed = agents::claude_code::ParseHookInput(
        R"({"session_id":"s1","hook_event_name":"StopFailure","last_assistant_message":"private"})",
        1200);
    CHECK(failed && failed->state == agents::ActivityState::failed);

    CHECK(!agents::claude_code::ParseHookInput(
        R"({"session_id":"s1","hook_event_name":"UnknownEvent","prompt":"private"})",
        1300));
}

void testCodexAdapterAndBounds() {
    CHECK(agents::codex::RequiresJsonSuccessOutput(
        R"({"session_id":"s1","hook_event_name":"Stop"})"));
    CHECK(agents::codex::RequiresJsonSuccessOutput(
        R"({"session_id":"s1","hook_event_name":"SubagentStop"})"));
    CHECK(!agents::codex::RequiresJsonSuccessOutput(
        R"({"session_id":"s1","hook_event_name":"Interrupt"})"));

    const auto interrupted = agents::codex::ParseHookInput(
        R"({"session_id":"codex-session-2","hook_event_name":"Interrupt","input_messages":["private"]})",
        2000);
    CHECK(interrupted.has_value());
    if (interrupted) {
        CHECK(interrupted->tool == "codex");
        CHECK(interrupted->state == agents::ActivityState::cancelled);
    }

    CHECK(!agents::codex::ParseHookInput(
        R"({"session_id":"s1","hook_event_name":"PreToolUse","transcript_path":"/private/path"})",
        -1));
    CHECK(!agents::codex::ParseHookInput(
        R"({"session_id":42,"hook_event_name":"PreToolUse"})", 2100));
    CHECK(!agents::codex::ParseHookInput("{broken", 2100));

    const std::string oversized(agents::adapters::kMaxHookInputBytes + 1, ' ');
    CHECK(!agents::codex::ParseHookInput(oversized, 2100));
}

void testTrackerAggregationAndUnknown() {
    using agents::ActivityEvent;
    using agents::ActivityState;
    using agents::SourceHealth;

    agents::ActivityTracker tracker;
    tracker.SetSourceHealth("claude-code", SourceHealth::awaitingEvent);
    tracker.SetSourceHealth("codex", SourceHealth::needsTrust);

    auto snapshot = tracker.SnapshotAt(1000);
    CHECK(snapshot.state == ActivityState::unknown);
    CHECK(snapshot.unknown);

    CHECK(tracker.Apply(ActivityEvent{
        "claude-code", "s1", ActivityState::working, 1000}));
    CHECK(tracker.Apply(ActivityEvent{
        "claude-code", "s2", ActivityState::waiting, 1100}));
    snapshot = tracker.SnapshotAt(1200);
    CHECK(snapshot.state == ActivityState::waiting);
    CHECK(snapshot.unknown);  // Codex 尚未被信任，整体仍有未知来源。
    CHECK(snapshot.activeTools == std::vector<std::string>{"claude-code"});
    CHECK(snapshot.sources.size() == 2);
    CHECK(snapshot.sources[0].activeSessions == 2);
    CHECK(snapshot.sources[0].waitingSessions == 1);

    // 迟到的旧事件不覆盖同一 session 的新状态。
    CHECK(!tracker.Apply(ActivityEvent{
        "claude-code", "s2", ActivityState::working, 1050}));
    CHECK(tracker.Apply(ActivityEvent{
        "claude-code", "s2", ActivityState::sessionEnded, 1300}));
    snapshot = tracker.SnapshotAt(1300);
    CHECK(snapshot.state == ActivityState::working);
    CHECK(snapshot.sources[0].activeSessions == 1);

    CHECK(tracker.Apply(ActivityEvent{
        "claude-code", "s1", ActivityState::completed, 1400}));
    snapshot = tracker.SnapshotAt(1400);
    CHECK(snapshot.state == ActivityState::completed);
    snapshot = tracker.SnapshotAt(10'000);  // Claude 完成提示窗已过，可确认来源空闲。
    CHECK(snapshot.sources[0].state == ActivityState::idle);
    CHECK(snapshot.state == ActivityState::unknown);  // Codex 仍是 needsTrust。
    CHECK(snapshot.unknown);

    snapshot = tracker.SnapshotAt(70'000);  // 无新 hook，不能无限期推断 idle。
    CHECK(snapshot.state == ActivityState::unknown);
    CHECK(snapshot.unknown);

    tracker.SetSourceHealth("claude-code", SourceHealth::disabled);
    CHECK(!tracker.Apply(ActivityEvent{
        "claude-code", "s3", ActivityState::working, 71'000}));
    snapshot = tracker.SnapshotAt(71'000);
    CHECK(snapshot.state == ActivityState::unknown);
    CHECK(snapshot.unknown);  // 所有已配置来源都未提供可信活动状态。
}

void testTrackerStaleSessionBecomesUnknown() {
    agents::ActivityTracker tracker;
    tracker.SetSourceHealth("codex", agents::SourceHealth::awaitingEvent);
    CHECK(tracker.Apply(agents::ActivityEvent{
        "codex", "session", agents::ActivityState::working, 1000}));
    CHECK(tracker.SnapshotAt(2000).state == agents::ActivityState::working);

    const auto stale = tracker.SnapshotAt(6 * 60 * 1000);
    CHECK(stale.state == agents::ActivityState::unknown);
    CHECK(stale.unknown);
    CHECK(stale.sources.size() == 1);
    CHECK(stale.sources[0].activeSessions == 0);
}

void testObservedIdleIsDistinctFromUnknown() {
    agents::ActivityTracker tracker;
    tracker.SetSourceHealth("claude-code", agents::SourceHealth::awaitingEvent);
    CHECK(tracker.Apply(agents::ActivityEvent{
        "claude-code", "session", agents::ActivityState::idle, 1000}));

    const auto idle = tracker.SnapshotAt(2000);
    CHECK(idle.state == agents::ActivityState::idle);
    CHECK(!idle.unknown);
    CHECK(idle.sources.size() == 1);
    CHECK(idle.sources[0].state == agents::ActivityState::idle);
}

void writeFile(const std::filesystem::path& path, std::string_view text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

void testHookConfigInstallIsIncremental() {
    const auto root = std::filesystem::temp_directory_path() /
        std::format("llmswitch-agents-{}", testenv::getpid());
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    testenv::setenv("HOME", root / "home");
    testenv::setenv("LLMSWITCH_DATA_DIR", root / "data");
    testenv::setenv("LLMSWITCH_CLAUDE_SETTINGS", root / "home/.claude/settings.json");
    testenv::setenv("LLMSWITCH_CODEX_HOOKS", root / "home/.codex/hooks.json");
    testenv::setenv("LLMSWITCH_CODEX_CONFIG", root / "home/.codex/config.toml");

    const auto helper = root / "bin/llm-switch-agent-hook";
    writeFile(helper, "test executable placeholder");
    const auto claudeSettings = cfg::claudeSettingsFile();
    const std::string claudeOriginal =
        R"({"env":{"KEEP":"value"},"hooks":{"Stop":[{"matcher":"Bash","hooks":[{"type":"command","command":"user-stop-hook"}]}]}})";
    writeFile(claudeSettings, claudeOriginal);

    agents::hooks::Configure("claude-code", helper, true);
    auto claude = nlohmann::json::parse(readFile(claudeSettings));
    CHECK(claude["env"]["KEEP"] == "value");
    CHECK(agents::hooks::Inspect("claude-code").installed);
    bool userStopStillThere = false;
    for (const auto& group : claude["hooks"]["Stop"]) {
        for (const auto& handler : group["hooks"]) {
            if (handler.value("command", std::string{}).find("user-stop-hook") !=
                std::string::npos) userStopStillThere = true;
        }
    }
    CHECK(userStopStillThere);
    const std::string installedClaude = readFile(claudeSettings);
    agents::hooks::Configure("claude-code", helper, true);
    CHECK(readFile(claudeSettings) == installedClaude);  // 幂等，不重复堆叠
    agents::hooks::Configure("claude-code", helper, false);
    claude = nlohmann::json::parse(readFile(claudeSettings));
    CHECK(claude["env"]["KEEP"] == "value");
    CHECK(!agents::hooks::Inspect("claude-code").installed);
    CHECK(readFile(claudeSettings).find("user-stop-hook") != std::string::npos);

    const auto codexConfig = cfg::codexConfigFile();
    const auto codexHooks = cfg::codexHooksFile();
    writeFile(codexConfig, "[features]\nhooks = false\n");
    CHECK(throwsRuntimeError([&] {
        agents::hooks::Configure("codex", helper, true);
    }));
    CHECK(!std::filesystem::exists(codexHooks));
    writeFile(codexConfig, "[features]\nhooks = true\n");
    writeFile(codexHooks,
        R"({"description":"user file","hooks":{"Stop":[{"hooks":[{"type":"command","command":"user-codex-stop"}]}]}})");
    agents::hooks::Configure("codex", helper, true);
    CHECK(agents::hooks::Inspect("codex").installed);
    const auto installedCodex = nlohmann::json::parse(readFile(codexHooks));
    CHECK(installedCodex["hooks"]["SessionEnd"][0]["matcher"] == "other");
    agents::hooks::Configure("codex", helper, false);
    const auto codex = nlohmann::json::parse(readFile(codexHooks));
    CHECK(codex["description"] == "user file");
    CHECK(readFile(codexHooks).find("user-codex-stop") != std::string::npos);
    CHECK(!agents::hooks::Inspect("codex").installed);

    std::filesystem::remove_all(root, ec);
}

void testBridgeLoopbackRoundTrip() {
    const auto root = std::filesystem::temp_directory_path() /
        std::format("llmswitch-agent-bridge-{}", testenv::getpid());
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    const auto descriptor = root / "agents/bridge.json";
    agents::ActivityBridge bridge;
    CHECK(bridge.Start(descriptor));
    CHECK(bridge.Running());
    bridge.SetSourceHealth("claude-code", agents::SourceHealth::awaitingEvent);
    CHECK(agents::SendToBridge(descriptor, agents::ActivityEvent{
        "claude-code", "short-lived-session", agents::ActivityState::working, 5000}));
    agents::ActivitySnapshot snapshot;
    for (int i = 0; i < 50; ++i) {
        snapshot = bridge.SnapshotAt(5100);
        if (!snapshot.activeTools.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(snapshot.state == agents::ActivityState::working);
    CHECK(snapshot.activeTools == std::vector<std::string>{"claude-code"});
    bridge.Stop();
    CHECK(!std::filesystem::exists(descriptor));
    std::filesystem::remove_all(root, ec);
}

} // namespace

int main() {
    testClaudeCodeAdapter();
    testCodexAdapterAndBounds();
    testTrackerAggregationAndUnknown();
    testTrackerStaleSessionBecomesUnknown();
    testObservedIdleIsDistinctFromUnknown();
    testHookConfigInstallIsIncremental();
    testBridgeLoopbackRoundTrip();

    if (g_failures != 0) {
        std::println(stderr, "{} Agent activity check(s) failed", g_failures);
        return 1;
    }
    std::println("All Agent activity checks passed");
    return 0;
}
