module;
#include <pugixml.hpp>

module llmswitch.shimeji;

import std;
import nlohmann.json;

namespace shimeji {
namespace {

namespace fs = std::filesystem;

std::string ReadFile(const fs::path& path, std::size_t limit) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec || size > limit) throw std::runtime_error("Shimeji 配置文件缺失或超过大小限制");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("无法读取 Shimeji 配置文件");
    std::string content(static_cast<std::size_t>(size), '\0');
    if (size > 0) input.read(content.data(), static_cast<std::streamsize>(size));
    if (!input && size > 0) throw std::runtime_error("读取 Shimeji 配置文件失败");
    return content;
}

pugi::xml_document ParseXml(const fs::path& path, std::size_t limit) {
    const std::string content = ReadFile(path, limit);
    pugi::xml_document document;
    const pugi::xml_parse_result result = document.load_buffer(
        content.data(), content.size(), pugi::parse_default, pugi::encoding_utf8);
    if (!result) throw std::runtime_error("Shimeji XML 解析失败：" + path.string());
    return document;
}

float ParseFloat(std::string_view value) {
    float result = 0.0F;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        !std::isfinite(result)) {
        throw std::runtime_error("Shimeji 动作包含无效数值");
    }
    return result;
}

Vec2 ParseVec2(std::string_view value) {
    const std::size_t comma = value.find(',');
    if (comma == std::string_view::npos) throw std::runtime_error("Shimeji 动作坐标格式无效");
    return {ParseFloat(value.substr(0, comma)), ParseFloat(value.substr(comma + 1))};
}

bool IsWithin(const fs::path& root, const fs::path& candidate) {
    auto rootIt = root.begin();
    auto candidateIt = candidate.begin();
    for (; rootIt != root.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == candidate.end() || *rootIt != *candidateIt) return false;
    }
    return true;
}

fs::path SafeImagePath(const Pack& pack, std::string_view image) {
    while (!image.empty() && image.front() == '/') image.remove_prefix(1);
    if (image.empty() || image.find('\0') != std::string_view::npos ||
        image.find('\\') != std::string_view::npos || image.find(':') != std::string_view::npos) {
        throw std::runtime_error("Shimeji 图片路径无效");
    }
    const fs::path relative = fs::path(std::string(image)).lexically_normal();
    if (relative.is_absolute() || relative.empty() || *relative.begin() == "..") {
        throw std::runtime_error("Shimeji 图片路径越出模型包");
    }
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(pack.root, ec);
    if (ec) throw std::runtime_error("Shimeji 模型包路径无效");
    const fs::path candidate = fs::weakly_canonical(pack.imageRoot / relative, ec);
    if (ec || !IsWithin(root, candidate) || !fs::is_regular_file(candidate, ec)) {
        throw std::runtime_error("Shimeji 图片不存在或越出模型包：" + relative.string());
    }
    return candidate;
}

void AddWarning(Pack& pack, std::string warning) {
    if (pack.warnings.size() < 16) pack.warnings.push_back(std::move(warning));
}

Vec2 ParseAnchor(const pugi::xml_node pose) {
    const std::string_view value = pose.attribute("ImageAnchor").value();
    return value.empty() ? Vec2{} : ParseVec2(value);
}

Vec2 ParseVelocity(const pugi::xml_node pose) {
    const std::string_view value = pose.attribute("Velocity").value();
    return value.empty() ? Vec2{} : ParseVec2(value);
}

void ParseAction(const pugi::xml_node node, Pack& pack, LoadLimits& limits,
                 std::size_t& poseCount) {
    const pugi::xml_node animation = node.child("Animation");
    if (!animation) return;
    const std::string name = node.attribute("Name").value();
    if (name.empty() || pack.actions.contains(name)) return;
    if (pack.actions.size() >= limits.actions) throw std::runtime_error("Shimeji 动作数量超过限制");
    if (node.attribute("Condition")) {
        AddWarning(pack, "已跳过带条件的动作：" + name);
    }
    Action action{
        .name = name,
        .type = node.attribute("Type").value(),
        .borderType = node.attribute("BorderType").value(),
        .loop = node.attribute("Loop").as_bool(
            !std::string_view(node.attribute("Type").value()).starts_with("Animate")),
    };
    for (const pugi::xml_node poseNode : animation.children("Pose")) {
        if (++poseCount > limits.poses) throw std::runtime_error("Shimeji 帧数量超过限制");
        const std::string_view image = poseNode.attribute("Image").value();
        Pose pose{
            .image = std::string(image),
            .anchor = ParseAnchor(poseNode),
            .velocity = ParseVelocity(poseNode),
            .durationTicks = std::max(1, poseNode.attribute("Duration").as_int(1)),
        };
        static_cast<void>(SafeImagePath(pack, image));
        action.poses.push_back(std::move(pose));
    }
    if (action.poses.empty()) return;
    pack.actions.emplace(name, std::move(action));
}

