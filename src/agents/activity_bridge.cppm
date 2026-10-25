// activity_bridge.cppm — app-lifetime loopback receiver for sanitized hook events.
export module llmswitch.agents.bridge;

import std;
import llmswitch.agents;

namespace agents {

export class ActivityBridge final {
public:
    ActivityBridge();
    ~ActivityBridge();
    ActivityBridge(const ActivityBridge&) = delete;
    ActivityBridge& operator=(const ActivityBridge&) = delete;

    // Binds only 127.0.0.1, publishes a private short-lived descriptor, then
    // receives events on a background thread. Returns false if unavailable.
    bool Start(const std::filesystem::path& descriptorFile);
    void Stop() noexcept;
    [[nodiscard]] bool Running() const noexcept;
    void SetSourceHealth(std::string tool, SourceHealth health);
    [[nodiscard]] ActivitySnapshot SnapshotAt(std::int64_t nowMillis) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Called by the sibling helper executable. It reads the current descriptor,
// connects to loopback, and sends only fields already reduced by an adapter.
export bool SendToBridge(const std::filesystem::path& descriptorFile,
                         const ActivityEvent& event) noexcept;

} // namespace agents
