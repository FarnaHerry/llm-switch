// activity_bridge.cpp — authenticated bounded loopback transport; no event logs.
module;
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#endif

module llmswitch.agents.bridge;

import std;
import nlohmann.json;
import llmswitch.agents;

namespace agents {
namespace {

constexpr std::size_t kMaxFrameBytes = 4096;
constexpr std::size_t kMaxDescriptorBytes = 4096;
constexpr auto kSelectInterval = std::chrono::milliseconds(250);

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
void CloseSocket(Socket socket) noexcept {
    if (socket != kInvalidSocket) ::closesocket(socket);
}
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
void CloseSocket(Socket socket) noexcept {
    if (socket != kInvalidSocket) ::close(socket);
}
#endif

std::string HexToken() {
    std::random_device random;
    constexpr char alphabet[] = "0123456789abcdef";
    std::string result(64, '0');
    for (char& value : result) value = alphabet[random() & 0x0fU];
    return result;
}

void WritePrivateDescriptor(const std::filesystem::path& path,
                            std::uint16_t port, std::string_view token) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) throw std::runtime_error("无法创建 Agent bridge 目录");
#ifndef _WIN32
    ::chmod(path.parent_path().c_str(), S_IRWXU);
#endif
    const auto temp = std::filesystem::path(path.string() + ".tmp");
    const nlohmann::json data{{"version", 1}, {"port", port}, {"token", token}};
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("无法创建 Agent bridge 描述符");
        out << data.dump() << '\n';
        out.flush();
        if (!out) throw std::runtime_error("写入 Agent bridge 描述符失败");
    }
    std::filesystem::permissions(temp, std::filesystem::perms::owner_read |
        std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, ec);
    if (ec) throw std::runtime_error("无法限制 Agent bridge 描述符权限");
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        ec.clear();
        std::filesystem::rename(temp, path, ec);
    }
    if (ec) throw std::runtime_error("替换 Agent bridge 描述符失败");
}

struct BridgeDescriptor {
    std::uint16_t port = 0;
    std::string token;
};

std::optional<BridgeDescriptor> ReadDescriptor(
    const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return std::nullopt;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > kMaxDescriptorBytes) return std::nullopt;
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::string contents(static_cast<std::size_t>(size), '\0');
    if (size) in.read(contents.data(), static_cast<std::streamsize>(size));
    if (!in && !in.eof()) return std::nullopt;
    try {
        const auto root = nlohmann::json::parse(contents, nullptr, false);
        if (!root.is_object() || !root.contains("version") ||
            !root["version"].is_number_integer() || root["version"] != 1 ||
            !root.contains("port") || !root["port"].is_number_integer() ||
            !root.contains("token") || !root["token"].is_string()) {
            return std::nullopt;
        }
        const int port = root["port"].get<int>();
        std::string token = root["token"].get<std::string>();
        if (port <= 0 || port > 65535 || token.size() != 64) return std::nullopt;
        return BridgeDescriptor{static_cast<std::uint16_t>(port), std::move(token)};
    } catch (...) {
        return std::nullopt;
    }
}

std::string StateName(ActivityState state) {
    switch (state) {
        case ActivityState::unknown: return "unknown";
        case ActivityState::idle: return "idle";
        case ActivityState::working: return "working";
        case ActivityState::waiting: return "waiting";
        case ActivityState::completed: return "completed";
        case ActivityState::failed: return "failed";
        case ActivityState::cancelled: return "cancelled";
        case ActivityState::sessionEnded: return "session-ended";
    }
    return "unknown";
}

std::optional<ActivityState> ParseState(std::string_view state) {
    if (state == "unknown") return ActivityState::unknown;
    if (state == "idle") return ActivityState::idle;
    if (state == "working") return ActivityState::working;
    if (state == "waiting") return ActivityState::waiting;
    if (state == "completed") return ActivityState::completed;
    if (state == "failed") return ActivityState::failed;
    if (state == "cancelled") return ActivityState::cancelled;
    if (state == "session-ended") return ActivityState::sessionEnded;
    return std::nullopt;
}

