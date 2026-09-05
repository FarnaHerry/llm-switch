// config.cppm — llmswitch.config：用户数据目录与 Claude Code / Codex live 配置路径解析。
// 无 UI 依赖，models / store / UI 共用。
//
// live 配置文件（被切换工具实际读取的文件）默认在 ~ 下，全部支持环境变量覆盖，
// 测试把 HOME 与 LLMSWITCH_* 指到临时目录即可完全隔离：
//   LLMSWITCH_CLAUDE_SETTINGS → ~/.claude/settings.json
//   LLMSWITCH_CODEX_AUTH      → ~/.codex/auth.json
//   LLMSWITCH_CODEX_CONFIG    → ~/.codex/config.toml
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

// ---- live 配置路径（环境变量覆盖优先，便于测试与非常规安装）-----------------

// Claude Code 全局设置（供应商 baseUrl / token 写在 env 块）。
export std::filesystem::path claudeSettingsFile() {
    if (const char* e = std::getenv("LLMSWITCH_CLAUDE_SETTINGS"); e && *e) {
        return std::filesystem::path(e);
    }
    return homeDir() / ".claude" / "settings.json";
}

// Codex 凭据（OPENAI_API_KEY）。
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

// 系统是否偏好深色（"跟随系统"主题模式用）。启动时读取一次即可。
export bool systemPrefersDark() {
#if defined(_WIN32)
    HKEY key;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
                      "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    DWORD value = 1, size = sizeof(value);
    const LONG rc = RegQueryValueExA(key, "AppsUseLightTheme", nullptr, nullptr,
                                     reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS && value == 0;
#elif defined(__APPLE__)
    FILE* pipe = ::popen("defaults read -g AppleInterfaceStyle 2>/dev/null", "r");
    if (pipe == nullptr) return false;
    std::array<char, 32> buf{};
    const bool dark = std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr &&
                      std::string_view(buf.data()).starts_with("Dark");
    ::pclose(pipe);
    return dark;
#else
    FILE* pipe = ::popen("gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null", "r");
    if (pipe == nullptr) return false;
    std::array<char, 64> buf{};
    const bool dark = std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr &&
                      std::string_view(buf.data()).find("prefer-dark") != std::string_view::npos;
    ::pclose(pipe);
    return dark;
#endif
}

} // namespace cfg
