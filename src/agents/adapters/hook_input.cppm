// hook_input.cppm — 受限的 JSON 输入解析共用层。
export module llmswitch.agents.hook_input;

import std;
import llmswitch.agents;

namespace agents::adapters {

export using HookEventMap =
    std::map<std::string, ActivityState, std::less<>>;

// Agent hook 可把完整上下文送到 stdin；只取 session_id 与 hook_event_name。
// 大输入、坏 JSON 和未识别事件都静默忽略，绝不把原始输入放入返回值。
export inline constexpr std::size_t kMaxHookInputBytes = 1024 * 1024;
export std::optional<ActivityEvent> ParseHookInput(
    std::string_view input, std::string_view tool,
    const HookEventMap& eventMap, std::int64_t nowMillis);

} // namespace agents::adapters
