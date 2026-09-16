// store.cpp — llmswitch.store 实现单元（核心）：配置库读写、CRUD、设置。
//
// 实现单元拆分：live 配置文件同步（switchTo / detectCurrent /
// restoreOfficial / importLive / importFrom）在 store_live.cpp，ZCode
// 条目同步（upsertZcodeEntry 等）在 store_zcode.cpp；三个单元共用的
// 文件工具与 ZCode 条目助手以模块链接声明在 store.cppm。
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
    if (tool == "zcode") {
        return std::filesystem::exists(cfg::zcodeConfigFile(), ec);
    }
    if (tool == "dsh") {
        return std::filesystem::exists(cfg::dshSettingsFile(), ec);
    }
    if (tool == "claude") {
        const auto profile = claudeDesktopProfileFile();
        return !profile.empty() && std::filesystem::exists(profile, ec);
    }
    return false;
}

} // namespace

// ---- 模块内共享工具（声明在 store.cppm，store_live / store_zcode 复用）----

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
    // 例外：zcode 每次启动都全量同步——它的 config.json 是自己 provider/
    // 模型清单的事实源（不定长 models、外部可自行增删），收编按条目键
    // 原位更新，llm-switch 侧的用量设置等字段不受影响。
    // importLive 失败（如 opencode 的 JSON5 注释文件）静默跳过——不能因为
    // 一个工具的 live 文件让 load 整个垮掉，用户可在 UI 里看到组为空再处理。
    for (const auto& t : models::toolRegistry()) {
        const auto it = store.config_.groups.find(std::string(t.id));
        const bool empty = it == store.config_.groups.end() ||
                           it->second.providers.empty();
        const bool resync = t.id == "zcode";

        // Codex 官方订阅使用 OAuth（auth_mode=chatgpt + tokens），没有
        // OPENAI_API_KEY。旧版本曾把这种 auth.json 收编成空白「当前配置」
        // 第三方卡；启动时清掉这个可识别的历史占位，恢复「OpenAI 官方」卡。
        if (t.id == "codex" && it != store.config_.groups.end()) {
            const auto auth = readJsonOrNull(cfg::codexAuthFile());
            const auto tokens = auth.is_object() ? auth.find("tokens") : auth.end();
            const bool hasOAuthToken =
                auth.is_object() &&
                (jsonStr(auth, "auth_mode") == "chatgpt" ||
                 (tokens != auth.end() && tokens->is_object() &&
                  !jsonStr(*tokens, "access_token").empty()));
            if (hasOAuthToken && jsonStr(auth, "OPENAI_API_KEY").empty()) {
                auto& codexGroup = it->second;
                const auto oldSize = codexGroup.providers.size();
                std::erase_if(codexGroup.providers, [](const models::Provider& p) {
                    return p.name == "当前配置" && p.baseUrl.empty() &&
                           p.apiKey.empty() && p.model.empty() &&
                           p.codexConfigToml.empty();
                });
                if (codexGroup.providers.size() != oldSize) {
                    const bool currentStillExists = std::ranges::any_of(
                        codexGroup.providers, [&](const models::Provider& p) {
                            return p.id == codexGroup.current;
                        });
                    if (!currentStillExists) codexGroup.current.clear();
                    store.save();
                }
            }
        }
        if ((empty || resync) && liveFileExists(t.id)) {
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

void ProviderStore::setCloseBehavior(std::string behavior) {
    if (behavior != "ask" && behavior != "tray" && behavior != "quit") {
        behavior = "ask";
    }
    config_.closeBehavior = std::move(behavior);
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

std::string ProviderStore::addProvider(std::string_view tool,
                                       models::Provider provider) {
    auto& g = groupRef(tool);
    if (provider.id.empty()) provider.id = generateId();
    if (provider.createdAt == 0) provider.createdAt = nowMillis();
    if (tool == "zcode") upsertZcodeEntry(provider);
    std::string id = provider.id;
    g.providers.push_back(std::move(provider));
    save();
    return id;
}

void ProviderStore::updateProvider(std::string_view tool,
                                   const models::Provider& provider) {
    auto& g = groupRef(tool);
    for (auto& cur : g.providers) {
        if (cur.id == provider.id) {
            cur = provider;
            save();
            if (tool == "zcode") upsertZcodeEntry(provider);
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

void ProviderStore::exportTo(const std::filesystem::path& path) const {
    atomicWrite(path, models::toJson(config_).dump(2) + "\n");
}

std::string loadUsageTemplatesOverride() {
    const std::filesystem::path file = cfg::usageTemplatesFile();
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) return {};
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        throw std::runtime_error(
            std::format("读取用量模板覆盖失败：{}", file.string()));
    }
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

} // namespace store
