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

// 条目键 → 组内 id：本应用托管的 llmswitch:<id> 剥前缀，ZCode 原生条目
// （键就是 ZCode 自己生成的 id）原样当 id。
std::string zcodeProviderIdFor(std::string_view key) {
    return key.starts_with("llmswitch:") ? std::string(key.substr(10))
                                         : std::string(key);
}

// config.json 的一个 provider 条目 → Provider。收编与全量导入共用，保证两边
// 读出来的字段一致；name 缺失回退条目键，kind 走 apiFormat 三档归一。
models::Provider zcodeProviderFromEntry(std::string_view key,
                                        const nlohmann::json& entry) {
    models::Provider p;
    p.id = zcodeProviderIdFor(key);
    p.name = entry.is_object() ? jsonStr(entry, "name") : "";
    if (p.name.empty()) p.name = std::string(key);
    p.apiFormat = entry.is_object() && jsonStr(entry, "kind") == "anthropic"
                      ? "anthropic"
                      : "openai-chat";
    const auto& options = entry.is_object() ? entry["options"] : nlohmann::json();
    if (options.is_object()) {
        p.baseUrl = jsonStr(options, "baseURL");
        p.apiKey = jsonStr(options, "apiKey");
    }
    if (entry.is_object() && entry.contains("models") &&
        entry["models"].is_object()) {
        for (auto mit = entry["models"].begin(); mit != entry["models"].end();
             ++mit) {
            p.models.push_back(mit.key());
            if (mit.value().is_object()) p.modelsMeta[mit.key()] = mit.value();
        }
        if (!p.models.empty()) p.model = p.models.front();
    }
    return p;
}

// 保存同步：把 provider 原位写成 config.json 的条目——优先已有的
// llmswitch:<id>，ZCode 原生自建条目（键 = id）直接原位更新不另起重复
// 条目；已存在时保留其 enabled（含缺省，字段不补写），新建时 enabled 置
// false（启用走显式开关）。与 ZCode 页面的第三方供应商一一对应，无需
// 切换即可改清单。
void ProviderStore::upsertZcodeEntry(const models::Provider& provider) {
    writeZcodeEntry(provider, /*overwrite=*/true, /*makeEnabled=*/false);
}

