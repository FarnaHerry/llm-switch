// test_store.cpp — llmswitch.store 领域层测试（无框架：CHECK 失败计数，非零即败）。
// 全程隔离在临时目录：HOME / XDG_DATA_HOME / LLMSWITCH_* 指向
// temp_directory_path()/llmswitch-test-<pid>，live 文件与 dataDir 都不碰真实环境。
//
// 覆盖：空载默认值、首次导入收编、claude-code 切换深合并（保留非 env 字段）、
// codex 切换（auth.json + config.toml 整段替换 + 顶层 model 行级重写）、
// opencode（additive upsert、顶层 model、anthropic 变体、JSON5 报错不碰文件）、
// pi（双文件、权限位、apiFormat 映射）、claude desktop（Linux 不支持报错 +
// 覆盖后四文件）、备份生成、detectCurrent、导出/导入回滚、apiFormat 三档
// 映射与归一、usage 三字段与全局设置持久化、逐 Agent 路由开关持久化、
// Claude Code 跳过初次安装检查开关、
// restoreOfficial 三工具还原
// （codex 模型收回）、官方厂商名（officialVendorName）与预设列表、
// claude 系三档模型映射（env 六键 /
// desktop inferenceModels 的显示名与 supports1m / 收编 / 擦除）、旧格式 config.json 迁移、
// 损坏 config.json 挪走不崩溃。
#include <cstdio>    // stderr（std 模块不导出 stdout/stderr 宏）
#include "test_env.h"  // setenv/getpid/unsetenv 可移植封装

import std;
import nlohmann.json;
import llmswitch.config;
import llmswitch.models;
import llmswitch.store;

namespace {

int g_failures = 0;

#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            std::println(stderr, "FAIL {}: {}", __LINE__, #cond); \
            ++g_failures;                                        \
        }                                                        \
    } while (0)

template <class Function>
bool throwsRuntimeError(Function&& function) {
    try {
        std::invoke(std::forward<Function>(function));
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

void writeFile(const std::filesystem::path& path, std::string_view content) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

nlohmann::json readJson(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return nlohmann::json::parse(std::string(std::istreambuf_iterator<char>(in),
                                             std::istreambuf_iterator<char>()));
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

// 目录下匹配 <prefix>.<时间戳>.bak 的文件数。
int countBackups(const std::filesystem::path& dir, const std::string& prefix) {
    std::error_code ec;
    int n = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.starts_with(prefix + ".") && name.ends_with(".bak")) ++n;
    }
    return n;
}

} // namespace

