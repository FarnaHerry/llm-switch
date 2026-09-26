// config.cppm — llmswitch.config：用户数据目录与 Claude Code / Codex live 配置路径解析。
// 无 UI 依赖，models / store / UI 共用。
//
// live 配置文件（被切换工具实际读取的文件）默认在 ~ 下，全部支持环境变量覆盖，
// 测试把 HOME 与 LLMSWITCH_* 指到临时目录即可完全隔离：
//   LLMSWITCH_CLAUDE_SETTINGS     → ~/.claude/settings.json        （claude-code）
//   LLMSWITCH_CODEX_AUTH          → ~/.codex/auth.json             （codex）
//   LLMSWITCH_CODEX_CONFIG        → ~/.codex/config.toml           （codex）
//   LLMSWITCH_OPENCODE_CONFIG     → ~/.config/opencode/opencode.json（opencode）
//   LLMSWITCH_PI_DIR              → ~/.pi/agent/                   （pi；官方
//                                   PI_CODING_AGENT_DIR 亦受尊重，优先级低于前者）
//   LLMSWITCH_CLAUDE_DESKTOP_DIR  → macOS ~/Library/Application Support/Claude /
//                                   Windows %LOCALAPPDATA%/Claude  （claude desktop，
//                                   Linux 不支持；3p 目录取其兄弟 <dir>-3p）
//   LLMSWITCH_ZCODE_CONFIG      → ~/.zcode/v2/config.json        （zcode）
//   LLMSWITCH_DSH_SETTINGS      → ~/.dsh/settings.yaml           （dsh）
//   LLMSWITCH_DSH_CREDENTIALS   → ~/.dsh/.credentials.yaml       （dsh）
//   LLMSWITCH_HERMES_CONFIG     → ~/.hermes/config.yaml          （hermes，
//                                 Windows %LOCALAPPDATA%\hermes）
module;

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>  // readlink
#include <limits.h>  // PATH_MAX
#endif
#include <cstdio>  // popen / pclose（跟随系统的深色检测）

export module llmswitch.config;

import std;

namespace cfg {

namespace {

// 跟随系统的深色检测要碰系统资源：popen 管道与 Windows 注册表键。两者都包成
// RAII，提前 return / 抛异常都不会漏掉 pclose / RegCloseKey。
// 注意：popen / pclose 是 POSIX，MSVC 的全局命名空间里没有这两个名字——
// 这个 closer 只允许在真正使用它的 macOS / Linux 分支编译，不能提到
// #if 之外（否则 Windows 构建直接 C3861）。
#if !defined(_WIN32)
struct PipeCloser {
    void operator()(std::FILE* pipe) const noexcept {
        if (pipe != nullptr) ::pclose(pipe);
    }
};
using UniquePipe = std::unique_ptr<std::FILE, PipeCloser>;
#else
struct RegKeyCloser {
    void operator()(HKEY key) const noexcept {
        if (key != nullptr) ::RegCloseKey(key);
    }
};
using UniqueRegKey = std::unique_ptr<std::remove_pointer_t<HKEY>, RegKeyCloser>;
#endif

} // namespace

// 可执行文件目录（资源回退用）。
export std::filesystem::path executableDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(buf).parent_path();
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    if (_NSGetExecutablePath(nullptr, &size) != -1 || size == 0) return {};
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    return std::filesystem::weakly_canonical(buffer).parent_path();
#else
    char buf[PATH_MAX]{};
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    return std::filesystem::path(std::string(buf, static_cast<size_t>(n))).parent_path();
#endif
}

// 用户主目录：~ 展开只认环境变量（HOME / Windows 回落 USERPROFILE），
// 这样测试可以 setenv("HOME", 临时目录) 把 live 配置路径全部隔离。
export std::filesystem::path homeDir() {
    if (const char* h = std::getenv("HOME"); h && *h) {
        return std::filesystem::path(h);
    }
#ifdef _WIN32
    if (const char* u = std::getenv("USERPROFILE"); u && *u) {
        return std::filesystem::path(u);
    }
#endif
    return {};
}

