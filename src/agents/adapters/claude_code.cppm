// claude_code.cppm — Claude Code hooks 的独立适配器接口。
export module llmswitch.agents.claude_code;

import std;
import llmswitch.agents;

namespace agents::claude_code {

export std::optional<ActivityEvent> ParseHookInput(
    std::string_view input, std::int64_t nowMillis);

} // namespace agents::claude_code
