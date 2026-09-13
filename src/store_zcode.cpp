// store_zcode.cpp — llmswitch.store 实现单元：ZCode config.json 条目同步。
//
// 保存同步 upsertZcodeEntry（键解析优先 llmswitch:<id>，原生裸 id 条目
// 原位合并，enabled 字段缺省 = 启用）与启停开关 setZcodeEntryEnabled /
// zcodeEntryEnabled；条目构造与合并助手同时被 store_live.cpp 的
// switchTo / importLive / detectCurrent 使用（模块链接声明在 store.cppm）。
module llmswitch.store;

import std;
import nlohmann.json;
import llmswitch.config;
import llmswitch.models;

namespace store {

// ZCode provider 条目（config.json 的 provider map 值）：字段与 ZCode 自建
// 条目一一对应。enabled 由调用方决定（switchTo 置 true；保存同步保留原值，
// 新建默认停用）。kind 是 ZCode 的规范拼写 openai-compatible——ZCode 会把
// 其他写法归一成它，直接写规范值省一次外部回写。
nlohmann::json buildZcodeEntry(const models::Provider& target,
                               const std::string& baseUrl) {
    nlohmann::json entry;
    entry["name"] = target.name;
    entry["kind"] = models::normalizeApiFormat(target.apiFormat) == "anthropic"
                        ? "anthropic"
                        : "openai-compatible";
    entry["options"]["apiKey"] = target.apiKey;
    entry["options"]["baseURL"] = baseUrl;
    entry["source"] = "custom";
    // models 是不定长清单。ZCode 的存储结构里没有主模型概念（条目只有
    // 模型清单，选模型是运行时行为），所以只写清单本身——不把主模型强行
    // 塞进去；清单为空而主模型非空时退化为单模型清单。每个模型的参数
    // （reasoning/limit/modalities 等）按收编时的原值回放，没有原值的写
    // ZCode 兼容的最小条目。
    if (!target.models.empty() || !target.model.empty()) {
        nlohmann::json modelsMap = nlohmann::json::object();
        const auto put = [&target, &modelsMap](const std::string& id) {
            if (target.modelsMeta.contains(id) &&
                target.modelsMeta[id].is_object()) {
                modelsMap[id] = target.modelsMeta[id];
            } else {
                modelsMap[id] = nlohmann::json::object(
                    {{"zcode", nlohmann::json::object({{"priority", 100}})}});
            }
        };
        if (!target.models.empty()) {
            for (const auto& id : target.models) put(id);
        } else {
            put(target.model);
        }
        entry["models"] = std::move(modelsMap);
    }
    return entry;
}

// provider id → config.json 条目键：优先本应用托管的 llmswitch:<id>；否则
// 复用 ZCode 原生条目（收编身份 = 条目键，ZCode 自建第三方供应商的键是裸
// id）；都不存在才落到 llmswitch:<id>（新建）。启用状态读写、保存同步、
// 切换统一走这里，避免把原生条目撇下另起重复条目。
std::string zcodeEntryKeyFor(const nlohmann::json& providers,
                             const std::string& id) {
    const std::string ours = "llmswitch:" + id;
    if (providers.contains(ours)) return ours;
    if (providers.contains(id)) return id;
    return ours;
}

// 条目是否启用：ZCode 原生条目不写 enabled 字段（省略 = 启用，显式 false
// 才是停用），缺省读 true；本应用写入的条目恒有显式值，不受缺省影响。
bool zcodeEntryOn(const nlohmann::json& entry) {
    return entry.is_object() && entry.value("enabled", true);
}

// 用本应用负责的字段（name/kind/options/models/source）覆盖条目，其余
// 字段（enabled、options.apiKeyRequired 等 ZCode 自己维护的键）原样保留。
nlohmann::json mergeZcodeEntry(const nlohmann::json& existing,
                               const nlohmann::json& built) {
    nlohmann::json merged =
        existing.is_object() ? existing : nlohmann::json::object();
    for (const char* field : {"name", "kind", "source"}) {
        merged[field] = built[field];
    }
    if (!merged.contains("options") || !merged["options"].is_object()) {
        merged["options"] = nlohmann::json::object();
    }
    for (auto opt = built["options"].begin(); opt != built["options"].end();
         ++opt) {
        merged["options"][opt.key()] = opt.value();
    }
    if (built.contains("models")) {
        merged["models"] = built["models"];
    } else {
        merged.erase("models");
    }
    return merged;
}

// 保存同步：把 provider 原位写成 config.json 的条目——优先已有的
// llmswitch:<id>，ZCode 原生自建条目（键 = id）直接原位更新不另起重复
// 条目；已存在时保留其 enabled（含缺省，字段不补写），新建时 enabled 置
// false（启用走显式开关）。与 ZCode 页面的第三方供应商一一对应，无需
// 切换即可改清单。
void ProviderStore::upsertZcodeEntry(const models::Provider& provider) {
    const auto file = cfg::zcodeConfigFile();
    nlohmann::json doc = readJsonOrNull(file);
    if (!doc.is_object()) doc = nlohmann::json::object();
    if (!doc.contains("provider") || !doc["provider"].is_object()) {
        doc["provider"] = nlohmann::json::object();
    }
    backupLiveFile("zcode", file);
    auto& providers = doc["provider"];
    const std::string entryKey = zcodeEntryKeyFor(providers, provider.id);
    nlohmann::json entry =
        buildZcodeEntry(provider, models::effectiveBaseUrl(provider));
    const auto existing = providers.find(entryKey);
    if (existing != providers.end() && existing->is_object()) {
        entry = mergeZcodeEntry(*existing, entry);
    } else {
        entry["enabled"] = false;
    }
    providers[entryKey] = std::move(entry);
    atomicWrite(file, doc.dump(2) + "\n");
}

// 查询 ZCode 里条目的启用状态；条目不存在返回 false，enabled 字段缺省
// 视为启用（ZCode 原生条目不写该字段）。
bool ProviderStore::zcodeEntryEnabled(const std::string& id) const {
    const auto j = readJsonOrNull(cfg::zcodeConfigFile());
    if (!j.is_object() || !j.contains("provider") ||
        !j["provider"].is_object()) {
        return false;
    }
    const std::string key = zcodeEntryKeyFor(j["provider"], id);
    const auto entry = j["provider"].find(key);
    return entry != j["provider"].end() ? zcodeEntryOn(*entry) : false;
}

// 启用/停用 ZCode 里的条目（仅翻该条目的 enabled，不动其他条目，也不改
// 组内 current——启用互斥仍只在 switchTo 发生）；条目不存在抛
// std::runtime_error。
void ProviderStore::setZcodeEntryEnabled(const std::string& id, bool enabled) {
    const auto file = cfg::zcodeConfigFile();
    nlohmann::json doc = readJsonOrNull(file);
    if (!doc.is_object() || !doc.contains("provider") ||
        !doc["provider"].is_object()) {
        throw std::runtime_error(std::format("ZCode 条目不存在：{}", id));
    }
    const std::string key = zcodeEntryKeyFor(doc["provider"], id);
    if (!doc["provider"].contains(key) ||
        !doc["provider"][key].is_object()) {
        throw std::runtime_error(std::format("ZCode 条目不存在：{}", id));
    }
    backupLiveFile("zcode", file);
    doc["provider"][key]["enabled"] = enabled;
    atomicWrite(file, doc.dump(2) + "\n");
}

} // namespace store
