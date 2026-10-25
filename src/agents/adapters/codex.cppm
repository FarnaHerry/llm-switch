// codex.cppm — Codex hooks 的独立适配器接口。
export module llmswitch.agents.codex;

import std;
import llmswitch.agents;

namespace agents::codex {

export std::optional<ActivityEvent> ParseHookInput(
    std::string_view input, std::int64_t nowMillis);

} // namespace agents::codex
