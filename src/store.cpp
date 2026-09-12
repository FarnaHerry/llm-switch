// store.cpp — llmswitch.store 实现单元。
//
// 文件安全约定：
//   - 所有写都走 atomicWrite（<file>.tmp → rename，失败回落 remove+rename，
//     参考姊妹项目 apitab src/preferences.cppm 的做法）；
//   - 多数 JSON 读走 readJsonOrNull：解析失败把坏文件挪到
//     <file>.corrupt-<毫秒> 再按「无内容」继续，绝不因用户手改坏文件而崩溃；
//     例外是 opencode（官方配置允许 JSON5 注释，挪走用户文件不可接受）——
//     改写用 readJsonStrict（解析失败抛错且不碰原文件），只读探测用
//     readJsonPassive（解析失败按无内容，不挪文件）；
//   - 改写任何 live 文件前先 backupLiveFile 快照到 backupsDir()/<tool>/，
//     每工具每文件只留最近 10 份。
// nlohmann::json 模块下禁用 .items() 结构化绑定，遍历用 it.key()/it.value()。
module llmswitch.store;

import std;
import nlohmann.json;
import llmswitch.config;
import llmswitch.models;

namespace store {
namespace {

// 每工具每文件保留的备份份数上限。
constexpr std::size_t kMaxBackups = 10;

// Claude Desktop 3p profile 的固定 id（对齐 cc-switch，Claude Desktop 的
// configLibrary 按 id 索引，entries 里注册同名条目）。
constexpr std::string_view kClaudeDesktopProfileId =
    "00000000-0000-4000-8000-000000157210";
constexpr std::string_view kClaudeDesktopProfileName = "llm-switch";

std::int64_t nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// provider id：毫秒时间戳 + 8 位随机 hex（可读且碰撞概率可忽略）。
std::string generateId() {
    std::random_device rd;
    std::mt19937_64 gen((static_cast<std::uint64_t>(rd()) << 32) ^ rd());
    return std::format("{}-{:08x}", nowMillis(),
                       static_cast<std::uint32_t>(gen()));
}

// 原子写文本文件：先写 <file>.tmp 再 rename；rename 失败（Windows 目标被
// 占用等）回落 remove + rename。任何一步失败抛 std::runtime_error。
void atomicWrite(const std::filesystem::path& dest, std::string_view content) {
    std::error_code ec;
    if (dest.has_parent_path()) std::filesystem::create_directories(dest.parent_path(), ec);
    const std::filesystem::path tmp = dest.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error(std::format("无法写入文件：{}", dest.string()));
        }
        out << content;
        out.flush();
        if (!out) {
            throw std::runtime_error(std::format("写入文件失败：{}", dest.string()));
        }
    }
    std::filesystem::rename(tmp, dest, ec);
    if (ec) {
        ec.clear();
        std::filesystem::remove(dest, ec);
        ec.clear();
        std::filesystem::rename(tmp, dest, ec);
        if (ec) {
            throw std::runtime_error(
                std::format("保存文件失败：{}（{}）", dest.string(), ec.message()));
        }
    }
}

std::string readTextFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return "";
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

// 读 JSON 文件：不存在 → null；损坏 → 挪到 <file>.corrupt-<毫秒> 并返回 null。
nlohmann::json readJsonOrNull(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) return nullptr;
    const std::string text = readTextFile(file);
    if (text.empty()) return nullptr;
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded()) {
        const auto corrupt =
            std::filesystem::path(file.string() + ".corrupt-" + std::to_string(nowMillis()));
        std::filesystem::rename(file, corrupt, ec);
        return nullptr;
    }
    return j;
}

// 只读探测用：解析失败按「无内容」返回 null，不挪用户文件（opencode 的
// JSON5 注释文件会被 nlohmann 判为损坏，但它仍是用户的有效配置）。
nlohmann::json readJsonPassive(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) return nullptr;
    const std::string text = readTextFile(file);
    if (text.empty()) return nullptr;
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded()) return nullptr;
    return j;
}

// opencode 专用：解析失败抛中文错（提示 JSON5 注释暂不支持），
// 不挪文件不覆盖 —— 宁可拒绝切换也不能静默吞掉用户配置。
nlohmann::json readJsonStrict(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) return nullptr;
    const std::string text = readTextFile(file);
    if (text.empty()) return nullptr;
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded()) {
        throw std::runtime_error(std::format(
            "无法解析 {}：官方配置允许 JSON5 注释，暂不支持。"
            "请把注释去掉后重试（原文件未被修改）。",
            file.string()));
    }
    return j;
}

// 深合并：patch 的对象递归并入 base，其余类型整体覆盖。
void deepMerge(nlohmann::json& base, const nlohmann::json& patch) {
    if (!patch.is_object()) {
        base = patch;
        return;
    }
    if (!base.is_object()) base = nlohmann::json::object();
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        if (it.value().is_object() && base.contains(it.key()) &&
            base[it.key()].is_object()) {
            deepMerge(base[it.key()], it.value());
        } else {
            base[it.key()] = it.value();
        }
    }
}

// 备份清理：dir 下 <prefix>.<毫秒>.bak 只保留最新 kMaxBackups 份
// （毫秒时间戳定宽 13 位，文件名字典序即时间序）。
void pruneBackups(const std::filesystem::path& dir, const std::string& prefix) {
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.starts_with(prefix + ".") && name.ends_with(".bak")) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    while (files.size() > kMaxBackups) {
        std::filesystem::remove(files.front(), ec);
        files.erase(files.begin());
    }
}

// 改写 live 文件前的快照：复制到 backupsDir()/<tool>/<文件名>.<毫秒>.bak。
// 文件不存在则无事发生；复制失败抛异常（宁可不切换也不能无备份改写）。
void backupLiveFile(std::string_view tool, const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) return;
    const auto dir = cfg::backupsDir() / std::string(tool);
    std::filesystem::create_directories(dir, ec);
    // 同毫秒内连续切换时递增时间戳避免覆盖。
    std::int64_t ts = nowMillis();
    std::filesystem::path dest;
    do {
        dest = dir / std::format("{}.{}.bak", file.filename().string(), ts++);
    } while (std::filesystem::exists(dest, ec));
    std::filesystem::copy_file(file, dest, ec);
    if (ec) {
        throw std::runtime_error(
            std::format("备份失败：{}（{}）", file.string(), ec.message()));
    }
    pruneBackups(dir, file.filename().string());
}

