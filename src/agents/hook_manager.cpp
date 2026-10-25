// hook_manager.cpp — Claude Code / Codex 用户级 hooks 的保留式增量写入。
module;
#ifndef _WIN32
#include <sys/stat.h>
#endif

module llmswitch.agents.hook_manager;

import std;
import nlohmann.json;
import llmswitch.config;

namespace agents::hooks {
namespace {

constexpr std::size_t kMaxConfigBytes = 4 * 1024 * 1024;
constexpr std::size_t kBackupsToKeep = 10;

struct ToolHooks {
    std::string_view id;
    std::filesystem::path (*path)();
    std::span<const std::string_view> events;
};

std::filesystem::path ClaudePath() { return cfg::claudeSettingsFile(); }
std::filesystem::path CodexPath() { return cfg::codexHooksFile(); }

constexpr std::array<std::string_view, 8> kClaudeEvents{
    "SessionStart", "UserPromptSubmit", "PermissionRequest", "Stop",
    "StopFailure", "SessionEnd", "SubagentStart", "SubagentStop"};
constexpr std::array<std::string_view, 8> kCodexEvents{
    "SessionStart", "UserPromptSubmit", "PermissionRequest", "Stop",
    "Interrupt", "SessionEnd", "SubagentStart", "SubagentStop"};

const ToolHooks& Spec(std::string_view tool) {
    static const ToolHooks claude{"claude-code", &ClaudePath, kClaudeEvents};
    static const ToolHooks codex{"codex", &CodexPath, kCodexEvents};
    if (tool == claude.id) return claude;
    if (tool == codex.id) return codex;
    throw std::invalid_argument("unsupported native Agent hook source");
}

std::string Marker(std::string_view tool) {
    return "--llmswitch-hook=" + std::string(tool);
}

bool IsOwnedHandler(const nlohmann::json& handler, std::string_view tool) {
    if (!handler.is_object()) return false;
    const auto type = handler.find("type");
    const auto command = handler.find("command");
    return type != handler.end() && type->is_string() &&
           type->get<std::string>() == "command" &&
           command != handler.end() && command->is_string() &&
           command->get_ref<const std::string&>().find(Marker(tool)) !=
               std::string::npos;
}

nlohmann::json ReadConfig(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return nlohmann::json::object();
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > kMaxConfigBytes) {
        throw std::runtime_error("Agent hooks 配置文件过大或不可读取");
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("无法读取 Agent hooks 配置文件");
    std::string contents(static_cast<std::size_t>(size), '\0');
    if (size > 0) in.read(contents.data(), static_cast<std::streamsize>(size));
    if (!in && !in.eof()) throw std::runtime_error("读取 Agent hooks 配置失败");
    nlohmann::json parsed = nlohmann::json::parse(contents, nullptr, false);
    if (!parsed.is_object()) {
        throw std::runtime_error("Agent hooks 配置不是有效 JSON 对象；未修改原文件");
    }
    return parsed;
}

bool CodexHooksDisabled() {
    std::ifstream in(cfg::codexConfigFile(), std::ios::binary);
    if (!in) return false;
    bool inFeatures = false;
    std::string line;
    while (std::getline(in, line)) {
        const auto comment = line.find('#');
        if (comment != std::string::npos) line.resize(comment);
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) continue;
        const auto last = line.find_last_not_of(" \t\r");
        const std::string_view trimmed(line.data() + first, last - first + 1);
        if (trimmed.starts_with('[') && trimmed.ends_with(']')) {
            inFeatures = trimmed == "[features]";
            continue;
        }
        if (!inFeatures) continue;
        const auto equals = trimmed.find('=');
        if (equals == std::string_view::npos) continue;
        const auto keyEnd = trimmed.find_last_not_of(" \t", equals - 1);
        const std::string_view key = trimmed.substr(0, keyEnd + 1);
        const auto valueStart = trimmed.find_first_not_of(" \t", equals + 1);
        if (key == "hooks" && valueStart != std::string_view::npos &&
            trimmed.substr(valueStart) == "false") {
            return true;
        }
    }
    return false;
}

bool ClaudeHooksDisabled(const nlohmann::json& root) {
    const auto it = root.find("disableAllHooks");
    return it != root.end() && it->is_boolean() && it->get<bool>();
}

std::string ShellQuote(const std::filesystem::path& executable) {
    const std::string value = std::filesystem::absolute(executable).string();
#ifdef _WIN32
    std::string result = "\"";
    for (char c : value) {
        if (c == '"') result += "\\\"";
        else result += c;
    }
    result += '"';
    return result;
#else
    std::string result = "'";
    for (char c : value) {
        if (c == '\'') result += "'\\''";
        else result += c;
    }
    result += '\'';
    return result;
#endif
}

std::string HookCommand(std::string_view tool,
                        const std::filesystem::path& executable) {
    return ShellQuote(executable) + " --tool " + std::string(tool) + " " +
           Marker(tool);
}

std::int64_t NowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void RestrictToOwner(const std::filesystem::path& file) {
#ifndef _WIN32
    if (::chmod(file.c_str(), S_IRUSR | S_IWUSR) != 0) {
        throw std::runtime_error("无法限制 Agent hooks 配置文件权限");
    }
#else
    (void)file;
#endif
}

void Backup(const std::filesystem::path& source, std::string_view tool) {
    std::error_code ec;
    if (!std::filesystem::exists(source, ec)) return;
    const auto directory = cfg::dataDir() / "backups" / "agents" /
                           std::string(tool);
    std::filesystem::create_directories(directory, ec);
    if (ec) throw std::runtime_error("无法创建 Agent hooks 备份目录");
#ifndef _WIN32
    ::chmod(directory.parent_path().c_str(), S_IRWXU);
    ::chmod(directory.c_str(), S_IRWXU);
#endif
    const auto backupPath = directory /
        (source.filename().string() + "." + std::to_string(NowMillis()) + ".bak");
    std::filesystem::copy_file(source, backupPath,
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    if (ec) throw std::runtime_error("无法备份 Agent hooks 配置文件");
    RestrictToOwner(backupPath);

    std::vector<std::filesystem::directory_entry> backups;
    for (const auto& item : std::filesystem::directory_iterator(directory, ec)) {
        if (!ec && item.is_regular_file()) backups.push_back(item);
    }
    std::sort(backups.begin(), backups.end(), [](const auto& a, const auto& b) {
        return a.last_write_time() > b.last_write_time();
    });
    for (std::size_t i = kBackupsToKeep; i < backups.size(); ++i) {
        std::filesystem::remove(backups[i].path(), ec);
    }
}

void AtomicWrite(const std::filesystem::path& destination,
                 const nlohmann::json& value) {
    std::error_code ec;
    const bool parentExisted = std::filesystem::exists(destination.parent_path(), ec);
    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec) throw std::runtime_error("无法创建 Agent 配置目录");
#ifndef _WIN32
    if (!parentExisted) ::chmod(destination.parent_path().c_str(), S_IRWXU);
#endif
    const auto temp = std::filesystem::path(destination.string() + ".llmswitch.tmp");
    struct TempGuard {
        std::filesystem::path path;
        ~TempGuard() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } guard{temp};
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("无法写入 Agent hooks 临时文件");
        out << value.dump(2) << '\n';
        out.flush();
        if (!out) throw std::runtime_error("写入 Agent hooks 临时文件失败");
    }
    std::filesystem::permissions(temp, std::filesystem::perms::owner_read |
        std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, ec);
    if (ec) throw std::runtime_error("无法限制 Agent hooks 临时文件权限");
    std::filesystem::rename(temp, destination, ec);
    if (ec) {
        // Windows rename 不覆盖现有目标；此时已有带权限备份可恢复。
        std::error_code removeError;
        std::filesystem::remove(destination, removeError);
        ec.clear();
        std::filesystem::rename(temp, destination, ec);
    }
    if (ec) throw std::runtime_error("替换 Agent hooks 配置失败");
}

