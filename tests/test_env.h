// test_env.h — 测试用环境/进程可移植封装（POSIX setenv/getpid ↔ MSVC _putenv_s/_getpid）。
#pragma once

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
#include <cstdlib>

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

// MSVC 无 unsetenv；置空串即可——cfg 各读取处都以空串视同未设置。
inline void unsetenv(const char* name) {
#ifdef _WIN32
    ::_putenv_s(name, "");
#else
    ::unsetenv(name);
#endif
}

} // namespace testenv
