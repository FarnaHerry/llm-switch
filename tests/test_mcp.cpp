// test_mcp.cpp —— llmswitch.mcp 领域层测试（无框架：CHECK 失败计数，非零即败）。
// 全程隔离在临时目录：HOME / XDG_DATA_HOME / LLMSWITCH_CLAUDE_JSON /
// LLMSWITCH_OPENCODE_CONFIG / LLMSWITCH_CODEX_CONFIG 指向
// temp_directory_path()/llmswitch-test-mcp-<pid>，live 文件与 dataDir 都不碰真实环境。
//
// 覆盖：upsert + claude-code live 写入（深合并保留其余字段/条目）、opencode
// local/remote 两种类型、codex TOML 行级 section 重写（保留原有内容、禁用后节
// 移除、字符串转义）、禁用 claude-code、remove 全工具清理、损坏 mcp.json 挪走
// 不崩溃、importFromTool 收编 claude-code 条目、备份生成、不支持工具报错。
#include <cstdio>    // stderr（std 模块不导出 stdout/stderr 宏）
#include <cstdlib>   // setenv
#include <unistd.h>  // getpid

import std;
import nlohmann.json;
import llmswitch.config;
import llmswitch.mcp;

namespace {

int g_failures = 0;

#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            std::println(stderr, "FAIL {}: {}", __LINE__, #cond); \
            ++g_failures;                                        \
        }                                                        \
    } while (0)

void writeFile(const std::filesystem::path& path, std::string_view content) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

nlohmann::json readJson(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return nlohmann::json::parse(std::string(std::istreambuf_iterator<char>(in),
                                             std::istreambuf_iterator<char>()));
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

bool pathExists(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::exists(p, ec) && !ec;
}

// 目录下匹配 <prefix>.<时间戳>.bak 的文件数。
int countBackups(const std::filesystem::path& dir, const std::string& prefix) {
    std::error_code ec;
    int n = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.starts_with(prefix + ".") && name.ends_with(".bak")) ++n;
    }
    return n;
}

// 目录下匹配 <prefix>* 的文件数（.corrupt-<毫秒> 检查用）。
int countPrefixed(const std::filesystem::path& dir, const std::string& prefix) {
    std::error_code ec;
    int n = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().filename().string().starts_with(prefix)) ++n;
    }
    return n;
}

} // namespace