bool SendAll(Socket socket, std::string_view data) noexcept {
    std::size_t sent = 0;
    while (sent < data.size()) {
#ifdef _WIN32
        const int count = ::send(socket, data.data() + sent,
                                 static_cast<int>(data.size() - sent), 0);
#else
        const ssize_t count = ::send(socket, data.data() + sent,
                                     data.size() - sent, MSG_NOSIGNAL);
#endif
        if (count <= 0) return false;
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

} // namespace

struct ActivityBridge::Impl {
    mutable std::mutex mutex;
    ActivityTracker tracker;
    std::filesystem::path descriptorFile;
    std::string token;
    Socket listener = kInvalidSocket;
    std::thread worker;
    std::atomic<bool> stopping = false;
    std::atomic<bool> running = false;
#ifdef _WIN32
    bool winsockStarted = false;
#endif

    void Handle(Socket client) noexcept {
#ifdef _WIN32
        DWORD timeout = 750;
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        timeval timeout{0, 750000};
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
        std::string frame;
        frame.reserve(512);
        char buffer[512];
        while (frame.size() < kMaxFrameBytes) {
#ifdef _WIN32
            const int count = ::recv(client, buffer,
                                     static_cast<int>(sizeof(buffer)), 0);
#else
            const ssize_t count = ::recv(client, buffer, sizeof(buffer), 0);
#endif
            if (count <= 0) break;
            const auto newline = std::find(buffer, buffer + count, '\n');
            frame.append(buffer, static_cast<std::size_t>(newline - buffer));
            if (newline != buffer + count || frame.size() >= kMaxFrameBytes) break;
        }

        bool accepted = false;
        try {
            const auto root = nlohmann::json::parse(frame, nullptr, false);
            if (root.is_object() && root.contains("version") &&
                root["version"].is_number_integer() && root["version"] == 1 &&
                root.contains("token") && root["token"].is_string() &&
                root["token"].get_ref<const std::string&>() == token &&
                root.contains("tool") && root["tool"].is_string() &&
                root.contains("session") && root["session"].is_string() &&
                root.contains("state") && root["state"].is_string() &&
                root.contains("ts") && root["ts"].is_number_integer()) {
                const std::string tool = root["tool"].get<std::string>();
                const std::string session = root["session"].get<std::string>();
                const auto state = ParseState(root["state"].get<std::string>());
                const std::int64_t timestamp = root["ts"].get<std::int64_t>();
                if (state && !tool.empty() && tool.size() <= 64 && !session.empty() &&
                    session.size() <= 256 && timestamp >= 0) {
                    std::lock_guard lock(mutex);
                    accepted = tracker.Apply(ActivityEvent{
                        tool, session, *state, timestamp});
                }
            }
        } catch (...) {
            accepted = false;
        }
        const char ack = accepted ? '1' : '0';
        (void)SendAll(client, std::string_view(&ack, 1));
        CloseSocket(client);
    }

    void Serve() noexcept {
        while (!stopping.load()) {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(listener, &readSet);
            timeval timeout{
                static_cast<long>(kSelectInterval.count() / 1000),
                static_cast<long>((kSelectInterval.count() % 1000) * 1000)};
#ifdef _WIN32
            const int ready = ::select(0, &readSet, nullptr, nullptr, &timeout);
#else
            const int ready = ::select(listener + 1, &readSet, nullptr, nullptr, &timeout);
#endif
            if (ready <= 0 || stopping.load()) continue;
            sockaddr_in peer{};
#ifdef _WIN32
            int peerSize = sizeof(peer);
#else
            socklen_t peerSize = sizeof(peer);
#endif
            const Socket client = ::accept(listener,
                reinterpret_cast<sockaddr*>(&peer), &peerSize);
            if (client == kInvalidSocket) continue;
            if (peer.sin_family != AF_INET || ntohl(peer.sin_addr.s_addr) != INADDR_LOOPBACK) {
                CloseSocket(client);
                continue;
            }
            Handle(client);
        }
        running = false;
    }
};

ActivityBridge::ActivityBridge() : impl_(std::make_unique<Impl>()) {}

ActivityBridge::~ActivityBridge() { Stop(); }

bool ActivityBridge::Start(const std::filesystem::path& descriptorFile) {
    Stop();
#ifdef _WIN32
    WSADATA data{};
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
    impl_->winsockStarted = true;
#endif
    impl_->listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl_->listener == kInvalidSocket) {
#ifdef _WIN32
        ::WSACleanup();
        impl_->winsockStarted = false;
#endif
        return false;
    }
    int reuse = 1;
    (void)::setsockopt(impl_->listener, SOL_SOCKET, SO_REUSEADDR,
#ifdef _WIN32
                       reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
                       &reuse, sizeof(reuse));
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(impl_->listener, reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) != 0 || ::listen(impl_->listener, 16) != 0) {
        Stop();
        return false;
    }
