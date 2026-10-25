// launch_at_login.cpp — 当前用户的登录启动项（不执行 shell 命令）。

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winreg.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <vector>
#elif defined(__linux__)
#include <limits.h>
#include <unistd.h>
#endif

namespace llmswitch::ui {
namespace {

[[noreturn]] void Fail(std::string message) {
    throw std::runtime_error(std::move(message));
}

#if defined(_WIN32)
std::filesystem::path ExecutablePath() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        Fail("无法确定应用程序路径");
    }
    buffer.resize(length);
    return std::filesystem::path(buffer);
}
#elif defined(__APPLE__)
std::filesystem::path ExecutablePath() {
    std::uint32_t size = 0;
    static_cast<void>(_NSGetExecutablePath(nullptr, &size));
    if (size == 0) Fail("无法确定应用程序路径");
    std::vector<char> buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        Fail("无法确定应用程序路径");
    }
    return std::filesystem::path(buffer.data());
}
#elif defined(__linux__)
std::filesystem::path ExecutablePath() {
    if (const char* appImage = std::getenv("APPIMAGE");
        appImage != nullptr && *appImage != '\0') {
        std::filesystem::path path(appImage);
        if (path.is_relative()) path = std::filesystem::absolute(path);
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec) && !ec) return path;
    }
    std::string buffer(static_cast<std::size_t>(PATH_MAX) + 1, '\0');
    const ssize_t length = readlink("/proc/self/exe", buffer.data(),
                                    buffer.size() - 1);
    if (length <= 0) Fail("无法确定应用程序路径");
    buffer.resize(static_cast<std::size_t>(length));
    return std::filesystem::path(buffer);
}
#endif

#if defined(__linux__) || defined(__APPLE__)
std::filesystem::path HomeDirectory() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') Fail("无法确定当前用户目录");
    return std::filesystem::path(home);
}

void WriteAtomically(const std::filesystem::path& destination,
                     const std::string& contents) {
    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec) Fail("无法创建登录启动目录：" + ec.message());

    auto temporary = destination;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) Fail("无法写入登录启动配置");
        output.write(contents.data(),
                     static_cast<std::streamsize>(contents.size()));
        output.flush();
        if (!output) {
            output.close();
            std::filesystem::remove(temporary, ec);
            Fail("无法写入登录启动配置");
        }
    }
    std::filesystem::permissions(
        temporary,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        Fail("无法设置登录启动配置权限");
    }
    std::filesystem::rename(temporary, destination, ec);
    if (ec) {
        const std::string reason = ec.message();
        std::filesystem::remove(temporary, ec);
        Fail("无法保存登录启动配置：" + reason);
    }
}

std::string XmlEscape(std::string_view value) {
    std::string result;
    for (const char character : value) {
        switch (character) {
        case '&': result += "&amp;"; break;
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '"': result += "&quot;"; break;
        case '\'': result += "&apos;"; break;
        default: result += character; break;
        }
    }
    return result;
}
#endif

#if defined(__linux__)
std::filesystem::path LinuxAutostartPath() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && *xdg != '\0' &&
        std::filesystem::path(xdg).is_absolute()) {
        return std::filesystem::path(xdg) / "autostart/llm-switch.desktop";
    }
    return HomeDirectory() / ".config/autostart/llm-switch.desktop";
}

std::string DesktopExecEscape(std::string_view value) {
    std::string result;
    for (const char character : value) {
        if (character == '%') {
            result += "%%";
        } else {
            if (character == '\\' || character == '"') result += '\\';
            result += character;
        }
    }
    return result;
}
#endif

} // namespace

void SetLaunchAtLogin(bool enabled) {
#if defined(_WIN32)
    constexpr wchar_t kRunKey[] =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr wchar_t kValueName[] = L"llm-switch";
    HKEY key = nullptr;
    const LONG opened = RegCreateKeyExW(
        HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
        &key, nullptr);
    if (opened != ERROR_SUCCESS) {
        Fail("无法打开当前用户的登录启动设置");
    }
    const auto closeKey = [&] { RegCloseKey(key); };
    LONG result = ERROR_SUCCESS;
    if (enabled) {
        const std::wstring command = L"\"" + ExecutablePath().native() + L"\"";
        result = RegSetValueExW(
            key, kValueName, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(key, kValueName);
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    closeKey();
    if (result != ERROR_SUCCESS) Fail("无法更新当前用户的登录启动设置");
#elif defined(__APPLE__)
    const auto destination =
        HomeDirectory() / "Library/LaunchAgents/dev.farna.llm-switch.plist";
    std::error_code ec;
    if (!enabled) {
        std::filesystem::remove(destination, ec);
        if (ec) Fail("无法移除登录启动配置：" + ec.message());
        return;
    }
    const std::string executable = XmlEscape(ExecutablePath().string());
    const std::string contents =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\"><dict>\n"
        "<key>Label</key><string>dev.farna.llm-switch</string>\n"
        "<key>ProgramArguments</key><array><string>" + executable +
        "</string></array>\n<key>RunAtLoad</key><true/>\n"
        "</dict></plist>\n";
    WriteAtomically(destination, contents);
#elif defined(__linux__)
    const auto destination = LinuxAutostartPath();
    std::error_code ec;
    if (!enabled) {
        std::filesystem::remove(destination, ec);
        if (ec) Fail("无法移除登录启动配置：" + ec.message());
        return;
    }
    const std::string executable =
        DesktopExecEscape(ExecutablePath().string());
    const std::string contents =
        "[Desktop Entry]\nType=Application\nVersion=1.0\n"
        "Name=llm-switch\nComment=AI desktop companion\nExec=\"" +
        executable + "\"\nTerminal=false\nX-GNOME-Autostart-enabled=true\n";
    WriteAtomically(destination, contents);
#else
    if (enabled) Fail("此平台暂不支持登录时启动");
#endif
}

} // namespace llmswitch::ui
