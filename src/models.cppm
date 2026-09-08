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
    // 是否有三档模型映射字段 haiku/sonnet/opus（claude-code 写
    // ANTHROPIC_DEFAULT_*_MODEL env；claude desktop 写 inferenceModels 映射条目）
    bool hasModelMappings;
};

// 注册表顺序即 UI 侧栏/托盘菜单顺序。
export constexpr std::array<ToolSpec, 5> kToolRegistry{{
    ToolSpec{.id = "claude-code",
             .displayName = "Claude Code",
             .iconName = "claudecode",
             .needsModel = false,
             .hasApiFormat = false,
             .hasModelMappings = true},
    ToolSpec{.id = "claude",
             .displayName = "Claude Desktop",
             .iconName = "claude",
             .needsModel = false,
             .hasApiFormat = false,
             .hasModelMappings = true},
    ToolSpec{.id = "codex",
             .displayName = "Codex",
             .iconName = "codex",
             .needsModel = false,
             .hasApiFormat = false,
             .hasModelMappings = false},
    ToolSpec{.id = "opencode",
             .displayName = "opencode",
             .iconName = "opencode",
             .needsModel = true,
             .hasApiFormat = true,
             .hasModelMappings = false},
    ToolSpec{.id = "pi",
             .displayName = "Pi",
             .iconName = "pi",
             .needsModel = true,
             .hasApiFormat = true,
             .hasModelMappings = false},
}};

export std::span<const ToolSpec> toolRegistry() { return kToolRegistry; }

// 按 id 查注册表；未注册返回 nullptr。
export const ToolSpec* findTool(std::string_view id) {
    for (const auto& t : kToolRegistry) {
        if (t.id == id) return &t;
    }
    return nullptr;
}

// ---- API 协议（三档）-----------------------------------------------------------
// Provider.apiFormat 语义 = 供应商端点所说的协议。序列化存原值（"" 即默认档），
// 判定/显示/映射一律经 normalizeApiFormat 归一，各处不各自解释字符串。

// 归一化到三档之一："openai-chat"（OpenAI Chat Completions，默认）/
// "openai-responses"（OpenAI Responses）/ "anthropic"（Anthropic Messages
// 原生）。旧值 "openai" 与未知值都归到默认档。
export std::string_view normalizeApiFormat(std::string_view apiFormat) {
    if (apiFormat == "anthropic") return "anthropic";
    if (apiFormat == "openai-responses") return "openai-responses";
    return "openai-chat";
}

// 显示名（表单/详情用）。
export std::string_view apiFormatLabel(std::string_view apiFormat) {
    const auto f = normalizeApiFormat(apiFormat);
    if (f == "anthropic") return "Anthropic Messages（原生）";
    if (f == "openai-responses") return "OpenAI Responses";
    return "OpenAI Chat Completions";
}

// ---- 数据模型 -----------------------------------------------------------------