#ifdef _WIN32
    int addressSize = sizeof(address);
#else
    socklen_t addressSize = sizeof(address);
#endif
    if (::getsockname(impl_->listener, reinterpret_cast<sockaddr*>(&address),
                      &addressSize) != 0) {
        Stop();
        return false;
    }
    impl_->descriptorFile = descriptorFile;
    impl_->token = HexToken();
    try {
        WritePrivateDescriptor(descriptorFile, ntohs(address.sin_port), impl_->token);
    } catch (...) {
        Stop();
        return false;
    }
    impl_->stopping = false;
    impl_->running = true;
    try {
        impl_->worker = std::thread([impl = impl_.get()] { impl->Serve(); });
    } catch (...) {
        impl_->running = false;
        Stop();
        return false;
    }
    return true;
}

void ActivityBridge::Stop() noexcept {
    if (!impl_) return;
    impl_->stopping = true;
    // The listener is only closed after Serve has left select/accept, avoiding
    // a descriptor data race. Its select loop wakes at most every 250 ms.
    if (impl_->worker.joinable()) impl_->worker.join();
    if (impl_->listener != kInvalidSocket) {
        CloseSocket(impl_->listener);
        impl_->listener = kInvalidSocket;
    }
    std::error_code ec;
    if (!impl_->descriptorFile.empty()) {
        const auto descriptor = ReadDescriptor(impl_->descriptorFile);
        if (descriptor && descriptor->token == impl_->token) {
            std::filesystem::remove(impl_->descriptorFile, ec);
        }
        impl_->descriptorFile.clear();
    }
    impl_->token.clear();
#ifdef _WIN32
    if (impl_->winsockStarted) {
        ::WSACleanup();
        impl_->winsockStarted = false;
    }
#endif
}

bool ActivityBridge::Running() const noexcept {
    return impl_ && impl_->running.load();
}

void ActivityBridge::SetSourceHealth(std::string tool, SourceHealth health) {
    std::lock_guard lock(impl_->mutex);
    impl_->tracker.SetSourceHealth(std::move(tool), health);
}

ActivitySnapshot ActivityBridge::SnapshotAt(std::int64_t nowMillis) const {
    std::lock_guard lock(impl_->mutex);
    return impl_->tracker.SnapshotAt(nowMillis);
}

bool SendToBridge(const std::filesystem::path& descriptorFile,
                  const ActivityEvent& event) noexcept {
#ifdef _WIN32
    WSADATA data{};
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
#endif
    const auto descriptor = ReadDescriptor(descriptorFile);
    if (!descriptor) {
#ifdef _WIN32
        ::WSACleanup();
#endif
        return false;
    }
    try {
        const nlohmann::json frame{
            {"version", 1}, {"token", descriptor->token},
            {"tool", event.tool}, {"session", event.sessionKey},
            {"state", StateName(event.state)}, {"ts", event.occurredAtMillis}};
        const std::string payload = frame.dump() + '\n';
        Socket client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (client == kInvalidSocket) {
#ifdef _WIN32
            ::WSACleanup();
#endif
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(descriptor->port);
        const bool connected = ::connect(client,
            reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
        bool accepted = false;
        if (connected && SendAll(client, payload)) {
#ifdef _WIN32
            char ack = '0';
            accepted = ::recv(client, &ack, 1, 0) == 1 && ack == '1';
#else
            char ack = '0';
            accepted = ::recv(client, &ack, 1, 0) == 1 && ack == '1';
#endif
        }
        CloseSocket(client);
#ifdef _WIN32
        ::WSACleanup();
#endif
        return accepted;
    } catch (...) {
#ifdef _WIN32
        ::WSACleanup();
#endif
        return false;
    }
}

} // namespace agents