// 用户数据目录：Linux $XDG_DATA_HOME/llm-switch（~/.local/share/llm-switch）/
// Windows %APPDATA%\llm-switch / macOS ~/Library/Application Support/llm-switch。
// config.json、backups/ 都放这里 —— 安装版启动时 cwd 可能不可写，不能依赖 cwd。
export std::filesystem::path dataDir() {
    // LLMSWITCH_DATA_DIR 全平台最优先覆盖（测试隔离与非常规安装用）。
    if (const char* d = std::getenv("LLMSWITCH_DATA_DIR"); d && *d) {
        return std::filesystem::path(d);
    }
#ifdef _WIN32
    if (const char* a = std::getenv("APPDATA"); a && *a) {
        return std::filesystem::path(a) / "llm-switch";
    }
    // APPDATA 缺失（服务/受限会话）：回落 exe 旁，保证打包形态仍可写。
    if (const std::filesystem::path dir = executableDir(); !dir.empty()) {
        return dir;
    }
#elif defined(__APPLE__)
    if (const char* h = std::getenv("HOME"); h && *h) {
        return std::filesystem::path(h) / "Library" / "Application Support" / "llm-switch";
    }
#else
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) {
        return std::filesystem::path(x) / "llm-switch";
    }
    if (const char* h = std::getenv("HOME"); h && *h) {
        return std::filesystem::path(h) / ".local" / "share" / "llm-switch";
    }
#endif
    return std::filesystem::current_path();
}

// 应用配置文件 dataDir()/config.json（确保父目录存在）。
export std::filesystem::path configFile() {
    const std::filesystem::path dir = dataDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / "config.json";
}