bool RemoveOwned(nlohmann::json& root, const ToolHooks& spec) {
    bool changed = false;
    auto hooks = root.find("hooks");
    if (hooks == root.end()) return false;
    if (!hooks->is_object()) {
        throw std::runtime_error("Agent hooks 字段格式不兼容；未修改原文件");
    }
    for (const auto event : spec.events) {
        auto eventIt = hooks->find(std::string(event));
        if (eventIt == hooks->end()) continue;
        if (!eventIt->is_array()) {
            throw std::runtime_error("Agent hook 事件格式不兼容；未修改原文件");
        }
        auto& groups = *eventIt;
        for (auto groupIt = groups.begin(); groupIt != groups.end();) {
            if (!groupIt->is_object()) {
                ++groupIt;
                continue;
            }
            auto handlers = groupIt->find("hooks");
            if (handlers == groupIt->end() || !handlers->is_array()) {
                ++groupIt;
                continue;
            }
            auto& values = *handlers;
            const auto oldSize = values.size();
            for (auto valueIt = values.begin(); valueIt != values.end();) {
                if (IsOwnedHandler(*valueIt, spec.id)) {
                    valueIt = values.erase(valueIt);
                } else {
                    ++valueIt;
                }
            }
            if (values.size() != oldSize) {
                changed = true;
                if (values.empty()) {
                    groupIt->erase("hooks");
                    if (groupIt->size() == 1 && groupIt->contains("matcher")) {
                        groupIt = groups.erase(groupIt);
                        continue;
                    }
                }
            }
            ++groupIt;
        }
        if (groups.empty()) hooks->erase(eventIt);
    }
    if (hooks->empty()) root.erase(hooks);
    return changed;
}