// 单条增量写入：只动自己这一条。overwrite=false 且条目已在 live 里时保持
// 内容原样（只按需翻 enabled）——「设为启用」不该覆盖用户在 ZCode 侧的手改；
// overwrite=true 时整条重建（原生条目走 mergeZcodeEntry 原位合并，它自己维护
// 的其它字段与 options 键保留）。没有实际变化就不碰文件（不备份、不写）。
void ProviderStore::writeZcodeEntry(const models::Provider& p, bool overwrite,
                                    bool makeEnabled) {
    const auto file = cfg::zcodeConfigFile();
    nlohmann::json doc = readJsonOrNull(file);
    if (!doc.is_object()) doc = nlohmann::json::object();
    if (!doc.contains("provider") || !doc["provider"].is_object()) {
        doc["provider"] = nlohmann::json::object();
    }
    auto& providers = doc["provider"];
    const std::string entryKey = zcodeEntryKeyFor(providers, p.id);
    const auto existing = providers.find(entryKey);
    const bool hasEntry = existing != providers.end() && existing->is_object();
    if (hasEntry && !overwrite && !makeEnabled) return;

    nlohmann::json entry = buildZcodeEntry(p, models::effectiveBaseUrl(p));
    if (hasEntry) {
        entry = mergeZcodeEntry(*existing, entry);
        if (!makeEnabled) {
            // 内容与启用状态都没变：不写文件。
            if (entry == *existing) return;
        } else {
            entry["enabled"] = true;
            if (entry == *existing) return;
        }
    } else {
        entry["enabled"] = makeEnabled;
    }
    backupLiveFile("zcode", file);
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

// live provider map 实况（只读）：每条给出页面左列需要的字段与「是否已纳管」。
// 组内 id → 条目的对应关系反向算：先看 llmswitch:<id>，再看原生键 <id>
// （与 zcodeEntryKeyFor 同一套规则），命中即已纳管。
std::vector<ZcodeLiveProvider> ProviderStore::zcodeLiveProviders() const {
    std::vector<ZcodeLiveProvider> out;
    const auto doc = readJsonOrNull(cfg::zcodeConfigFile());
    if (!doc.is_object() || !doc.contains("provider") ||
        !doc["provider"].is_object()) {
        return out;
    }
    const auto& g = group("zcode");
    for (auto it = doc["provider"].begin(); it != doc["provider"].end(); ++it) {
        const auto& entry = it.value();
        if (!entry.is_object()) continue;
        ZcodeLiveProvider live;
        live.key = it.key();
        live.builtin = live.key.starts_with("builtin:");
        const models::Provider parsed = zcodeProviderFromEntry(live.key, entry);
        live.displayName = parsed.name;
        live.baseUrl = parsed.baseUrl;
        live.kind = jsonStr(entry, "kind");
        live.apiFormat = parsed.apiFormat;
        live.apiKey = parsed.apiKey;
        live.model = parsed.model;
        live.models = parsed.models;
        live.enabled = zcodeEntryOn(entry);
        if (!live.builtin) {
            for (const auto& p : g.providers) {
                if (p.id == parsed.id &&
                    zcodeEntryKeyFor(doc["provider"], p.id) == live.key) {
                    live.providerId = p.id;
                    break;
                }
            }
        }
        out.push_back(std::move(live));
    }
    return out;
}

// 右列「写入 / 更新」：只重建这一条（原生键的原位更新，保留它自己的字段）。
void ProviderStore::writeZcodeProvider(const std::string& id) {
    const auto& g = group("zcode");
    for (const auto& p : g.providers) {
        if (p.id != id) continue;
        writeZcodeEntry(p, /*overwrite=*/true, /*makeEnabled=*/false);
        return;
    }
    throw std::runtime_error(std::format("供应商不存在：{}", id));
}

// 收编：把 live 条目记进本地列表。live 一字不动——ZCode 原生条目保持它自己
// 的键与内容（本应用只是开始管理它），llmswitch:<id> 条目本来就是我们的形状。
models::Provider ProviderStore::adoptZcodeProvider(const std::string& key) {
    auto& g = groupRef("zcode");
    const auto file = cfg::zcodeConfigFile();
    const auto doc = readJsonOrNull(file);
    if (!doc.is_object() || !doc.contains("provider") ||
        !doc["provider"].is_object() || !doc["provider"].contains(key) ||
        !doc["provider"][key].is_object()) {
        throw std::runtime_error(std::format("ZCode 配置里没有条目：{}", key));
    }
    if (key.starts_with("builtin:")) {
        throw std::runtime_error(
            std::format("ZCode 官方套餐条目不能收编：{}", key));
    }
    models::Provider p = zcodeProviderFromEntry(key, doc["provider"][key]);
    const bool enabled = zcodeEntryOn(doc["provider"][key]);
    bool replaced = false;
    for (auto& cur : g.providers) {
        if (cur.id != p.id) continue;
        const std::int64_t created = cur.createdAt;
        cur = p;
        cur.createdAt = created;
        replaced = true;
        break;
    }
    if (!replaced) {
        p.createdAt = nowMillis();
        g.providers.push_back(p);
    }
    if (enabled) g.current = p.id;
    save();
    return p;
}

// 从 config.json 删掉这一条：只删它，别的条目（含 builtin:* 与 ZCode 原生
// 条目）一字不动。key 不存在也报错，避免用户以为删掉了。
void ProviderStore::removeZcodeProvider(const std::string& key) {
    const auto file = cfg::zcodeConfigFile();
    if (!eraseZcodeEntry(key)) {
        throw std::runtime_error(std::format("ZCode 配置里没有条目：{}", key));
    }
}

// 在 ZCode 中启用 live 里的某一条（左列每行的「在 ZCode 中启用」）：只翻
// enabled——这一条置 true，其余本应用托管的 llmswitch:* 条目停用（本应用只能
// 保证自己那几条互斥，ZCode 原生条目与 builtin:* 的启停不代管），其它字段
// 一字不动。builtin:* 与不存在的键都抛错。
void ProviderStore::enableZcodeKey(const std::string& key) {
    if (key.starts_with("builtin:")) {
        throw std::runtime_error(
            std::format("ZCode 官方套餐条目不由本应用启用：{}", key));
    }
    const auto file = cfg::zcodeConfigFile();
    nlohmann::json doc = readJsonOrNull(file);
    if (!doc.is_object() || !doc.contains("provider") ||
        !doc["provider"].is_object() || !doc["provider"].contains(key) ||
        !doc["provider"][key].is_object()) {
        throw std::runtime_error(std::format("ZCode 配置里没有条目：{}", key));
    }
    auto& providers = doc["provider"];
    bool changed = false;
    if (!zcodeEntryOn(providers[key])) {
        providers[key]["enabled"] = true;
        changed = true;
    }
    for (auto it = providers.begin(); it != providers.end(); ++it) {
        if (it.key() == key || !it.key().starts_with("llmswitch:") ||
            !it.value().is_object()) {
            continue;
        }
        if (zcodeEntryOn(it.value())) {
            it.value()["enabled"] = false;
            changed = true;
        }
    }
    if (changed) {
        backupLiveFile("zcode", file);
        atomicWrite(file, doc.dump(2) + "\n");
    }
    // 组内 current 跟着走：启用的是本应用条目（llmswitch:<id>）就记成它，
    // 否则清空——启用了未纳管的原生条目时，本应用没有「正在生效的供应商」。
    auto& g = groupRef("zcode");
    const std::string id = zcodeProviderIdFor(key);
    std::string next;
    for (const auto& p : g.providers) {
        if (p.id == id) next = id;
    }
    if (g.current == next) return;
    g.current = next;
    save();
}

// 只从 live 删掉这个键。builtin:* 抛错；键不存在返回 false（不写文件）。
bool ProviderStore::eraseZcodeEntry(const std::string& key) {
    if (key.starts_with("builtin:")) {
        throw std::runtime_error(
            std::format("ZCode 官方套餐条目不能删除：{}", key));
    }
    const auto file = cfg::zcodeConfigFile();
    nlohmann::json doc = readJsonOrNull(file);
    if (!doc.is_object() || !doc.contains("provider") ||
        !doc["provider"].is_object() || !doc["provider"].contains(key)) {
        return false;
    }
    backupLiveFile("zcode", file);
    doc["provider"].erase(key);
    atomicWrite(file, doc.dump(2) + "\n");
    return true;
}

// 显式整组重建（「全部写入 ZCode」按钮）：把组内每条写成它对应的条目
// （已有 llmswitch:<id> 或原生键原位更新，都没有则新建 llmswitch:<id>），
// 并清掉不再属于组内的孤儿 llmswitch:* 条目。builtin:* 与 ZCode 原生条目
// 一律不动——那是 ZCode 自己的配置。
void ProviderStore::syncZcodeProviders() {
    auto& g = groupRef("zcode");
    const auto file = cfg::zcodeConfigFile();
    nlohmann::json doc = readJsonOrNull(file);
    if (!doc.is_object()) doc = nlohmann::json::object();
    if (!doc.contains("provider") || !doc["provider"].is_object()) {
        doc["provider"] = nlohmann::json::object();
    }
    nlohmann::json next = doc["provider"];
    std::set<std::string> kept;
    for (const auto& p : g.providers) {
        const std::string key = zcodeEntryKeyFor(doc["provider"], p.id);
        nlohmann::json entry = buildZcodeEntry(p, models::effectiveBaseUrl(p));
        const auto existing = doc["provider"].find(key);
        if (existing != doc["provider"].end() && existing->is_object()) {
            entry = mergeZcodeEntry(*existing, entry);
        } else {
            entry["enabled"] = false;
        }
        kept.insert(key);
        next[key] = std::move(entry);
    }
    // 清孤儿：不在组内、且是本应用托管的 llmswitch:* 条目。
    for (auto it = doc["provider"].begin(); it != doc["provider"].end(); ++it) {
        if (it.key().starts_with("llmswitch:") && !kept.contains(it.key())) {
            next.erase(it.key());
        }
    }
    if (next == doc["provider"]) return;
    backupLiveFile("zcode", file);
    doc["provider"] = std::move(next);
    atomicWrite(file, doc.dump(2) + "\n");
}

} // namespace store