// 备份目录 dataDir()/backups/：改写任何 live 文件 / 导入配置前的快照都落这里
// （按 <工具>/<文件名>.<毫秒时间戳>.bak 组织，见 llmswitch.store）。
export std::filesystem::path backupsDir() {
    const std::filesystem::path dir = dataDir() / "backups";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// 用量查询模板的用户覆盖表 dataDir()/usage_templates.json（可选；存在即整体
// 替换资源包内置的官方默认表 resources/usage_templates.json）。
export std::filesystem::path usageTemplatesFile() {
    return dataDir() / "usage_templates.json";
}

// ---- live 配置路径（环境变量覆盖优先，便于测试与非常规安装）-----------------

// Claude Code 全局设置（供应商 baseUrl / token 写在 env 块）。
export std::filesystem::path claudeSettingsFile() {
    if (const char* e = std::getenv("LLMSWITCH_CLAUDE_SETTINGS"); e && *e) {
        return std::filesystem::path(e);
    }
    return homeDir() / ".claude" / "settings.json";
}

// Codex 凭据（OAuth tokens 或 OPENAI_API_KEY）。
export std::filesystem::path codexAuthFile() {
    if (const char* e = std::getenv("LLMSWITCH_CODEX_AUTH"); e && *e) {
        return std::filesystem::path(e);
    }
    return homeDir() / ".codex" / "auth.json";
}

// Codex 主配置（model_provider / model_providers 段，TOML 原文整体替换）。
export std::filesystem::path codexConfigFile() {
    if (const char* e = std::getenv("LLMSWITCH_CODEX_CONFIG"); e && *e) {
        return std::filesystem::path(e);
    }
    return homeDir() / ".codex" / "config.toml";
}

// opencode 主配置（全平台 ~/.config/opencode/opencode.json；additive 模式——
// 往顶层 provider map upsert，其余顶层字段保留。官方文件可能是 JSON5 带注释，
// store 层解析失败会明确报错，不静默覆盖）。
export std::filesystem::path opencodeConfigFile() {
    if (const char* e = std::getenv("LLMSWITCH_OPENCODE_CONFIG"); e && *e) {
        return std::filesystem::path(e);
    }
    return homeDir() / ".config" / "opencode" / "opencode.json";
}

// pi-mono coding agent 目录（默认 ~/.pi/agent；官方 PI_CODING_AGENT_DIR 优先于
// 默认路径，LLMSWITCH_PI_DIR 优先于两者——测试覆盖用）。
export std::filesystem::path piAgentDir() {
    if (const char* e = std::getenv("LLMSWITCH_PI_DIR"); e && *e) {
        return std::filesystem::path(e);
    }
    if (const char* e = std::getenv("PI_CODING_AGENT_DIR"); e && *e) {
        return std::filesystem::path(e);
    }
    return homeDir() / ".pi" / "agent";
}

// pi 的 providers 注册表（顶层 providers map upsert）。
export std::filesystem::path piModelsFile() {
    return piAgentDir() / "models.json";
}

// pi 的全局设置（深合并 defaultProvider / defaultModel）。
export std::filesystem::path piSettingsFile() {
    return piAgentDir() / "settings.json";
}

// gemini-cli 系 coding agent（Gemini CLI / Qwen Code）目录：认证与端点
// 覆盖写在目录下的 .env（KEY=VALUE 行级 upsert），auth 类型写 settings.json。
// LLMSWITCH_GEMINI_DIR / LLMSWITCH_QWEN_DIR 覆盖供测试与非常规安装。
export std::filesystem::path geminiDir() {
    if (const char* e = std::getenv("LLMSWITCH_GEMINI_DIR"); e && *e) {
        return std::filesystem::path(e);
    }
    return homeDir() / ".gemini";
}

// Gemini CLI 的 .env（GEMINI_API_KEY / GOOGLE_GEMINI_BASE_URL / GEMINI_MODEL）。
export std::filesystem::path geminiEnvFile() {
    return geminiDir() / ".env";
}

// Gemini CLI 全局设置（security.auth.selectedType 深合并）。
export std::filesystem::path geminiSettingsFile() {
    return geminiDir() / "settings.json";
}

export std::filesystem::path qwenDir() {
    if (const char* e = std::getenv("LLMSWITCH_QWEN_DIR"); e && *e) {
        return std::filesystem::path(e);
    }
    return homeDir() / ".qwen";
}

// Qwen Code 的 .env（OPENAI_API_KEY / OPENAI_BASE_URL / OPENAI_MODEL）。
export std::filesystem::path qwenEnvFile() {
    return qwenDir() / ".env";
}

// Qwen Code 全局设置（security.auth.selectedType 深合并）。
export std::filesystem::path qwenSettingsFile() {
    return qwenDir() / "settings.json";
}

// ZCode 的 provider 注册表（provider map upsert + enabled 切换；样本字段
// name/kind/options{apiKey,baseURL}/models）。LLMSWITCH_ZCODE_CONFIG 覆盖。
export std::filesystem::path zcodeConfigFile() {
    if (const char* e = std::getenv("LLMSWITCH_ZCODE_CONFIG"); e && *e) {
        return std::filesystem::path(e);
    }
    return homeDir() / ".zcode" / "v2" / "config.json";
}

// DeepSeek Harness（dsh）主设置（llm-pi-ai.providers 手写路由 +
// agent-default-model 指向，YAML 行级改写，其余键原样保留）。
// $DSH_HOME 为官方目录覆盖变量，优先于默认 ~/.dsh，低于 LLMSWITCH_* 覆盖。
export std::filesystem::path dshSettingsFile() {
    if (const char* e = std::getenv("LLMSWITCH_DSH_SETTINGS"); e && *e) {
        return std::filesystem::path(e);
    }
    if (const char* e = std::getenv("DSH_HOME"); e && *e) {
        return std::filesystem::path(e) / "settings.yaml";
    }
    return homeDir() / ".dsh" / "settings.yaml";
}

// dsh 的密钥库（env 名 → 密钥值的 YAML map，热监听即时生效；目录 0700、
// 文件 0600）。settings.yaml 只写 apiKeyEnv 引用，密钥一律不落主设置。
export std::filesystem::path dshCredentialsFile() {
    if (const char* e = std::getenv("LLMSWITCH_DSH_CREDENTIALS"); e && *e) {
        return std::filesystem::path(e);
    }
    if (const char* e = std::getenv("DSH_HOME"); e && *e) {
        return std::filesystem::path(e) / ".credentials.yaml";
    }
    return homeDir() / ".dsh" / ".credentials.yaml";
}

// Hermes Agent 主配置（custom_providers 列表 + 顶层 model 节指向，YAML 行级
// 改写，其余节原样保留）。$HERMES_HOME 为官方目录覆盖变量（对齐 cc-switch
// 与 hermes_cli 的 get_hermes_home），Windows 默认 %LOCALAPPDATA%\hermes。
export std::filesystem::path hermesConfigFile() {
    if (const char* e = std::getenv("LLMSWITCH_HERMES_CONFIG"); e && *e) {
        return std::filesystem::path(e);
    }
#ifdef _WIN32
    if (const char* e = std::getenv("HERMES_HOME"); e && *e) {
        return std::filesystem::path(e) / "config.yaml";
    }
    if (const char* e = std::getenv("LOCALAPPDATA"); e && *e) {
        return std::filesystem::path(e) / "hermes" / "config.yaml";
    }
    return homeDir() / "AppData" / "Local" / "hermes" / "config.yaml";
#else
    if (const char* e = std::getenv("HERMES_HOME"); e && *e) {
        return std::filesystem::path(e) / "config.yaml";
    }
    return homeDir() / ".hermes" / "config.yaml";
#endif
}

// Claude Desktop 配置目录（3p Direct 模式；**Linux 不支持**，返回空路径——
// store 层据此报「不支持」错误。LLMSWITCH_CLAUDE_DESKTOP_DIR 覆盖供测试与
// 非常规安装，覆盖即放行平台门）。
export std::filesystem::path claudeDesktopDir() {
    if (const char* e = std::getenv("LLMSWITCH_CLAUDE_DESKTOP_DIR"); e && *e) {
        return std::filesystem::path(e);
    }
#ifdef _WIN32
    if (const char* a = std::getenv("LOCALAPPDATA"); a && *a) {
        return std::filesystem::path(a) / "Claude";
    }
    return {};
#elif defined(__APPLE__)
    if (const auto h = homeDir(); !h.empty()) {
        return h / "Library" / "Application Support" / "Claude";
    }
    return {};
#else
    return {};  // Linux 不支持
#endif
}

// Claude Desktop 的 3p 数据目录（Claude 的兄弟目录 Claude-3p；
// 覆盖变量生效时取 <覆盖目录>-3p 兄弟路径）。
export std::filesystem::path claudeDesktop3pDir() {
    const std::filesystem::path base = claudeDesktopDir();
    if (base.empty()) return {};
    return base.parent_path() / (base.filename().string() + "-3p");
}

// 系统是否偏好深色（"跟随系统"主题模式用）。启动时读取一次即可。
export bool systemPrefersDark() {
#if defined(_WIN32)
    HKEY raw_key = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
                      "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &raw_key) != ERROR_SUCCESS)
        return false;
    const UniqueRegKey key(raw_key);
    DWORD value = 1, size = sizeof(value);
    const LONG rc = RegQueryValueExA(key.get(), "AppsUseLightTheme", nullptr, nullptr,
                                     reinterpret_cast<LPBYTE>(&value), &size);
    return rc == ERROR_SUCCESS && value == 0;
#elif defined(__APPLE__)
    const UniquePipe pipe(::popen("defaults read -g AppleInterfaceStyle 2>/dev/null", "r"));
    if (!pipe) return false;
    std::array<char, 32> buf{};
    return std::fgets(buf.data(), static_cast<int>(buf.size()), pipe.get()) != nullptr &&
           std::string_view(buf.data()).starts_with("Dark");
#else
    const UniquePipe pipe(
        ::popen("gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null", "r"));
    if (!pipe) return false;
    std::array<char, 64> buf{};
    return std::fgets(buf.data(), static_cast<int>(buf.size()), pipe.get()) != nullptr &&
           std::string_view(buf.data()).find("prefer-dark") != std::string_view::npos;
#endif
}

// ---- 本地路由统计（llmswitch.router）----

// 路由统计目录：默认 dataDir()/router/；LLMSWITCH_STATS_DIR 覆盖（测试隔离用）。
export std::filesystem::path statsDir() {
    if (const char* e = std::getenv("LLMSWITCH_STATS_DIR"); e && *e) {
        return std::filesystem::path(e);
    }
    return dataDir() / "router";
}

// 请求统计 JSONL 文件 statsDir()/requests.jsonl（确保父目录存在）。
// llmswitch.router 每请求追加一行，启动时从它回填内存统计。
export std::filesystem::path statsFile() {
    const std::filesystem::path dir = statsDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / "requests.jsonl";
}

// 用量导入账本目录（dataDir()/usage/）：各 agent 会话日志解析出的 token 记录
// 与增量扫描状态都放这里，与路由请求日志分开，互不影响。
export inline std::filesystem::path usageDir() {
    const std::filesystem::path dir = dataDir() / "usage";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// ---- MCP 管理（llmswitch.mcp）----

// Claude Code 全局用户配置（顶层 mcpServers map 写这里；**不是** settings.json）。
export std::filesystem::path claudeJsonFile() {
    if (const char* e = std::getenv("LLMSWITCH_CLAUDE_JSON"); e && *e) {
        return std::filesystem::path(e);
    }
    return homeDir() / ".claude.json";
}

// MCP 服务器统一清单（SSOT）：dataDir()/mcp.json（确保父目录存在）。
export std::filesystem::path mcpStoreFile() {
    const std::filesystem::path dir = dataDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / "mcp.json";
}

// ---- Skills / 会话管理（llmswitch.skills / llmswitch.sessions）----

// skills 中央库：dataDir()/skills-store/<name>/（SKILL.md + 附带文件），
// 通过符号链接同步到各工具的 skills 目录（见 llmswitch.skills）。
export std::filesystem::path skillsStoreDir() {
    if (const char* e = std::getenv("LLMSWITCH_SKILLS_STORE"); e && *e) {
        return std::filesystem::path(e);
    }
    return dataDir() / "skills-store";
}

// Claude Code 的 skills 目录（~/.claude/skills）。
export std::filesystem::path claudeSkillsDir() {
    if (const char* e = std::getenv("LLMSWITCH_CLAUDE_SKILLS"); e && *e) {
        return std::filesystem::path(e);
    }
    return homeDir() / ".claude" / "skills";
}

// Codex 的 skills 目录（~/.codex/skills）。
export std::filesystem::path codexSkillsDir() {
    if (const char* e = std::getenv("LLMSWITCH_CODEX_SKILLS"); e && *e) {
        return std::filesystem::path(e);
    }
    return homeDir() / ".codex" / "skills";
}

// Claude Code 历史会话根（~/.claude/projects/<项目>/*.jsonl）。
export std::filesystem::path claudeProjectsDir() {
    if (const char* e = std::getenv("LLMSWITCH_CLAUDE_PROJECTS"); e && *e) {
        return std::filesystem::path(e);
    }
    return homeDir() / ".claude" / "projects";
}

// Codex 历史会话根（~/.codex/sessions/<年>/<月>/<日>/*.jsonl）。
export std::filesystem::path codexSessionsDir() {
    if (const char* e = std::getenv("LLMSWITCH_CODEX_SESSIONS"); e && *e) {
        return std::filesystem::path(e);
    }
    return homeDir() / ".codex" / "sessions";
}

// ---- 用量导入（llmswitch.usage）的会话/账本根 ------------------------------
// 各 agent 记录的 token 用量位置不同：Qwen 有专门的月度账本，pi / zcode 藏在
// 会话或模型调用记录里，Claude / Codex 复用上面的会话根。全部可被 LLMSWITCH_*
// 覆盖，测试可隔离到临时目录。

// Qwen Code 用量账本目录（token-usage-<年>-<月>.jsonl）。
export std::filesystem::path qwenUsageDir() {
    if (const char* e = std::getenv("LLMSWITCH_QWEN_USAGE"); e && *e) {
        return std::filesystem::path(e);
    }
    return homeDir() / ".qwen" / "usage";
}

// pi 会话根（~/.pi/agent/sessions/<项目>/*.jsonl）。
export std::filesystem::path piSessionsDir() {
    if (const char* e = std::getenv("LLMSWITCH_PI_SESSIONS"); e && *e) {
        return std::filesystem::path(e);
    }
    return homeDir() / ".pi" / "agent" / "sessions";
}

// zcode 模型调用记录根（~/.zcode/cli/rollout/model-io-sess-*.jsonl）。
export std::filesystem::path zcodeRolloutDir() {
    if (const char* e = std::getenv("LLMSWITCH_ZCODE_ROLLOUT"); e && *e) {
        return std::filesystem::path(e);
    }
    return homeDir() / ".zcode" / "cli" / "rollout";
}

// 用量账本目录（dataDir()/usage/）：usage.jsonl 与 scan-state.json。
// 定义在上方 statsFile() 之后（同文件内已可见）。

// ---- 本地环境检查（设置页）-------------------------------------------------

// 在 PATH 与几个已知安装目录里查找可执行文件；命中返回绝对路径，找不到返回空。
// 纯文件系统查询，不起子进程（HuxerUI 也没有子进程 API），所以只判断「装没装」
// 与装在哪，不判断版本。
//
// 除 PATH 外补的回落目录都是实测见过的装法：官方安装脚本的 ~/.local/bin
// （claude / codex）、opencode 自有的 ~/.opencode/bin、pnpm 全局的
// ~/.local/share/pnpm/bin（dsh / qwen）。从桌面启动的应用 PATH 常常比登录 shell
// 窄，只查 PATH 会误报未安装。
export std::filesystem::path findExecutable(std::string_view name) {
    if (name.empty()) return {};
    const auto probe = [&name](const std::filesystem::path& dir)
        -> std::filesystem::path {
        if (dir.empty()) return {};
        std::error_code ec;
        std::filesystem::path candidate = dir / std::string(name);
#ifdef _WIN32
        // Windows 下可执行是 .exe/.cmd/.bat：按 PATHEXT 依次试。
        const char* pathext = std::getenv("PATHEXT");
        const std::string exts =
            (pathext != nullptr && *pathext != '\0') ? pathext
                                                     : ".COM;.EXE;.BAT;.CMD";
        std::size_t pos = 0;
        while (pos <= exts.size()) {
            const auto semi = exts.find(';', pos);
            const std::string ext =
                exts.substr(pos, semi == std::string::npos ? std::string::npos
                                                           : semi - pos);
            if (!ext.empty()) {
                std::filesystem::path withExt =
                    candidate.string() + ext;
                if (std::filesystem::is_regular_file(withExt, ec) && !ec) {
                    return withExt;
                }
            }
            if (semi == std::string::npos) break;
            pos = semi + 1;
        }
        return {};
#else
        if (!std::filesystem::is_regular_file(candidate, ec) || ec) return {};
        // POSIX 还要有可执行位，避免把同名数据文件当成 CLI。
        const auto perms = std::filesystem::status(candidate, ec).permissions();
        if (ec) return {};
        if ((perms & (std::filesystem::perms::owner_exec |
                      std::filesystem::perms::group_exec |
                      std::filesystem::perms::others_exec)) ==
            std::filesystem::perms::none) {
            return {};
        }
        return candidate;
#endif
    };

    if (const char* pathEnv = std::getenv("PATH"); pathEnv != nullptr && *pathEnv) {
#ifdef _WIN32
        constexpr char kSep = ';';
#else
        constexpr char kSep = ':';
#endif
        std::string_view paths(pathEnv);
        while (!paths.empty()) {
            const auto sep = paths.find(kSep);
            const std::string_view dir =
                sep == std::string_view::npos ? paths : paths.substr(0, sep);
            if (!dir.empty()) {
                if (auto hit = probe(std::filesystem::path(dir)); !hit.empty()) {
                    return hit;
                }
            }
            if (sep == std::string_view::npos) break;
            paths.remove_prefix(sep + 1);
        }
    }
    for (const auto& fallback :
         {homeDir() / ".local" / "bin", homeDir() / ".opencode" / "bin",
          homeDir() / ".local" / "share" / "pnpm" / "bin"}) {
        if (auto hit = probe(fallback); !hit.empty()) return hit;
    }
    return {};
}

} // namespace cfg