// 从 claude-code settings.json 提取 env 字符串字段（文件/env 缺失均为空串）。
std::string claudeEnvValue(const nlohmann::json& settings, std::string_view key) {
    if (!settings.is_object()) return "";
    const auto it = settings.find("env");
    if (it == settings.end() || !it->is_object()) return "";
    const auto v = it->find(std::string(key));
    if (v == it->end() || !v->is_string()) return "";
    return v->get<std::string>();
}

// 合并导入的组：provider 按 id 覆盖/新增；导入的 current 指向合并后仍存在
// 的 provider 时才采用，否则保留现状。
void mergeGroup(models::ProviderGroup& dst, const models::ProviderGroup& src) {
    for (const auto& p : src.providers) {
        bool replaced = false;
        for (auto& cur : dst.providers) {
            if (cur.id == p.id) {
                cur = p;
                replaced = true;
                break;
            }
        }
        if (!replaced) dst.providers.push_back(p);
    }
    if (!src.current.empty()) {
        for (const auto& p : dst.providers) {
            if (p.id == src.current) {
                dst.current = src.current;
                break;
            }
        }
    }
}

// 对象里的字符串字段（缺失/非字符串 → 空串）。
std::string jsonStr(const nlohmann::json& j, std::string_view key) {
    if (!j.is_object()) return "";
    const auto it = j.find(std::string(key));
    if (it == j.end() || !it->is_string()) return "";
    return it->get<std::string>();
}

// ---- 各工具 live 文件读写 ------------------------------------------------------

// pi 的 api 字段映射（三档，经 models::normalizeApiFormat 归一）：
// anthropic → anthropic-messages；openai-responses → openai-responses；
// 其余（openai-chat / 默认）→ openai-completions。
std::string piApiValue(std::string_view apiFormat) {
    const auto f = models::normalizeApiFormat(apiFormat);
    if (f == "anthropic") return "anthropic-messages";
    if (f == "openai-responses") return "openai-responses";
    return "openai-completions";
}

// 反映射（importLive / detectCurrent 用）。
std::string piApiFormatValue(std::string_view api) {
    if (api == "anthropic-messages") return "anthropic";
    if (api == "openai-responses") return "openai-responses";
    if (api == "openai-completions") return "openai-chat";
    return "";
}

// opencode 的 npm 适配器映射（三档）：anthropic → @ai-sdk/anthropic；
// openai-responses → @ai-sdk/openai；其余（openai-chat / 默认）→
// @ai-sdk/openai-compatible。
std::string_view opencodeNpmValue(std::string_view apiFormat) {
    const auto f = models::normalizeApiFormat(apiFormat);
    if (f == "anthropic") return "@ai-sdk/anthropic";
    if (f == "openai-responses") return "@ai-sdk/openai";
    return "@ai-sdk/openai-compatible";
}

// ---- codex config.toml 顶层 model 键 -----------------------------------------
// TOML 顶层键必须出现在任何 [table] 节之前，因此两个 helper 只扫第一个 `[`
// 节头之前的行，节内内容一律不碰。

// 去掉前导空白。
std::string_view trimLeft(std::string_view s) {
    const auto it = std::ranges::find_if(
        s, [](unsigned char c) { return !std::isspace(c); });
    return s.substr(static_cast<std::size_t>(it - s.begin()));
}

// 行级解析顶层 model 键：去掉前导空白与可选的 `#` 注释前缀后，若余下文本以
// `model` + 空白/`=` 开头则返回等号右侧的原始值文本（trim 后，不去引号）。
// 不是 model 行返回空；commentedOut 传出该行是否为注释行。
std::string codexModelLineValue(std::string_view line, bool& commentedOut) {
    std::string_view s = trimLeft(line);
    commentedOut = false;
    if (s.starts_with('#')) {
        commentedOut = true;
        s = trimLeft(s.substr(1));
    }
    if (!s.starts_with("model")) return "";
    s.remove_prefix(5);
    if (!s.empty() && s.front() != '=' &&
        !std::isspace(static_cast<unsigned char>(s.front()))) {
        return "";  // model_provider 等前缀键不算
    }
    s = trimLeft(s);
    if (!s.starts_with('=')) return "";
    s = trimLeft(s.substr(1));
    return std::string(s);
}

// 读顶层 model = "..." 的值（只认未注释行；best-effort，读不到返回空串）。
std::string parseCodexModel(std::string_view toml) {
    std::istringstream in{std::string(toml)};
    for (std::string line; std::getline(in, line);) {
        const auto trimmed = trimLeft(line);
        if (trimmed.starts_with('[')) break;  // 顶层结束
        bool commentedOut = false;
        const std::string value = codexModelLineValue(line, commentedOut);
        if (value.empty() || commentedOut) continue;
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            return value.substr(1, value.size() - 2);
        }
        return "";
    }
    return "";
}

// 把顶层 model = "<model>" 写进 config.toml 文本：有未注释 model 行就替换值；
// 否则替换第一条 `# model = ...` 注释行；都没有则在文件开头插入。model 为空
// 返回原文。其余内容（含所有 [table] 节）原样保留。
std::string applyCodexModel(std::string_view toml, std::string_view model) {
    if (model.empty()) return std::string(toml);
    std::string escaped;
    for (const char c : model) {
        if (c == '\\' || c == '"') escaped += '\\';
        escaped += c;
    }
    const std::string newLine = "model = \"" + escaped + "\"";
    std::string out;
    std::istringstream in{std::string(toml)};
    std::string line;
    bool inSection = false;    // 已进入 [table] 节区，停止 model 行匹配
    bool replaced = false;     // 已写入未注释 model 行
    bool sawCommented = false;
    std::string::size_type commentedPos = 0;
    while (std::getline(in, line)) {
        if (inSection || replaced) {
            out += line;
            out += '\n';
            continue;
        }
        if (trimLeft(line).starts_with('[')) {
            inSection = true;  // 进入节区，停止扫描
            out += line;
            out += '\n';
            continue;
        }
        bool commentedOut = false;
        if (!codexModelLineValue(line, commentedOut).empty()) {
            if (!commentedOut) {
                out += newLine;
                out += '\n';
                replaced = true;
                continue;
            }
            if (!sawCommented) {
                sawCommented = true;
                commentedPos = out.size();
            }
        }
        out += line;
        out += '\n';
    }
    if (!replaced) {  // 顶层没有未注释 model 行
        if (sawCommented) {
            // 整行替换第一条注释行（行末必带 \n）。
            const auto nl = out.find('\n', commentedPos);
            out.replace(commentedPos, nl - commentedPos, newLine);
        } else {
            out.insert(0, newLine + "\n");
        }
    }
    return out;
}