export struct Provider {
    std::string id;         // 生成：毫秒时间戳 + 随机 hex（见 llmswitch.store）
    std::string name;
    std::string baseUrl;
    std::string modelFetchUrl;   // 可选，空 = 按 baseUrl + 上游格式自动获取模型
    std::string apiKey;
    std::string model;          // 可选，空 = 切换时不写 env.ANTHROPIC_MODEL
    // 三档模型映射（仅 hasModelMappings 工具：claude-code / claude，均选填）：
    // * *Model 是发送给上游的实际模型 ID；*DisplayName 是 Claude Desktop
    //   菜单显示名；*Supports1m 是写入 inferenceModels 的 1M 能力声明。
    // claude-code 切换时写 env.ANTHROPIC_DEFAULT_HAIKU/SONNET/OPUS_MODEL；
    // claude desktop 切换时每档写成 inferenceModels 的一个映射条目。
    std::string haikuModel;
    std::string sonnetModel;
    std::string opusModel;
    std::string haikuDisplayName;
    std::string sonnetDisplayName;
    std::string opusDisplayName;
    bool haikuSupports1m = false;
    bool sonnetSupports1m = false;
    bool opusSupports1m = false;
    std::string website;
    std::string notes;
    std::string codexConfigToml;  // 仅 codex 组用：config.toml 整段原文（空 = 切换时不改 config.toml）
    std::string apiFormat;        // 仅 opencode / pi 组用："" / "openai-chat"（默认）/
                                  // "openai-responses" / "anthropic"
    // 用量查询（可选；usageUrl 空 = 不查）：
    std::string usageUrl;    // 用量查询端点（GET + Bearer）
    std::string usagePath;   // 响应 JSON 点分取值路径（支持数组下标，如
                             // balance_infos.0.total_balance）
    std::string usageLabel;  // 显示单位/说明（如 "CNY 余额"）
    std::int64_t createdAt = 0;   // 毫秒
    // URL 输入模式：默认值保持旧配置/代码构造 Provider 的完整 URL 语义；
    // 新增表单会显式设为 false，让访问时按 upstreamFormat 追加默认后缀。
    std::string upstreamFormat = "openai";  // "anthropic" / "openai"
    bool fullUrl = true;

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
    // 用量查询全局设置（Provider.usageUrl 非空的供应商才参与）：
    bool usageEnabled = true;      // 总开关
    int usageRefreshMinutes = 10;  // 轮询间隔；0 = 仅手动刷新
    // 本地路由（llmswitch.router）设置：
    bool routerEnabled = false;    // 启动应用时自动开启本地路由
    int routerPort = 15731;        // 监听 127.0.0.1:<port>
    bool routerFailover = true;    // 上游 429/5xx 时故障转移到组内下一个供应商
    // 允许通过本地路由代理的工具 id；旧配置缺字段时默认全部启用，保持兼容。
    std::vector<std::string> routerTools{
        "claude-code", "claude", "codex", "opencode", "pi"};

    bool operator==(const AppConfig&) const = default;
};

// ---- JSON 序列化 -------------------------------------------------------------

export nlohmann::json toJson(const Provider& p) {
    nlohmann::json j;
    j["id"] = p.id;
    j["name"] = p.name;
    j["baseUrl"] = p.baseUrl;
    j["modelFetchUrl"] = p.modelFetchUrl;
    j["apiKey"] = p.apiKey;
    j["model"] = p.model;
    j["haikuModel"] = p.haikuModel;
    j["sonnetModel"] = p.sonnetModel;
    j["opusModel"] = p.opusModel;
    j["haikuDisplayName"] = p.haikuDisplayName;
    j["sonnetDisplayName"] = p.sonnetDisplayName;
    j["opusDisplayName"] = p.opusDisplayName;
    j["haikuSupports1m"] = p.haikuSupports1m;
    j["sonnetSupports1m"] = p.sonnetSupports1m;
    j["opusSupports1m"] = p.opusSupports1m;
    j["website"] = p.website;
    j["notes"] = p.notes;
    j["codexConfigToml"] = p.codexConfigToml;
    j["apiFormat"] = p.apiFormat;
    j["usageUrl"] = p.usageUrl;
    j["usagePath"] = p.usagePath;
    j["usageLabel"] = p.usageLabel;
    j["createdAt"] = p.createdAt;
    j["upstreamFormat"] = p.upstreamFormat;
    j["fullUrl"] = p.fullUrl;
    return j;
}

export Provider providerFromJson(const nlohmann::json& j) {
    Provider p;
    if (!j.is_object()) return p;
    p.id = j.value("id", "");
    p.name = j.value("name", "");
    p.baseUrl = j.value("baseUrl", "");
    p.modelFetchUrl = j.value("modelFetchUrl", "");
    p.apiKey = j.value("apiKey", "");
    p.model = j.value("model", "");
    p.haikuModel = j.value("haikuModel", "");
    p.sonnetModel = j.value("sonnetModel", "");
    p.opusModel = j.value("opusModel", "");
    p.haikuDisplayName = j.value("haikuDisplayName", "");
    p.sonnetDisplayName = j.value("sonnetDisplayName", "");
    p.opusDisplayName = j.value("opusDisplayName", "");
    p.haikuSupports1m = j.value("haikuSupports1m", false);
    p.sonnetSupports1m = j.value("sonnetSupports1m", false);
    p.opusSupports1m = j.value("opusSupports1m", false);
    p.website = j.value("website", "");
    p.notes = j.value("notes", "");
    p.codexConfigToml = j.value("codexConfigToml", "");
    p.apiFormat = j.value("apiFormat", "");
    p.usageUrl = j.value("usageUrl", "");
    p.usagePath = j.value("usagePath", "");
    p.usageLabel = j.value("usageLabel", "");
    p.createdAt = j.value("createdAt", std::int64_t{0});
    p.upstreamFormat = j.value("upstreamFormat", "openai");
    // 缺少新字段的旧配置保存的是已经可直接访问的 URL，不能按默认
    // 后缀再次拼接，否则会把 /v1 或 /anthropic 复制一遍。
    p.fullUrl = j.contains("fullUrl") ? j.value("fullUrl", true) : true;
    return p;
}

