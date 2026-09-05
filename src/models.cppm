// models.cppm — llmswitch.models：供应商数据模型与 config.json 序列化。
//
// 数据形态对齐 cc-switch：一个工具（claude / codex）一组供应商，组内
// current 指向当前激活项；切换时由 llmswitch.store 把选中项写进该工具的
// live 配置文件。JSON 序列化用 nlohmann::json 模块 —— 该模块下禁用
// `.items()` 结构化绑定（迭代器退化问题），遍历时用 it.key()/it.value()。
export module llmswitch.models;

import std;
import nlohmann.json;

namespace models {

export struct Provider {
    std::string id;         // 生成：毫秒时间戳 + 随机 hex（见 llmswitch.store）
    std::string name;
    std::string baseUrl;
    std::string apiKey;
    std::string model;          // 可选，空 = 切换时不写 env.ANTHROPIC_MODEL
    std::string website;
    std::string notes;
    std::string codexConfigToml;  // 仅 codex 组用：config.toml 整段原文（空 = 切换时不改 config.toml）
    std::int64_t createdAt = 0;   // 毫秒

    bool operator==(const Provider&) const = default;
};

// 一个工具（claude / codex）的供应商集合。
export struct ProviderGroup {
    std::vector<Provider> providers;
    std::string current;  // 当前激活 provider id，空 = 未设置

    bool operator==(const ProviderGroup&) const = default;
};

export struct AppConfig {
    ProviderGroup claude;
    ProviderGroup codex;
    std::string themeMode = "system";  // system / dark / light

    bool operator==(const AppConfig&) const = default;
};

// ---- JSON 序列化 -------------------------------------------------------------

export nlohmann::json toJson(const Provider& p) {
    nlohmann::json j;
    j["id"] = p.id;
    j["name"] = p.name;
    j["baseUrl"] = p.baseUrl;
    j["apiKey"] = p.apiKey;
    j["model"] = p.model;
    j["website"] = p.website;
    j["notes"] = p.notes;
    j["codexConfigToml"] = p.codexConfigToml;
    j["createdAt"] = p.createdAt;
    return j;
}

export Provider providerFromJson(const nlohmann::json& j) {
    Provider p;
    if (!j.is_object()) return p;
    p.id = j.value("id", "");
    p.name = j.value("name", "");
    p.baseUrl = j.value("baseUrl", "");
    p.apiKey = j.value("apiKey", "");
    p.model = j.value("model", "");
    p.website = j.value("website", "");
    p.notes = j.value("notes", "");
    p.codexConfigToml = j.value("codexConfigToml", "");
    p.createdAt = j.value("createdAt", std::int64_t{0});
    return p;
}

export nlohmann::json toJson(const ProviderGroup& g) {
    nlohmann::json j;
    j["providers"] = nlohmann::json::array();
    for (const auto& p : g.providers) j["providers"].push_back(toJson(p));
    j["current"] = g.current;
    return j;
}

export ProviderGroup groupFromJson(const nlohmann::json& j) {
    ProviderGroup g;
    if (!j.is_object()) return g;
    if (j.contains("providers") && j["providers"].is_array()) {
        for (const auto& item : j["providers"]) {
            g.providers.push_back(providerFromJson(item));
        }
    }
    g.current = j.value("current", "");
    return g;
}

export nlohmann::json toJson(const AppConfig& c) {
    nlohmann::json j;
    j["claude"] = toJson(c.claude);
    j["codex"] = toJson(c.codex);
    j["themeMode"] = c.themeMode;
    return j;
}

export AppConfig fromJson(const nlohmann::json& j) {
    AppConfig c;
    if (!j.is_object()) return c;
    if (j.contains("claude")) c.claude = groupFromJson(j["claude"]);
    if (j.contains("codex")) c.codex = groupFromJson(j["codex"]);
    c.themeMode = j.value("themeMode", "system");
    return c;
}

// ---- 内置预设 -----------------------------------------------------------------
// 新建供应商时的模板：name/baseUrl/website 填好，apiKey 一律留空由用户填。
// 字段不确定时宁可留空也不编造；id/createdAt 由 store 在添加时生成。
export std::vector<Provider> builtinPresets(std::string_view tool) {
    if (tool == "claude") {
        // Anthropic 兼容端点（Claude Code 走 ANTHROPIC_BASE_URL）。
        return {
            Provider{.name = "DeepSeek",
                     .baseUrl = "https://api.deepseek.com/anthropic",
                     .website = "https://platform.deepseek.com"},
            Provider{.name = "Kimi（Moonshot）",
                     .baseUrl = "https://api.moonshot.cn/anthropic",
                     .website = "https://platform.moonshot.cn"},
            Provider{.name = "GLM（智谱）",
                     .baseUrl = "https://open.bigmodel.cn/api/anthropic",
                     .website = "https://open.bigmodel.cn"},
        };
    }
    if (tool == "codex") {
        // OpenAI 兼容端点（Codex 走 auth.json 的 OPENAI_API_KEY + config.toml 的
        // model_providers 段；wire_api 用各家都支持的 chat completions，model
        // 以注释提示，避免写死一个用户没有的模型）。
        return {
            Provider{.name = "OpenRouter",
                     .baseUrl = "https://openrouter.ai/api/v1",
                     .website = "https://openrouter.ai",
                     .codexConfigToml =
                         R"toml(model_provider = "openrouter"
# model = "openai/gpt-5"   # 按需填写要使用的模型

[model_providers.openrouter]
name = "OpenRouter"
base_url = "https://openrouter.ai/api/v1"
wire_api = "chat"
)toml"},
            Provider{.name = "DeepSeek",
                     .baseUrl = "https://api.deepseek.com/v1",
                     .website = "https://platform.deepseek.com",
                     .codexConfigToml =
                         R"toml(model_provider = "deepseek"
# model = "deepseek-chat"   # 按需填写要使用的模型

[model_providers.deepseek]
name = "DeepSeek"
base_url = "https://api.deepseek.com/v1"
wire_api = "chat"
)toml"},
        };
    }
    return {};
}

} // namespace models