// 把供应商的实际访问 URL 写入 config.toml 的第一个未注释 base_url 键。
// codex 的模板可能包含多个节，但当前供应商模板只需要第一个上游地址；
// 没有 base_url 时保留原文，兼容只通过其他配置提供地址的模板。
std::string applyCodexBaseUrl(std::string_view toml, std::string_view baseUrl) {
    if (baseUrl.empty()) return std::string(toml);
    std::string escaped;
    for (const char c : baseUrl) {
        if (c == '\\' || c == '"') escaped += '\\';
        escaped += c;
    }
    std::string out;
    std::istringstream in{std::string(toml)};
    bool replaced = false;
    for (std::string line; std::getline(in, line);) {
        const std::string_view trimmed = trimLeft(line);
        std::string_view key = trimmed;
        const bool commented = key.starts_with('#');
        if (!commented && key.starts_with("base_url")) {
            key.remove_prefix(8);
            if (key.empty() || key.front() == '=' ||
                std::isspace(static_cast<unsigned char>(key.front()))) {
                const std::size_t indent = line.size() - trimmed.size();
                line = line.substr(0, indent) +
                       "base_url = \"" + escaped + "\"";
                replaced = true;
            }
        }
        out += line;
        out += '\n';
        if (replaced) {
            // 模板只替换当前供应商的第一条地址，避免误改其他 provider 节。
            for (std::string rest; std::getline(in, rest);) {
                out += rest;
                out += '\n';
            }
            break;
        }
    }
    return out;
}

// pi 目录/文件权限：目录 0700、文件 0600（对齐官方对凭据目录的约定）。
void restrictPiDir(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
#ifndef _WIN32
    std::filesystem::permissions(dir, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, ec);
#endif
}

void restrictPiFile(const std::filesystem::path& file) {
#ifndef _WIN32
    std::error_code ec;
    std::filesystem::permissions(file,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec);
#endif
}

// Claude Desktop 3p profile 路径（base 为空 = 平台不支持，返回空）。
std::filesystem::path claudeDesktopProfileFile() {
    const auto dir = cfg::claudeDesktop3pDir();
    if (dir.empty()) return {};
    return dir / "configLibrary" /
           (std::string(kClaudeDesktopProfileId) + ".json");
}

// 对齐 cc-switch 上游 is_claude_safe_model_id：Claude Desktop 的模型菜单只认
// claude-(sonnet|opus|haiku|fable)-* / anthropic/claude-* 前缀的 route id，
// 且角色前缀后必须有实际模型标识；其它名字（kimi-k2 等）写入 profile 会触发
// 桌面端 fail-all 拒收整组。
bool isClaudeSafeModelId(std::string model) {
    // trim + 小写归一
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    const auto first = std::ranges::find_if(model, notSpace);
    const auto last = std::find_if(model.rbegin(), model.rend(), notSpace).base();
    model = first < last ? std::string(first, last) : std::string{};
    std::ranges::transform(model, model.begin(),
                           [](unsigned char c) { return std::tolower(c); });
    std::string_view tail = model;
    if (tail.starts_with("anthropic/claude-")) {
        tail.remove_prefix(std::string_view("anthropic/claude-").size());
    } else if (tail.starts_with("claude-")) {
        tail.remove_prefix(std::string_view("claude-").size());
    } else {
        return false;
    }
    for (const std::string_view role : {"sonnet-", "opus-", "haiku-", "fable-"}) {
        if (tail.starts_with(role) && tail.size() > role.size()) return true;
    }
    return false;
}

// 组一条 inferenceModels 条目：actual 本身是白名单 route id 就直接写 name；
// 否则借用 borrowedId（该档的安全角色名）。labelOverride 只放菜单显示名，
// supports1m 只在勾选时声明；实际请求模型仍由 Provider 的 *Model 字段保存。
nlohmann::json claudeDesktopModelEntry(const std::string& actual,
                                       std::string_view borrowedId,
                                       std::string_view displayName = {},
                                       bool supports1m = false) {
    nlohmann::json m;
    const bool safe = isClaudeSafeModelId(actual);
    if (safe) {
        m["name"] = actual;
    } else {
        m["name"] = borrowedId;
    }
    if (!displayName.empty()) {
        m["labelOverride"] = displayName;
    } else if (!safe) {
        // 旧配置没有独立显示名时保持历史行为，避免菜单变成空白名称。
        m["labelOverride"] = actual;
    }
    if (supports1m) {
        m["supports1m"] = true;
    }
    return m;
}

// 工具的 live 文件是否已存在（load() 首次导入判定用）。
bool liveFileExists(std::string_view tool) {
    std::error_code ec;
    if (tool == "claude-code") {
        return std::filesystem::exists(cfg::claudeSettingsFile(), ec);
    }
    if (tool == "codex") {
        return std::filesystem::exists(cfg::codexAuthFile(), ec);
    }
    if (tool == "opencode") {
        return std::filesystem::exists(cfg::opencodeConfigFile(), ec);
    }
    if (tool == "pi") {
        return std::filesystem::exists(cfg::piModelsFile(), ec) ||
               std::filesystem::exists(cfg::piSettingsFile(), ec);
    }
    if (tool == "claude") {
        const auto profile = claudeDesktopProfileFile();
        return !profile.empty() && std::filesystem::exists(profile, ec);
    }
    return false;
}

// 组内按 baseUrl+apiKey 找匹配项（找不到返回 nullptr）。
const models::Provider* matchByUrlKey(const models::ProviderGroup& g,
                                      const std::string& baseUrl,
                                      const std::string& apiKey) {
    for (const auto& p : g.providers) {
        if (models::effectiveBaseUrl(p) == baseUrl && p.apiKey == apiKey) {
            return &p;
        }
    }
    return nullptr;
}

