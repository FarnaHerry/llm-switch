// claude_code.cpp — Claude Code hook 事件映射。
module llmswitch.agents.claude_code;

import std;
import llmswitch.agents.hook_input;

namespace agents::claude_code {

std::optional<ActivityEvent> ParseHookInput(
    std::string_view input, std::int64_t nowMillis) {
    static const adapters::HookEventMap events{
        {"SessionStart", ActivityState::idle},
        {"UserPromptSubmit", ActivityState::working},
        {"PreToolUse", ActivityState::working},
        {"PostToolUse", ActivityState::working},
        // 单个工具失败后 Agent 仍可能继续当前 turn，不把它当成整轮失败。
        {"PostToolUseFailure", ActivityState::working},
        {"PermissionRequest", ActivityState::waiting},
        {"Stop", ActivityState::completed},
        {"StopFailure", ActivityState::failed},
        {"SessionEnd", ActivityState::sessionEnded},
        {"SubagentStart", ActivityState::working},
        {"SubagentStop", ActivityState::working},
    };
    return adapters::ParseHookInput(input, "claude-code", events, nowMillis);
}

} // namespace agents::claude_code
