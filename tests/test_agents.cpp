// test_agents.cpp — 原生 Agent hooks 的隐私白名单、事件映射与内存聚合测试。
#include <cstdio>

import std;
import llmswitch.agents;
import llmswitch.agents.claude_code;
import llmswitch.agents.codex;
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

} // namespace

int main() {
    testClaudeCodeAdapter();
    testCodexAdapterAndBounds();
    testTrackerAggregationAndUnknown();
    testTrackerStaleSessionBecomesUnknown();
    testObservedIdleIsDistinctFromUnknown();

    if (g_failures != 0) {
        std::println(stderr, "{} Agent activity check(s) failed", g_failures);
        return 1;
    }
    std::println("All Agent activity checks passed");
    return 0;
}
