// codex.cpp — Codex hook 事件映射。
module llmswitch.agents.codex;

import std;
import nlohmann.json;
import llmswitch.agents.hook_input;

namespace agents::codex {

std::optional<ActivityEvent> ParseHookInput(
    std::string_view input, std::int64_t nowMillis) {
    static const adapters::HookEventMap events{
        {"SessionStart", ActivityState::idle},
        {"UserPromptSubmit", ActivityState::working},
        {"PreToolUse", ActivityState::working},
        {"PostToolUse", ActivityState::working},
        {"PermissionRequest", ActivityState::waiting},
        {"Stop", ActivityState::completed},
        {"Interrupt", ActivityState::cancelled},
        {"SessionEnd", ActivityState::sessionEnded},
        {"SubagentStart", ActivityState::working},
        {"SubagentStop", ActivityState::working},
    };
    return adapters::ParseHookInput(input, "codex", events, nowMillis);
}

bool RequiresJsonSuccessOutput(std::string_view input) {
    if (input.empty() || input.size() > adapters::kMaxHookInputBytes) return false;
    const nlohmann::json root =
        nlohmann::json::parse(input.begin(), input.end(), nullptr, false);
    if (!root.is_object()) return false;
    const auto event = root.find("hook_event_name");
    if (event == root.end() || !event->is_string()) return false;
    const std::string name = event->get<std::string>();
    return name == "Stop" || name == "SubagentStop";
}

} // namespace agents::codex
