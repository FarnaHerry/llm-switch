// open_path.cpp — 用系统默认编辑器打开配置文件。
//
// 配置路径由 cfg::*File() 解析（可能来自 LLMSWITCH_* / XDG / HOME），这里只
// 负责把路径交给桌面默认关联程序；不读取文件内容，也不等待编辑器退出。
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

#include "ui.h"

namespace llmswitch::ui {

namespace {

#if !defined(_WIN32)
// POSIX shell 单引号转义：路径来自环境变量，避免空格/引号破坏命令。
std::string ShellQuote(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('\'');
    for (const char c : value) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}
#endif

} // namespace

bool OpenInSystemEditor(const std::filesystem::path& path) {
    if (path.empty()) return false;
#if defined(_WIN32)
    // start 的第一个引号参数是窗口标题；空标题避免带空格的路径被当成标题。
    const std::wstring command =
        L"start \"\" \"" + path.wstring() + L"\" >nul 2>&1";
    return ::_wsystem(command.c_str()) == 0;
#elif defined(__APPLE__)
    const std::string command =
        "open -t " + ShellQuote(path.string()) + " >/dev/null 2>&1";
    return std::system(command.c_str()) == 0;
#else
    // Linux 优先 xdg-open；精简桌面可能只装了 gio。
    const std::string quoted = ShellQuote(path.string());
    if (std::system(("xdg-open " + quoted + " >/dev/null 2>&1").c_str()) ==
        0) {
        return true;
    }
    return std::system(("gio open " + quoted + " >/dev/null 2>&1").c_str()) ==
           0;
#endif
}

} // namespace llmswitch::ui
