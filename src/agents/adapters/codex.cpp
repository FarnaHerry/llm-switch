// codex.cpp — Codex hook 事件映射。
module llmswitch.agents.codex;

import std;
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

} // namespace agents::codex