void AddHooks(nlohmann::json& root, const ToolHooks& spec,
              const std::filesystem::path& executable) {
    if (!root.contains("hooks")) root["hooks"] = nlohmann::json::object();
    auto& hooks = root["hooks"];
    if (!hooks.is_object()) {
        throw std::runtime_error("Agent hooks 字段格式不兼容；未修改原文件");
    }
    const std::string command = HookCommand(spec.id, executable);
    for (const auto event : spec.events) {
        const std::string key(event);
        if (!hooks.contains(key)) hooks[key] = nlohmann::json::array();
        auto& groups = hooks[key];
        if (!groups.is_array()) {
            throw std::runtime_error("Agent hook 事件格式不兼容；未修改原文件");
        }
        nlohmann::json handler{
            {"type", "command"}, {"command", command}, {"async", true}};
#ifdef _WIN32
        handler["commandWindows"] = command;
#endif
        // Codex filters SessionEnd against its `reason` field (currently
        // always "other"); `*` is not the documented all-reasons matcher.
        const std::string_view matcher =
            spec.id == "codex" && event == "SessionEnd" ? "other" : "*";
        groups.push_back(nlohmann::json{
            {"matcher", matcher}, {"hooks", nlohmann::json::array({handler})}});
    }
}

} // namespace

HookStatus Inspect(std::string_view tool) {
    const ToolHooks& spec = Spec(tool);
    nlohmann::json root = ReadConfig(spec.path());
    const bool blocked = tool == "codex" ? CodexHooksDisabled()
                                          : ClaudeHooksDisabled(root);
    bool installed = false;
    const auto hooks = root.find("hooks");
    if (hooks != root.end() && hooks->is_object()) {
        for (const auto event : spec.events) {
            const auto groups = hooks->find(std::string(event));
            if (groups == hooks->end() || !groups->is_array()) continue;
            for (const auto& group : *groups) {
                if (!group.is_object()) continue;
                const auto handlers = group.find("hooks");
                if (handlers == group.end() || !handlers->is_array()) continue;
                if (std::any_of(handlers->begin(), handlers->end(),
                        [&](const auto& handler) { return IsOwnedHandler(handler, tool); })) {
                    installed = true;
                    break;
                }
            }
            if (installed) break;
        }
    }
    return {.installed = installed, .blockedByUser = blocked};
}

void Configure(std::string_view tool,
               const std::filesystem::path& helperExecutable, bool enabled) {
    const ToolHooks& spec = Spec(tool);
    const std::filesystem::path destination = spec.path();
    nlohmann::json root = ReadConfig(destination);
    if (enabled) {
        if (!std::filesystem::is_regular_file(helperExecutable)) {
            throw std::runtime_error("找不到随应用安装的 Agent hook 接收程序");
        }
        if ((tool == "codex" && CodexHooksDisabled()) ||
            (tool == "claude-code" && ClaudeHooksDisabled(root))) {
            throw std::runtime_error("该 Agent 已在配置中关闭 hooks；请先在 Agent 自身启用");
        }
    }
    const nlohmann::json original = root;
    (void)RemoveOwned(root, spec);
    if (enabled) AddHooks(root, spec, helperExecutable);
    if (root == original) return;
    Backup(destination, tool);
    AtomicWrite(destination, root);
}

} // namespace agents::hooks