int main() {
    namespace fs = std::filesystem;
    using namespace std::string_literals;

    // ---- 环境隔离 -----------------------------------------------------------
    const fs::path root =
        fs::temp_directory_path() / std::format("llmswitch-test-{}", testenv::getpid());
    {
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root);
    }
    const fs::path home = root / "home";
    testenv::setenv("HOME", home);
    testenv::setenv("XDG_DATA_HOME", (root / "xdg"));
    testenv::setenv("LLMSWITCH_DATA_DIR", (root / "data"));
    const fs::path claudeSettings = home / ".claude" / "settings.json";
    const fs::path codexAuth = home / ".codex" / "auth.json";
    const fs::path codexConfig = home / ".codex" / "config.toml";
    const fs::path opencodeConfig = root / "opencode" / "opencode.json";
    const fs::path piDir = root / "pi-agent";
    const fs::path piModels = piDir / "models.json";
    const fs::path piSettings = piDir / "settings.json";
    testenv::setenv("LLMSWITCH_CLAUDE_SETTINGS", claudeSettings);
    testenv::setenv("LLMSWITCH_CODEX_AUTH", codexAuth);
    testenv::setenv("LLMSWITCH_CODEX_CONFIG", codexConfig);
    testenv::setenv("LLMSWITCH_OPENCODE_CONFIG", opencodeConfig);
    testenv::setenv("LLMSWITCH_PI_DIR", piDir);
    testenv::unsetenv("PI_CODING_AGENT_DIR");
    testenv::unsetenv("LLMSWITCH_CLAUDE_DESKTOP_DIR");  // 默认 Linux 不支持

    // 1. 空环境 load → 默认空配置
    {
        auto s = store::ProviderStore::load();
        CHECK(s.config().groups.empty());
        CHECK(s.config().themeMode == "system");
        CHECK(s.config().routerTools.size() == models::toolRegistry().size());
        CHECK(std::ranges::find(s.config().routerTools, "claude-code") !=
              s.config().routerTools.end());
        CHECK(s.detectCurrent("claude-code").empty());
        CHECK(s.detectCurrent("claude").empty());  // Linux 无桌面目录 → 空
        // 注册表自检：5 个工具、id 可互查
        CHECK(models::toolRegistry().size() == 5);
        CHECK(models::findTool("claude-code") != nullptr);
        CHECK(models::findTool("opencode")->needsModel);
        CHECK(models::findTool("pi")->hasApiFormat);
        CHECK(!models::findTool("codex")->needsModel);
        CHECK(models::findTool("claude-code")->hasModelMappings);
        CHECK(models::findTool("claude")->hasModelMappings);
        CHECK(!models::findTool("codex")->hasModelMappings);
        CHECK(models::findTool("nope") == nullptr);
    }

    // 2. 写入假 settings.json（含非 env 字段）→ 首次 load 自动收编成「当前配置」
    writeFile(claudeSettings, R"json({
  "permissions": {"allow": ["Bash(*)"]},
  "env": {
    "ANTHROPIC_BASE_URL": "https://live.example.com/anthropic",
    "ANTHROPIC_AUTH_TOKEN": "sk-live-token",
    "ANTHROPIC_MODEL": "claude-live"
  }
}
)json");
    auto s = store::ProviderStore::load();
    {
        CHECK(s.group("claude-code").providers.size() == 1);
        const auto& p = s.group("claude-code").providers.front();
        CHECK(p.name == "当前配置");
        CHECK(p.baseUrl == "https://live.example.com/anthropic");
        CHECK(p.apiKey == "sk-live-token");
        CHECK(p.model == "claude-live");
        CHECK(s.group("claude-code").current == p.id);
        CHECK(s.detectCurrent("claude-code") == p.id);
        // 再次 load：已有收编项，不重复建
        auto again = store::ProviderStore::load();
        CHECK(again.group("claude-code").providers.size() == 1);
    }

    // 3. addProvider 两个 + switchTo：env 三字段写入、permissions 保留
    models::Provider pa{.name = "供应商A",
                        .baseUrl = "https://a.example.com",
                        .apiKey = "sk-a",
                        .model = "model-a"};
    models::Provider pb{.name = "供应商B",
                        .baseUrl = "https://b.example.com",
                        .apiKey = "sk-b"};  // model 留空
    s.addProvider("claude-code", pa);
    s.addProvider("claude-code", pb);
    CHECK(s.group("claude-code").providers.size() == 3);
    const std::string idA = s.group("claude-code").providers[1].id;
    const std::string idB = s.group("claude-code").providers[2].id;
    CHECK(!idA.empty() && !idB.empty() && idA != idB);

    s.switchTo("claude-code", idB);
    {
        const auto j = readJson(claudeSettings);
        CHECK(j["env"]["ANTHROPIC_BASE_URL"] == "https://b.example.com");
        CHECK(j["env"]["ANTHROPIC_AUTH_TOKEN"] == "sk-b");
        // model 为空 → 不动既有 ANTHROPIC_MODEL
        CHECK(j["env"]["ANTHROPIC_MODEL"] == "claude-live");
        // 非 env 字段原样保留
        CHECK(j["permissions"]["allow"][0] == "Bash(*)");
        CHECK(s.group("claude-code").current == idB);
        CHECK(s.detectCurrent("claude-code") == idB);
    }
    s.switchTo("claude-code", idA);
    {
        const auto j = readJson(claudeSettings);
        CHECK(j["env"]["ANTHROPIC_MODEL"] == "model-a");
        CHECK(s.group("claude-code").current == idA);
    }

    // 3a. Claude Code 跳过初次安装检查：写入 DISABLE_INSTALLATION_CHECKS=1，
    // 关闭时只移除该键。
    CHECK(!s.claudeCodeSkipInstallationChecks());
    s.setClaudeCodeSkipInstallationChecks(true);
    CHECK(s.claudeCodeSkipInstallationChecks());
    {
        const auto j = readJson(claudeSettings);
        CHECK(j["env"]["DISABLE_INSTALLATION_CHECKS"] == "1");
        CHECK(j["permissions"]["allow"][0] == "Bash(*)");
    }
    s.setClaudeCodeSkipInstallationChecks(false);
    CHECK(!s.claudeCodeSkipInstallationChecks());
    CHECK(!readJson(claudeSettings)["env"].contains(
        "DISABLE_INSTALLATION_CHECKS"));

    // 3b. 根 URL + Anthropic 上游格式：切换时写入默认 /anthropic 后缀，
    // detectCurrent 也按实际访问 URL 反向匹配。
    models::Provider formatted{.name = "格式化 URL",
                               .baseUrl = "https://formatted.example.com",
                               .apiKey = "sk-formatted"};
    formatted.upstreamFormat = "anthropic";
    formatted.fullUrl = false;
    s.addProvider("claude-code", formatted);
    const std::string idFormatted = s.group("claude-code").providers.back().id;
    s.switchTo("claude-code", idFormatted);
    CHECK(readJson(claudeSettings)["env"]["ANTHROPIC_BASE_URL"] ==
          "https://formatted.example.com/anthropic");
    CHECK(s.detectCurrent("claude-code") == idFormatted);

    // 4. codex：switchTo 写 auth.json 的 OPENAI_API_KEY + config.toml 原文替换
    writeFile(codexAuth, R"json({"OPENAI_API_KEY": "sk-old"}
)json");
    writeFile(codexConfig, "# 旧的 codex 配置\nmodel = \"old\"\n");
    models::Provider pc{.name = "OpenRouter",
                        .baseUrl = "https://openrouter.ai/api",
                        .apiKey = "sk-or-key",
                        .codexConfigToml =
                            "model_provider = \"openrouter\"\n"
                            "[model_providers.openrouter]\n"
                            "base_url = \"https://old.example.com\"\n"};
    pc.upstreamFormat = "openai";
    pc.fullUrl = false;
    s.addProvider("codex", pc);
    const std::string idC = s.group("codex").providers.back().id;
    s.switchTo("codex", idC);
    {
        const auto j = readJson(codexAuth);
        CHECK(j["OPENAI_API_KEY"] == "sk-or-key");
        CHECK(readText(codexConfig) ==
              "model_provider = \"openrouter\"\n"
              "[model_providers.openrouter]\n"
              "base_url = \"https://openrouter.ai/api/v1\"\n");
        CHECK(s.group("codex").current == idC);
        CHECK(s.detectCurrent("codex") == idC);
    }

    // 5. 备份生成在 backups 目录（claude-code 切了两次 → ≥2 份；codex 各 1 份）
    {
        const auto backups = cfg::backupsDir();
        CHECK(countBackups(backups / "claude-code", "settings.json") >= 2);
        CHECK(countBackups(backups / "codex", "auth.json") == 1);
        CHECK(countBackups(backups / "codex", "config.toml") == 1);
    }

    // 6. detectCurrent：不匹配 → 空串；live 文件改回匹配值 → 恢复命中
    writeFile(claudeSettings, R"json({"env": {"ANTHROPIC_BASE_URL": "https://elsewhere.example.com", "ANTHROPIC_AUTH_TOKEN": "sk-unknown"}}
)json");
    CHECK(s.detectCurrent("claude-code").empty());
    writeFile(claudeSettings, R"json({"env": {"ANTHROPIC_BASE_URL": "https://a.example.com", "ANTHROPIC_AUTH_TOKEN": "sk-a"}}
)json");
    CHECK(s.detectCurrent("claude-code") == idA);

    // 7. opencode：additive upsert + 顶层 model + anthropic 变体 + JSON5 报错
    writeFile(opencodeConfig, R"json({
  "$schema": "https://opencode.ai/config.json",
  "theme": "opencode",
  "provider": {"existing": {"npm": "@ai-sdk/openai-compatible"}}
}
)json");
    models::Provider po{.name = "DeepSeek",
                        .baseUrl = "https://api.deepseek.com/v1",
                        .apiKey = "sk-oc",
                        .model = "deepseek-chat"};
    s.addProvider("opencode", po);
    const std::string idO = s.group("opencode").providers.back().id;
    s.switchTo("opencode", idO);
    {
        const auto j = readJson(opencodeConfig);
        // 既有顶层字段与既有 provider 条目保留
        CHECK(j["theme"] == "opencode");
        CHECK(j["provider"].contains("existing"));
        const auto& entry = j["provider"][idO];
        CHECK(entry["npm"] == "@ai-sdk/openai-compatible");
        CHECK(entry["options"]["baseURL"] == "https://api.deepseek.com/v1");
        CHECK(entry["options"]["apiKey"] == "sk-oc");
        CHECK(entry["models"].contains("deepseek-chat"));
        CHECK(j["model"] == idO + "/deepseek-chat");
        CHECK(s.detectCurrent("opencode") == idO);
        CHECK(countBackups(cfg::backupsDir() / "opencode", "opencode.json") == 1);
    }
    // anthropic 变体：npm 段切到 @ai-sdk/anthropic
    {
        models::Provider updated = s.group("opencode").providers.back();
        updated.apiFormat = "anthropic";
        s.updateProvider("opencode", updated);
        s.switchTo("opencode", idO);
        const auto j = readJson(opencodeConfig);
        CHECK(j["provider"][idO]["npm"] == "@ai-sdk/anthropic");
    }
    // openai-responses 变体：npm 段切到 @ai-sdk/openai；未知值回落默认档
    {
        models::Provider updated = s.group("opencode").providers.back();
        updated.apiFormat = "openai-responses";
        s.updateProvider("opencode", updated);
        s.switchTo("opencode", idO);
        CHECK(readJson(opencodeConfig)["provider"][idO]["npm"] ==
              "@ai-sdk/openai");
        updated.apiFormat = "weird";
        s.updateProvider("opencode", updated);
        s.switchTo("opencode", idO);
        CHECK(readJson(opencodeConfig)["provider"][idO]["npm"] ==
              "@ai-sdk/openai-compatible");
    }
    // JSON5（带注释）→ switchTo 抛错且不碰原文件
    {
        const std::string json5 = "{\n  // 官方允许注释\n  \"theme\": \"opencode\"\n}\n";
        writeFile(opencodeConfig, json5);
        bool threw = false;
        try {
            s.switchTo("opencode", idO);
        } catch (const std::exception& e) {
            threw = true;
            CHECK(std::string_view(e.what()).contains("JSON5"));
        }
        CHECK(threw);
        CHECK(readText(opencodeConfig) == json5);  // 原文件未被修改
        CHECK(s.detectCurrent("opencode").empty());  // 解析失败按无内容
    }

    // 8. pi：双文件写入、权限位、apiFormat 映射、detectCurrent、备份
    // 预置既有内容：验证 upsert/深合并不破坏无关字段，且首次切换即有备份。
    writeFile(piModels, R"json({"providers": {"pre-existing": {"baseUrl": "https://x.example.com", "apiKey": "k", "api": "openai-completions"}}}
)json");
    writeFile(piSettings, R"json({"timeout": 30}
)json");
    models::Provider pp{.name = "Kimi",
                        .baseUrl = "https://api.moonshot.cn/v1",
                        .apiKey = "sk-pi",
                        .model = "kimi-k2-0905-preview",
                        .apiFormat = "openai-responses"};
    s.addProvider("pi", pp);
    const std::string idP = s.group("pi").providers.back().id;
    s.switchTo("pi", idP);
    {
        const auto models = readJson(piModels);
        const auto& entry = models["providers"][idP];
        CHECK(models["providers"].contains("pre-existing"));  // 无关条目保留
        CHECK(entry["baseUrl"] == "https://api.moonshot.cn/v1");
        CHECK(entry["apiKey"] == "sk-pi");
        CHECK(entry["api"] == "openai-responses");  // apiFormat 映射
        CHECK(entry["models"][0] == "kimi-k2-0905-preview");
        const auto settings = readJson(piSettings);
        CHECK(settings["defaultProvider"] == idP);
        CHECK(settings["defaultModel"] == "kimi-k2-0905-preview");
        CHECK(settings["timeout"] == 30);  // 深合并保留无关字段
#if !defined(_WIN32)
        // 权限：目录 0700、文件 0600（Windows 无 POSIX 权限位语义，跳过）
        CHECK((fs::status(piDir).permissions() & fs::perms::all) ==
              fs::perms::owner_all);
        CHECK((fs::status(piModels).permissions() & fs::perms::all) ==
              (fs::perms::owner_read | fs::perms::owner_write));
        CHECK((fs::status(piSettings).permissions() & fs::perms::all) ==
              (fs::perms::owner_read | fs::perms::owner_write));
#endif
        CHECK(s.detectCurrent("pi") == idP);
        CHECK(countBackups(cfg::backupsDir() / "pi", "models.json") == 1);
        CHECK(countBackups(cfg::backupsDir() / "pi", "settings.json") == 1);
    }
    // 默认档（apiFormat 留空）→ openai-completions
    {
        models::Provider pp3{.name = "旧协议",
                             .baseUrl = "https://legacy.example.com/v1",
                             .apiKey = "sk-pi-legacy"};  // apiFormat 默认 ""
        s.addProvider("pi", pp3);
        const std::string idP3 = s.group("pi").providers.back().id;
        s.switchTo("pi", idP3);
        CHECK(readJson(piModels)["providers"][idP3]["api"] ==
              "openai-completions");
    }
    // anthropic → anthropic-messages 映射 + detectCurrent 的 apiKey/baseUrl 回退
    {
        models::Provider pa2{.name = "Claude 直连",
                             .baseUrl = "https://api.anthropic.com",
                             .apiKey = "sk-pi-ant",
                             .apiFormat = "anthropic"};
        s.addProvider("pi", pa2);
        const std::string idP2 = s.group("pi").providers.back().id;
        s.switchTo("pi", idP2);
        CHECK(readJson(piModels)["providers"][idP2]["api"] == "anthropic-messages");
        // defaultProvider 指向未知 id 时按 apiKey+baseUrl 匹配命中：把
        // models.json 换成只有「外部键 + pa2 凭据」的条目，消除 idP 的歧义。
        writeFile(piSettings, R"json({"defaultProvider": "ghost"}
)json");
        writeFile(piModels, R"json({"providers": {"ext-key": {"baseUrl": "https://api.anthropic.com", "apiKey": "sk-pi-ant", "api": "anthropic-messages"}}}
)json");
        CHECK(s.detectCurrent("pi") == idP2);
    }

    // 9. claude desktop：Linux 默认不支持（抛错）；设覆盖目录后写四个文件
    models::Provider pd{.name = "桌面网关",
                        .baseUrl = "https://desktop.example.com/anthropic",
                        .apiKey = "sk-desk",
                        .model = "kimi-k2-0905-preview"};  // 非白名单模型名
    s.addProvider("claude", pd);
    const std::string idD = s.group("claude").providers.back().id;