// 收编用 id：优先用外部键（opencode 的 provider key / pi 的 defaultProvider），
// 已被占用则退回 generateId。
std::string importId(const models::ProviderGroup& g, const std::string& preferred) {
    if (!preferred.empty()) {
        bool taken = false;
        for (const auto& p : g.providers) {
            if (p.id == preferred) taken = true;
        }
        if (!taken) return preferred;
    }
    return generateId();
}

} // namespace

models::ProviderGroup& ProviderStore::groupRef(std::string_view tool) {
    if (models::findTool(tool) == nullptr) {
        throw std::runtime_error(std::format("未知的工具：{}", tool));
    }
    return config_.groups[std::string(tool)];
}

const models::ProviderGroup& ProviderStore::group(std::string_view tool) const {
    return const_cast<ProviderStore*>(this)->groupRef(tool);
}

ProviderStore ProviderStore::load() {
    ProviderStore store;
    const auto j = readJsonOrNull(cfg::configFile());  // 损坏文件已在内部挪走
    if (!j.is_null()) store.config_ = models::fromJson(j);
    // 首次导入：组为空且 live 文件存在 → 把当前生效配置收编进来。
    // importLive 失败（如 opencode 的 JSON5 注释文件）静默跳过——不能因为
    // 一个工具的 live 文件让 load 整个垮掉，用户可在 UI 里看到组为空再处理。
    for (const auto& t : models::toolRegistry()) {
        const auto it = store.config_.groups.find(std::string(t.id));
        const bool empty = it == store.config_.groups.end() ||
                           it->second.providers.empty();
        if (empty && liveFileExists(t.id)) {
            try {
                store.importLive(t.id);
            } catch (...) {
            }
        }
    }
    return store;
}

void ProviderStore::save() const {
    atomicWrite(cfg::configFile(), models::toJson(config_).dump(2) + "\n");
}

void ProviderStore::setThemeMode(std::string mode) {
    config_.themeMode = std::move(mode);
    save();
}

bool ProviderStore::claudeCodeSkipInstallationChecks() const {
    const auto settings = readJsonPassive(cfg::claudeSettingsFile());
    const std::string value =
        claudeEnvValue(settings, "DISABLE_INSTALLATION_CHECKS");
    return value == "1" || value == "true" || value == "TRUE";
}

void ProviderStore::setClaudeCodeSkipInstallationChecks(bool enabled) {
    const auto file = cfg::claudeSettingsFile();
    nlohmann::json settings = readJsonOrNull(file);
    if (!settings.is_object()) {
        if (!enabled) return;
        settings = nlohmann::json::object();
    }

    const std::string current =
        claudeEnvValue(settings, "DISABLE_INSTALLATION_CHECKS");
    const auto env = settings.find("env");
    const bool hasSetting = env != settings.end() && env->is_object() &&
                            env->contains("DISABLE_INSTALLATION_CHECKS");
    if ((enabled && current == "1") || (!enabled && !hasSetting)) return;

    backupLiveFile("claude-code", file);
    if (enabled) {
        settings["env"]["DISABLE_INSTALLATION_CHECKS"] = "1";
    } else if (settings.contains("env") && settings["env"].is_object()) {
        settings["env"].erase("DISABLE_INSTALLATION_CHECKS");
    }
    atomicWrite(file, settings.dump(2) + "\n");
}

void ProviderStore::setRouterEnabled(bool enabled) {
    config_.routerEnabled = enabled;
    save();
}

void ProviderStore::setRouterPort(int port) {
    config_.routerPort = port;
    save();
}

void ProviderStore::setRouterFailover(bool enabled) {
    config_.routerFailover = enabled;
    save();
}

void ProviderStore::setRouterToolEnabled(std::string_view tool, bool enabled) {
    if (models::findTool(tool) == nullptr) {
        throw std::runtime_error(std::format("未知工具：{}", tool));
    }
    auto& tools = config_.routerTools;
    const auto it = std::ranges::find(tools, tool);
    if (enabled && it == tools.end()) {
        tools.emplace_back(tool);
    } else if (!enabled && it != tools.end()) {
        tools.erase(it);
    } else {
        return;
    }
    save();
}

void ProviderStore::addProvider(std::string_view tool, models::Provider provider) {
    auto& g = groupRef(tool);
    if (provider.id.empty()) provider.id = generateId();
    if (provider.createdAt == 0) provider.createdAt = nowMillis();
    g.providers.push_back(std::move(provider));
    save();
}

void ProviderStore::updateProvider(std::string_view tool,
                                   const models::Provider& provider) {
    auto& g = groupRef(tool);
    for (auto& cur : g.providers) {
        if (cur.id == provider.id) {
            cur = provider;
            save();
            return;
        }
    }
    throw std::runtime_error(std::format("供应商不存在：{}", provider.id));
}

void ProviderStore::removeProvider(std::string_view tool, const std::string& id) {
    auto& g = groupRef(tool);
    std::erase_if(g.providers, [&](const models::Provider& p) { return p.id == id; });
    if (g.current == id) g.current.clear();
    save();
}

models::Provider ProviderStore::duplicateProvider(std::string_view tool,
                                                  const std::string& id) {
    auto& g = groupRef(tool);
    for (std::size_t i = 0; i < g.providers.size(); ++i) {
        if (g.providers[i].id != id) continue;
        models::Provider copy = g.providers[i];
        copy.id = generateId();
        copy.name += "（副本）";
        copy.createdAt = nowMillis();
        g.providers.insert(g.providers.begin() + static_cast<std::ptrdiff_t>(i + 1), copy);
        save();
        return copy;
    }
    throw std::runtime_error(std::format("供应商不存在：{}", id));
}

