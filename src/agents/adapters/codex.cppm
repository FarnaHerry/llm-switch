// codex.cppm — Codex hooks 的独立适配器接口。
export module llmswitch.agents.codex;

import std;
import llmswitch.agents;

namespace agents::codex {

export std::optional<ActivityEvent> ParseHookInput(
    std::string_view input, std::int64_t nowMillis);

// Codex requires a JSON success payload for Stop and SubagentStop handlers.
// This inspects only hook_event_name so the helper can emit a fixed response.
export bool RequiresJsonSuccessOutput(std::string_view input);

} // namespace agents::codex
