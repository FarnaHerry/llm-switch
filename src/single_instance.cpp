#include "single_instance.h"

#if defined(__linux__)

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace llmswitch::single_instance {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kSocketName = "llm-switch.sock";
constexpr std::string_view kLockName = "llm-switch.lock";

// POSIX 文件描述符的 RAII 所有权：析构即 close，杜绝每条错误分支手写 ::close
// 漏掉一条就泄漏 fd。禁止拷贝、允许移动（-1 表示空）。
class UniqueFd final {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() { Reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            Reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    [[nodiscard]] int Get() const noexcept { return fd_; }
    [[nodiscard]] bool Valid() const noexcept { return fd_ >= 0; }

    // 接管新 fd（先关旧的）；传入 -1 等价于只关闭。
    void Reset(int fd = -1) noexcept {
        if (fd_ >= 0) ::close(fd_);
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

bool IsOwnedPrivateDirectory(const fs::path& directory) noexcept {
    struct stat info {};
    if (::stat(directory.c_str(), &info) != 0) return false;
    return S_ISDIR(info.st_mode) && info.st_uid == ::getuid() && (info.st_mode & 0077) == 0;
}

std::optional<fs::path> RuntimeDirectory() noexcept {
    fs::path parent;
    if (const char* xdg_runtime = std::getenv("XDG_RUNTIME_DIR"); xdg_runtime && *xdg_runtime) {
        std::error_code ec;
        const fs::path candidate(xdg_runtime);
        if (fs::is_directory(candidate, ec) && ::access(candidate.c_str(), W_OK | X_OK) == 0) {
            parent = candidate;
        }
    }

    if (parent.empty()) {
        std::error_code ec;
        parent = fs::temp_directory_path(ec);
        if (ec || parent.empty()) return std::nullopt;
        parent /= "llm-switch-" + std::to_string(static_cast<unsigned long long>(::getuid()));
        if (!fs::exists(parent, ec) && !fs::create_directory(parent, ec)) return std::nullopt;
        if (ec) return std::nullopt;
    } else {
        parent /= "llm-switch";
        std::error_code ec;
        if (!fs::exists(parent, ec) && !fs::create_directory(parent, ec)) return std::nullopt;
        if (ec) return std::nullopt;
    }

    if (::chmod(parent.c_str(), 0700) != 0 || !IsOwnedPrivateDirectory(parent)) {
        return std::nullopt;
    }
    return parent;
}

bool IsSocketPath(const fs::path& path) noexcept {
    struct stat info {};
    if (::lstat(path.c_str(), &info) != 0) return errno == ENOENT;
    return S_ISSOCK(info.st_mode);
}

bool RemoveSocketIfPresent(const fs::path& path) noexcept {
    if (!IsSocketPath(path)) return false;
    if (::unlink(path.c_str()) == 0 || errno == ENOENT) return true;
    return false;
}

bool FillAddress(const fs::path& path, sockaddr_un& address) noexcept {
    const std::string value = path.string();
    if (value.size() >= sizeof(address.sun_path)) return false;
    std::memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, value.c_str(), value.size() + 1);
    return true;
}

class Coordinator final {
public:
    ~Coordinator() { Stop(); }

    bool StartOrActivate() {
        if (server_fd_.Valid()) return true;

        const std::optional<fs::path> directory = RuntimeDirectory();
        if (!directory) return true;
        socket_path_ = *directory / std::string(kSocketName);
        const fs::path lock_path = *directory / std::string(kLockName);

        lock_fd_.Reset(::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
        if (!lock_fd_.Valid()) return true;

        if (::flock(lock_fd_.Get(), LOCK_EX | LOCK_NB) != 0) {
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                CloseLock();
                return true;
            }
            for (int attempt = 0; attempt < 20; ++attempt) {
                if (ForwardActivation(socket_path_)) {
                    CloseLock();
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                if (::flock(lock_fd_.Get(), LOCK_EX | LOCK_NB) == 0) break;
                if (attempt == 19) {
                    CloseLock();
                    return false;
                }
            }
        }

        if (!CreateServer()) {
            Stop();
            return true;
        }
        return true;
    }

    void SetActivationHandler(std::function<void()> handler) {
        std::function<void()> pending_handler;
        {
            std::lock_guard lock(handler_mutex_);
            handler_ = std::move(handler);
            if (handler_ && pending_activation_) {
                pending_activation_ = false;
                pending_handler = handler_;
            }
        }
        InvokeHandler(std::move(pending_handler));
    }

    void ClearActivationHandler() noexcept {
        std::lock_guard lock(handler_mutex_);
        handler_ = {};
        pending_activation_ = false;
    }

    void Stop() noexcept {
        ClearActivationHandler();
        stopping_.store(true, std::memory_order_release);
        if (listener_.joinable()) listener_.join();
        ClearActivationHandler();
        server_fd_.Reset();
        if (!socket_path_.empty()) RemoveSocketIfPresent(socket_path_);
        CloseLock();
        socket_path_.clear();
        stopping_.store(false, std::memory_order_release);
    }

private:
    bool CreateServer() {
        if (!RemoveSocketIfPresent(socket_path_)) return false;

        sockaddr_un address {};
        if (!FillAddress(socket_path_, address)) return false;
        UniqueFd socket_fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
        if (!socket_fd.Valid()) return false;
        if (::bind(socket_fd.Get(), reinterpret_cast<const sockaddr*>(&address),
                   sizeof(address)) != 0 ||
            ::listen(socket_fd.Get(), 8) != 0) {
            RemoveSocketIfPresent(socket_path_);
            return false;
        }

        server_fd_ = std::move(socket_fd);
        stopping_.store(false, std::memory_order_release);
        try {
            listener_ = std::thread([this] { Listen(); });
        } catch (...) {
            server_fd_.Reset();
            RemoveSocketIfPresent(socket_path_);
            return false;
        }
        return true;
    }

    static bool ForwardActivation(const fs::path& path) noexcept {
        sockaddr_un address {};
        if (!FillAddress(path, address)) return false;
        UniqueFd socket_fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
        if (!socket_fd.Valid()) return false;
        const bool connected =
            ::connect(socket_fd.Get(), reinterpret_cast<const sockaddr*>(&address),
                      sizeof(address)) == 0;
        if (connected) {
            constexpr char message[] = "activate\n";
            (void)::send(socket_fd.Get(), message, sizeof(message) - 1, MSG_NOSIGNAL);
        }
        return connected;
    }

    void Listen() noexcept {
        while (!stopping_.load(std::memory_order_acquire)) {
            pollfd descriptor{server_fd_.Get(), POLLIN, 0};
            const int result = ::poll(&descriptor, 1, 250);
            if (result <= 0) continue;
            if ((descriptor.revents & POLLIN) == 0) continue;

            const UniqueFd client_fd(::accept(server_fd_.Get(), nullptr, nullptr));
            if (!client_fd.Valid()) continue;
            NotifyActivation();
        }
    }

    void NotifyActivation() noexcept {
        std::function<void()> handler;
        {
            std::lock_guard lock(handler_mutex_);
            if (handler_) {
                handler = handler_;
            } else {
                pending_activation_ = true;
            }
        }
        InvokeHandler(std::move(handler));
    }

    static void InvokeHandler(std::function<void()> handler) noexcept {
        if (!handler) return;
        try {
            handler();
        } catch (...) {
            // The owning HuxerUI scope may be closing while the listener is delivering an activation.
        }
    }

    void CloseLock() noexcept {
        if (lock_fd_.Valid()) (void)::flock(lock_fd_.Get(), LOCK_UN);
        lock_fd_.Reset();
    }

    UniqueFd lock_fd_;
    UniqueFd server_fd_;
    fs::path socket_path_;
    std::atomic<bool> stopping_{false};
    std::thread listener_;
    std::mutex handler_mutex_;
    std::function<void()> handler_;
    bool pending_activation_ = false;
};

Coordinator& Instance() {
    static Coordinator instance;
    return instance;
}

} // namespace

bool StartOrActivate() {
    return Instance().StartOrActivate();
}

void SetActivationHandler(std::function<void()> handler) {
    Instance().SetActivationHandler(std::move(handler));
}

void ClearActivationHandler() noexcept {
    Instance().ClearActivationHandler();
}

void Stop() noexcept {
    Instance().Stop();
}

} // namespace llmswitch::single_instance

#else

namespace llmswitch::single_instance {

bool StartOrActivate() {
    return true;
}

void SetActivationHandler(std::function<void()>) {}
void ClearActivationHandler() noexcept {}
void Stop() noexcept {}

} // namespace llmswitch::single_instance

#endif