void ProviderStore::switchTo(std::string_view tool, const std::string& id) {
    auto& g = groupRef(tool);
    const models::Provider* target = nullptr;
    for (const auto& p : g.providers) {
        if (p.id == id) target = &p;
    }
    if (target == nullptr) {
        throw std::runtime_error(std::format("供应商不存在：{}", id));
    }
    const std::string baseUrl = models::effectiveBaseUrl(*target);

    if (tool == "claude-code") {
        // 深合并 env 三字段，permissions 等其余字段原样保留。
        const auto file = cfg::claudeSettingsFile();
        nlohmann::json settings = readJsonOrNull(file);
        if (!settings.is_object()) settings = nlohmann::json::object();
        backupLiveFile(tool, file);
        nlohmann::json patch;
        patch["env"]["ANTHROPIC_BASE_URL"] = baseUrl;
        patch["env"]["ANTHROPIC_AUTH_TOKEN"] = target->apiKey;
        if (!target->model.empty()) patch["env"]["ANTHROPIC_MODEL"] = target->model;
        // 三档模型映射（官方 env，均选填）。
        if (!target->haikuModel.empty()) {
            patch["env"]["ANTHROPIC_DEFAULT_HAIKU_MODEL"] = target->haikuModel;
        }
        if (!target->sonnetModel.empty()) {
            patch["env"]["ANTHROPIC_DEFAULT_SONNET_MODEL"] = target->sonnetModel;
        }
        if (!target->opusModel.empty()) {
            patch["env"]["ANTHROPIC_DEFAULT_OPUS_MODEL"] = target->opusModel;
        }
        deepMerge(settings, patch);
        atomicWrite(file, settings.dump(2) + "\n");
    } else if (tool == "codex") {
        // auth.json 只深合并 OPENAI_API_KEY；codexConfigToml 非空时
        // config.toml 整段替换（TOML 不做结构化合并，原文即模板）；model
        // 非空时再行级重写顶层 model 键（其余内容原样保留）。
        const auto authFile = cfg::codexAuthFile();
        nlohmann::json auth = readJsonOrNull(authFile);
        if (!auth.is_object()) auth = nlohmann::json::object();
        backupLiveFile(tool, authFile);
        nlohmann::json patch;
        patch["OPENAI_API_KEY"] = target->apiKey;
        deepMerge(auth, patch);
        atomicWrite(authFile, auth.dump(2) + "\n");
        if (!target->codexConfigToml.empty()) {
            const auto tomlFile = cfg::codexConfigFile();
            backupLiveFile(tool, tomlFile);
            atomicWrite(tomlFile,
                        applyCodexModel(
                            applyCodexBaseUrl(target->codexConfigToml, baseUrl),
                            target->model));
        }
    } else if (tool == "opencode") {
        // additive 模式：往顶层 provider map upsert 本工具条目，其余顶层字段
        // （theme、agent 等）原样保留；model 非空写顶层 model="<id>/<model>"。
        const auto file = cfg::opencodeConfigFile();
        nlohmann::json doc = readJsonStrict(file);  // JSON5 注释 → 抛错，不碰文件
        if (!doc.is_object()) doc = nlohmann::json::object();
        backupLiveFile(tool, file);
        nlohmann::json entry;
        entry["npm"] = opencodeNpmValue(target->apiFormat);
        entry["options"]["baseURL"] = baseUrl;
        entry["options"]["apiKey"] = target->apiKey;
        if (!target->model.empty()) {
            entry["models"][target->model] = nlohmann::json::object();
        }
        if (!doc.contains("provider") || !doc["provider"].is_object()) {
            doc["provider"] = nlohmann::json::object();
        }
        doc["provider"][target->id] = entry;
        if (!target->model.empty()) {
            doc["model"] = target->id + "/" + target->model;
        }
        atomicWrite(file, doc.dump(2) + "\n");
    } else if (tool == "pi") {
        // models.json upsert providers[id]；settings.json 深合并
        // defaultProvider(+defaultModel)。凭据文件权限收紧：目录 0700、文件 0600。
        const auto dir = cfg::piAgentDir();
        restrictPiDir(dir);
        const auto modelsFile = cfg::piModelsFile();
        nlohmann::json models = readJsonOrNull(modelsFile);
        if (!models.is_object()) models = nlohmann::json::object();
        backupLiveFile(tool, modelsFile);
        nlohmann::json entry;
        entry["baseUrl"] = baseUrl;
        entry["apiKey"] = target->apiKey;
        entry["api"] = piApiValue(target->apiFormat);
        if (!target->model.empty()) {
            entry["models"] = nlohmann::json::array({target->model});
        }
        if (!models.contains("providers") || !models["providers"].is_object()) {
            models["providers"] = nlohmann::json::object();
        }
        models["providers"][target->id] = entry;
        atomicWrite(modelsFile, models.dump(2) + "\n");
        restrictPiFile(modelsFile);

        const auto settingsFile = cfg::piSettingsFile();
        nlohmann::json settings = readJsonOrNull(settingsFile);
        if (!settings.is_object()) settings = nlohmann::json::object();
        backupLiveFile(tool, settingsFile);
        nlohmann::json patch;
        patch["defaultProvider"] = target->id;
        if (!target->model.empty()) patch["defaultModel"] = target->model;
        deepMerge(settings, patch);
        atomicWrite(settingsFile, settings.dump(2) + "\n");
        restrictPiFile(settingsFile);
    } else if (tool == "claude") {
        // Claude Desktop 3p 直连（对齐 cc-switch）：Linux 不支持。
        const auto baseDir = cfg::claudeDesktopDir();
        if (baseDir.empty()) {
            throw std::runtime_error(
                "Claude Desktop 不支持 Linux（仅 macOS / Windows）");
        }
        const auto threepDir = cfg::claudeDesktop3pDir();
        // 两份 claude_desktop_config.json（正常目录 + 3p 目录）都置
        // deploymentMode=3p，其余字段保留。
        for (const auto& file :
             {baseDir / "claude_desktop_config.json",
              threepDir / "claude_desktop_config.json"}) {
            nlohmann::json doc = readJsonOrNull(file);
            if (!doc.is_object()) doc = nlohmann::json::object();
            backupLiveFile(tool, file);
            nlohmann::json patch;
            patch["deploymentMode"] = "3p";
            deepMerge(doc, patch);
            atomicWrite(file, doc.dump(2) + "\n");
        }
        // configLibrary 下固定 id 的 profile（网关字段 + 可选模型标签）。
        const auto profileFile = claudeDesktopProfileFile();
        nlohmann::json profile;
        profile["coworkEgressAllowedHosts"] = nlohmann::json::array({"*"});
        profile["disableDeploymentModeChooser"] = true;
        profile["inferenceGatewayApiKey"] = target->apiKey;
        profile["inferenceGatewayAuthScheme"] = "bearer";
        profile["inferenceGatewayBaseUrl"] = baseUrl;
        profile["inferenceProvider"] = "gateway";
        // inferenceModels：主模型（直连官方时通常就这一条）+ 三档映射（每档
        // 映射到供应商真实模型名）。name 必须是桌面端白名单 route id（见
        // claudeDesktopModelEntry / isClaudeSafeModelId）。
        nlohmann::json inferenceModels = nlohmann::json::array();
        if (!target->model.empty()) {
            inferenceModels.push_back(
                claudeDesktopModelEntry(target->model, "claude-sonnet-4-6", {},
                                        target->modelSupports1m));
        }
        if (!target->haikuModel.empty()) {
            inferenceModels.push_back(
                claudeDesktopModelEntry(target->haikuModel, "claude-haiku-4-5",
                                        target->haikuDisplayName,
                                        target->haikuSupports1m));
        }
        if (!target->sonnetModel.empty()) {
            inferenceModels.push_back(
                claudeDesktopModelEntry(target->sonnetModel, "claude-sonnet-4-6",
                                        target->sonnetDisplayName,
                                        target->sonnetSupports1m));
        }
        if (!target->opusModel.empty()) {
            inferenceModels.push_back(
                claudeDesktopModelEntry(target->opusModel, "claude-opus-4-8",
                                        target->opusDisplayName,
                                        target->opusSupports1m));
        }
        if (!inferenceModels.empty()) {
            profile["inferenceModels"] = std::move(inferenceModels);
        }
        backupLiveFile(tool, profileFile);
        atomicWrite(profileFile, profile.dump(2) + "\n");
        // _meta.json：注册条目（同名去重）并指为 appliedId。
        const auto metaFile = profileFile.parent_path() / "_meta.json";
        nlohmann::json meta = readJsonOrNull(metaFile);
        if (!meta.is_object()) meta = nlohmann::json::object();
        backupLiveFile(tool, metaFile);
        nlohmann::json entries = nlohmann::json::array();
        if (meta.contains("entries") && meta["entries"].is_array()) {
            for (const auto& e : meta["entries"]) {
                if (jsonStr(e, "id") != kClaudeDesktopProfileId) {
                    entries.push_back(e);
                }
            }
        }
        nlohmann::json self;
        self["id"] = kClaudeDesktopProfileId;
        self["name"] = kClaudeDesktopProfileName;
        entries.push_back(self);
        meta["entries"] = entries;
        meta["appliedId"] = kClaudeDesktopProfileId;
        atomicWrite(metaFile, meta.dump(2) + "\n");
    } else {
        throw std::runtime_error(std::format("未知的工具：{}", tool));
    }

    g.current = id;
    save();
}