void ParseActionList(const pugi::xml_node node, Pack& pack, LoadLimits& limits,
                     std::size_t& poseCount) {
    for (const pugi::xml_node child : node.children()) {
        if (child.name() == std::string_view("Action")) {
            ParseAction(child, pack, limits, poseCount);
        } else if (child.name() == std::string_view("ActionList")) {
            ParseActionList(child, pack, limits, poseCount);
        }
    }
}

void ParseBehavior(const pugi::xml_node node, Pack& pack, LoadLimits limits) {
    Behavior behavior{
        .name = node.attribute("Name").value(),
        .frequency = std::max(0, node.attribute("Frequency").as_int(0)),
        .hidden = node.attribute("Hidden").as_bool(false),
    };
    if (behavior.name.empty()) return;
    for (const pugi::xml_node next : node.child("NextBehavior").children("BehaviorReference")) {
        behavior.next.push_back({
            .name = next.attribute("Name").value(),
            .frequency = std::max(0, next.attribute("Frequency").as_int(0)),
        });
    }
    pack.behaviors.push_back(std::move(behavior));
    if (pack.behaviors.size() > limits.behaviors) {
        throw std::runtime_error("Shimeji 行为数量超过限制");
    }
}

void ParseBehaviorNodes(const pugi::xml_node node, Pack& pack, LoadLimits limits) {
    for (const pugi::xml_node child : node.children()) {
        if (child.name() == std::string_view("Behavior")) {
            ParseBehavior(child, pack, limits);
        } else {
            ParseBehaviorNodes(child, pack, limits);
        }
    }
}

void ReadStateAction(const nlohmann::json& actions, std::string_view key,
                     std::string& target) {
    const auto it = actions.find(std::string(key));
    if (it != actions.end() && it->is_string() && !it->get<std::string>().empty()) {
        target = it->get<std::string>();
    }
}

void LoadProfile(Pack& pack, const LoadLimits& limits) {
    const fs::path profilePath = pack.root / "pet.json";
    std::error_code ec;
    if (!fs::is_regular_file(profilePath, ec)) return;
    const nlohmann::json profile = nlohmann::json::parse(
        ReadFile(profilePath, limits.profileBytes), nullptr, true, true);
    if (!profile.is_object()) throw std::runtime_error("Shimeji pet.json 必须是对象");
    pack.name = profile.value("display_name", pack.name);
    pack.license = profile.value("license", "");
    if (const auto it = profile.find("window"); it != profile.end() && it->is_object()) {
        const float width = it->value("width", pack.width);
        const float height = it->value("height", pack.height);
        if (!std::isfinite(width) || !std::isfinite(height) ||
            width < 64.0F || height < 64.0F || width > 512.0F || height > 512.0F) {
            throw std::runtime_error("Shimeji 窗口尺寸无效");
        }
        pack.width = width;
        pack.height = height;
    }
    if (const auto it = profile.find("actions"); it != profile.end() && it->is_object()) {
        ReadStateAction(*it, "idle", pack.stateActions.idle);
        ReadStateAction(*it, "working", pack.stateActions.working);
        ReadStateAction(*it, "waiting", pack.stateActions.waiting);
        ReadStateAction(*it, "completed", pack.stateActions.completed);
        ReadStateAction(*it, "failed", pack.stateActions.failed);
        ReadStateAction(*it, "dragged", pack.stateActions.dragged);
        ReadStateAction(*it, "falling", pack.stateActions.falling);
    }
}

fs::path FindImageRoot(const fs::path& root) {
    const fs::path image = root / "img";
    std::error_code ec;
    if (!fs::is_directory(image, ec)) throw std::runtime_error("Shimeji 模型包缺少 img 目录");
    std::vector<fs::path> directories;
    for (const auto& entry : fs::directory_iterator(image, ec)) {
        if (entry.is_directory()) directories.push_back(entry.path());
    }
    if (ec) throw std::runtime_error("无法扫描 Shimeji 图片目录");
    if (directories.empty()) return image;
    std::ranges::sort(directories);
    return directories.front();
}

