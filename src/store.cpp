// store.cpp — llmswitch.store 实现单元。
//
// 文件安全约定：
//   - 所有写都走 atomicWrite（<file>.tmp → rename，失败回落 remove+rename，
//     参考姊妹项目 apitab src/preferences.cppm 的做法）；
//   - 所有 JSON 读都走 readJsonOrNull：解析失败把坏文件挪到
//     <file>.corrupt-<毫秒> 再按「无内容」继续，绝不因用户手改坏文件而崩溃；
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

// 读 JSON 文件：不存在 → null；损坏 → 挪到 <file>.corrupt-<毫秒> 并返回 null。
nlohmann::json readJsonOrNull(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) return nullptr;
    std::ifstream in(file, std::ios::binary);
    if (!in) return nullptr;
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded()) {
        const auto corrupt =
            std::filesystem::path(file.string() + ".corrupt-" + std::to_string(nowMillis()));
        std::filesystem::rename(file, corrupt, ec);
        return nullptr;
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

// 从 claude settings.json 提取 env 字符串字段（文件/env 缺失均为空串）。
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

} // namespace

models::ProviderGroup& ProviderStore::groupRef(std::string_view tool) {
    if (tool == kToolClaude) return config_.claude;
    if (tool == kToolCodex) return config_.codex;
    throw std::runtime_error(std::format("未知的工具：{}", tool));
}

const models::ProviderGroup& ProviderStore::group(std::string_view tool) const {
    return const_cast<ProviderStore*>(this)->groupRef(tool);
}

ProviderStore ProviderStore::load() {
    ProviderStore store;
    const auto j = readJsonOrNull(cfg::configFile());  // 损坏文件已在内部挪走
    if (!j.is_null()) store.config_ = models::fromJson(j);
    // 首次导入：组为空且 live 文件存在 → 把当前生效配置收编进来。
    std::error_code ec;
    if (store.config_.claude.providers.empty() &&
        std::filesystem::exists(cfg::claudeSettingsFile(), ec)) {
        store.importLive(kToolClaude);
    }
    if (store.config_.codex.providers.empty() &&
        std::filesystem::exists(cfg::codexAuthFile(), ec)) {
        store.importLive(kToolCodex);
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

    if (tool == kToolClaude) {
        // 深合并 env 三字段，permissions 等其余字段原样保留。
        const auto file = cfg::claudeSettingsFile();
        nlohmann::json settings = readJsonOrNull(file);
        if (!settings.is_object()) settings = nlohmann::json::object();
        backupLiveFile(tool, file);
        nlohmann::json patch;
        patch["env"]["ANTHROPIC_BASE_URL"] = target->baseUrl;
        patch["env"]["ANTHROPIC_AUTH_TOKEN"] = target->apiKey;
        if (!target->model.empty()) patch["env"]["ANTHROPIC_MODEL"] = target->model;
        deepMerge(settings, patch);
        atomicWrite(file, settings.dump(2) + "\n");
    } else {
        // codex：auth.json 只深合并 OPENAI_API_KEY；codexConfigToml 非空时
        // config.toml 整段替换（TOML 不做结构化合并，原文即模板）。
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
            atomicWrite(tomlFile, target->codexConfigToml);
        }
    }

    g.current = id;
    save();
}

std::string ProviderStore::detectCurrent(std::string_view tool) const {
    if (tool == kToolClaude) {
        const auto j = readJsonOrNull(cfg::claudeSettingsFile());
        const std::string baseUrl = claudeEnvValue(j, "ANTHROPIC_BASE_URL");
        const std::string apiKey = claudeEnvValue(j, "ANTHROPIC_AUTH_TOKEN");
        if (baseUrl.empty() && apiKey.empty()) return "";
        for (const auto& p : config_.claude.providers) {
            if (p.baseUrl == baseUrl && p.apiKey == apiKey) return p.id;
        }
        return "";
    }
    if (tool == kToolCodex) {
        const auto j = readJsonOrNull(cfg::codexAuthFile());
        std::string apiKey;
        if (j.is_object()) {
            if (const auto it = j.find("OPENAI_API_KEY");
                it != j.end() && it->is_string()) {
                apiKey = it->get<std::string>();
            }
        }
        if (apiKey.empty()) return "";
        for (const auto& p : config_.codex.providers) {
            if (p.apiKey == apiKey) return p.id;
        }
        return "";
    }
    throw std::runtime_error(std::format("未知的工具：{}", tool));
}

models::Provider ProviderStore::importLive(std::string_view tool) {
    auto& g = groupRef(tool);
    std::error_code ec;

    if (tool == kToolClaude) {
        const auto file = cfg::claudeSettingsFile();
        if (!std::filesystem::exists(file, ec)) return {};
        const auto j = readJsonOrNull(file);
        const std::string baseUrl = claudeEnvValue(j, "ANTHROPIC_BASE_URL");
        const std::string apiKey = claudeEnvValue(j, "ANTHROPIC_AUTH_TOKEN");
        // 已有匹配项则复用（同 detectCurrent 的匹配规则），不重复收编。
        for (const auto& p : g.providers) {
            if (p.baseUrl == baseUrl && p.apiKey == apiKey) {
                g.current = p.id;
                save();
                return p;
            }
        }
        models::Provider p;
        p.id = generateId();
        p.name = "当前配置";
        p.baseUrl = baseUrl;
        p.apiKey = apiKey;
        p.model = claudeEnvValue(j, "ANTHROPIC_MODEL");
        p.createdAt = nowMillis();
        g.providers.push_back(p);
        g.current = p.id;
        save();
        return p;
    }
    if (tool == kToolCodex) {
        const auto file = cfg::codexAuthFile();
        if (!std::filesystem::exists(file, ec)) return {};
        const auto j = readJsonOrNull(file);
        std::string apiKey;
        if (j.is_object()) {
            if (const auto it = j.find("OPENAI_API_KEY");
                it != j.end() && it->is_string()) {
                apiKey = it->get<std::string>();
            }
        }
        for (const auto& p : g.providers) {
            if (p.apiKey == apiKey) {
                g.current = p.id;
                save();
                return p;
            }
        }
        models::Provider p;
        p.id = generateId();
        p.name = "当前配置";
        p.apiKey = apiKey;
        p.createdAt = nowMillis();
        g.providers.push_back(p);
        g.current = p.id;
        save();
        return p;
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
    const auto imported = models::fromJson(j);
    mergeGroup(config_.claude, imported.claude);
    mergeGroup(config_.codex, imported.codex);
    if (!imported.themeMode.empty()) config_.themeMode = imported.themeMode;
    save();
}

} // namespace store