std::string ProviderStore::detectCurrent(std::string_view tool) const {
    const auto git = config_.groups.find(std::string(tool));
    static const models::ProviderGroup kEmpty;
    const auto& g = git != config_.groups.end() ? git->second : kEmpty;

    if (tool == "claude-code") {
        const auto j = readJsonOrNull(cfg::claudeSettingsFile());
        const std::string baseUrl = claudeEnvValue(j, "ANTHROPIC_BASE_URL");
        const std::string apiKey = claudeEnvValue(j, "ANTHROPIC_AUTH_TOKEN");
        if (baseUrl.empty() && apiKey.empty()) return "";
        const auto* p = matchByUrlKey(g, baseUrl, apiKey);
        return p != nullptr ? p->id : "";
    }
    if (tool == "codex") {
        const auto j = readJsonOrNull(cfg::codexAuthFile());
        const std::string apiKey = jsonStr(j, "OPENAI_API_KEY");
        if (apiKey.empty()) return "";
        for (const auto& p : g.providers) {
            if (p.apiKey == apiKey) return p.id;
        }
        return "";
    }
    if (tool == "opencode") {
        // 顶层 model="<providerKey>/<model>" 前缀匹配组内 id。
        const auto j = readJsonPassive(cfg::opencodeConfigFile());
        const std::string model = jsonStr(j, "model");
        if (model.empty()) return "";
        for (const auto& p : g.providers) {
            if (model.starts_with(p.id + "/")) return p.id;
        }
        return "";
    }
    if (tool == "pi") {
        // 先按 settings.json 的 defaultProvider 命中组内 id。
        const auto settings = readJsonOrNull(cfg::piSettingsFile());
        const std::string def = jsonStr(settings, "defaultProvider");
        if (!def.empty()) {
            for (const auto& p : g.providers) {
                if (p.id == def) return p.id;
            }
        }
        // 再按 models.json 条目的 apiKey+baseUrl 匹配。
        const auto models = readJsonOrNull(cfg::piModelsFile());
        if (models.is_object() && models.contains("providers") &&
            models["providers"].is_object()) {
            for (auto it = models["providers"].begin();
                 it != models["providers"].end(); ++it) {
                const std::string baseUrl = jsonStr(it.value(), "baseUrl");
                const std::string apiKey = jsonStr(it.value(), "apiKey");
                if (baseUrl.empty() && apiKey.empty()) continue;
                const auto* p = matchByUrlKey(g, baseUrl, apiKey);
                if (p != nullptr) return p->id;
            }
        }
        return "";
    }
    if (tool == "claude") {
        const auto profile = claudeDesktopProfileFile();
        if (profile.empty()) return "";  // Linux 不支持
        const auto j = readJsonOrNull(profile);
        const std::string baseUrl = jsonStr(j, "inferenceGatewayBaseUrl");
        const std::string apiKey = jsonStr(j, "inferenceGatewayApiKey");
        if (baseUrl.empty() && apiKey.empty()) return "";
        const auto* p = matchByUrlKey(g, baseUrl, apiKey);
        return p != nullptr ? p->id : "";
    }
    throw std::runtime_error(std::format("未知的工具：{}", tool));
}