int main() {
    namespace fs = std::filesystem;

    // ---- 环境隔离 -----------------------------------------------------------
    const fs::path root =
        fs::temp_directory_path() / std::format("llmswitch-test-mcp-{}", ::getpid());
    {
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root);
    }
    const fs::path home = root / "home";
    ::setenv("HOME", home.c_str(), 1);
    ::setenv("XDG_DATA_HOME", (root / "xdg").c_str(), 1);
    const fs::path claudeJson = home / ".claude.json";
    const fs::path codexConfig = home / ".codex" / "config.toml";
    const fs::path opencodeConfig = root / "opencode" / "opencode.json";
    ::setenv("LLMSWITCH_CLAUDE_JSON", claudeJson.c_str(), 1);
    ::setenv("LLMSWITCH_CODEX_CONFIG", codexConfig.c_str(), 1);
    ::setenv("LLMSWITCH_OPENCODE_CONFIG", opencodeConfig.c_str(), 1);
    const fs::path mcpFile = root / "xdg" / "llm-switch" / "mcp.json";
    const fs::path mcpBackups = root / "xdg" / "llm-switch" / "backups" / "mcp";

    // 1. upsert stdio 服务器并启用 claude-code：mcpServers 条目正确，
    //    原有其他字段与用户条目保留。
    writeFile(claudeJson, R"json({
  "numStartups": 7,
  "mcpServers": {
    "user-own": {"command": "user-bin", "args": ["--keep"]}
  }
}
)json");
    auto st = mcp::McpStore::load();
    CHECK(st.servers().empty());
    mcp::McpServer fsSrv;
    fsSrv.name = "fs";
    fsSrv.type = "stdio";
    fsSrv.command = "npx";
    fsSrv.args = {"-y", "@modelcontextprotocol/server-filesystem", "/tmp"};
    fsSrv.env = {{"API_KEY", "secret"}};
    fsSrv.enabledTools = {"claude-code"};
    st.upsert(fsSrv);
    CHECK(st.servers().size() == 1);
    {
        const auto j = readJson(claudeJson);
        CHECK(j["mcpServers"]["fs"]["command"] == "npx");
        CHECK(j["mcpServers"]["fs"]["args"].size() == 3);
        CHECK(j["mcpServers"]["fs"]["args"][1] ==
              "@modelcontextprotocol/server-filesystem");
        CHECK(j["mcpServers"]["fs"]["env"]["API_KEY"] == "secret");
        CHECK(j["mcpServers"]["user-own"]["command"] == "user-bin");  // 用户条目保留
        CHECK(j["numStartups"] == 7);                                 // 其余字段保留
        // mcp.json 已持久化
        CHECK(pathExists(mcpFile));
        auto st2 = mcp::McpStore::load();
        CHECK(st2.servers().size() == 1);
        CHECK(st2.servers().front() == st.servers().front());
    }

    // 2. 启用 opencode：local（stdio）与 remote（http）两种类型各一例。
    writeFile(opencodeConfig, R"json({
  "model": "anthropic/claude-sonnet-4"
}
)json");
    mcp::McpServer remote;
    remote.name = "remote";
    remote.type = "http";
    remote.url = "https://mcp.example.com/api";
    remote.headers = {{"Authorization", "Bearer t"}};
    remote.enabledTools = {"opencode"};
    st.upsert(remote);
    st.setEnabled("fs", "opencode", true);
    {
        const auto j = readJson(opencodeConfig);
        CHECK(j["mcp"]["fs"]["type"] == "local");
        CHECK(j["mcp"]["fs"]["command"].size() == 4);  // command + args 拼一个数组
        CHECK(j["mcp"]["fs"]["command"][0] == "npx");
        CHECK(j["mcp"]["fs"]["command"][2] ==
              "@modelcontextprotocol/server-filesystem");
        CHECK(j["mcp"]["fs"]["environment"]["API_KEY"] == "secret");
        CHECK(j["mcp"]["fs"]["enabled"] == true);
        CHECK(j["mcp"]["remote"]["type"] == "remote");
        CHECK(j["mcp"]["remote"]["url"] == "https://mcp.example.com/api");
        CHECK(j["mcp"]["remote"]["headers"]["Authorization"] == "Bearer t");
        CHECK(j["mcp"]["remote"]["enabled"] == true);
        CHECK(j["model"] == "anthropic/claude-sonnet-4");  // 顶层其他字段保留
        const auto& s = st.servers()[1];
        CHECK(s.name == "remote" && s.enabledTools.size() == 1);
        const auto& f = st.servers()[0];
        CHECK(f.enabledTools.size() == 2);
    }

    // 3. 启用 codex：config.toml 保留原有 model_provider 等内容、生成
    //    [mcp_servers.x] 节（含 env 子节与字符串转义）；再禁用 → 节被移除且
    //    其余内容原样。
    writeFile(codexConfig,
              "model_provider = \"my-provider\"\n"
              "model = \"gpt-5\"\n"
              "\n"
              "[model_providers.my-provider]\n"
              "name = \"My Provider\"\n"
              "base_url = \"https://api.example.com/v1\"\n");
    mcp::McpServer esc;
    esc.name = "esc";
    esc.type = "stdio";
    esc.command = "C:\\tools\\mcp.exe";
    esc.args = {"say \"hi\"", "line\nbreak"};
    esc.enabledTools = {"codex"};
    st.upsert(esc);
    st.setEnabled("fs", "codex", true);
    {
        const std::string toml = readText(codexConfig);
        CHECK(toml.contains("model_provider = \"my-provider\""));
        CHECK(toml.contains("[model_providers.my-provider]"));
        CHECK(toml.contains("[mcp_servers.fs]"));
        CHECK(toml.contains("command = \"npx\""));
        CHECK(toml.contains(
            "args = [\"-y\", \"@modelcontextprotocol/server-filesystem\", \"/tmp\"]"));
        CHECK(toml.contains("[mcp_servers.fs.env]"));
        CHECK(toml.contains("API_KEY = \"secret\""));
        // 转义：反斜杠、引号、换行
        CHECK(toml.contains("command = \"C:\\\\tools\\\\mcp.exe\""));
        CHECK(toml.contains("\"say \\\"hi\\\"\""));
        CHECK(toml.contains("\"line\\nbreak\""));
        CHECK(st.servers()[0].enabledTools.size() == 3);
    }
    st.setEnabled("fs", "codex", false);
    {
        const std::string toml = readText(codexConfig);
        CHECK(!toml.contains("[mcp_servers.fs]"));      // fs 节移除
        CHECK(toml.contains("[mcp_servers.esc]"));      // esc 仍启用，保留
        CHECK(toml.contains("model_provider = \"my-provider\""));
        CHECK(toml.contains("base_url = \"https://api.example.com/v1\""));
    }

    // 4. 禁用 claude-code：条目移除，用户条目与其余字段保留。
    st.setEnabled("fs", "claude-code", false);
    {
        const auto j = readJson(claudeJson);
        CHECK(!j["mcpServers"].contains("fs"));
        CHECK(j["mcpServers"]["user-own"]["command"] == "user-bin");
        CHECK(j["numStartups"] == 7);
    }

    // 5. remove：各工具 live 条目都清掉。
    st.setEnabled("fs", "claude-code", true);
    st.setEnabled("fs", "codex", true);
    st.remove("fs");
    st.remove("esc");
    st.remove("remote");
    CHECK(st.servers().empty());
    {
        const auto j = readJson(claudeJson);
        CHECK(!j["mcpServers"].contains("fs"));
        CHECK(j["mcpServers"]["user-own"]["command"] == "user-bin");
        const auto o = readJson(opencodeConfig);
        CHECK(!o["mcp"].contains("fs"));
        CHECK(!o["mcp"].contains("remote"));
        const std::string toml = readText(codexConfig);
        CHECK(!toml.contains("mcp_servers"));
        CHECK(toml.contains("model_provider = \"my-provider\""));
        // remove 不存在的名字：静默无事
        st.remove("nope");
    }

    // 6. 损坏 mcp.json：load 不崩溃且坏文件被挪走。
    writeFile(mcpFile, "{ not valid json !!!");
    {
        auto broken = mcp::McpStore::load();
        CHECK(broken.servers().empty());
        CHECK(!pathExists(mcpFile));
        CHECK(countPrefixed(mcpFile.parent_path(), "mcp.json.corrupt-") == 1);
    }

    // 7. importFromTool 收编 claude-code 已有条目；重复导入只补 enabledTools。
    writeFile(claudeJson, R"json({
  "mcpServers": {
    "local-srv": {"command": "srv", "args": ["--x"], "env": {"K": "V"}},
    "remote-srv": {"type": "sse", "url": "https://sse.example.com",
                   "headers": {"H": "h"}}
  }
}
)json");
    {
        auto imp = mcp::McpStore::load();
        CHECK(imp.importFromTool("claude-code") == 2);
        CHECK(imp.servers().size() == 2);
        const auto& a = imp.servers()[0];
        CHECK(a.name == "local-srv");
        CHECK(a.type == "stdio");
        CHECK(a.command == "srv" && a.args.size() == 1);
        CHECK(a.env.at("K") == "V");
        CHECK(a.enabledTools.size() == 1 && a.enabledTools[0] == "claude-code");
        const auto& b = imp.servers()[1];
        CHECK(b.name == "remote-srv");
        CHECK(b.type == "sse");
        CHECK(b.url == "https://sse.example.com");
        CHECK(b.headers.at("H") == "h");
        // 再次导入：无新增，enabledTools 不重复
        CHECK(imp.importFromTool("claude-code") == 0);
        CHECK(imp.servers()[0].enabledTools.size() == 1);
        // codex 回读不做
        CHECK(imp.importFromTool("codex") == 0);
    }

    // 8. 备份文件生成：live 改写前快照到 backups/mcp/，保留最近 10 份。
    CHECK(countBackups(mcpBackups, ".claude.json") >= 1);
    CHECK(countBackups(mcpBackups, "opencode.json") >= 1);
    CHECK(countBackups(mcpBackups, "config.toml") >= 1);
    CHECK(countBackups(mcpBackups, "config.toml") <= 10);

    // 9. 不支持的工具：claude / pi 抛「该工具暂不支持 MCP 管理」。
    {
        bool threw = false;
        try {
            st.setEnabled("whatever", "claude", true);
        } catch (const std::runtime_error& e) {
            threw = std::string(e.what()).contains("该工具暂不支持 MCP 管理");
        }
        CHECK(threw);
        threw = false;
        try {
            st.importFromTool("pi");
        } catch (const std::runtime_error& e) {
            threw = std::string(e.what()).contains("该工具暂不支持 MCP 管理");
        }
        CHECK(threw);
    }

    if (g_failures == 0) {
        std::println("OK");
    } else {
        std::println(stderr, "{} failure(s)", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