const Action* FallbackAction(const Pack& pack, std::string_view preferred) noexcept {
    if (const Action* action = FindAction(pack, preferred)) return action;
    if (const Action* action = FindAction(pack, "Stand")) return action;
    if (const Action* action = FindAction(pack, "Sit")) return action;
    return pack.actions.empty() ? nullptr : &pack.actions.begin()->second;
}

} // namespace

std::vector<fs::path> ScanPacks(const fs::path& root) {
    std::vector<fs::path> result;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return result;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        if (fs::is_regular_file(entry.path() / "conf" / "actions.xml", ec)) {
            result.push_back(entry.path());
        }
    }
    if (ec) throw std::runtime_error("扫描 Shimeji 模型目录失败");
    std::ranges::sort(result);
    return result;
}

Pack LoadPack(const fs::path& root, LoadLimits limits) {
    std::error_code ec;
    Pack pack{
        .root = fs::canonical(root, ec),
        .id = root.filename().string(),
    };
    if (ec || !fs::is_directory(pack.root, ec)) {
        throw std::runtime_error("Shimeji 模型包目录无效");
    }
    pack.name = pack.id;
    pack.imageRoot = FindImageRoot(pack.root);
    pugi::xml_document actions = ParseXml(pack.root / "conf" / "actions.xml", limits.xmlBytes);
    std::size_t poseCount = 0;
    ParseActionList(actions.child("Mascot"), pack, limits, poseCount);
    if (pack.actions.empty()) throw std::runtime_error("Shimeji 模型包没有可支持的动作");
    const fs::path behaviorsPath = pack.root / "conf" / "behaviors.xml";
    if (fs::is_regular_file(behaviorsPath, ec)) {
        pugi::xml_document behaviors = ParseXml(behaviorsPath, limits.xmlBytes);
        ParseBehaviorNodes(behaviors.child("Mascot"), pack, limits);
    } else {
        AddWarning(pack, "缺少 behaviors.xml，使用内置行为调度");
    }
    LoadProfile(pack, limits);
    return pack;
}

const Action* FindAction(const Pack& pack, std::string_view name) noexcept {
    const auto it = pack.actions.find(name);
    return it == pack.actions.end() ? nullptr : &it->second;
}

const Action* ActionForActivity(const Pack& pack, Activity activity) noexcept {
    switch (activity) {
        case Activity::working: return FallbackAction(pack, pack.stateActions.working);
        case Activity::waiting: return FallbackAction(pack, pack.stateActions.waiting);
        case Activity::completed: return FallbackAction(pack, pack.stateActions.completed);
        case Activity::failed: return FallbackAction(pack, pack.stateActions.failed);
        case Activity::unknown:
        case Activity::idle: break;
    }
    return FallbackAction(pack, pack.stateActions.idle);
}

std::string NextIdleAction(const Pack& pack, std::string_view current,
                           std::uint64_t randomValue) {
    std::vector<BehaviorReference> choices;
    for (const Behavior& behavior : pack.behaviors) {
        if (behavior.name == current && !behavior.next.empty()) {
            choices = behavior.next;
            break;
        }
    }
    if (choices.empty()) {
        for (const Behavior& behavior : pack.behaviors) {
            if (behavior.frequency > 0) choices.push_back({behavior.name, behavior.frequency});
        }
    }
    std::uint64_t total = 0;
    for (const BehaviorReference& choice : choices) {
        if (choice.frequency > 0 && FindAction(pack, choice.name)) {
            total += static_cast<std::uint64_t>(choice.frequency);
        }
    }
    if (total == 0) {
        if (const Action* fallback = FallbackAction(pack, "Stand")) return fallback->name;
        return {};
    }
    std::uint64_t selected = randomValue % total;
    for (const BehaviorReference& choice : choices) {
        if (choice.frequency <= 0 || !FindAction(pack, choice.name)) continue;
        const auto frequency = static_cast<std::uint64_t>(choice.frequency);
        if (selected < frequency) return choice.name;
        selected -= frequency;
    }
    return "Stand";
}

fs::path ImagePath(const Pack& pack, const Pose& pose) {
    return SafeImagePath(pack, pose.image);
}

} // namespace shimeji
