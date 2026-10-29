export module llmswitch.shimeji;

import std;

namespace shimeji {

export enum class Activity { unknown, idle, working, waiting, completed, failed };

export struct Vec2 {
    float x = 0.0F;
    float y = 0.0F;
    bool operator==(const Vec2&) const = default;
};

export struct Pose {
    std::string image;
    Vec2 anchor;
    Vec2 velocity;
    int durationTicks = 1;
    bool operator==(const Pose&) const = default;
};

export struct Action {
    std::string name;
    std::string type;
    std::string borderType;
    bool loop = true;
    std::vector<Pose> poses;
    bool operator==(const Action&) const = default;
};

export struct BehaviorReference {
    std::string name;
    int frequency = 0;
    bool operator==(const BehaviorReference&) const = default;
};

export struct Behavior {
    std::string name;
    int frequency = 0;
    bool hidden = false;
    std::vector<BehaviorReference> next;
    bool operator==(const Behavior&) const = default;
};

export struct StateActions {
    std::string idle = "Stand";
    std::string working = "Walk";
    std::string waiting = "Sit";
    std::string completed = "Bouncing";
    std::string failed = "Tripping";
    std::string dragged = "Pinched";
    std::string falling = "Falling";
    bool operator==(const StateActions&) const = default;
};

export struct Pack {
    std::filesystem::path root;
    std::filesystem::path imageRoot;
    std::string id;
    std::string name;
    float width = 160.0F;
    float height = 160.0F;
    std::map<std::string, Action, std::less<>> actions;
    std::vector<Behavior> behaviors;
    StateActions stateActions;
    std::vector<std::string> warnings;
    std::string license;
    bool operator==(const Pack&) const = default;
};

export struct LoadLimits {
    std::size_t xmlBytes = 4 * 1024 * 1024;
    std::size_t profileBytes = 64 * 1024;
    std::size_t actions = 512;
    std::size_t poses = 8192;
    std::size_t behaviors = 512;
};

export std::vector<std::filesystem::path> ScanPacks(
    const std::filesystem::path& root);
export Pack LoadPack(const std::filesystem::path& root, LoadLimits limits = {});
export const Action* FindAction(const Pack& pack, std::string_view name) noexcept;
export const Action* ActionForActivity(const Pack& pack, Activity activity) noexcept;
export std::string NextIdleAction(const Pack& pack, std::string_view current,
                                  std::uint64_t randomValue);
export std::filesystem::path ImagePath(const Pack& pack, const Pose& pose);

} // namespace shimeji
