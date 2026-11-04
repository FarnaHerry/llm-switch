// llm-switch-agent-hook — fail-open adapter executable invoked by native hooks.
// It reads bounded stdin, reduces it to an allowlisted ActivityEvent, forwards
// that event to the loopback bridge, and never writes hook payloads to disk/stdout.

import std;
import nlohmann.json;
import llmswitch.config;
import llmswitch.agents;
import llmswitch.agents.bridge;
import llmswitch.agents.claude_code;
import llmswitch.agents.codex;

namespace {

constexpr std::size_t kMaxInputBytes = 1024 * 1024;

std::optional<std::string> ToolArgument(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) == "--tool") {
            const std::string_view tool(argv[i + 1]);
            if (tool == "claude-code" || tool == "codex") return std::string(tool);
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::string ReadBoundedInput() {
    std::string input;
    input.reserve(4096);
    char buffer[4096];
    while (std::cin) {
        std::cin.read(buffer, sizeof(buffer));
        const auto count = std::cin.gcount();
        if (count <= 0) break;
        if (input.size() + static_cast<std::size_t>(count) > kMaxInputBytes) {
            return {};
        }
        input.append(buffer, static_cast<std::size_t>(count));
    }
    return input;
}

std::int64_t NowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

int main(int argc, char** argv) {
    // Every failure is intentionally silent and successful from the Agent's
    // perspective: observing activity must never control or block its workflow.
    try {
        const auto tool = ToolArgument(argc, argv);
        if (!tool) return 0;
        const std::string input = ReadBoundedInput();
        if (input.empty()) return 0;
        const std::int64_t now = NowMillis();
        const auto event = *tool == "claude-code"
            ? agents::claude_code::ParseHookInput(input, now)
            : agents::codex::ParseHookInput(input, now);
        if (event) (void)agents::SendToBridge(cfg::agentBridgeFile(), *event);
        if (*tool == "codex" &&
            agents::codex::RequiresJsonSuccessOutput(input)) {
            // Codex requires JSON on successful Stop/SubagentStop hooks. This
            // constant response has no event or conversation data and leaves
            // the turn unchanged.
            std::cout << R"({"continue":true})" << '\n';
        }
    } catch (...) {
    }
    return 0;
}
