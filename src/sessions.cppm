// sessions.cppm — llmswitch.sessions：扫描各 agent 工具的历史会话文件。
//
// claude-code：~/.claude/projects/<项目>/*.jsonl（项目 = 目录名）；
// codex：~/.codex/sessions/<年>/<月>/<日>/*.jsonl（project = 相对日期路径）。
// 所有标题/行数提取都是 best-effort，解析失败一律回落，绝不抛。
// 路径全部走 llmswitch.config（LLMSWITCH_* 可覆盖，测试可隔离）。
export module llmswitch.sessions;

import std;

namespace sessions {

export struct SessionInfo {
    std::string tool;            // "claude-code" / "codex"
    std::string id;              // 文件 stem
    std::string project;         // claude: projects 下的目录名；codex: 相对日期路径
    std::string title;           // 首条用户消息摘要（截取 80 字符；取不到用文件 stem）
    std::filesystem::path path;
    std::int64_t mtimeMillis = 0;
    std::uintmax_t sizeBytes = 0;
    std::size_t messageCount = 0;   // jsonl 行数（上限数到 10000）
    bool operator==(const SessionInfo&) const = default;
};

// 两工具合并，按 mtime 倒序。
export std::vector<SessionInfo> listSessions();
// 单工具（"claude-code" / "codex"），按 mtime 倒序；未知工具抛中文错。
export std::vector<SessionInfo> listSessions(std::string_view toolId);

// 删除会话文件。安全检查：必须在已知的 sessions 根之下才删，否则抛中文错。
export void deleteSession(const std::filesystem::path& p);

// 复制到指定目录，返回目标路径（同名覆盖）。
export std::filesystem::path exportSession(const std::filesystem::path& p,
                                           const std::filesystem::path& destDir);

} // namespace sessions
