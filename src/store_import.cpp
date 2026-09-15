// store_import.cpp — llmswitch.store 实现单元：收编、探测与配置导入。
//
// importLive（live 文件 → 供应商卡，条目键即身份）/
// detectCurrent（探测 live 当前命中的供应商）/ importFrom（配置库导入
// 合并，带回滚快照），以及它们专属的辅助（端点+密钥匹配、收编 id、
// pi 的 api 字段映射、codex config.toml 顶层 model 解析、组合并）。
// 与 store_live.cpp 共享的格式工具以模块链接声明在 store.cppm。
module llmswitch.store;

import std;
import nlohmann.json;
import llmswitch.config;
import llmswitch.models;

namespace store {
namespace {

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

// 反映射（importLive / detectCurrent 用）。
std::string piApiFormatValue(std::string_view api) {
    if (api == "anthropic-messages") return "anthropic";
    if (api == "openai-responses") return "openai-responses";
    if (api == "openai-completions") return "openai-chat";
    return "";
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
    if (tool == "gemini" || tool == "qwen") {
        // .env 的端点+密钥匹配组内供应商；两者都空 = 官方登录，未切换。
        const auto envFile = tool == "gemini" ? cfg::geminiEnvFile() : cfg::qwenEnvFile();
        const std::string baseVar =
            tool == "gemini" ? "GOOGLE_GEMINI_BASE_URL" : "OPENAI_BASE_URL";
        const std::string keyVar = tool == "gemini" ? "GEMINI_API_KEY" : "OPENAI_API_KEY";
        const std::string baseUrl = readEnvValue(envFile, baseVar);
        const std::string apiKey = readEnvValue(envFile, keyVar);
        if (baseUrl.empty() && apiKey.empty()) return "";
        const auto* p = matchByUrlKey(g, baseUrl, apiKey);
        return p != nullptr ? p->id : "";
    }
    if (tool == "zcode") {
        // 启用中的条目命中组内 id：优先本应用托管（llmswitch:*）条目，
        // 其次 ZCode 原生自建条目（enabled 缺省视为启用）；builtin:* 是
        // 官方域，不算任何供应商卡的 current；组里已删则视为未切换。
        const auto j = readJsonOrNull(cfg::zcodeConfigFile());
        if (!j.is_object() || !j.contains("provider") ||
            !j["provider"].is_object()) {
            return "";
        }
        for (int pass = 0; pass < 2; ++pass) {
            for (auto it = j["provider"].begin(); it != j["provider"].end();
                 ++it) {
                if (!it.value().is_object()) continue;
                const bool ours = it.key().starts_with("llmswitch:");
                if ((pass == 0) != ours) continue;
                if (!zcodeEntryOn(it.value())) continue;
                const std::string id =
                    ours ? it.key().substr(10) : it.key();
                for (const auto& p : g.providers) {
                    if (p.id == id) return p.id;
                }
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
        // 官方 Codex 订阅是 OAuth 登录（auth_mode=chatgpt / tokens），没有
        // OPENAI_API_KEY；这类 auth.json 不能收编为空白第三方供应商。
        if (p.apiKey.empty()) return {};
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
    if (tool == "gemini" || tool == "qwen") {
        const auto envFile = tool == "gemini" ? cfg::geminiEnvFile() : cfg::qwenEnvFile();
        if (!std::filesystem::exists(envFile, ec)) return {};
        const std::string baseVar =
            tool == "gemini" ? "GOOGLE_GEMINI_BASE_URL" : "OPENAI_BASE_URL";
        const std::string keyVar = tool == "gemini" ? "GEMINI_API_KEY" : "OPENAI_API_KEY";
        const std::string modelVar = tool == "gemini" ? "GEMINI_MODEL" : "OPENAI_MODEL";
        models::Provider p;
        p.baseUrl = readEnvValue(envFile, baseVar);
        p.apiKey = readEnvValue(envFile, keyVar);
        p.model = readEnvValue(envFile, modelVar);
        if (p.baseUrl.empty() && p.apiKey.empty()) return {};
        return adopt(std::move(p));
    }
    if (tool == "zcode") {
        // 全量收编：config.json 里每个带凭据的自建 provider 条目都进列表
        //（含未启用的；OAuth 等无凭据条目跳过）。builtin:* 是 ZCode 官方
        // 套餐条目（体验/个人套餐、API key 同属官方域，ZCode 自己融合成
        // 一个订阅商内部切换），不收编为供应商卡，官方状态由「ZCode 官方」
        // 卡表达。current 跟踪启用中的自建条目（托管 llmswitch:* 优先，
        // 原生条目 enabled 缺省视为启用）：仅官方条目启用时保持为空。
        const auto file = cfg::zcodeConfigFile();
        if (!std::filesystem::exists(file, ec)) return {};
        const auto j = readJsonOrNull(file);
        if (!j.is_object() || !j.contains("provider") || !j["provider"].is_object()) {
            return {};
        }
        // 旧版本曾把 builtin:* 收编成供应商卡；改为官方域后启动同步时
        // 清掉残留卡（current 若指向它们由下方重算）。
        for (auto pit = g.providers.begin(); pit != g.providers.end();) {
            if (pit->id.starts_with("builtin:")) {
                pit = g.providers.erase(pit);
            } else {
                ++pit;
            }
        }
        std::string currentKey;
        // current 两段式：优先本应用托管（llmswitch:*）的启用条目，否则
        // ZCode 原生自建条目（enabled 缺省视为启用；builtin:* 是官方域，
        // 不参与）。
        for (int pass = 0; pass < 2 && currentKey.empty(); ++pass) {
            for (auto it = j["provider"].begin(); it != j["provider"].end();
                 ++it) {
                if (!it.value().is_object()) continue;
                const bool ours = it.key().starts_with("llmswitch:");
                if ((pass == 0) != ours) continue;
                if (pass == 1 && it.key().starts_with("builtin:")) continue;
                if (zcodeEntryOn(it.value())) {
                    currentKey = it.key();
                    break;
                }
            }
        }
        std::string currentId;
        for (auto it = j["provider"].begin(); it != j["provider"].end(); ++it) {
            const auto& entry = it.value();
            if (!entry.is_object()) continue;
            if (it.key().starts_with("builtin:")) continue;
            const auto& options = entry["options"];
            const std::string baseUrl =
                options.is_object() ? jsonStr(options, "baseURL") : "";
            const std::string apiKey =
                options.is_object() ? jsonStr(options, "apiKey") : "";
            if (baseUrl.empty() && apiKey.empty()) continue;
            models::Provider p;
            p.name = jsonStr(entry, "name");
            if (p.name.empty()) p.name = it.key();
            p.apiFormat =
                jsonStr(entry, "kind") == "anthropic" ? "anthropic" : "openai-chat";
            p.baseUrl = baseUrl;
            p.apiKey = apiKey;
            if (entry.contains("models") && entry["models"].is_object()) {
                for (auto mit = entry["models"].begin();
                     mit != entry["models"].end(); ++mit) {
                    p.models.push_back(mit.key());
                    if (mit.value().is_object()) {
                        p.modelsMeta[mit.key()] = mit.value();
                    }
                }
                if (!p.models.empty()) p.model = p.models.front();
            }
            // 条目键即身份：同端点+同密钥的两条自建条目也是两个供应商，
            // 不得按端点+密钥合并（ZCode 页面显示几条就收编几条）；
            // llmswitch: 前缀还原为原始 id，本应用创建的供应商原位更新。
            const std::string preferred =
                it.key().starts_with("llmswitch:") ? it.key().substr(10)
                                                   : it.key();
            models::Provider* slot = nullptr;
            for (auto& cur : g.providers) {
                if (cur.id == preferred) {
                    slot = &cur;
                    break;
                }
            }
            if (slot == nullptr) {
                p.id = preferred;
                p.createdAt = nowMillis();
                g.providers.push_back(p);
            } else {
                slot->name = p.name;
                slot->apiFormat = p.apiFormat;
                slot->model = p.model;
                slot->models = p.models;
                slot->modelsMeta = p.modelsMeta;
            }
            if (it.key() == currentKey) {
                currentId = slot != nullptr ? slot->id : p.id;
            }
        }
        if (currentId.empty()) {
            // 没有任何启用的自建条目（仅 builtin 原生启用或全部停用）
            // = 官方原生状态，current 置空让「ZCode 官方」卡亮起。
            g.current.clear();
            save();
            return {};
        }
        g.current = currentId;
        for (const auto& p : g.providers) {
            if (p.id == currentId) {
                save();
                return p;
            }
        }
        return {};
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