void ProviderStore::restoreOfficial(std::string_view tool) {
    auto& g = groupRef(tool);  // 未注册工具抛错

    if (tool == "claude-code") {
        // 回到官方 OAuth 登录：撤掉 env 块里本应用写入的三个覆盖键，其余
        // env 键与 permissions 等字段原样保留。
        const auto file = cfg::claudeSettingsFile();
        std::error_code ec;
        if (std::filesystem::exists(file, ec)) {
            nlohmann::json settings = readJsonOrNull(file);
            if (settings.is_object()) {
                backupLiveFile(tool, file);
                if (settings.contains("env") && settings["env"].is_object()) {
                    auto& env = settings["env"];
                    env.erase("ANTHROPIC_BASE_URL");
                    env.erase("ANTHROPIC_AUTH_TOKEN");
                    env.erase("ANTHROPIC_MODEL");
                    env.erase("ANTHROPIC_DEFAULT_HAIKU_MODEL");
                    env.erase("ANTHROPIC_DEFAULT_SONNET_MODEL");
                    env.erase("ANTHROPIC_DEFAULT_OPUS_MODEL");
                }
                atomicWrite(file, settings.dump(2) + "\n");
            }
        }
    } else if (tool == "codex") {
        // 回到官方 ChatGPT 登录：auth.json 删 OPENAI_API_KEY（tokens 等其余
        // 字段保留；删完为空对象则删文件）。config.toml 仅当内容与组内某
        // provider 的 codexConfigToml 完全一致（本应用写入且未被手改）才删，
        // 用户手改过的文件不动。
        const auto authFile = cfg::codexAuthFile();
        std::error_code ec;
        if (std::filesystem::exists(authFile, ec)) {
            nlohmann::json auth = readJsonOrNull(authFile);
            if (auth.is_object()) {
                backupLiveFile(tool, authFile);
                auth.erase("OPENAI_API_KEY");
                if (auth.empty()) {
                    std::filesystem::remove(authFile, ec);
                } else {
                    atomicWrite(authFile, auth.dump(2) + "\n");
                }
            }
        }
        const auto tomlFile = cfg::codexConfigFile();
        if (std::filesystem::exists(tomlFile, ec)) {
            const std::string current = readTextFile(tomlFile);
            for (const auto& p : g.providers) {
                if (!p.codexConfigToml.empty() &&
                    (current == p.codexConfigToml ||
                     current == applyCodexModel(p.codexConfigToml, p.model))) {
                    backupLiveFile(tool, tomlFile);
                    std::filesystem::remove(tomlFile, ec);
                    break;
                }
            }
        }
    } else if (tool == "claude") {
        // 撤掉 3p 直连：两份 claude_desktop_config.json 删 deploymentMode 键；
        // _meta.json 移除本应用 profile 条目并清 appliedId（profile 文件本体
        // 保留，未被引用即无害）。Linux 不支持。
        const auto baseDir = cfg::claudeDesktopDir();
        if (baseDir.empty()) {
            throw std::runtime_error(
                "Claude Desktop 不支持 Linux（仅 macOS / Windows）");
        }
        const auto threepDir = cfg::claudeDesktop3pDir();
        std::error_code ec;
        for (const auto& file :
             {baseDir / "claude_desktop_config.json",
              threepDir / "claude_desktop_config.json"}) {
            if (!std::filesystem::exists(file, ec)) continue;
            nlohmann::json doc = readJsonOrNull(file);
            if (!doc.is_object()) continue;
            backupLiveFile(tool, file);
            doc.erase("deploymentMode");
            atomicWrite(file, doc.dump(2) + "\n");
        }
        const auto profileFile = claudeDesktopProfileFile();
        const auto metaFile = profileFile.parent_path() / "_meta.json";
        if (std::filesystem::exists(metaFile, ec)) {
            nlohmann::json meta = readJsonOrNull(metaFile);
            if (meta.is_object()) {
                backupLiveFile(tool, metaFile);
                if (meta.contains("entries") && meta["entries"].is_array()) {
                    nlohmann::json entries = nlohmann::json::array();
                    for (const auto& e : meta["entries"]) {
                        if (jsonStr(e, "id") != kClaudeDesktopProfileId) {
                            entries.push_back(e);
                        }
                    }
                    meta["entries"] = entries;
                }
                if (jsonStr(meta, "appliedId") == kClaudeDesktopProfileId) {
                    meta.erase("appliedId");
                }
                atomicWrite(metaFile, meta.dump(2) + "\n");
            }
        }
    } else {
        const auto* spec = models::findTool(tool);
        throw std::runtime_error(std::format(
            "{} 没有官方默认状态可恢复",
            spec != nullptr ? spec->displayName : tool));
    }

    g.current.clear();
    save();
}

