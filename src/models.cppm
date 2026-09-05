// models.cppm — llmswitch.models：工具注册表、供应商数据模型与 config.json 序列化。
//
// 数据形态对齐 cc-switch：一个工具一组供应商，组内 current 指向当前激活项；
// 切换时由 llmswitch.store 把选中项写进该工具的 live 配置文件。工具集合由
// toolRegistry() 注册表定义（claude-code / claude / codex / opencode / pi），
// AppConfig.groups 以注册表 id 为键——新增工具只需扩展注册表与 store 的
// writer/detect/import 分发。
// JSON 序列化用 nlohmann::json 模块 —— 该模块下禁用
// `.items()` 结构化绑定（迭代器退化问题），遍历时用 it.key()/it.value()。
export module llmswitch.models;

import std;
import nlohmann.json;

namespace models {

// ---- 工具注册表 ---------------------------------------------------------------

// 一个受管工具（agent）的静态描述。live 文件路径在 llmswitch.config，
// 切换/探测/收编的分发在 llmswitch.store。
export struct ToolSpec {
    std::string_view id;            // config.json groups 的键，稳定不本地化
    std::string_view displayName;   // UI 展示名
    std::string_view iconName;      // resources 图标名（app::images::<iconName>）
    bool needsModel;    // 是否有「默认模型」字段（opencode / pi）
    bool hasApiFormat;  // 是否有 apiFormat 字段（opencode / pi）
};

// 注册表顺序即 UI 侧栏/托盘菜单顺序。
export constexpr std::array<ToolSpec, 5> kToolRegistry{{
    ToolSpec{.id = "claude-code",
             .displayName = "Claude Code",
             .iconName = "claudecode",
             .needsModel = false,
             .hasApiFormat = false},
    ToolSpec{.id = "claude",
             .displayName = "Claude Desktop",
             .iconName = "claude",
             .needsModel = false,
             .hasApiFormat = false},
    ToolSpec{.id = "codex",
             .displayName = "Codex",
             .iconName = "codex",
             .needsModel = false,
             .hasApiFormat = false},
    ToolSpec{.id = "opencode",
             .displayName = "opencode",
             .iconName = "opencode",
             .needsModel = true,
             .hasApiFormat = true},
    ToolSpec{.id = "pi",
             .displayName = "Pi",
             .iconName = "pi",
             .needsModel = true,
             .hasApiFormat = true},
}};

export std::span<const ToolSpec> toolRegistry() { return kToolRegistry; }

// 按 id 查注册表；未注册返回 nullptr。
export const ToolSpec* findTool(std::string_view id) {
    for (const auto& t : kToolRegistry) {
        if (t.id == id) return &t;
    }
    return nullptr;
}

// ---- 数据模型 -----------------------------------------------------------------

export struct Provider {
    std::string id;         // 生成：毫秒时间戳 + 随机 hex（见 llmswitch.store）
    std::string name;
    std::string baseUrl;
    std::string apiKey;
    std::string model;          // 可选，空 = 切换时不写 env.ANTHROPIC_MODEL
    std::string website;
    std::string notes;
    std::string codexConfigToml;  // 仅 codex 组用：config.toml 整段原文（空 = 切换时不改 config.toml）
    std::string apiFormat;        // 仅 opencode / pi 组用："" / "openai"（默认）/
                                  // "openai-responses" / "anthropic"
    std::int64_t createdAt = 0;   // 毫秒

    bool operator==(const Provider&) const = default;
};

// 一个工具的供应商集合。
export struct ProviderGroup {
    std::vector<Provider> providers;
    std::string current;  // 当前激活 provider id，空 = 未设置

    bool operator==(const ProviderGroup&) const = default;
};

export struct AppConfig {
    // 键 = 注册表工具 id（claude-code / claude / codex / opencode / pi）；
    // 用 map 而非固定字段，新增工具不改序列化结构。
    std::map<std::string, ProviderGroup> groups;
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
    j["apiFormat"] = p.apiFormat;
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
    p.apiFormat = j.value("apiFormat", "");
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
    j["groups"] = nlohmann::json::object();
    for (const auto& [id, g] : c.groups) {
        j["groups"][id] = toJson(g);
    }
    j["themeMode"] = c.themeMode;
    return j;
}

export AppConfig fromJson(const nlohmann::json& j) {
    AppConfig c;
    if (!j.is_object()) return c;
    // 新格式：{"groups": {"<toolId>": {...}}}。
    if (j.contains("groups") && j["groups"].is_object()) {
        for (auto it = j["groups"].begin(); it != j["groups"].end(); ++it) {
            c.groups[it.key()] = groupFromJson(it.value());
        }
    }
    // 旧格式迁移（v1：顶层 "claude" / "codex" 两个固定组）；新格式已有的键
    // 优先，旧键只补空缺。
    if (j.contains("claude") && !c.groups.contains("claude-code")) {
        c.groups["claude-code"] = groupFromJson(j["claude"]);
    }
    if (j.contains("codex") && !c.groups.contains("codex")) {
        c.groups["codex"] = groupFromJson(j["codex"]);
    }
    c.themeMode = j.value("themeMode", "system");
    return c;
}

// ---- 内置预设 -----------------------------------------------------------------
// 新建供应商时的模板：name/baseUrl/website 填好，apiKey 一律留空由用户填。
// 字段不确定时宁可留空也不编造；id/createdAt 由 store 在添加时生成。
// 参数为注册表工具 id。
export std::vector<Provider> builtinPresets(std::string_view tool) {
    if (tool == "claude-code") {
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
    if (tool == "opencode" || tool == "pi") {
        // OpenAI 兼容端点（opencode 走 @ai-sdk/openai-compatible；pi 走
        // openai-completions），model 填各家的主力模型。
        return {
            Provider{.name = "DeepSeek",
                     .baseUrl = "https://api.deepseek.com/v1",
                     .model = "deepseek-chat",
                     .website = "https://platform.deepseek.com"},
            Provider{.name = "Kimi（Moonshot）",
                     .baseUrl = "https://api.moonshot.cn/v1",
                     .model = "kimi-k2-0905-preview",
                     .website = "https://platform.moonshot.cn"},
        };
    }
    // claude（Claude Desktop 3p 直连）：暂无可靠公共端点预设。
    return {};
}

} // namespace models
