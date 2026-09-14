#include <cstdio>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>

#include "test_env.h"
#include "single_instance.h"

import std;

namespace {

int g_failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::println(stderr, "FAIL {}: {}", __LINE__, #condition);         \
            ++g_failures;                                                       \
        }                                                                       \
    } while (false)

int RunChild() {
    const bool owns_instance = llmswitch::single_instance::StartOrActivate();
    if (owns_instance) llmswitch::single_instance::Stop();
    return owns_instance ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--child") return RunChild();

    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
                          ("llmswitch-single-instance-" + std::to_string(static_cast<unsigned long long>(::getpid())));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    CHECK(!ec);
    testenv::setenv("XDG_RUNTIME_DIR", root);

    CHECK(llmswitch::single_instance::StartOrActivate());

    std::mutex mutex;
    std::condition_variable condition;
    bool activated = false;
    llmswitch::single_instance::SetActivationHandler([&] {
        {
            std::lock_guard lock(mutex);
            activated = true;
        }
        condition.notify_one();
    });

    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        ::execl("/proc/self/exe", "/proc/self/exe", "--child", static_cast<char*>(nullptr));
        _exit(127);
    }

    if (child > 0) {
        int status = 0;
        CHECK(::waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    {
        std::unique_lock lock(mutex);
        CHECK(condition.wait_for(lock, std::chrono::seconds(2), [&] { return activated; }));
    }

    llmswitch::single_instance::ClearActivationHandler();
    llmswitch::single_instance::Stop();
    CHECK(llmswitch::single_instance::StartOrActivate());
    llmswitch::single_instance::Stop();

    fs::remove_all(root, ec);
    return g_failures;
}