// ---- 上游 URL -----------------------------------------------------------------

// 归一化上游格式。未知值与空值回落 OpenAI 兼容格式，保持配置可编辑。
export std::string_view normalizeUpstreamFormat(std::string_view format) {
    return format == "anthropic" ? std::string_view{"anthropic"}
                                  : std::string_view{"openai"};
}

// 上游格式的默认路径后缀：Anthropic 兼容网关通常挂在 /anthropic，
// OpenAI 兼容网关通常挂在 /v1。
export std::string_view upstreamFormatSuffix(std::string_view format) {
    return normalizeUpstreamFormat(format) == "anthropic" ? "/anthropic" : "/v1";
}

export std::string effectiveBaseUrl(std::string_view baseUrl,
                                    std::string_view upstreamFormat,
                                    bool fullUrl) {
    std::string url(baseUrl);
    if (fullUrl || url.empty()) return url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    const auto suffix = upstreamFormatSuffix(upstreamFormat);
    if (!url.ends_with(suffix)) url += suffix;
    return url;
}

export std::string effectiveBaseUrl(const Provider& provider) {
    return effectiveBaseUrl(provider.baseUrl, provider.upstreamFormat,
                            provider.fullUrl);
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
    j["usageEnabled"] = c.usageEnabled;
    j["usageRefreshMinutes"] = c.usageRefreshMinutes;
    j["routerEnabled"] = c.routerEnabled;
    j["routerPort"] = c.routerPort;
    j["routerFailover"] = c.routerFailover;
    j["routerTools"] = c.routerTools;
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
    // 旧配置缺字段 → 默认值。
    c.usageEnabled = j.value("usageEnabled", true);
    c.usageRefreshMinutes = j.value("usageRefreshMinutes", 10);
    c.routerEnabled = j.value("routerEnabled", false);
    c.routerPort = j.value("routerPort", 15731);
    c.routerFailover = j.value("routerFailover", true);
    // 字段存在且为数组时尊重显式选择（包括空数组）；过滤未知 id 与重复项。
    // 字段缺失则保留 AppConfig 的“全部启用”默认值，兼容旧配置。
    if (j.contains("routerTools") && j["routerTools"].is_array()) {
        c.routerTools.clear();
        for (const auto& item : j["routerTools"]) {
            if (!item.is_string()) continue;
            const std::string id = item.get<std::string>();
            if (findTool(id) == nullptr ||
                std::ranges::find(c.routerTools, id) != c.routerTools.end()) {
                continue;
            }
            c.routerTools.push_back(id);
        }
    }
    return c;
}

