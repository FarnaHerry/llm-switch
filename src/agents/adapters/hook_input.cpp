// hook_input.cpp — 白名单字段解析实现。
module llmswitch.agents.hook_input;

import std;
import nlohmann.json;
import llmswitch.agents;

namespace agents::adapters {
namespace {

constexpr std::size_t kMaxToolIdBytes = 64;
constexpr std::size_t kMaxSessionKeyBytes = 256;
constexpr std::size_t kMaxEventNameBytes = 64;

} // namespace

std::optional<ActivityEvent> ParseHookInput(
    std::string_view input, std::string_view tool,
    const HookEventMap& eventMap, std::int64_t nowMillis) {
    if (input.empty() || input.size() > kMaxHookInputBytes || tool.empty() ||
        tool.size() > kMaxToolIdBytes || nowMillis < 0) {
        return std::nullopt;
    }

    const nlohmann::json root =
        nlohmann::json::parse(input.begin(), input.end(), nullptr, false);
    if (!root.is_object()) return std::nullopt;

    const auto eventIt = root.find("hook_event_name");
    const auto sessionIt = root.find("session_id");
    if (eventIt == root.end() || !eventIt->is_string() ||
        sessionIt == root.end() || !sessionIt->is_string()) {
        return std::nullopt;
    }

    const std::string eventName = eventIt->get<std::string>();
    const std::string sessionKey = sessionIt->get<std::string>();
    if (eventName.empty() || eventName.size() > kMaxEventNameBytes ||
        sessionKey.empty() || sessionKey.size() > kMaxSessionKeyBytes) {
        return std::nullopt;
    }

    const auto mapped = eventMap.find(eventName);
    if (mapped == eventMap.end()) return std::nullopt;
    return ActivityEvent{std::string(tool), sessionKey, mapped->second, nowMillis};
}

} // namespace agents::adapters
