// sessions.cppm — llmswitch.sessions：扫描各 agent 工具的历史会话文件。
//
// claude-code：~/.claude/projects/<项目>/*.jsonl（项目 = 目录名）；
// codex：~/.codex/sessions/<年>/<月>/<日>/*.jsonl（project = 相对日期路径）。
// 列表扫描枚举文件并在同一个后台批次中读取固定大小的头尾摘要，解析失败一律回落；
// 完整会话只在详情页按需读取。路径全部走 llmswitch.config（LLMSWITCH_* 可覆盖，
// 测试可隔离）。
export module llmswitch.sessions;

import std;

namespace sessions {

export struct SessionInfo {
    std::string tool;            // "claude-code" / "codex"
    std::string id;              // 文件 stem
    std::string project;         // claude: projects 下的目录名；codex: 相对日期路径
    std::string title;           // 首条有效用户消息；取不到时回落文件 stem
    std::string preview;         // 文件尾部最近一条有效消息；取不到时回落 title
    std::filesystem::path path;
    std::int64_t mtimeMillis = 0;
    std::uintmax_t sizeBytes = 0;
    bool operator==(const SessionInfo&) const = default;
};

export struct SessionSummary {
    std::string title;
    std::string preview;
    bool operator==(const SessionSummary&) const = default;
};

export struct SessionMessage {
    std::string role;            // "user" / "assistant"
    std::string text;
    std::uintmax_t sourceOffset = 0; // JSONL 中的稳定字节位置（分页读取时提供）
    bool operator==(const SessionMessage&) const = default;
};

export struct SessionMessagePage {
    std::vector<SessionMessage> messages; // 当前页内按时间正序
    std::uintmax_t nextBeforeOffset = 0;  // 下一页从该字节偏移之前继续
    bool hasMore = false;
};

// 两工具合并，按 mtime 倒序。
export std::vector<SessionInfo> listSessions();
// 单工具（"claude-code" / "codex"），按 mtime 倒序；未知工具抛中文错。
export std::vector<SessionInfo> listSessions(std::string_view toolId);

// 读取一个会话的固定大小摘要。调用方必须在 worker 线程执行；路径必须位于已知
// sessions 根目录之下。列表扫描已批量完成同样的摘要读取，此接口保留给其他按需调用方。
export SessionSummary summarizeSession(
    std::string_view toolId, const std::filesystem::path& path);

// 读取指定会话的完整可显示文本消息。调用方必须在 worker 线程执行。
export std::vector<SessionMessage> readSession(
    std::string_view toolId, const std::filesystem::path& path);

// 从 beforeOffset（0 表示文件末尾）向前读取最多 maxMessages 条可显示消息。
// 用于详情页从最新消息开始、向上滚动时增量加载历史。
export SessionMessagePage readSessionPage(
    std::string_view toolId, const std::filesystem::path& path,
    std::uintmax_t beforeOffset, std::size_t maxMessages);

// 删除会话文件。安全检查：必须在已知的 sessions 根之下才删，否则抛中文错。
export void deleteSession(const std::filesystem::path& p);

// 复制到指定目录，返回目标路径（同名覆盖）。
export std::filesystem::path exportSession(const std::filesystem::path& p,
                                           const std::filesystem::path& destDir);

} // namespace sessions