// ---- 内置预设 -----------------------------------------------------------------
// 新建供应商时的模板：name/baseUrl/website 填好，apiKey 一律留空由用户填。
// 字段不确定时宁可留空也不编造；id/createdAt 由 store 在添加时生成。
// 参数为注册表工具 id。
export std::vector<Provider> builtinPresets(std::string_view tool) {
    if (tool == "claude-code") {
        // Anthropic 兼容端点（Claude Code 走 ANTHROPIC_BASE_URL）；官方端点
        // 由列表常驻「官方」卡承担（officialVendorName），不在预设里重复。
        return {
            Provider{.name = "DeepSeek",
                     .baseUrl = "https://api.deepseek.com",
                     .website = "https://platform.deepseek.com",
                     .upstreamFormat = "anthropic",
                     .fullUrl = false},
            Provider{.name = "Kimi（Moonshot）",
                     .baseUrl = "https://api.moonshot.cn",
                     .website = "https://platform.moonshot.cn",
                     .upstreamFormat = "anthropic",
                     .fullUrl = false},
            Provider{.name = "GLM（智谱）",
                     .baseUrl = "https://open.bigmodel.cn/api",
                     .website = "https://open.bigmodel.cn",
                     .upstreamFormat = "anthropic",
                     .fullUrl = false},
        };
    }
    if (tool == "codex") {
        // OpenAI 兼容端点（Codex 走 auth.json 的 OPENAI_API_KEY + config.toml 的
        // model_providers 段；wire_api 两种取值："chat"（OpenAI Chat
        // Completions，各家都支持）或 "responses"（OpenAI Responses）——模板
        // 默认 chat；model 以注释提示，避免写死一个用户没有的模型）。
        return {
            Provider{.name = "OpenRouter",
                     .baseUrl = "https://openrouter.ai/api",
                     .website = "https://openrouter.ai",
                     .codexConfigToml =
                         R"toml(model_provider = "openrouter"
# model = "openai/gpt-5"   # 按需填写要使用的模型

[model_providers.openrouter]
name = "OpenRouter"
base_url = "https://openrouter.ai/api"
wire_api = "chat"
)toml",
                     .upstreamFormat = "openai",
                     .fullUrl = false},
            Provider{.name = "DeepSeek",
                     .baseUrl = "https://api.deepseek.com",
                     .website = "https://platform.deepseek.com",
                     .codexConfigToml =
                         R"toml(model_provider = "deepseek"
# model = "deepseek-chat"   # 按需填写要使用的模型

[model_providers.deepseek]
name = "DeepSeek"
base_url = "https://api.deepseek.com"
wire_api = "chat"
)toml",
                     .upstreamFormat = "openai",
                     .fullUrl = false},
        };
    }
    if (tool == "opencode" || tool == "pi") {
        // OpenAI 兼容端点（opencode 走 @ai-sdk/openai-compatible；pi 走
        // openai-completions），model 填各家的主力模型。
        return {
            Provider{.name = "DeepSeek",
                     .baseUrl = "https://api.deepseek.com",
                     .model = "deepseek-chat",
                     .website = "https://platform.deepseek.com",
                     .upstreamFormat = "openai",
                     .fullUrl = false},
            Provider{.name = "Kimi（Moonshot）",
                     .baseUrl = "https://api.moonshot.cn",
                     .model = "kimi-k2-0905-preview",
                     .website = "https://platform.moonshot.cn",
                     .upstreamFormat = "openai",
                     .fullUrl = false},
        };
    }
    // claude（Claude Desktop 3p 直连）：暂无第三方预设（官方走常驻卡）。
    return {};
}

// ---- 官方厂商 -----------------------------------------------------------------
// 工具的官方厂商名：供应商列表首位常驻「官方」卡用（切换 = store 的
// restoreOfficial 还原厂商原生状态）。空串 = 该工具无官方默认状态。
export std::string_view officialVendorName(std::string_view tool) {
    if (tool == "claude-code" || tool == "claude") return "Anthropic 官方";
    if (tool == "codex") return "OpenAI 官方";
    return "";
}

// ---- 用量查询模板 ---------------------------------------------------------------
// 已知厂商的用量查询建议（只收有官方文档的，没把握的不编）。
// 返回 {usageUrl, usagePath}（usageLabel 由调用方决定，如 "CNY"）；
// 不认识该 baseUrl 返回 std::nullopt。
export std::optional<std::pair<std::string, std::string>> suggestUsageQuery(
    std::string_view baseUrl) {
    // DeepSeek 官方：GET /user/balance（Bearer），响应
    // {"balance_infos": [{"total_balance": "...", ...}]}。
    if (baseUrl.find("api.deepseek.com") != std::string_view::npos) {
        return std::pair{std::string("https://api.deepseek.com/user/balance"),
                         std::string("balance_infos.0.total_balance")};
    }
    return std::nullopt;
}

} // namespace models