#if defined(__linux__)
    // 仅 Linux 默认不支持（macOS/Windows 走真实目录）；错误信息中文断言依赖 /utf-8。
    {
        bool threw = false;
        try {
            s.switchTo("claude", idD);
        } catch (const std::exception& e) {
            threw = true;
            CHECK(std::string_view(e.what()).contains("不支持 Linux"));
        }
        CHECK(threw);
    }
#endif
    const fs::path deskDir = root / "Claude";
    testenv::setenv("LLMSWITCH_CLAUDE_DESKTOP_DIR", deskDir);
    // 既有配置里放无关字段，验证深合并保留
    writeFile(deskDir / "claude_desktop_config.json",
              R"json({"theme": "dark", "deploymentMode": "1p"}
)json");
    s.switchTo("claude", idD);
    {
        const fs::path threepDir = root / "Claude-3p";
        const auto normal = readJson(deskDir / "claude_desktop_config.json");
        CHECK(normal["deploymentMode"] == "3p");
        CHECK(normal["theme"] == "dark");  // 其余字段保留
        const auto threep = readJson(threepDir / "claude_desktop_config.json");
        CHECK(threep["deploymentMode"] == "3p");
        const auto profile = readJson(
            threepDir / "configLibrary" /
            "00000000-0000-4000-8000-000000157210.json");
        CHECK(profile["inferenceProvider"] == "gateway");
        CHECK(profile["inferenceGatewayBaseUrl"] ==
              "https://desktop.example.com/anthropic");
        CHECK(profile["inferenceGatewayApiKey"] == "sk-desk");
        CHECK(profile["inferenceGatewayAuthScheme"] == "bearer");
        CHECK(profile["disableDeploymentModeChooser"] == true);
        CHECK(profile["coworkEgressAllowedHosts"][0] == "*");
        // 非白名单模型名：借用安全 route id，真名放 labelOverride（对齐
        // cc-switch 上游：桌面端只认 claude-(sonnet|opus|haiku|fable)-*）
        CHECK(profile["inferenceModels"][0]["name"] == "claude-sonnet-4-6");
        CHECK(profile["inferenceModels"][0]["labelOverride"] ==
              "kimi-k2-0905-preview");
        const auto meta = readJson(threepDir / "configLibrary" / "_meta.json");
        CHECK(meta["appliedId"] == "00000000-0000-4000-8000-000000157210");
        CHECK(meta["entries"][0]["name"] == "llm-switch");
        CHECK(s.group("claude").current == idD);
        CHECK(s.detectCurrent("claude") == idD);
        CHECK(countBackups(cfg::backupsDir() / "claude",
                           "claude_desktop_config.json") == 1);
    }
    // 白名单模型名：直接作为 name，不写 labelOverride
    {
        models::Provider pd2{.name = "官方安全名",
                             .baseUrl = "https://desktop2.example.com",
                             .apiKey = "sk-desk2",
                             .model = "claude-opus-4-8"};
        s.addProvider("claude", pd2);
        const std::string idD2 = s.group("claude").providers.back().id;
        s.switchTo("claude", idD2);
        const auto profile = readJson(
            root / "Claude-3p" / "configLibrary" /
            "00000000-0000-4000-8000-000000157210.json");
        CHECK(profile["inferenceModels"][0]["name"] == "claude-opus-4-8");
        CHECK(!profile["inferenceModels"][0].contains("labelOverride"));
    }

    // 10. exportTo → 改库 → importFrom 恢复（导入前自动备份 config.json）
    const fs::path exportPath = root / "export.json";
    s.exportTo(exportPath);
    CHECK(fs::exists(exportPath));
    s.removeProvider("claude-code", idA);
    CHECK(s.group("claude-code").providers.size() == 3);
    s.importFrom(exportPath);
    {
        CHECK(s.group("claude-code").providers.size() == 4);
        bool found = false;
        for (const auto& p : s.group("claude-code").providers) {
            if (p.id == idA) found = true;
        }
        CHECK(found);
        CHECK(countBackups(cfg::backupsDir(), "config.json") >= 1);
    }

    // 11. models 层三档直测 + usage 字段持久化 + usage 全局设置
    {
        CHECK(models::normalizeApiFormat("") == "openai-chat");
        CHECK(models::normalizeApiFormat("openai") == "openai-chat");
        CHECK(models::normalizeApiFormat("openai-chat") == "openai-chat");
        CHECK(models::normalizeApiFormat("openai-responses") ==
              "openai-responses");
        CHECK(models::normalizeApiFormat("anthropic") == "anthropic");
        CHECK(models::apiFormatLabel("openai-chat") == "OpenAI Chat Completions");
        CHECK(models::apiFormatLabel("openai-responses") == "OpenAI Responses");
        CHECK(models::apiFormatLabel("anthropic") ==
              "Anthropic Messages（原生）");
        CHECK(models::normalizeUpstreamFormat("anthropic") == "anthropic");
        CHECK(models::normalizeUpstreamFormat("unknown") == "openai");
        CHECK(models::upstreamFormatSuffix("anthropic") == "/anthropic");
        CHECK(models::upstreamFormatSuffix("openai") == "/v1");
        CHECK(models::effectiveBaseUrl("https://api.example.com/", "anthropic",
                                       false) == "https://api.example.com/anthropic");
        CHECK(models::effectiveBaseUrl("https://api.example.com/root", "openai",
                                       true) == "https://api.example.com/root");
        CHECK(models::effectiveBaseUrl("https://api.example.com/root/", "anthropic",
                                       true) == "https://api.example.com/root/");
        CHECK(models::effectiveBaseUrl("https://api.example.com/v1", "openai",
                                       false) == "https://api.example.com/v1");
        CHECK(models::effectiveBaseUrl("https://api.example.com/anthropic/",
                                       "anthropic", false) ==
              "https://api.example.com/anthropic");
        CHECK(models::effectiveBaseUrl("https://api.example.com/v1/models",
                                       "openai", false) ==
              "https://api.example.com/v1/models");
        CHECK(models::effectiveBaseUrl("https://api.example.com/anthropic/v1",
                                       "anthropic", false) ==
              "https://api.example.com/anthropic/v1");
        models::Provider encoded{.name = "编码",
                                 .baseUrl = "https://encoded.example.com",
                                 .upstreamFormat = "anthropic",
                                 .fullUrl = false};
        const auto encodedJson = models::toJson(encoded);
        const auto decoded = models::providerFromJson(encodedJson);
        CHECK(decoded.upstreamFormat == "anthropic" && !decoded.fullUrl);
        models::Provider customFetch = encoded;
        customFetch.modelFetchUrl = "https://models.example.com/v1/models";
        const auto customFetchDecoded =
            models::providerFromJson(models::toJson(customFetch));
        CHECK(customFetchDecoded.modelFetchUrl ==
              "https://models.example.com/v1/models");
        const auto legacy = models::providerFromJson(
            nlohmann::json{{"baseUrl", "https://legacy.example.com/v1"}});
        CHECK(legacy.fullUrl);
        const auto legacyUsage = models::providerFromJson(
            nlohmann::json{{"usageUrl", "https://legacy.example.com/usage"}});
        CHECK(legacyUsage.usageEnabled);
        const auto disabledUsage = models::providerFromJson(
            nlohmann::json{{"usageEnabled", false},
                           {"usageUrl", "https://legacy.example.com/usage"}});
        CHECK(!disabledUsage.usageEnabled);
        const auto sug =
            models::suggestUsageQuery("https://api.deepseek.com/v1");
        CHECK(sug.has_value());
        CHECK(sug->first == "https://api.deepseek.com/user/balance");
        CHECK(sug->second == "balance_infos.0.total_balance");
        CHECK(!models::suggestUsageQuery("https://x.example.com").has_value());
    }
    {
        // usage 三字段随落盘持久
        models::Provider pu{.name = "带用量",
                            .baseUrl = "https://api.deepseek.com/v1",
                            .apiKey = "sk-usage",
                            .usageEnabled = true,
                            .usageRefreshMinutes = 5,
                            .usageUrl = "https://api.deepseek.com/user/balance",
                            .usagePath = "balance_infos.0.total_balance",
                            .usageLabel = "CNY"};
        s.addProvider("claude-code", pu);
        const std::string idU = s.group("claude-code").providers.back().id;
        auto reloaded = store::ProviderStore::load();
        bool found = false;
        for (const auto& p : reloaded.group("claude-code").providers) {
            if (p.id == idU) {
                found = true;
                CHECK(p.usageEnabled);
                CHECK(p.usageRefreshMinutes == 5);
                CHECK(p.usageUrl == "https://api.deepseek.com/user/balance");
                CHECK(p.usagePath == "balance_infos.0.total_balance");
                CHECK(p.usageLabel == "CNY");
            }
        }
        CHECK(found);
        // 是否查询与刷新间隔均由每个 Provider 独立保存。
        auto updated = std::ranges::find(reloaded.group("claude-code").providers,
                                         idU, &models::Provider::id);
        CHECK(updated != reloaded.group("claude-code").providers.end());
        if (updated != reloaded.group("claude-code").providers.end()) {
            auto updatedProvider = *updated;
            updatedProvider.usageRefreshMinutes = 0;
            reloaded.updateProvider("claude-code", updatedProvider);
            const auto again = store::ProviderStore::load();
            const auto updatedAgain =
                std::ranges::find(again.group("claude-code").providers, idU,
                                  &models::Provider::id);
            CHECK(updatedAgain != again.group("claude-code").providers.end());
            if (updatedAgain != again.group("claude-code").providers.end()) {
                CHECK(updatedAgain->usageRefreshMinutes == 0);
            }
            CHECK(!models::toJson(again.config()).contains("usageRefreshMinutes"));
        }
    }

    // 路由工具开关：旧配置默认全开，显式空数组保持全关；setter 立即持久化。
    {
        const auto legacy = models::fromJson(nlohmann::json::object());
        CHECK(legacy.routerTools.size() == models::toolRegistry().size());

        const auto selected = models::fromJson(nlohmann::json::parse(
            R"json({"routerTools":["codex","unknown","codex"]})json"));
        CHECK(selected.routerTools.size() == 1);
        CHECK(selected.routerTools.front() == "codex");
        const auto none = models::fromJson(
            nlohmann::json::parse(R"json({"routerTools":[]})json"));
        CHECK(none.routerTools.empty());

        auto routing = store::ProviderStore::load();
        routing.setRouterToolEnabled("codex", false);
        auto reloaded = store::ProviderStore::load();
        CHECK(std::ranges::find(reloaded.config().routerTools, "codex") ==
              reloaded.config().routerTools.end());
        reloaded.setRouterToolEnabled("codex", true);
        auto restored = store::ProviderStore::load();
        CHECK(std::ranges::find(restored.config().routerTools, "codex") !=
              restored.config().routerTools.end());
        CHECK(throwsRuntimeError(
            [&] { restored.setRouterToolEnabled("unknown", true); }));
    }

    // 12. codex 模型行级重写（switchTo 时 Provider.model 写进 config.toml
    // 顶层 model 键）+ importLive 收回模型
    {
        // a) 已有未注释 model 行 → 替换值，节区原样保留
        models::Provider pm1{.name = "M1",
                             .apiKey = "sk-m1",
                             .model = "gpt-5-codex",
                             .codexConfigToml =
                                 "model = \"old\"\nmodel_provider = \"x\"\n\n"
                                 "[model_providers.x]\nname = \"X\"\n"};
        s.addProvider("codex", pm1);
        const std::string idM1 = s.group("codex").providers.back().id;
        s.switchTo("codex", idM1);
        CHECK(readText(codexConfig) ==
              "model = \"gpt-5-codex\"\nmodel_provider = \"x\"\n\n"
              "[model_providers.x]\nname = \"X\"\n");
        // b) 只有注释行 → 注释行整行替换
        models::Provider pm2{.name = "M2",
                             .apiKey = "sk-m2",
                             .model = "gpt-5.1",
                             .codexConfigToml =
                                 "model_provider = \"openai\"\n"
                                 "# model = \"gpt-5\"   # 按需填写\n\n"
                                 "[model_providers.openai]\n"
                                 "wire_api = \"responses\"\n"};
        s.addProvider("codex", pm2);
        const std::string idM2 = s.group("codex").providers.back().id;
        s.switchTo("codex", idM2);
        CHECK(readText(codexConfig) ==
              "model_provider = \"openai\"\nmodel = \"gpt-5.1\"\n\n"
              "[model_providers.openai]\nwire_api = \"responses\"\n");
        // c) 无 model 行 → 文件开头插入
        models::Provider pm3{.name = "M3",
                             .apiKey = "sk-m3",
                             .model = "deepseek-chat",
                             .codexConfigToml = "model_provider = \"deepseek\"\n"};
        s.addProvider("codex", pm3);
        const std::string idM3 = s.group("codex").providers.back().id;
        s.switchTo("codex", idM3);
        CHECK(readText(codexConfig) ==
              "model = \"deepseek-chat\"\nmodel_provider = \"deepseek\"\n");
        // d) model 为空 → config.toml 原文不动
        models::Provider pm4{.name = "M4",
                             .apiKey = "sk-m4",
                             .codexConfigToml = "model_provider = \"raw\"\n"};
        s.addProvider("codex", pm4);
        const std::string idM4 = s.group("codex").providers.back().id;
        s.switchTo("codex", idM4);
        CHECK(readText(codexConfig) == "model_provider = \"raw\"\n");
        // importLive 顺带收回 config.toml 顶层 model
        writeFile(codexAuth, R"json({"OPENAI_API_KEY": "sk-imported"}
)json");
        writeFile(codexConfig, "model = \"imp-model\"\nmodel_provider = \"z\"\n");
        const auto imported = s.importLive("codex");
        CHECK(imported.name == "当前配置");
        CHECK(imported.apiKey == "sk-imported");
        CHECK(imported.model == "imp-model");
        CHECK(s.group("codex").current == imported.id);
    }

    // 13. restoreOfficial：撤掉本应用写入的 live 覆盖，current 清空
    {
        // claude-code：env 三键删除，其余 env 键与 permissions 保留
        writeFile(claudeSettings, R"json({
  "permissions": {"allow": ["Bash(*)"]},
  "env": {
    "ANTHROPIC_BASE_URL": "https://a.example.com",
    "ANTHROPIC_AUTH_TOKEN": "sk-a",
    "ANTHROPIC_MODEL": "model-a",
    "OTHER": "keep"
  }
}
)json");
        s.restoreOfficial("claude-code");
        const auto j = readJson(claudeSettings);
        CHECK(!j["env"].contains("ANTHROPIC_BASE_URL"));
        CHECK(!j["env"].contains("ANTHROPIC_AUTH_TOKEN"));
        CHECK(!j["env"].contains("ANTHROPIC_MODEL"));
        CHECK(j["env"]["OTHER"] == "keep");
        CHECK(j["permissions"]["allow"][0] == "Bash(*)");
        CHECK(s.group("claude-code").current.empty());
        CHECK(s.detectCurrent("claude-code").empty());

        // codex：auth.json 只剩 OPENAI_API_KEY → 删键后为空对象 → 删文件；
        // config.toml 与组内 provider 模板（应用 model 后）一致 → 删除
        writeFile(codexAuth, R"json({"OPENAI_API_KEY": "sk-m1"}
)json");
        writeFile(codexConfig,
                  "model = \"gpt-5-codex\"\nmodel_provider = \"x\"\n\n"
                  "[model_providers.x]\nname = \"X\"\n");
        s.restoreOfficial("codex");
        CHECK(!fs::exists(codexAuth));
        CHECK(!fs::exists(codexConfig));
        CHECK(s.group("codex").current.empty());
        // auth.json 含 tokens 等其他字段 → 只删键；config.toml 被手改过
        // （与任何 provider 模板都不一致）→ 不动
        writeFile(codexAuth, R"json({"OPENAI_API_KEY": "sk-x", "tokens": {"refresh": "r"}}
)json");
        writeFile(codexConfig, "# 用户手改\nmodel_provider = \"mine\"\n");
        s.restoreOfficial("codex");
        CHECK(fs::exists(codexAuth));
        CHECK(!readJson(codexAuth).contains("OPENAI_API_KEY"));
        CHECK(readJson(codexAuth)["tokens"]["refresh"] == "r");
        CHECK(readText(codexConfig) == "# 用户手改\nmodel_provider = \"mine\"\n");

        // claude desktop（覆盖目录）：两份 config 删 deploymentMode，_meta.json
        // 移除本应用条目并清 appliedId；profile 文件本体保留
        s.restoreOfficial("claude");
        const auto normal = readJson(deskDir / "claude_desktop_config.json");
        CHECK(!normal.contains("deploymentMode"));
        CHECK(normal["theme"] == "dark");
        const auto threep =
            readJson(root / "Claude-3p" / "claude_desktop_config.json");
        CHECK(!threep.contains("deploymentMode"));
        const auto meta =
            readJson(root / "Claude-3p" / "configLibrary" / "_meta.json");
        CHECK(!meta.contains("appliedId"));
        CHECK(meta["entries"].empty());
        CHECK(fs::exists(root / "Claude-3p" / "configLibrary" /
                         "00000000-0000-4000-8000-000000157210.json"));
        CHECK(s.group("claude").current.empty());

        // opencode / pi 没有官方默认状态 → 抛错
        bool threw = false;
        try {
            s.restoreOfficial("opencode");
        } catch (const std::exception& e) {
            threw = true;
            CHECK(std::string_view(e.what()).contains("没有官方默认状态"));
        }
        CHECK(threw);
        threw = false;
        try {
            s.restoreOfficial("pi");
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
    }

    // 14. 预设与官方厂商：官方走列表常驻卡（officialVendorName），不进预设
    {
        CHECK(models::officialVendorName("claude-code") == "Anthropic 官方");
        CHECK(models::officialVendorName("claude") == "Anthropic 官方");
        CHECK(models::officialVendorName("codex") == "OpenAI 官方");
        CHECK(models::officialVendorName("opencode").empty());
        CHECK(models::officialVendorName("pi").empty());
        // 订阅组：claude-code 首位 PackyCode，端点为直连地址（fullUrl）；
        // 按量组：claude-code 首位 DeepSeek，codex 首位 OpenRouter；claude 无预设。
        const auto cc = models::builtinPresets("claude-code");
        CHECK(!cc.subscription.empty() && cc.subscription.front().name == "PackyCode");
        CHECK(cc.subscription.front().fullUrl);
        CHECK(!cc.metered.empty() && cc.metered.front().name == "DeepSeek");
        const auto cx = models::builtinPresets("codex");
        CHECK(!cx.subscription.empty() && cx.subscription.front().name == "PackyCode");
        CHECK(!cx.metered.empty() && cx.metered.front().name == "OpenRouter");
        const auto cd = models::builtinPresets("claude");
        CHECK(cd.subscription.empty() && cd.metered.empty());
        // Kimi For Coding：主模型与三档映射都填端点别名 kimi-for-coding。
        const auto kfc = std::ranges::find_if(cc.subscription,
            [](const models::Provider& p) { return p.name == "Kimi For Coding"; });
        CHECK(kfc != cc.subscription.end() && kfc->model == "kimi-for-coding" &&
              kfc->haikuModel == "kimi-for-coding" && kfc->opusModel == "kimi-for-coding");
    }

    // 15. 三档模型映射：claude-code env 写入/收回/擦除 + desktop
    // inferenceModels 映射条目 + 序列化往返
    {
        // claude-code：主模型 + 三档映射写入 env 六键
        models::Provider pm{.name = "映射",
                            .baseUrl = "https://map.example.com",
                            .apiKey = "sk-map",
                            .model = "main-model",
                            .haikuModel = "haiku-x",
                            .sonnetModel = "sonnet-x",
                            .opusModel = "opus-x",
                            .haikuDisplayName = "Haiku 显示名",
                            .sonnetDisplayName = "Sonnet 显示名",
                            .opusDisplayName = "Opus 显示名",
                            .haikuSupports1m = true};
        s.addProvider("claude-code", pm);
        const std::string idMap = s.group("claude-code").providers.back().id;
        s.switchTo("claude-code", idMap);
        const auto jm = readJson(claudeSettings);
        CHECK(jm["env"]["ANTHROPIC_MODEL"] == "main-model");
        CHECK(jm["env"]["ANTHROPIC_DEFAULT_HAIKU_MODEL"] == "haiku-x");
        CHECK(jm["env"]["ANTHROPIC_DEFAULT_SONNET_MODEL"] == "sonnet-x");
        CHECK(jm["env"]["ANTHROPIC_DEFAULT_OPUS_MODEL"] == "opus-x");
        // 序列化往返：重新 load 后映射字段仍在
        {
            auto re = store::ProviderStore::load();
            bool found = false;
            for (const auto& p : re.group("claude-code").providers) {
                if (p.id == idMap) {
                    found = true;
                    CHECK(p.haikuModel == "haiku-x");
                    CHECK(p.sonnetModel == "sonnet-x");
                    CHECK(p.opusModel == "opus-x");
                    CHECK(p.haikuDisplayName == "Haiku 显示名");
                    CHECK(p.sonnetDisplayName == "Sonnet 显示名");
                    CHECK(p.opusDisplayName == "Opus 显示名");
                    CHECK(p.haikuSupports1m);
                }
            }
            CHECK(found);
        }
        // importLive 收回映射（换一把 key 让收编新建条目）
        writeFile(claudeSettings, R"json({"env": {"ANTHROPIC_BASE_URL": "https://imp2.example.com", "ANTHROPIC_AUTH_TOKEN": "sk-imp2", "ANTHROPIC_DEFAULT_HAIKU_MODEL": "hk-imp"}}
)json");
        const auto imp = s.importLive("claude-code");
        CHECK(imp.name == "当前配置");
        CHECK(imp.haikuModel == "hk-imp");
        // restoreOfficial 连映射键一起擦除
        s.restoreOfficial("claude-code");
        const auto je = readJson(claudeSettings);
        CHECK(!je["env"].contains("ANTHROPIC_BASE_URL"));
        CHECK(!je["env"].contains("ANTHROPIC_DEFAULT_HAIKU_MODEL"));
        CHECK(!je["env"].contains("ANTHROPIC_DEFAULT_SONNET_MODEL"));
        CHECK(!je["env"].contains("ANTHROPIC_DEFAULT_OPUS_MODEL"));

        // claude desktop：主模型 + 映射 → inferenceModels 条目
        // （safe 名直写 name；非 safe 借该档 route id，显示名写 labelOverride，
        // supports1m 按字段写入）
        models::Provider pdm{.name = "桌面映射",
                             .baseUrl = "https://d3.example.com",
                             .apiKey = "sk-d3",
                             .model = "claude-sonnet-4-6",
                             .modelSupports1m = true,
                             .haikuModel = "deepseek-chat",
                             .opusModel = "kimi-k2",
                             .haikuDisplayName = "DeepSeek Haiku",
                             .opusDisplayName = "Kimi Opus",
                             .haikuSupports1m = true,
                             .opusSupports1m = true};
        s.addProvider("claude", pdm);
        const std::string idDM = s.group("claude").providers.back().id;
        s.switchTo("claude", idDM);
        const auto profile = readJson(
            root / "Claude-3p" / "configLibrary" /
            "00000000-0000-4000-8000-000000157210.json");
        CHECK(profile["inferenceModels"].size() == 3);
        CHECK(profile["inferenceModels"][0]["name"] == "claude-sonnet-4-6");
        CHECK(!profile["inferenceModels"][0].contains("labelOverride"));
        CHECK(profile["inferenceModels"][0]["supports1m"] == true);
        CHECK(profile["inferenceModels"][1]["name"] == "claude-haiku-4-5");
        CHECK(profile["inferenceModels"][1]["labelOverride"] == "DeepSeek Haiku");
        CHECK(profile["inferenceModels"][1]["supports1m"] == true);
        CHECK(profile["inferenceModels"][2]["name"] == "claude-opus-4-8");
        CHECK(profile["inferenceModels"][2]["labelOverride"] == "Kimi Opus");
        CHECK(profile["inferenceModels"][2]["supports1m"] == true);
        // importLive 收回：labelOverride 优先，按 route id 前缀归档
        writeFile(root / "Claude-3p" / "configLibrary" /
                      "00000000-0000-4000-8000-000000157210.json",
                  R"json({
  "inferenceGatewayBaseUrl": "https://imp3.example.com",
  "inferenceGatewayApiKey": "sk-imp3",
  "inferenceModels": [
    {"name": "claude-haiku-4-5", "labelOverride": "hk-real", "supports1m": true},
    {"name": "claude-opus-4-9"}
  ]
}
)json");
        const auto imd = s.importLive("claude");
        CHECK(imd.name == "当前配置");
        CHECK(imd.model == "hk-real");  // 第一条同时填 model
        CHECK(imd.modelSupports1m);
        CHECK(imd.haikuModel == "hk-real");
        CHECK(imd.opusModel == "claude-opus-4-9");
        CHECK(imd.sonnetModel.empty());
    }

    // 16. 旧格式 config.json（顶层 claude/codex）→ load 自动迁移成 groups 键
    // 迁移前先清掉所有 live 文件，避免首次导入收编干扰断言。
    fs::remove_all(home / ".claude");
    fs::remove_all(home / ".codex");
    fs::remove_all(root / "opencode");
    fs::remove_all(piDir);
    fs::remove_all(deskDir);
    fs::remove_all(root / "Claude-3p");
    writeFile(cfg::configFile(), R"json({
  "claude": {"providers": [{"id": "old-1", "name": "旧Claude", "baseUrl": "https://old.example.com", "apiKey": "sk-old"}], "current": "old-1"},
  "codex": {"providers": [{"id": "old-2", "name": "旧Codex", "apiKey": "sk-old-codex"}], "current": ""},
  "themeMode": "dark",
  "usageRefreshMinutes": 5
}
)json");
    {
        auto migrated = store::ProviderStore::load();
        CHECK(migrated.group("claude-code").providers.size() == 1);
        CHECK(migrated.group("claude-code").providers[0].id == "old-1");
        CHECK(migrated.group("claude-code").current == "old-1");
        CHECK(migrated.group("codex").providers.size() == 1);
        CHECK(migrated.group("codex").providers[0].id == "old-2");
        CHECK(migrated.config().themeMode == "dark");
        // 旧版本的全局刷新间隔迁移到旧 Provider。
        CHECK(migrated.group("claude-code").providers[0].usageRefreshMinutes ==
              5);
        CHECK(migrated.group("codex").providers[0].usageRefreshMinutes == 5);
    }

    // 17. 损坏的 config.json → load 不崩溃，坏文件被挪到 .corrupt-<时间戳>
    writeFile(cfg::configFile(), "这不是 JSON {{{\n");
    {
        auto broken = store::ProviderStore::load();
        CHECK(broken.config().groups.empty());
        CHECK(!fs::exists(cfg::configFile()));  // 已被挪走且未重建
        bool corruptMoved = false;
        for (const auto& entry : fs::directory_iterator(cfg::dataDir())) {
            if (entry.path().filename().string().starts_with("config.json.corrupt-")) {
                corruptMoved = true;
            }
        }
        CHECK(corruptMoved);
    }

    // 18. 清理临时目录
    {
        std::error_code ec;
        fs::remove_all(root, ec);
        CHECK(!ec);
    }

    if (g_failures == 0) {
        std::println("test_store: ok");
        return 0;
    }
    std::println(stderr, "test_store: {} 项断言失败", g_failures);
    return 1;
}
