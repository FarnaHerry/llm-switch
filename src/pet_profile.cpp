module llmswitch.pet_profile;

import std;
import nlohmann.json;

namespace pet_profile {
namespace {

namespace fs = std::filesystem;

std::string ReadProfile(const fs::path& path) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec || size > 64 * 1024) throw std::runtime_error("Live2D pet.json 缺失或超过大小限制");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("无法读取 Live2D pet.json");
    std::string content(static_cast<std::size_t>(size), '\0');
    if (size > 0) input.read(content.data(), static_cast<std::streamsize>(size));
    if (!input && size > 0) throw std::runtime_error("读取 Live2D pet.json 失败");
    return content;
}

bool IsWithin(const fs::path& root, const fs::path& candidate) {
    auto rootIt = root.begin();
    auto candidateIt = candidate.begin();
    for (; rootIt != root.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == candidate.end() || *rootIt != *candidateIt) return false;
    }
    return true;
}

ParameterCue ParseParameter(const nlohmann::json& value) {
    if (!value.is_object()) throw std::runtime_error("Live2D 参数配置必须是对象");
    ParameterCue parameter{
        .id = value.value("id", ""),
        .value = value.value("value", 0.0F),
        .blend = value.value("blend", "override"),
        .weight = value.value("weight", 1.0F),
    };
    if (parameter.id.empty() || !std::isfinite(parameter.value) ||
        !std::isfinite(parameter.weight) || parameter.weight < 0.0F ||
        parameter.weight > 1.0F ||
        (parameter.blend != "override" && parameter.blend != "add")) {
        throw std::runtime_error("Live2D 参数配置无效");
    }
    return parameter;
}

MotionCue ParseCue(const nlohmann::json& value) {
    if (!value.is_object()) throw std::runtime_error("Live2D 动作配置必须是对象");
    MotionCue cue{
        .motion = value.value("motion", ""),
        .expression = value.value("expression", ""),
        .priority = value.value("priority", 1),
        .loop = value.value("loop", false),
    };
    if (cue.priority <= 0) throw std::runtime_error("Live2D 动作优先级无效");
    if (const auto it = value.find("parameters"); it != value.end()) {
        if (!it->is_array() || it->size() > 32) {
            throw std::runtime_error("Live2D 参数列表无效或超过限制");
        }
        for (const auto& parameter : *it) cue.parameters.push_back(ParseParameter(parameter));
    }
    return cue;
}

fs::path FindEntry(const fs::path& root) {
    std::vector<fs::path> entries;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string filename = entry.path().filename().string();
        if (filename.ends_with(".model3.json")) entries.push_back(entry.path());
    }
    if (ec) throw std::runtime_error("扫描 Live2D 模型入口失败");
    if (entries.empty()) throw std::runtime_error("模型目录缺少 .model3.json");
    std::ranges::sort(entries);
    return entries.front();
}

} // namespace

Live2DProfile LoadLive2DProfile(const fs::path& root) {
    Live2DProfile profile{
        .displayName = root.filename().string(),
        .states = {
            {"idle", {.motion = "Idle", .loop = true}},
            {"completed", {.motion = "TapBody", .priority = 3}},
            {"failed", {.motion = "TapBody", .priority = 3}},
        },
    };
    const fs::path path = root / "pet.json";
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return profile;
    const nlohmann::json json = nlohmann::json::parse(ReadProfile(path), nullptr, true, true);
    if (!json.is_object()) throw std::runtime_error("Live2D pet.json 必须是对象");
    profile.displayName = json.value("display_name", profile.displayName);
    profile.license = json.value("license", "");
    if (const auto it = json.find("window"); it != json.end() && it->is_object()) {
        const float width = it->value("width", profile.width);
        const float height = it->value("height", profile.height);
        if (!std::isfinite(width) || !std::isfinite(height) ||
            width < 128.0F || height < 128.0F || width > 1024.0F || height > 1024.0F) {
            throw std::runtime_error("Live2D 窗口尺寸无效");
        }
        profile.width = width;
        profile.height = height;
    }
    if (const auto it = json.find("states"); it != json.end()) {
        if (!it->is_object()) throw std::runtime_error("Live2D states 必须是对象");
        for (auto state = it->begin(); state != it->end(); ++state) {
            profile.states[state.key()] = ParseCue(state.value());
        }
    }
    if (const auto it = json.find("hit_areas"); it != json.end()) {
        if (!it->is_object()) throw std::runtime_error("Live2D hit_areas 必须是对象");
        for (auto area = it->begin(); area != it->end(); ++area) {
            profile.hitAreas[area.key()] = ParseCue(area.value());
        }
    }
    return profile;
}

std::vector<Live2DModel> ScanLive2DModels(const fs::path& root) {
    std::vector<Live2DModel> models;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return models;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        const fs::path modelRoot = entry.path();
        bool hasModel = false;
        for (const auto& candidate : fs::directory_iterator(modelRoot, ec)) {
            if (candidate.is_regular_file() &&
                candidate.path().filename().string().ends_with(".model3.json")) {
                hasModel = true;
                break;
            }
        }
        if (!hasModel) continue;
        const fs::path modelEntry = FindEntry(modelRoot);
        models.push_back({
            .id = modelRoot.filename().string(),
            .root = modelRoot,
            .entry = modelEntry,
            .profile = LoadLive2DProfile(modelRoot),
        });
    }
    if (ec) throw std::runtime_error("扫描 Live2D 模型目录失败");
    std::ranges::sort(models, {}, &Live2DModel::id);
    return models;
}

Live2DModel ResolveLive2DModel(const fs::path& root, std::string selected) {
    if (selected.empty()) {
        const auto models = ScanLive2DModels(root);
        if (models.empty()) throw std::runtime_error("未安装 Live2D 模型");
        return models.front();
    }
    const fs::path relative = fs::path(selected).lexically_normal();
    if (relative.is_absolute() || relative.empty() || *relative.begin() == "..") {
        throw std::runtime_error("Live2D 模型选择越出数据目录");
    }
    std::error_code ec;
    const fs::path canonicalRoot = fs::weakly_canonical(root, ec);
    const fs::path modelRoot = fs::weakly_canonical(root / relative, ec);
    if (ec || !IsWithin(canonicalRoot, modelRoot)) {
        throw std::runtime_error("Live2D 模型选择无效");
    }
    return {
        .id = modelRoot.filename().string(),
        .root = modelRoot,
        .entry = FindEntry(modelRoot),
        .profile = LoadLive2DProfile(modelRoot),
    };
}

const MotionCue* FindMotionCue(const Live2DProfile& profile,
                               std::string_view state) noexcept {
    const auto it = profile.states.find(state);
    return it == profile.states.end() ? nullptr : &it->second;
}

} // namespace pet_profile
