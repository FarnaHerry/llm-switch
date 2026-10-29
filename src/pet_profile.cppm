export module llmswitch.pet_profile;

import std;

namespace pet_profile {

export struct ParameterCue {
    std::string id;
    float value = 0.0F;
    std::string blend = "override";
    float weight = 1.0F;
    bool operator==(const ParameterCue&) const = default;
};

export struct MotionCue {
    std::string motion;
    std::string expression;
    int priority = 1;
    bool loop = false;
    std::vector<ParameterCue> parameters;
    bool operator==(const MotionCue&) const = default;
};

export struct Live2DProfile {
    std::string displayName;
    std::string license;
    float width = 300.0F;
    float height = 420.0F;
    std::map<std::string, MotionCue, std::less<>> states;
    std::map<std::string, MotionCue, std::less<>> hitAreas;
    bool operator==(const Live2DProfile&) const = default;
};

export struct Live2DModel {
    std::string id;
    std::filesystem::path root;
    std::filesystem::path entry;
    Live2DProfile profile;
    bool operator==(const Live2DModel&) const = default;
};

export std::vector<Live2DModel> ScanLive2DModels(const std::filesystem::path& root);
export Live2DModel ResolveLive2DModel(const std::filesystem::path& root,
                                      std::string selected);
export Live2DProfile LoadLive2DProfile(const std::filesystem::path& root);
export const MotionCue* FindMotionCue(const Live2DProfile& profile,
                                      std::string_view state) noexcept;

} // namespace pet_profile