models::Provider ProviderStore::importLive(std::string_view tool) {
    auto& g = groupRef(tool);
    std::error_code ec;

    // 收编公共尾段：已有匹配项复用，否则建「当前配置」并设为 current。
    const auto adopt = [&](models::Provider p) -> models::Provider {
        if (const auto* existing =
                matchByUrlKey(g, p.baseUrl, p.apiKey)) {
            g.current = existing->id;
            save();
            return *existing;
        }
        if (p.id.empty()) p.id = generateId();
        p.name = "当前配置";
        p.createdAt = nowMillis();
        g.providers.push_back(p);
        g.current = p.id;
        save();
        return p;
    };

    if (tool == "claude-code") {
        const auto file = cfg::claudeSettingsFile();
        if (!std::filesystem::exists(file, ec)) return {};
        const auto j = readJsonOrNull(file);
        models::Provider p;
        p.baseUrl = claudeEnvValue(j, "ANTHROPIC_BASE_URL");
        p.apiKey = claudeEnvValue(j, "ANTHROPIC_AUTH_TOKEN");
        p.model = claudeEnvValue(j, "ANTHROPIC_MODEL");
        p.haikuModel = claudeEnvValue(j, "ANTHROPIC_DEFAULT_HAIKU_MODEL");
        p.sonnetModel = claudeEnvValue(j, "ANTHROPIC_DEFAULT_SONNET_MODEL");
        p.opusModel = claudeEnvValue(j, "ANTHROPIC_DEFAULT_OPUS_MODEL");
        return adopt(std::move(p));
    }
    if (tool == "codex") {
        const auto file = cfg::codexAuthFile();
        if (!std::filesystem::exists(file, ec)) return {};
        const auto j = readJsonOrNull(file);
        models::Provider p;
        p.apiKey = jsonStr(j, "OPENAI_API_KEY");
        // 顺带从 config.toml 顶层 model 键收回模型（best-effort，读不到为空）。
        const auto tomlFile = cfg::codexConfigFile();
        if (std::filesystem::exists(tomlFile, ec)) {
            p.model = parseCodexModel(readTextFile(tomlFile));
        }
        // codex 组只凭 apiKey 匹配（无 baseUrl），这里直接内联复用逻辑。
        for (const auto& cur : g.providers) {
            if (cur.apiKey == p.apiKey) {
                g.current = cur.id;
                save();
                return cur;
            }
        }
        p.id = generateId();
        p.name = "当前配置";
        p.createdAt = nowMillis();
        g.providers.push_back(p);
        g.current = p.id;
        save();
        return p;
    }
    if (tool == "opencode") {
        const auto file = cfg::opencodeConfigFile();
        if (!std::filesystem::exists(file, ec)) return {};
        // JSON5 注释文件解析失败 → 抛明确错误（load() 里被吞掉，UI 手动
        // 收编时透传给用户）。
        const auto j = readJsonStrict(file);
        // 从顶层 model="<key>/<model>" 找对应 provider 条目；读不到不建。
        const std::string model = jsonStr(j, "model");
        const auto slash = model.find('/');
        if (slash == std::string::npos) return {};
        const std::string key = model.substr(0, slash);
        if (!j.is_object() || !j.contains("provider") ||
            !j["provider"].is_object() || !j["provider"].contains(key) ||
            !j["provider"][key].is_object()) {
            return {};
        }
        const auto& entry = j["provider"][key];
        models::Provider p;
        p.id = importId(g, key);
        p.baseUrl = jsonStr(entry.contains("options") ? entry["options"]
                                                      : nlohmann::json{},
                            "baseURL");
        p.apiKey = jsonStr(entry.contains("options") ? entry["options"]
                                                     : nlohmann::json{},
                           "apiKey");
        p.model = model.substr(slash + 1);
        if (jsonStr(entry, "npm") == "@ai-sdk/anthropic") p.apiFormat = "anthropic";
        return adopt(std::move(p));
    }
    if (tool == "pi") {
        // 经 settings.json 的 defaultProvider 找 models.json 里的条目。
        const auto settingsFile = cfg::piSettingsFile();
        const auto modelsFile = cfg::piModelsFile();
        if (!std::filesystem::exists(modelsFile, ec)) return {};
        const std::string def =
            jsonStr(readJsonOrNull(settingsFile), "defaultProvider");
        if (def.empty()) return {};
        const auto models = readJsonOrNull(modelsFile);
        if (!models.is_object() || !models.contains("providers") ||
            !models["providers"].is_object() ||
            !models["providers"].contains(def) ||
            !models["providers"][def].is_object()) {
            return {};
        }
        const auto& entry = models["providers"][def];
        models::Provider p;
        p.id = importId(g, def);
        p.baseUrl = jsonStr(entry, "baseUrl");
        p.apiKey = jsonStr(entry, "apiKey");
        p.apiFormat = piApiFormatValue(jsonStr(entry, "api"));
        p.model = jsonStr(readJsonOrNull(settingsFile), "defaultModel");
        return adopt(std::move(p));
    }
    if (tool == "claude") {
        const auto profile = claudeDesktopProfileFile();
        if (profile.empty() || !std::filesystem::exists(profile, ec)) return {};
        const auto j = readJsonOrNull(profile);
        models::Provider p;
        p.baseUrl = jsonStr(j, "inferenceGatewayBaseUrl");
        p.apiKey = jsonStr(j, "inferenceGatewayApiKey");
        // 收回 inferenceModels：条目真实名 = labelOverride（有）否则 name；
        // 按 route id 前缀归到三档映射字段，第一条同时填 model。best-effort。
        if (j.contains("inferenceModels") && j["inferenceModels"].is_array()) {
            bool first = true;
            for (const auto& m : j["inferenceModels"]) {
                const std::string label = jsonStr(m, "labelOverride");
                const std::string name = jsonStr(m, "name");
                const std::string& actual = label.empty() ? name : label;
                if (actual.empty()) continue;
                if (first) {
                    p.model = actual;
                    p.modelSupports1m = m.value("supports1m", false);
                    first = false;
                }
                if (name.starts_with("claude-haiku-")) {
                    p.haikuModel = actual;
                } else if (name.starts_with("claude-sonnet-")) {
                    p.sonnetModel = actual;
                } else if (name.starts_with("claude-opus-")) {
                    p.opusModel = actual;
                }
            }
        }
        return adopt(std::move(p));
    }
    throw std::runtime_error(std::format("未知的工具：{}", tool));
}

void ProviderStore::exportTo(const std::filesystem::path& path) const {
    atomicWrite(path, models::toJson(config_).dump(2) + "\n");
}

void ProviderStore::importFrom(const std::filesystem::path& path) {
    // 回滚快照：先落盘当前配置，再把 config.json 复制进 backups/。
    save();
    const auto cfgFile = cfg::configFile();
    std::error_code ec;
    if (std::filesystem::exists(cfgFile, ec)) {
        const auto dir = cfg::backupsDir();
        std::int64_t ts = nowMillis();
        std::filesystem::path dest;
        do {
            dest = dir / std::format("config.json.{}.bak", ts++);
        } while (std::filesystem::exists(dest, ec));
        std::filesystem::copy_file(cfgFile, dest, ec);
        if (ec) {
            throw std::runtime_error(
                std::format("备份当前配置失败：{}", ec.message()));
        }
        pruneBackups(dir, "config.json");
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error(std::format("无法读取导入文件：{}", path.string()));
    }
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        throw std::runtime_error(
            std::format("导入文件不是有效的配置 JSON：{}", path.string()));
    }
    // 旧格式（顶层 claude/codex）在 fromJson 内自动迁移成 groups 键。
    const auto imported = models::fromJson(j);
    for (const auto& [key, grp] : imported.groups) {
        mergeGroup(config_.groups[key], grp);
    }
    if (!imported.themeMode.empty()) config_.themeMode = imported.themeMode;
    save();
}

} // namespace store
