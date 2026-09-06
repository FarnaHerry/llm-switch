// test_env.h — 测试用环境/进程可移植封装（POSIX setenv/getpid ↔ MSVC _putenv_s/_getpid）。
#pragma once

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
#include <cstdlib>
#include <filesystem>
#include <string>

namespace testenv {

inline int getpid() {
#ifdef _WIN32
    return ::_getpid();
#else
    return ::getpid();
#endif
}

inline void setenv(const char* name, const char* value) {
#ifdef _WIN32
    ::_putenv_s(name, value);
#else
    ::setenv(name, value, 1);
#endif
}

// 路径直传版：Windows 上 path::c_str() 是 wchar_t*，统一以 UTF-8 窄字符串写入
// （CI 临时目录为 ASCII；cfg 侧按窄字符串读回）。
inline void setenv(const char* name, const std::filesystem::path& value) {
#ifdef _WIN32
    const std::u8string u8 = value.u8string();
    const std::string narrow(u8.begin(), u8.end());
    setenv(name, narrow.c_str());
#else
    setenv(name, value.c_str());
#endif
}

// MSVC 无 unsetenv；置空串即可——cfg 各读取处都以空串视同未设置。
inline void unsetenv(const char* name) {
#ifdef _WIN32
    ::_putenv_s(name, "");
#else
    ::unsetenv(name);
#endif
}

} // namespace testenv
