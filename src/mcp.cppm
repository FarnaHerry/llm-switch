// mcp.cppm —— llmswitch.mcp：MCP 服务器统一管理。
//
// SSOT 在 dataDir()/mcp.json（McpStore）；启停 = 写/删对应工具 live 配置里的条目：
//   claude-code → ~/.claude.json 顶层 mcpServers（深合并，保留其余字段；不是 settings.json）
//   codex       → ~/.codex/config.toml 行级 section 重写（丢弃所有 [mcp_servers.*] 节后
//                 按当前启用清单重新生成，其余内容原样保留）
//   opencode    → ~/.config/opencode/opencode.json 顶层 mcp（官方文件允许 JSON5 注释，
//                 解析失败抛中文错，不覆盖原文件）
// claude / pi：MVP 不支持，setEnabled / importFromTool 抛「该工具暂不支持 MCP 管理」。
//
// 所有 live 写入前先快照到 dataDir()/backups/mcp/（每文件保留最近 10 份），
// 写入走 .tmp + rename 原子写。移除条目只动 name 匹配的键，绝不动用户其他条目。
export module llmswitch.mcp;

import std;

export namespace mcp {

// 一个 MCP 服务器条目（name 为唯一键 slug）。
struct McpServer {
    std::string name;                 // 唯一键（slug）
    std::string type;                 // "stdio" | "sse" | "http"（空按 "stdio" 处理）
    std::string command;              // stdio 用
    std::vector<std::string> args;
    std::map<std::string, std::string> env;
    std::string url;                  // sse/http 用
    std::map<std::string, std::string> headers;
    std::vector<std::string> enabledTools;  // 已启用该服务器的工具 id 列表
    bool operator==(const McpServer&) const = default;
};

class McpStore {
public:
    static McpStore load();           // mcp.json，损坏文件挪 .corrupt-<毫秒> 不崩溃
    void save() const;                // 原子写（.tmp + rename）
    // CRUD
    void upsert(McpServer srv);       // 按 name 覆盖/新增；对 enabledTools 里每个工具同步写 live
    void remove(std::string_view name);  // 先从各 enabledTools 的 live 配置移除条目再删
    inline const std::vector<McpServer>& servers() const { return servers_; }
    // 启停 = 写/删该工具 live 配置里的对应条目 + 更新 enabledTools + save
    void setEnabled(std::string_view name, std::string_view toolId, bool enabled);
    // 从某工具 live 配置回读收编已有 MCP 条目（首次导入用；已存在同名则只补
    // enabledTools）。返回新收编的条数。codex 的 TOML 回读不做，返回 0。
    std::size_t importFromTool(std::string_view toolId);

private:
    std::vector<McpServer> servers_;
};

} // namespace mcp
