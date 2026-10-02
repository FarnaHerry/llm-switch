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

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
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

// dsh settings.yaml 文本里截出某个 providers 条目的块（首行 "    <key>:" 到
// 下一个同缩进键之前）。dsh 是增量多供应商，文件里同时存在多条本应用条目，
// 「某个供应商有没有声明某字段」必须按条目块查，不能整文件 find。
std::string settingsEntryBlock(const std::string& text, const std::string& key) {
    const std::string header = "    " + key + ":";
    const auto start = text.find("\n" + header);
    if (start == std::string::npos) return {};
    std::size_t pos = start + 1;
    for (;;) {
        pos = text.find("\n    ", pos + 1);
        if (pos == std::string::npos) return text.substr(start);
        if (pos + 5 < text.size() && text[pos + 5] != ' ') {
            return text.substr(start, pos - start);
        }
    }
}

// 某个 providers 条目块的文档顺序下标（-1 = 不存在）：用来验证「新增的条目
// 追加在最后、既有条目原样保留」。
int settingsEntryIndex(const std::string& text, const std::string& key) {
    int index = -1;
    const std::string header = "\n    " + key + ":";
    std::size_t pos = 0;
    for (;;) {
        pos = text.find("\n    ", pos);
        if (pos == std::string::npos) return index;
        if (pos + 5 < text.size() && text[pos + 5] != ' ') ++index;
        if (text.compare(pos, header.size(), header) == 0) return index;
        ++pos;
    }
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
    const fs::path geminiDir = root / "gemini";
    const fs::path geminiEnv = geminiDir / ".env";
    const fs::path geminiSettings = geminiDir / "settings.json";
    const fs::path qwenDir = root / "qwen";
    const fs::path qwenEnv = qwenDir / ".env";
    const fs::path qwenSettings = qwenDir / "settings.json";
    const fs::path zcodeConfig = root / "zcode" / "config.json";
    const fs::path dshDir = root / "dsh";
    const fs::path dshSettings = dshDir / "settings.yaml";
    const fs::path dshCredentials = dshDir / ".credentials.yaml";
    const fs::path hermesConfig = root / "hermes" / "config.yaml";
    testenv::setenv("LLMSWITCH_CLAUDE_SETTINGS", claudeSettings);
    testenv::setenv("LLMSWITCH_CODEX_AUTH", codexAuth);
    testenv::setenv("LLMSWITCH_CODEX_CONFIG", codexConfig);
    testenv::setenv("LLMSWITCH_OPENCODE_CONFIG", opencodeConfig);
    testenv::setenv("LLMSWITCH_PI_DIR", piDir);
    testenv::setenv("LLMSWITCH_GEMINI_DIR", geminiDir);
    testenv::setenv("LLMSWITCH_QWEN_DIR", qwenDir);
    testenv::setenv("LLMSWITCH_ZCODE_CONFIG", zcodeConfig);
    testenv::setenv("LLMSWITCH_DSH_SETTINGS", dshSettings);
    testenv::setenv("LLMSWITCH_DSH_CREDENTIALS", dshCredentials);
    testenv::setenv("LLMSWITCH_HERMES_CONFIG", hermesConfig);
    testenv::unsetenv("PI_CODING_AGENT_DIR");
    testenv::unsetenv("DSH_HOME");
    testenv::unsetenv("HERMES_HOME");
    testenv::unsetenv("LLMSWITCH_CLAUDE_DESKTOP_DIR");  // 默认 Linux 不支持

    // 1. 空环境 load → 默认空配置
    {
        auto s = store::ProviderStore::load();
        CHECK(s.config().groups.empty());
        CHECK(s.config().themeMode == "system");
        CHECK(s.config().closeBehavior == "ask");
        CHECK(s.config().routerTools.size() == models::toolRegistry().size());
        CHECK(std::ranges::find(s.config().routerTools, "claude-code") !=
              s.config().routerTools.end());
        CHECK(s.detectCurrent("claude-code").empty());
        CHECK(s.detectCurrent("claude").empty());  // Linux 无桌面目录 → 空
        // 注册表自检：10 个工具、id 可互查
        CHECK(models::toolRegistry().size() == 10);
        CHECK(models::findTool("claude-code") != nullptr);
        CHECK(models::findTool("opencode")->needsModel);
        CHECK(models::findTool("pi")->hasApiFormat);
        CHECK(models::findTool("dsh")->needsModel);
        CHECK(models::findTool("dsh")->hasApiFormat);
        CHECK(!models::findTool("dsh")->hasModelMappings);
        // 增量供应商：dsh / zcode 的 live 文件里同时存在多条本应用路由，
        // 页面走左右两列对照而非单选中。其余工具仍是「切换 = 只留一条」。
        CHECK(models::findTool("dsh")->additiveProviders);
        CHECK(models::findTool("zcode")->additiveProviders);
        CHECK(std::ranges::none_of(
            models::toolRegistry(), [](const auto& t) {
                return t.id != "dsh" && t.id != "zcode" &&
                       t.additiveProviders;
            }));
        CHECK(models::findTool("hermes")->needsModel);
        CHECK(models::findTool("hermes")->hasApiFormat);
        CHECK(models::findTool("hermes")->needsRestart);
        CHECK(models::findTool("gemini")->needsModel);
        CHECK(models::findTool("qwen")->needsModel);
        CHECK(!models::findTool("gemini")->hasApiFormat);
        CHECK(models::findTool("zcode")->hasApiFormat);
        CHECK(!models::findTool("codex")->needsModel);
        CHECK(models::findTool("claude-code")->hasModelMappings);
        CHECK(models::findTool("claude")->hasModelMappings);
        CHECK(!models::findTool("codex")->hasModelMappings);
        // needsRestart：claude-code 运行中重读配置、dsh 两份 YAML 均被热
        // 监听，切换无需重启；其余工具（codex / zcode / hermes 等）live
        // 配置在进程启动时读取，切换后需重启
        CHECK(!models::findTool("claude-code")->needsRestart);
        CHECK(!models::findTool("dsh")->needsRestart);
        CHECK(std::ranges::all_of(
            models::toolRegistry(), [](const auto& t) {
                return t.id == "claude-code" || t.id == "dsh" || t.needsRestart;
            }));
        CHECK(std::ranges::find(s.config().routerTools, "dsh") !=
              s.config().routerTools.end());
        CHECK(std::ranges::find(s.config().routerTools, "hermes") !=
              s.config().routerTools.end());
        CHECK(models::findTool("nope") == nullptr);
    }

    // 1a. 窗口关闭行为持久化，旧配置缺字段默认询问，未知值收敛为询问。
    {
        auto closeSettings = store::ProviderStore::load();
        closeSettings.setCloseBehavior("tray");
        CHECK(closeSettings.config().closeBehavior == "tray");
        auto reloaded = store::ProviderStore::load();
        CHECK(reloaded.config().closeBehavior == "tray");
        reloaded.setCloseBehavior("quit");
        CHECK(reloaded.config().closeBehavior == "quit");
        reloaded.setCloseBehavior("unknown");
        CHECK(reloaded.config().closeBehavior == "ask");
        CHECK(models::fromJson(nlohmann::json::parse(
                  R"json({"closeBehavior":"unknown"})json"))
                  .closeBehavior == "ask");
    }

    // 1b. Codex 官方订阅使用 OAuth auth.json（没有 OPENAI_API_KEY）时，不能
    // 收编成空白第三方卡；同时清理旧版本已经生成的「当前配置」占位卡。
    writeFile(codexAuth,
              R"json({"auth_mode":"chatgpt","tokens":{"access_token":"oauth-token"}})json");
    writeFile(cfg::configFile(), R"json({
  "groups": {
    "codex": {
      "current": "legacy-blank",
      "providers": [{
        "id": "legacy-blank",
        "name": "当前配置",
        "baseUrl": "",
        "apiKey": "",
        "model": "",
        "codexConfigToml": ""
      }]
    }
  }
})json");
    auto officialCodex = store::ProviderStore::load();
    CHECK(officialCodex.group("codex").providers.empty());
    CHECK(officialCodex.group("codex").current.empty());
    CHECK(officialCodex.detectCurrent("codex").empty());
    CHECK(officialCodex.importLive("codex").id.empty());
    CHECK(readJson(cfg::configFile())["groups"]["codex"]["providers"].empty());
    fs::remove(codexAuth);

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
    // 8b. gemini/qwen：.env 行级 upsert（保留既有变量与注释）、auth 类型
    // 深合并、detect/import 往返、restore 删行不碰其他变量。
    {
        writeFile(geminiEnv,
                  "# gemini env\nGEMINI_API_KEY=old-key\nCUSTOM_FLAG=1\n");
        writeFile(geminiSettings, R"json({"theme": "dark"}
)json");
        models::Provider pg{.name = "Gemini 中转",
                            .baseUrl = "https://relay.example.com",
                            .apiKey = "sk-gem",
                            .model = "gemini-3-pro"};
        s.addProvider("gemini", pg);
        const std::string idG = s.group("gemini").providers.back().id;
        s.switchTo("gemini", idG);
        {
            const std::string envText = readTextFile(geminiEnv);
            CHECK(envText.find("GEMINI_API_KEY=sk-gem") != std::string::npos);
            CHECK(envText.find("GOOGLE_GEMINI_BASE_URL=https://relay.example.com") !=
                  std::string::npos);
            CHECK(envText.find("GEMINI_MODEL=gemini-3-pro") != std::string::npos);
            CHECK(envText.find("CUSTOM_FLAG=1") != std::string::npos);      // 无关变量保留
            CHECK(envText.find("# gemini env") != std::string::npos);       // 注释保留
            CHECK(envText.find("old-key") == std::string::npos);            // 旧值被替换
            const auto settings = readJson(geminiSettings);
            CHECK(settings["security"]["auth"]["selectedType"] == "gemini-api-key");
            CHECK(settings["theme"] == "dark");  // 深合并保留无关字段
            CHECK(s.detectCurrent("gemini") == idG);
        }
        // importLive 往返：改 .env 后收编为新供应商并置 current。
        writeFile(geminiEnv,
                  "GEMINI_API_KEY=sk-live\nGOOGLE_GEMINI_BASE_URL=https://live.example.com\n");
        const auto imported = s.importLive("gemini");
        CHECK(imported.apiKey == "sk-live");
        CHECK(s.detectCurrent("gemini") == imported.id);
        // restoreOfficial：三行删除、其他变量保留、selectedType 移除。
        writeFile(geminiEnv, "GEMINI_API_KEY=sk-live\nGOOGLE_GEMINI_BASE_URL=https://live.example.com\nKEEP=1\n");
        s.restoreOfficial("gemini");
        {
            const std::string envText = readTextFile(geminiEnv);
            CHECK(envText.find("GEMINI_API_KEY") == std::string::npos);
            CHECK(envText.find("GOOGLE_GEMINI_BASE_URL") == std::string::npos);
            CHECK(envText.find("KEEP=1") != std::string::npos);
            const auto settings = readJson(geminiSettings);
            CHECK(!settings["security"]["auth"].contains("selectedType"));
            CHECK(s.detectCurrent("gemini").empty());
        }

        // qwen：OPENAI_* 变量族 + openai 认证类型，同一路径的第二个实例。
        models::Provider pq{.name = "Qwen 官方中转",
                            .baseUrl = "https://dashscope.example.com/compatible-mode/v1",
                            .apiKey = "sk-qwen",
                            .model = "qwen3.7-plus"};
        s.addProvider("qwen", pq);
        const std::string idQ = s.group("qwen").providers.back().id;
        s.switchTo("qwen", idQ);
        {
            const std::string envText = readTextFile(qwenEnv);
            CHECK(envText.find("OPENAI_API_KEY=sk-qwen") != std::string::npos);
            CHECK(envText.find("OPENAI_BASE_URL=https://dashscope.example.com/compatible-mode/v1") !=
                  std::string::npos);
            CHECK(envText.find("OPENAI_MODEL=qwen3.7-plus") != std::string::npos);
            const auto settings = readJson(qwenSettings);
            CHECK(settings["security"]["auth"]["selectedType"] == "openai");
            CHECK(s.detectCurrent("qwen") == idQ);
            const auto imported = s.importLive("qwen");
            CHECK(imported.apiKey == "sk-qwen");
            CHECK(s.detectCurrent("qwen") == imported.id);
        }

        // 8c. zcode：provider upsert + enabled 互斥 + kind 映射 + detect/
        // import/restore。预置 builtin 与手填条目验证互斥与恢复。
        writeFile(zcodeConfig, R"json({"provider": {
            "builtin:anthropic": {"name": "Anthropic", "kind": "anthropic", "options": {"apiKey": "builtin-key"}, "enabled": true, "source": "builtin"},
            "custom:manual": {"name": "手填", "kind": "openai", "options": {"apiKey": "k2", "baseURL": "https://m.example.com"}, "enabled": false, "models": {"m1": {}, "m2": {}}}
        }})json");
        // 主模型不在清单中：写入即清单本身，不把主模型塞进 ZCode 的
        // 模型备选（ZCode 条目只有清单，没有主模型概念）。
        models::Provider pz{.name = "ZCode 中转",
                            .baseUrl = "https://z.example.com",
                            .apiKey = "sk-z",
                            .model = "glm-5-extra",
                            .models = {"glm-5-air", "glm-5-pro"},
                            .apiFormat = "anthropic"};
        s.addProvider("zcode", pz);
        const std::string idZ = s.group("zcode").providers.back().id;
        s.switchTo("zcode", idZ);
        {
            const auto doc = readJson(zcodeConfig);
            const auto& entry = doc["provider"]["llmswitch:" + idZ];
            CHECK(entry["enabled"] == true);
            CHECK(entry["kind"] == "anthropic");  // apiFormat → provider.kind
            CHECK(entry["options"]["baseURL"] == "https://z.example.com");
            CHECK(entry["models"].contains("glm-5-air"));
            CHECK(entry["models"].contains("glm-5-pro"));
            CHECK(!entry["models"].contains("glm-5-extra"));
            // 互斥只停本应用托管（llmswitch:*）条目；builtin 与 ZCode 原生
            // 自建条目的启停由用户在 ZCode 侧管理，保持原状。
            CHECK(doc["provider"]["builtin:anthropic"]["enabled"] == true);
            CHECK(doc["provider"]["custom:manual"]["enabled"] == false);
            CHECK(s.detectCurrent("zcode") == idZ);
            // 导入：models 不定长清单全量读出（DS 等多模型条目不丢模型）。
            {
                auto s2 = store::ProviderStore::load();
                // 持久组此时非空（switchTo 已落盘），改走 importLive 直接断言。
                const auto imported = s.importLive("zcode");
                CHECK(imported.apiKey == "sk-z");
                bool sawM1 = false;
                bool sawM2 = false;
                for (const auto& p : s.group("zcode").providers) {
                    if (p.id == "custom:manual") {
                        sawM1 = sawM2 = false;
                        for (const auto& m : p.models) {
                            sawM1 = sawM1 || m == "m1";
                            sawM2 = sawM2 || m == "m2";
                        }
                    }
                }
                CHECK(sawM1);
                CHECK(sawM2);
            }
        }
        // 切第二家（openai 协议）：前一家停用、kind 映射 openai。
        models::Provider pz2{.name = "ZCode 二",
                             .baseUrl = "https://z2.example.com",
                             .apiKey = "sk-z2",
                             .model = "m2",
                             .apiFormat = "openai-chat"};
        s.addProvider("zcode", pz2);
        const std::string idZ2 = s.group("zcode").providers.back().id;
        s.switchTo("zcode", idZ2);
        CHECK(readJson(zcodeConfig)["provider"]["llmswitch:" + idZ]["enabled"] == false);
        CHECK(readJson(zcodeConfig)["provider"]["llmswitch:" + idZ2]["kind"] ==
              "openai-compatible");  // ZCode 规范拼写
        // 清单为空、主模型非空 → 退化为单模型清单。
        CHECK(readJson(zcodeConfig)["provider"]["llmswitch:" + idZ2]["models"]
                  .contains("m2"));
        CHECK(s.detectCurrent("zcode") == idZ2);
        // importLive：收编当前 enabled 条目。
        const auto importedZ = s.importLive("zcode");
        CHECK(importedZ.apiKey == "sk-z2");
        CHECK(s.detectCurrent("zcode") == importedZ.id);
        // restoreOfficial：llmswitch:* 停用、builtin 启用、detect 归零。
        s.restoreOfficial("zcode");
        {
            const auto doc = readJson(zcodeConfig);
            CHECK(doc["provider"]["llmswitch:" + idZ2]["enabled"] == false);
            CHECK(doc["provider"]["builtin:anthropic"]["enabled"] == true);
            CHECK(s.detectCurrent("zcode").empty());
        }

        // 启动自动收编：持久组换成旧版残留（一条 builtin:* 卡）+ live 文件
        // 存在 → load() 清掉 builtin 残留、读出全部自建条目（含未启用的）。
        // 仅 builtin 原生启用 = 官方原生状态，current 保持为空（「ZCode
        // 官方」卡亮起）。
        {
            auto persisted = readJson(cfg::configFile());
            persisted["groups"]["zcode"] = {
                {"current", "builtin:ghost"},
                {"providers",
                 nlohmann::json::array({nlohmann::json{
                     {"id", "builtin:ghost"},
                     {"name", "旧版残留"},
                     {"baseUrl", "https://ghost.example.com"},
                     {"apiKey", "sk-ghost"},
                     {"createdAt", 1}}})}};
            writeFile(cfg::configFile(), persisted.dump(2) + "\n");
            auto s2 = store::ProviderStore::load();
            const auto& zg = s2.group("zcode");
            CHECK(zg.providers.size() == 3);  // 手填 + 两条 llmswitch
            CHECK(zg.current.empty());
            bool foundGhost = false;
            bool foundCustom = false;
            for (const auto& p : zg.providers) {
                if (p.id == "custom:manual") foundCustom = true;
                if (p.id == "builtin:ghost") foundGhost = true;
            }
            CHECK(foundCustom);
            CHECK(!foundGhost);
        }

        // 组非空也同步：ZCode 侧模型清单变化（新增 m3）→ 启动即刷新。
        writeFile(zcodeConfig, R"json({"provider": {
            "builtin:anthropic": {"name": "Anthropic", "kind": "anthropic", "options": {"apiKey": "builtin-key"}, "enabled": true, "source": "builtin"},
            "custom:manual": {"name": "手填", "kind": "openai", "options": {"apiKey": "k2", "baseURL": "https://m.example.com"}, "enabled": false, "models": {"m1": {}, "m2": {}, "m3": {}}}
        }})json");
        {
            auto s3 = store::ProviderStore::load();
            bool sawM3 = false;
            for (const auto& p : s3.group("zcode").providers) {
                if (p.id == "custom:manual") {
                    for (const auto& m : p.models) sawM3 = sawM3 || m == "m3";
                }
            }
            CHECK(sawM3);
        }

        // 托管条目（llmswitch:*）生效时启动同步：current 跟到该条目，
        // 官方卡熄灭；builtin 全部停用不影响 current 指向。
        {
            nlohmann::json doc;
            auto& builtin = doc["provider"]["builtin:anthropic"];
            builtin = {{"name", "Anthropic"},
                       {"kind", "anthropic"},
                       {"options", {{"apiKey", "builtin-key"}}},
                       {"enabled", false},
                       {"source", "builtin"}};
            auto& managed = doc["provider"]["llmswitch:" + idZ2];
            managed = {{"name", "ZCode 二"},
                       {"kind", "openai"},
                       {"options",
                        {{"apiKey", "sk-z2"},
                         {"baseURL", "https://z2.example.com"}}},
                       {"enabled", true}};
            writeFile(zcodeConfig, doc.dump(2) + "\n");
            auto s4 = store::ProviderStore::load();
            CHECK(s4.group("zcode").current == idZ2);
        }

        // 同端点+同密钥的两条自建条目 = 两个供应商：收编按条目键区分身份，
        // 不按端点+密钥合并（ZCode 页面显示几条就收编几条）。
        writeFile(zcodeConfig, R"json({"provider": {
            "dup-one": {"name": "重复一", "kind": "openai", "options": {"apiKey": "k9", "baseURL": "https://dup.example.com"}, "enabled": false},
            "dup-two": {"name": "重复二", "kind": "openai", "options": {"apiKey": "k9", "baseURL": "https://dup.example.com"}, "enabled": false}
        }})json");
        {
            auto s5 = store::ProviderStore::load();
            int dupCount = 0;
            for (const auto& p : s5.group("zcode").providers) {
                if (p.baseUrl == "https://dup.example.com") ++dupCount;
            }
            CHECK(dupCount == 2);
        }

        // 保存即同步 ZCode 条目：新增建条目（未启用），编辑原位更新并保持
        // 启用状态（启用互斥只在切换时发生）；每模型参数原值随写回回放，
        // 无原值的模型落 ZCode 兼容最小条目。
        {
            models::Provider pn{.name = "保存同步",
                                .baseUrl = "https://sync.example.com",
                                .apiKey = "sk-sync",
                                .models = {"sm1"},
                                .apiFormat = "openai-chat"};
            pn.modelsMeta = nlohmann::json::object(
                {{"sm1",
                  nlohmann::json::object(
                      {{"modalities",
                        nlohmann::json::object({{"input",
                                                 nlohmann::json::array(
                                                     {"text", "image"})}})},
                       {"limit",
                        nlohmann::json::object({{"context", 1000000},
                                                {"output", 128000}})}})}});
            const std::string idN = s.addProvider("zcode", pn);
            {
                const auto doc = readJson(zcodeConfig);
                const auto& entry = doc["provider"]["llmswitch:" + idN];
                CHECK(entry["enabled"] == false);
                CHECK(entry["models"].contains("sm1"));
                CHECK(entry["models"]["sm1"]["modalities"]["input"][1] ==
                      "image");
            }
            s.switchTo("zcode", idN);
            models::Provider edited = s.group("zcode").providers.back();
            edited.models = {"sm1", "sm2"};
            s.updateProvider("zcode", edited);
            {
                const auto doc = readJson(zcodeConfig);
                const auto& entry = doc["provider"]["llmswitch:" + idN];
                CHECK(entry["enabled"] == true);
                CHECK(entry["models"].contains("sm2"));
                CHECK(entry["models"]["sm2"]["zcode"]["priority"] == 100);
                CHECK(entry["models"]["sm1"]["modalities"]["input"][1] ==
                      "image");
                CHECK(entry["models"]["sm1"]["limit"]["context"] == 1000000);
                CHECK(entry["models"]["sm1"]["limit"]["output"] == 128000);
            }
            // 启用/停用开关只翻该条目。
            s.setZcodeEntryEnabled(idN, false);
            {
                const auto doc = readJson(zcodeConfig);
                CHECK(doc["provider"]["llmswitch:" + idN]["enabled"] == false);
            }
        }

        // ZCode 原生自建条目（键 = 裸 id，无 enabled 字段 = 启用）：读状态
        // 缺省视为启用；保存同步原位更新（不另起 llmswitch: 重复条目、不
        // 补写 enabled、options 里 ZCode 自己的键原样保留）；切换只显式
        // 启用目标条目，互斥不波及其他原生条目；detect 命中原生条目。
        writeFile(zcodeConfig, R"json({"provider": {
            "native-a": {"name": "原生甲", "kind": "anthropic", "options": {"apiKey": "kn-a", "baseURL": "https://na.example.com", "apiKeyRequired": true}, "source": "custom", "models": {"na-1": {}}},
            "native-b": {"name": "原生乙", "kind": "openai-compatible", "options": {"apiKey": "kn-b", "baseURL": "https://nb.example.com"}, "enabled": false, "models": {"nb-1": {}}}
        }})json");
        {
            auto s6 = store::ProviderStore::load();
            bool sawA = false;
            models::Provider pe;
            for (const auto& p : s6.group("zcode").providers) {
                if (p.id == "native-a") {
                    sawA = true;
                    pe = p;
                }
            }
            CHECK(sawA);
            // enabled 缺省 = 启用；显式 false = 停用；current 跟到原生条目。
            CHECK(s6.zcodeEntryEnabled("native-a") == true);
            CHECK(s6.zcodeEntryEnabled("native-b") == false);
            CHECK(s6.group("zcode").current == "native-a");
            // 保存同步：原位更新，不另起重复条目、不补写 enabled、
            // apiKeyRequired 等 ZCode 自有键保留。
            pe.name = "原生甲改";
            pe.models = {"na-1", "na-2"};
            s6.updateProvider("zcode", pe);
            {
                const auto doc = readJson(zcodeConfig);
                CHECK(!doc["provider"].contains("llmswitch:native-a"));
                const auto& entry = doc["provider"]["native-a"];
                CHECK(!entry.contains("enabled"));
                CHECK(entry["name"] == "原生甲改");
                CHECK(entry["options"]["apiKeyRequired"] == true);
                CHECK(entry["models"].contains("na-2"));
            }
            // 切换：目标原生条目本来就是启用态（enabled 缺省 = 启用），单条
            // 增量不无谓地给 ZCode 自己的条目补字段、也不重写它的内容；原生乙
            // 的 enabled false 不被互斥改写（互斥只覆盖本应用托管条目）。
            s6.switchTo("zcode", "native-a");
            {
                const auto doc = readJson(zcodeConfig);
                CHECK(!doc["provider"]["native-a"].contains("enabled"));
                CHECK(doc["provider"]["native-a"]["name"] == "原生甲改");
                CHECK(doc["provider"]["native-b"]["enabled"] == false);
                CHECK(s6.detectCurrent("zcode") == "native-a");
            }
            // 停用开关在原生条目上显式写 false。
            s6.setZcodeEntryEnabled("native-a", false);
            CHECK(readJson(zcodeConfig)["provider"]["native-a"]["enabled"] ==
                  false);
            CHECK(s6.zcodeEntryEnabled("native-a") == false);
            CHECK(s6.detectCurrent("zcode").empty());
        }
    }

    // 8c1. zcode 的单条增量（与 dsh 同一套不变式，两个工具的差别在
    // 「当前用哪条」= 条目自己的 enabled、密钥就在条目里）：
    //   - 实况快照带 key / providerId / builtin / enabled / 模型清单；
    //   - 每个入口只动自己那一条：新增 / 更新 / 写入 / 启用 / 删除都不碰别的
    //     条目（builtin:* 与 ZCode 原生条目原样保留）；
    //   - 收编不改 live 一字（原生条目保留它自己的键与内容）；
    //   - 删除分「只删本应用」（文件全等）与「连同 ZCode 一起删」；
    //   - 整组重建只发生在 syncZcodeProviders，且只清孤儿 llmswitch:*。
    {
        while (!s.group("zcode").providers.empty()) {
            s.removeProvider("zcode", s.group("zcode").providers.back().id,
                             /*eraseLive=*/false);
        }
        writeFile(zcodeConfig, R"json({"provider": {
            "builtin:zai": {"name": "Z.ai", "kind": "anthropic", "options": {"apiKey": "bk"}, "enabled": true, "source": "builtin"},
            "native-x": {"name": "原生X", "kind": "anthropic", "options": {"apiKey": "kx", "baseURL": "https://nx.example.com", "apiKeyRequired": true}, "models": {"nx-1": {}}}
        }})json");
        const nlohmann::json beforeBuiltin =
            readJson(zcodeConfig)["provider"]["builtin:zai"];
        const nlohmann::json beforeNative =
            readJson(zcodeConfig)["provider"]["native-x"];
        const auto providersOf = [&] {
            return readJson(zcodeConfig)["provider"];
        };
        // 实况快照：builtin 标记、enabled 缺省语义、模型清单、未纳管。
        {
            const auto live = s.zcodeLiveProviders();
            CHECK(live.size() == 2);
            CHECK(live[0].key == "builtin:zai");
            CHECK(live[0].builtin);
            CHECK(live[0].providerId.empty());
            CHECK(live[0].enabled);
            CHECK(live[1].key == "native-x");
            CHECK(!live[1].builtin);
            CHECK(live[1].enabled);  // 没有 enabled 字段 = 启用
            CHECK(live[1].apiFormat == "anthropic");
            CHECK(live[1].apiKey == "kx");
            CHECK(live[1].models == std::vector<std::string>({"nx-1"}));
        }
        // 新增：只多自己那一条，且新建条目不自动启用。
        models::Provider pA{.name = "A",
                            .baseUrl = "https://a.example.com/v1",
                            .apiKey = "sk-a",
                            .model = "a-1",
                            .models = {"a-1", "a-2"}};
        const std::string idA = s.addProvider("zcode", pA);
        {
            const auto providers = providersOf();
            CHECK(providers["native-x"] == beforeNative);
            CHECK(providers["builtin:zai"] == beforeBuiltin);
            CHECK(providers["llmswitch:" + idA]["kind"] ==
                  "openai-compatible");
            CHECK(providers["llmswitch:" + idA]["enabled"] == false);
            CHECK(providers["llmswitch:" + idA]["models"].contains("a-2"));
        }
        // 切换：只翻 enabled，不按本应用留存重建条目内容（用户在 ZCode 侧的
        // 手改必须留着），别的 llmswitch:* 条目只被停用、内容不动。
        models::Provider pB{.name = "B",
                            .baseUrl = "https://b.example.com/v1",
                            .apiKey = "sk-b",
                            .model = "b-1"};
        const std::string idB = s.addProvider("zcode", pB);
        s.switchTo("zcode", idB);
        {
            auto doc = readJson(zcodeConfig);
            doc["provider"]["llmswitch:" + idB]["handEdited"] = "keepB";
            doc["provider"]["llmswitch:" + idA]["handEdited"] = "keepA";
            writeFile(zcodeConfig, doc.dump(2) + "\n");
        }
        s.switchTo("zcode", idA);
        {
            const auto providers = providersOf();
            CHECK(providers["llmswitch:" + idA]["enabled"] == true);
            CHECK(providers["llmswitch:" + idA]["handEdited"] == "keepA");
            CHECK(providers["llmswitch:" + idB]["enabled"] == false);
            CHECK(providers["llmswitch:" + idB]["handEdited"] == "keepB");
            CHECK(providers["native-x"] == beforeNative);
            CHECK(providers["builtin:zai"] == beforeBuiltin);
        }
        // 「写入 / 更新」：只重建这一条（原生条目原位合并，保留 ZCode 自己
        // 维护的字段），别的条目一字不动。
        {
            auto doc = readJson(zcodeConfig);
            doc["provider"]["llmswitch:" + idA]["options"]["baseURL"] =
                "https://hacked.example.com";
            writeFile(zcodeConfig, doc.dump(2) + "\n");
        }
        s.writeZcodeProvider(idA);
        {
            const auto providers = providersOf();
            CHECK(providers["llmswitch:" + idA]["options"]["baseURL"] ==
                  "https://a.example.com/v1");
            CHECK(providers["llmswitch:" + idA]["handEdited"] == "keepA");
            CHECK(providers["native-x"] == beforeNative);
            CHECK(providers["builtin:zai"] == beforeBuiltin);
        }
        // 收编：live 一字不动（原生条目保留它自己的键与内容），本地开始记录。
        {
            const std::string beforeAdopt = readTextFile(zcodeConfig);
            const auto adopted = s.adoptZcodeProvider("native-x");
            CHECK(adopted.id == "native-x");
            CHECK(adopted.apiKey == "kx");
            CHECK(adopted.models == std::vector<std::string>({"nx-1"}));
            CHECK(readTextFile(zcodeConfig) == beforeAdopt);
            CHECK(s.group("zcode").current == "native-x");  // 缺省 = 启用
            bool sawManaged = false;
            for (const auto& entry : s.zcodeLiveProviders()) {
                if (entry.key == "native-x") {
                    sawManaged = entry.providerId == "native-x";
                }
            }
            CHECK(sawManaged);
        }
        // 删除：eraseLive=false 只收回本地留存（文件全等）；true 才摘掉那一条
        // （别的条目——含 builtin 与原生条目——原样保留）。
        {
            const std::string before = readTextFile(zcodeConfig);
            s.removeProvider("zcode", idB, /*eraseLive=*/false);
            CHECK(readTextFile(zcodeConfig) == before);
            CHECK(providersOf().contains("llmswitch:" + idB));
            s.removeProvider("zcode", idB, /*eraseLive=*/true);
            CHECK(!providersOf().contains("llmswitch:" + idB));
            CHECK(providersOf()["native-x"] == beforeNative);
            CHECK(providersOf()["builtin:zai"] == beforeBuiltin);
        }
        // 左列「从 ZCode 删除」：只删这一条；builtin 与不存在的键都抛错。
        {
            bool threwBuiltin = false;
            try {
                s.removeZcodeProvider("builtin:zai");
            } catch (const std::exception&) {
                threwBuiltin = true;
            }
            CHECK(threwBuiltin);
            CHECK(providersOf().contains("builtin:zai"));
            bool threwMissing = false;
            try {
                s.removeZcodeProvider("ghost");
            } catch (const std::exception&) {
                threwMissing = true;
            }
            CHECK(threwMissing);
            s.removeZcodeProvider("native-x");
            CHECK(!providersOf().contains("native-x"));
            CHECK(providersOf()["builtin:zai"] == beforeBuiltin);
        }
        // 整组重建（「全部写入 ZCode」）：组内每条写成 llmswitch:<id>（原生键
        // 已删的这条就新建），清掉孤儿 llmswitch:*，builtin 与原生条目不动。
        {
            auto doc = readJson(zcodeConfig);
            doc["provider"]["llmswitch:orphan"] =
                nlohmann::json{{"name", "孤儿"},
                               {"kind", "anthropic"},
                               {"options", {{"baseURL", "https://orphan.example.com"}}}};
            writeFile(zcodeConfig, doc.dump(2) + "\n");
        }
        s.syncZcodeProviders();
        {
            const auto providers = providersOf();
            CHECK(!providers.contains("llmswitch:orphan"));
            CHECK(providers.contains("llmswitch:native-x"));
            CHECK(providers.contains("llmswitch:" + idA));
            CHECK(providers["builtin:zai"] == beforeBuiltin);
            CHECK(providers["llmswitch:native-x"]["options"]["baseURL"] ==
                  "https://nx.example.com");
        }
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
    // 8c. pi：detectCurrent 优先按 defaultProvider 指向的那条条目精确匹配（历史
    // 条目排在前面也不能顶替）；切换保留条目里已有的其它模型（不裁成一条）；
    // 收编时 defaultModel 缺失则回退到条目首个模型；detect 是只读探测。
    {
        models::Provider pStale{.name = "Pi 历史",
                                .baseUrl = "https://pi-stale.example.com",
                                .apiKey = "sk-pi-stale",
                                .model = "stale-model"};
        models::Provider pActive{.name = "Pi 手写默认",
                                 .baseUrl = "https://pi-active.example.com",
                                 .apiKey = "sk-pi-active",
                                 .model = "active-model"};
        const std::string idStale = s.addProvider("pi", pStale);
        const std::string idActive = s.addProvider("pi", pActive);
        // models.json：本应用的历史条目排在前（切换只 upsert、从不删旧条目），
        // 实际生效的是用户手写的 "hand" 条目，凭据属于 pActive。
        writeFile(piModels,
                  "{\"providers\": {\"" + idStale +
                      "\": {\"baseUrl\": \"https://pi-stale.example.com\", "
                      "\"apiKey\": \"sk-pi-stale\", \"api\": "
                      "\"openai-completions\", \"models\": [\"stale-model\"]}, "
                      "\"hand\": {\"baseUrl\": \"https://pi-active.example.com\", "
                      "\"apiKey\": \"sk-pi-active\", \"api\": "
                      "\"openai-completions\", "
                      "\"models\": [\"m-a\", \"m-b\"]}}}\n");
        writeFile(piSettings, R"json({"defaultProvider": "hand"}
)json");
        CHECK(s.detectCurrent("pi") == idActive);  // 不能报到排在前面的历史条目

        // 收编：defaultModel 缺失 → 回退到条目第一个模型（与 dsh 一致）；
        // 凭据不与组内任何供应商重合，走「新建」而不是复用已有匹配项。
        writeFile(piModels,
                  R"json({"providers": {"ext-pi": {"baseUrl": "https://pi-import.example.com", "apiKey": "sk-pi-import", "api": "anthropic-messages", "models": ["m-a", "m-b"]}}}
)json");
        writeFile(piSettings, R"json({"defaultProvider": "ext-pi"}
)json");
        s.importLive("pi");
        models::Provider hand;
        for (const auto& p : s.group("pi").providers) {
            if (p.id == "ext-pi") hand = p;
        }
        CHECK(hand.id == "ext-pi");  // 复用 models.json 的条目键作为收编 id
        CHECK(hand.baseUrl == "https://pi-import.example.com");
        CHECK(hand.apiKey == "sk-pi-import");
        CHECK(hand.apiFormat == "anthropic");
        CHECK(hand.model == "m-a");

        // 切换：主模型不在清单里 → 追加，条目里已有的 m-a / m-b 必须保留
        hand.model = "m-c";
        s.updateProvider("pi", hand);
        s.switchTo("pi", "ext-pi");
        auto handModels = readJson(piModels)["providers"]["ext-pi"]["models"];
        CHECK(handModels.size() == 3);
        CHECK(handModels[0] == "m-a");
        CHECK(handModels[1] == "m-b");
        CHECK(handModels[2] == "m-c");
        // 再切一次（主模型已在清单里）→ 不重复追加，输出稳定
        s.switchTo("pi", "ext-pi");
        CHECK(readJson(piModels)["providers"]["ext-pi"]["models"].size() == 3);

        // 元素是 {"id": ...} 对象时按同一形状补条目
        writeFile(piModels,
                  R"json({"providers": {"ext-pi": {"baseUrl": "https://pi-import.example.com", "apiKey": "sk-pi-import", "api": "anthropic-messages", "models": [{"id": "m-a"}]}}}
)json");
        writeFile(piSettings, R"json({"defaultProvider": "ext-pi"}
)json");
        s.switchTo("pi", "ext-pi");
        const auto objModels = readJson(piModels)["providers"]["ext-pi"]["models"];
        CHECK(objModels.size() == 2);
        CHECK(objModels[0]["id"] == "m-a");
        CHECK(objModels[1]["id"] == "m-c");

        // detect 是只读探测：defaultProvider 不是组内 id 时会去读 models.json，
        // 坏文件按无内容处理，且不能被挪走（readJsonOrNull 会挪成
        // <file>.corrupt-<毫秒>）。
        writeFile(piSettings, R"json({"defaultProvider": "ghost"}
)json");
        writeFile(piModels, "{ not json\n");
        CHECK(s.detectCurrent("pi").empty());
        CHECK(fs::exists(piModels));
    }

    // 8d. dsh：settings.yaml 行级 upsert（删旧 llmswitch-* 条目 +
    // agent-default-model 指向，无关键/注释/内置路由保留）+ 密钥只进
    // .credentials.yaml + detect/import 往返 + restore 回内置官方路由。
    {
        writeFile(dshSettings,
                  "# dsh 设置\ntheme: dark\n"
                  "llm-pi-ai:\n"
                  "  providers:\n"
                  "    deepseek-official:\n"
                  "      api: anthropic-messages\n"
                  "      baseURL: https://api.deepseek.com/anthropic\n"
                  "    llmswitch-stale:\n"
                  "      api: openai-completions\n"
                  "      baseURL: https://stale.example.com\n");
        writeFile(dshCredentials,
                  "version: 1\nrefs:\n  OTHER_KEY: \"keep-me\"\n");
        models::Provider pd2{.name = "DeepSeek 中转",
                             .baseUrl = "https://relay.example.com/anthropic",
                             .apiKey = "sk-dsh",
                             .model = "deepseek-v4-flash",
                             .reasoningEfforts = {"high", "off", "low"},
                             .apiFormat = "anthropic"};
        s.addProvider("dsh", pd2);
        const std::string idS = s.group("dsh").providers.back().id;
        const auto envNameOf = [](std::string_view id) {
            std::string out = "LLMSWITCH_";
            for (const unsigned char c : id) {
                out += std::isalnum(c) ? static_cast<char>(std::toupper(c)) : '_';
            }
            return out;
        };
        s.switchTo("dsh", idS);
        {
            const std::string y = readTextFile(dshSettings);
            CHECK(y.find("# dsh 设置") != std::string::npos);   // 注释保留
            CHECK(y.find("theme: dark") != std::string::npos);  // 无关键保留
            CHECK(y.find("deepseek-official:") != std::string::npos);  // 内置路由保留
            // 单条增量：新增 + 切换只动自己这一条，文件里原有的孤儿
            // llmswitch-stale 不会被顺手清掉（清理只发生在「全部写入 dsh」）。
            CHECK(y.find("    llmswitch-stale:") != std::string::npos);
            CHECK(y.find("stale.example.com") != std::string::npos);
            CHECK(y.find("    llmswitch-" + idS + ":") != std::string::npos);
            CHECK(y.find("api: anthropic-messages") != std::string::npos);
            CHECK(y.find("baseURL: \"https://relay.example.com/anthropic\"") !=
                  std::string::npos);
            CHECK(y.find("apiKeyEnv: " + envNameOf(idS)) != std::string::npos);
            CHECK(y.find("- id: \"deepseek-v4-flash\"") != std::string::npos);
            // 推理档位：只声明选中的档位，按规范升序，off 留空（不发思考参数）。
            CHECK(y.find("          reasoningEfforts:") != std::string::npos);
            CHECK(y.find("            \"off\":\n") != std::string::npos);
            CHECK(y.find("            \"low\": low") != std::string::npos);
            CHECK(y.find("            \"high\": high") != std::string::npos);
            CHECK(y.find("minimal") == std::string::npos);
            CHECK(y.find("agent-default-model:") != std::string::npos);
            CHECK(y.find("provider: llmswitch-" + idS) != std::string::npos);
            CHECK(y.find("model: \"deepseek-v4-flash\"") != std::string::npos);
            CHECK(y.find("sk-dsh") == std::string::npos);  // 密钥不进 settings
            const std::string cred = readTextFile(dshCredentials);
            CHECK(cred.find("version: 1") != std::string::npos);  // version 保留
            CHECK(cred.find("OTHER_KEY: \"keep-me\"") != std::string::npos);
            CHECK(cred.find(envNameOf(idS) + ": \"sk-dsh\"") != std::string::npos);
#if !defined(_WIN32)
            // 权限：目录 0700、凭据文件 0600（Windows 无 POSIX 权限位语义）
            CHECK((fs::status(dshDir).permissions() & fs::perms::all) ==
                  fs::perms::owner_all);
            CHECK((fs::status(dshCredentials).permissions() & fs::perms::all) ==
                  (fs::perms::owner_read | fs::perms::owner_write));
#endif
            CHECK(s.detectCurrent("dsh") == idS);
            // 「全部写入 dsh」是显式的整组重建：它才负责清孤儿，并把组内全部
            // 供应商（这里只有 idS）逐条写进去。
            s.syncDshProviders({});
            const std::string yFull = readTextFile(dshSettings);
            CHECK(yFull.find("llmswitch-stale") == std::string::npos);
            CHECK(yFull.find("stale.example.com") == std::string::npos);
            CHECK(yFull.find("    llmswitch-" + idS + ":") != std::string::npos);
            CHECK(yFull.find("provider: llmswitch-" + idS) != std::string::npos);
            CHECK(yFull.find("deepseek-official:") != std::string::npos);
        }
        // 二次切换：dsh 是增量多供应商——两条本应用条目并存，切换只改
        // agent-default-model 指向（不再把先前的条目替换掉）。
        models::Provider pd3{.name = "GLM 直连",
                             .baseUrl = "https://open.bigmodel.cn/api/paas/v4",
                             .apiKey = "sk-dsh-2",
                             .model = "glm-5.1"};  // apiFormat 默认
        s.addProvider("dsh", pd3);
        const std::string idS2 = s.group("dsh").providers.back().id;
        s.switchTo("dsh", idS2);
        {
            const std::string y = readTextFile(dshSettings);
            // 先前的条目原样在文件里，新增条目追加在后。
            CHECK(y.find("llmswitch-" + idS + ":") != std::string::npos);
            CHECK(y.find("llmswitch-" + idS2 + ":") != std::string::npos);
            CHECK(settingsEntryIndex(y, "llmswitch-" + idS) <
                  settingsEntryIndex(y, "llmswitch-" + idS2));
            CHECK(y.find("api: anthropic-messages") != std::string::npos);
            CHECK(y.find("api: openai-completions") != std::string::npos);
            CHECK(y.find("provider: llmswitch-" + idS2) != std::string::npos);
            // 档位声明跟着各自的条目走：pd2 有档位，pd3 没有——不能因为换了
            // 默认路由就把 pd2 的声明写丢。
            CHECK(y.find("reasoningEfforts") != std::string::npos);
            CHECK(y.find("            \"high\": high") != std::string::npos);
            CHECK(settingsEntryBlock(y, "llmswitch-" + idS2)
                      .find("reasoningEfforts") == std::string::npos);
            int admBlocks = 0;
            for (std::size_t pos = 0;
                 (pos = y.find("agent-default-model:", pos)) != std::string::npos;
                 pos += 1) {
                ++admBlocks;
            }
            CHECK(admBlocks == 1);
            CHECK(s.detectCurrent("dsh") == idS2);
            // live 实况列表：内置官方路由 + 两条本应用条目；默认指向 idS2。
            const auto entries = s.dshLiveProviders();
            CHECK(entries.size() == 3);
            int managed = 0;
            for (const auto& entry : entries) {
                if (!entry.providerId.empty()) ++managed;
                if (entry.providerId == idS) CHECK(!entry.isDefault);
                if (entry.providerId == idS2) CHECK(entry.isDefault);
                if (entry.key == "deepseek-official") {
                    CHECK(!entry.isDefault);
                    CHECK(entry.providerId.empty());
                }
            }
            CHECK(managed == 2);
        }
        // importLive 往返：从 live 文件收编（同端点+密钥命中 idS2 复用）。
        {
            const auto imported = s.importLive("dsh");
            CHECK(imported.id == idS2);
            CHECK(imported.apiKey == "sk-dsh-2");
            CHECK(imported.baseUrl == "https://open.bigmodel.cn/api/paas/v4");
            CHECK(imported.model == "glm-5.1");
            CHECK(s.detectCurrent("dsh") == idS2);
        }
        // restoreOfficial：块与 llmswitch-* 条目移除，内置路由与无关键保留。
        s.restoreOfficial("dsh");
        {
            const std::string y = readTextFile(dshSettings);
            CHECK(y.find("agent-default-model") == std::string::npos);
            CHECK(y.find("llmswitch-") == std::string::npos);
            CHECK(y.find("deepseek-official:") != std::string::npos);
            CHECK(y.find("theme: dark") != std::string::npos);
            CHECK(s.detectCurrent("dsh").empty());
        }
        // 8d2. settings.yaml 的 providers 写成 flow 风格（用户手写/其它工具）：
        // 收编要读得到，写入前要摊平成块风格——直接在 flow 块后插块条目会写出
        // 非法 YAML，原条目与既有键值必须原样保留。
        {
            writeFile(dshSettings,
                      "agent-default-model:\n"
                      "  provider: c\n"
                      "  model: \"deepseek/deepseek-v4.1-flash\"\n"
                      "llm-pi-ai:\n"
                      "  providers:\n"
                      "    {\n"
                      "      c:\n"
                      "        {\n"
                      "          apiKeyEnv: C_API_KEY,\n"
                      "          displayName: \"Command Code\",\n"
                      "          api: openai-responses,\n"
                      "          baseURL: https://api.commandcode.ai/provider/v1,\n"
                      "          models:\n"
                      "            [\n"
                      "              {\n"
                      "                  id: deepseek/deepseek-v4.1-flash,\n"
                      "                  contextWindow: 1000000,\n"
                      "                  maxTokens: 256000,\n"
                      "                  reasoningEfforts: { \"off\": null, low: low, high: high }\n"
                      "                }\n"
                      "            ]\n"
                      "        }\n"
                      "    }\n"
                      "agent-presets:\n"
                      "  default: cordis\n");
            writeFile(dshCredentials,
                      "version: 1\nrefs:\n  C_API_KEY: \"sk-flow-key\"\n");
            s.importLive("dsh");
            models::Provider imported;
            for (const auto& p : s.group("dsh").providers) {
                if (p.id == "c") imported = p;
            }
            CHECK(imported.id == "c");
            CHECK(imported.name == "Command Code");  // displayName 收编进来
            CHECK(imported.baseUrl == "https://api.commandcode.ai/provider/v1");
            CHECK(imported.apiFormat == "openai-responses");
            CHECK(imported.model == "deepseek/deepseek-v4.1-flash");
            CHECK(imported.apiKey == "sk-flow-key");
            CHECK(imported.reasoningEfforts ==
                  std::vector<std::string>({"off", "low", "high"}));

            s.switchTo("dsh", "c");
            const std::string y = readTextFile(dshSettings);
            // 摊平后不再有 flow 括号；收编过来的裸键被接管成 llmswitch-c
            // （内容不变、不残留重复条目），其余无关键原样保留。
            CHECK(y.find('{') == std::string::npos);
            CHECK(y.find('}') == std::string::npos);
            CHECK(y.find("\n    c:\n") == std::string::npos);
            // 单条增量：切换只写自己这一条（裸键 c 被接管改名成块风格条目），
            // 组内其它供应商不会被顺手推回 live——文件里本来没有它们。
            CHECK(settingsEntryBlock(y, "llmswitch-" + idS).empty());
            CHECK(settingsEntryBlock(y, "llmswitch-" + idS2).empty());
            CHECK(!settingsEntryBlock(y, "llmswitch-c").empty());
            CHECK(y.find("      apiKeyEnv: LLMSWITCH_C") != std::string::npos);
            CHECK(y.find("      displayName: \"Command Code\"") !=
                  std::string::npos);
            CHECK(y.find("      api: openai-responses") != std::string::npos);
            CHECK(y.find("      baseURL: \"https://api.commandcode.ai/provider/v1\"") !=
                  std::string::npos);
            CHECK(y.find("        - id: \"deepseek/deepseek-v4.1-flash\"") !=
                  std::string::npos);
            CHECK(y.find("          contextWindow: 1000000") !=
                  std::string::npos);
            CHECK(y.find("          maxTokens: 256000") != std::string::npos);
            // 嵌套 flow 值也摊平了：off 留空（原来的 null 不再出现），其余档位
            // 按本应用写侧形状加引号、保留同名拼写。
            CHECK(y.find("            \"off\":\n") != std::string::npos);
            CHECK(y.find("            \"low\": low") != std::string::npos);
            CHECK(y.find("            \"high\": high") != std::string::npos);
            CHECK(y.find("null") == std::string::npos);
            // 新条目按块风格追加，pointer 指向它，无关键保留。
            CHECK(y.find("    llmswitch-c:") != std::string::npos);
            CHECK(y.find("provider: llmswitch-c") != std::string::npos);
            CHECK(y.find("agent-presets:") != std::string::npos);
            CHECK(y.find("  default: cordis") != std::string::npos);
            // 档位声明跟着各自的条目走：单条「写入 / 更新」只重写这一条，
            // 收编来的 c 有档位、先写进来的 idS2 没有——各写各的，不互相串。
            CHECK(settingsEntryBlock(y, "llmswitch-c")
                      .find("reasoningEfforts") != std::string::npos);
            s.writeDshProvider(idS2);
            {
                const std::string y2 = readTextFile(dshSettings);
                CHECK(!settingsEntryBlock(y2, "llmswitch-" + idS2).empty());
                CHECK(settingsEntryBlock(y2, "llmswitch-" + idS2)
                          .find("reasoningEfforts") == std::string::npos);
                CHECK(settingsEntryBlock(y2, "llmswitch-c")
                          .find("reasoningEfforts") != std::string::npos);
            }
            // 增量语义：换默认路由不删别的本应用条目，只改 agent-default-model。
            s.switchTo("dsh", idS2);
            {
                const std::string yOther = readTextFile(dshSettings);
                CHECK(yOther.find("llmswitch-c:") != std::string::npos);
                CHECK(yOther.find("provider: llmswitch-" + idS2) !=
                      std::string::npos);
            }
            // 再切回来：条目已在 live 里就不再重写（切换只改指针），输出字节
            // 稳定——flow 摊平只发生一次。
            s.switchTo("dsh", "c");
            const std::string yStable = readTextFile(dshSettings);
            CHECK(yStable.find("provider: llmswitch-c") != std::string::npos);
            s.switchTo("dsh", idS2);
            s.switchTo("dsh", "c");
            CHECK(readTextFile(dshSettings) == yStable);
        }
        // 8d3. 多条手写路由各自声明档位：收编只认「每条路由的首个模型条目」，
        // 计数必须随路由重置——否则第二条及以后的路由会被上一条的计数影响，
        // 档位被静默丢掉（收编进组的 supplier 少档位 → 再切换就写丢了）。
        {
            writeFile(dshSettings,
                      "agent-default-model:\n"
                      "  provider: llmswitch-b\n"
                      "  model: \"model-b\"\n"
                      "llm-pi-ai:\n"
                      "  providers:\n"
                      "    llmswitch-a:\n"
                      "      api: openai-completions\n"
                      "      baseURL: https://dsh-a.example.com\n"
                      "      apiKeyEnv: A_KEY\n"
                      "      models:\n"
                      "        - id: model-a\n"
                      "          reasoningEfforts:\n"
                      "            \"off\":\n"
                      "            high: high\n"
                      "    llmswitch-b:\n"
                      "      api: anthropic-messages\n"
                      "      baseURL: https://dsh-b.example.com\n"
                      "      apiKeyEnv: B_KEY\n"
                      "      models:\n"
                      "        - id: model-b\n"
                      "          reasoningEfforts:\n"
                      "            low: low\n"
                      "        - id: model-b2\n"
                      "          reasoningEfforts:\n"
                      "            max: max\n");
            writeFile(dshCredentials,
                      "version: 1\nrefs:\n  A_KEY: \"sk-a\"\n  B_KEY: \"sk-b\"\n");
            s.importLive("dsh");
            models::Provider importedB;
            for (const auto& p : s.group("dsh").providers) {
                if (p.baseUrl == "https://dsh-b.example.com") importedB = p;
            }
            CHECK(importedB.id == "b");
            CHECK(importedB.model == "model-b");
            // 第二条路由自己的档位读到了；它的第二个模型条目的档位不参与。
            CHECK(importedB.reasoningEfforts ==
                  std::vector<std::string>({"low"}));
            // 切到它：条目已在 live 里且内容一致 → 只改 agent-default-model，
            // 内容一字不动（用户手写的 `low: low` 保持原拼写）；别人写的
            // llmswitch-a 也不会被清（它是孤儿，清理属于显式的「全部写入 dsh」）。
            s.switchTo("dsh", importedB.id);
            {
                const std::string yb = readTextFile(dshSettings);
                CHECK(yb.find("            low: low") != std::string::npos);
                CHECK(yb.find("    llmswitch-a:") != std::string::npos);
                CHECK(yb.find("    llmswitch-" + importedB.id + ":") !=
                      std::string::npos);
                CHECK(yb.find("https://dsh-b.example.com") != std::string::npos);
                CHECK(yb.find("provider: llmswitch-b") != std::string::npos);
            }
            // 档位不写丢：显式的单条「写入 / 更新」按本应用写侧形状重建这一条
            // （键加引号），档位与收编到的一致，第二个模型条目的档位不参与。
            s.writeDshProvider(importedB.id);
            {
                const std::string blockB = settingsEntryBlock(
                    readTextFile(dshSettings), "llmswitch-" + importedB.id);
                CHECK(blockB.find("            \"low\": low") !=
                      std::string::npos);
                CHECK(blockB.find("model-b2") == std::string::npos);
            }
            // 整体重建才清孤儿：a 没被收编，属于本应用历史条目。
            s.syncDshProviders({});
            CHECK(readTextFile(dshSettings).find("llmswitch-a:") ==
                  std::string::npos);
        }
        // 8d4. flow 值与 `providers:` 同行（`providers: { ... }`）也要摊平——
        // 这是 normalizer 的另一条分支（flow 从同一行的 `{` 起算）。
        {
            writeFile(dshSettings,
                      "llm-pi-ai:\n"
                      "  providers: {c: {apiKeyEnv: C2_KEY, api: openai-completions, baseURL: https://inline.example.com, models: [{id: inline-model, reasoningEfforts: {low: low}}]}}\n"
                      "agent-default-model:\n"
                      "  provider: c\n"
                      "  model: inline-model\n");
            writeFile(dshCredentials,
                      "version: 1\nrefs:\n  C2_KEY: \"sk-inline\"\n");
            s.importLive("dsh");
            models::Provider inlined;
            for (const auto& p : s.group("dsh").providers) {
                if (p.baseUrl == "https://inline.example.com") inlined = p;
            }
            CHECK(inlined.baseUrl == "https://inline.example.com");
            CHECK(inlined.model == "inline-model");
            CHECK(inlined.apiKey == "sk-inline");
            CHECK(inlined.reasoningEfforts == std::vector<std::string>({"low"}));
            s.switchTo("dsh", inlined.id);
            const std::string yi = readTextFile(dshSettings);
            CHECK(yi.find('{') == std::string::npos);
            CHECK(yi.find('}') == std::string::npos);
            CHECK(yi.find("      api: openai-completions") != std::string::npos);
            CHECK(yi.find("            \"low\": low") != std::string::npos);
        }
        // 8d5. 模型条目的官方能力字段（contextWindow / maxTokens / input）：
        // dsh 的 llm-pi-ai 把能力放在模型条目上，与 dsh 官方设置页编辑的是同一
        // 组字段（PiAiModelProfile）。写侧按官方顺序落盘、未声明就不写；读侧
        // 必须逐个读回来——否则收编后再切换就把用户的容量与视觉声明写丢了。
        {
            writeFile(dshSettings,
                      "llm-pi-ai:\n"
                      "  providers:\n"
                      "    deepseek-official:\n"
                      "      api: anthropic-messages\n"
                      "      baseURL: https://api.deepseek.com/anthropic\n");
            models::Provider vision{.name = "带视觉的中转",
                                    .baseUrl = "https://vision.example.com/v1",
                                    .apiKey = "sk-vision",
                                    .model = "vision-model",
                                    .contextWindow = 1000000,
                                    .maxTokens = 256000,
                                    .inputModalities = {"image", "text"}};
            s.addProvider("dsh", vision);
            const std::string idV = s.group("dsh").providers.back().id;
            s.switchTo("dsh", idV);
            const std::string y = readTextFile(dshSettings);
            // 官方字段紧接 id、按官方顺序（容量 → 模态 → 档位）。
            CHECK(y.find("        - id: \"vision-model\"\n"
                         "          contextWindow: 1000000\n"
                         "          maxTokens: 256000\n"
                         "          input: [text, image]\n") != std::string::npos);
            // 未声明的档位不出现（dsh 视为不支持思考）——只查这一条条目，
            // 组里其它供应商（先前测试留下的）各自带不带档位与此无关。
            CHECK(settingsEntryBlock(y, "llmswitch-" + idV)
                      .find("reasoningEfforts") == std::string::npos);
            // 第二个供应商一个都不声明：三个字段都不落盘。
            models::Provider plain{.name = "无声明",
                                   .baseUrl = "https://plain.example.com/v1",
                                   .apiKey = "sk-plain",
                                   .model = "plain-model"};
            s.addProvider("dsh", plain);
            const std::string idP = s.group("dsh").providers.back().id;
            s.switchTo("dsh", idP);
            const std::string yp = readTextFile(dshSettings);
            const std::string blockP = settingsEntryBlock(yp, "llmswitch-" + idP);
            CHECK(blockP.find("contextWindow:") == std::string::npos);
            CHECK(blockP.find("maxTokens:") == std::string::npos);
            CHECK(blockP.find("input:") == std::string::npos);
            // agent-default-model 的推理等级（官方键 reasoningEffort）不属于
            // 本应用的模型，但改写这块时必须原样带回：切换只是换 provider/model。
            writeFile(dshSettings,
                      "agent-default-model:\n"
                      "  provider: c\n"
                      "  model: \"old-model\"\n"
                      "  reasoningEffort: xhigh\n"
                      "llm-pi-ai:\n"
                      "  providers:\n"
                      "    c:\n"
                      "      api: openai-completions\n"
                      "      baseURL: https://keep-effort.example.com/v1\n");
            models::Provider effort{.name = "保留等级",
                                    .baseUrl = "https://keep-effort.example.com/v1",
                                    .apiKey = "sk-effort",
                                    .model = "effort-model"};
            s.addProvider("dsh", effort);
            const std::string idE = s.group("dsh").providers.back().id;
            s.switchTo("dsh", idE);
            const std::string ye = readTextFile(dshSettings);
            CHECK(ye.find("  provider: llmswitch-" + idE) != std::string::npos);
            CHECK(ye.find("  model: \"effort-model\"") != std::string::npos);
            CHECK(ye.find("  reasoningEffort: xhigh") != std::string::npos);
            CHECK(s.detectCurrent("dsh") == idE);

            // 读侧往返：手写一条块风格条目（本应用写出的就是这种形状），
            // 收编要把三个字段都读回来，再切换写回同样的文本。
            writeFile(dshSettings,
                      "agent-default-model:\n"
                      "  provider: roundtrip\n"
                      "llm-pi-ai:\n"
                      "  providers:\n"
                      "    roundtrip:\n"
                      "      api: openai-responses\n"
                      "      baseURL: https://roundtrip.example.com/v1\n"
                      "      apiKeyEnv: RT_KEY\n"
                      "      models:\n"
                      "        - id: roundtrip-model\n"
                      "          contextWindow: 1000000\n"
                      "          maxTokens: 256000\n"
                      "          input: [text, image]\n"
                      "          reasoningEfforts:\n"
                      "            \"off\":\n"
                      "            medium: medium\n");
            writeFile(dshCredentials,
                      "version: 1\nrefs:\n  RT_KEY: \"sk-rt\"\n");
            const auto rt = s.importLive("dsh");
            CHECK(rt.baseUrl == "https://roundtrip.example.com/v1");
            CHECK(rt.model == "roundtrip-model");
            CHECK(rt.contextWindow == 1000000);
            CHECK(rt.maxTokens == 256000);
            CHECK(rt.inputModalities ==
                  std::vector<std::string>({"text", "image"}));
            CHECK(rt.reasoningEfforts ==
                  std::vector<std::string>({"off", "medium"}));
            s.switchTo("dsh", rt.id);
            const std::string yr = readTextFile(dshSettings);
            CHECK(yr.find("        - id: \"roundtrip-model\"\n"
                          "          contextWindow: 1000000\n"
                          "          maxTokens: 256000\n"
                          "          input: [text, image]\n"
                          "          reasoningEfforts:\n"
                          "            \"off\":\n"
                          "            \"medium\": medium\n") != std::string::npos);
        }
        // 8d6. 手写路由的官方字段读回：块序列 input（flow 摊平后的形态）、
        // 同行 flow input、同行 flow reasoningEfforts、带 K 后缀的容量。
        {
            writeFile(dshSettings,
                      "agent-default-model:\n"
                      "  provider: block-seq\n"
                      "llm-pi-ai:\n"
                      "  providers:\n"
                      "    block-seq:\n"
                      "      api: openai-completions\n"
                      "      baseURL: https://seq.example.com/v1\n"
                      "      apiKeyEnv: SEQ_KEY\n"
                      "      models:\n"
                      "        - id: seq-model\n"
                      "          contextWindow: 128000\n"
                      "          input:\n"
                      "            - text\n"
                      "            - image\n");
            writeFile(dshCredentials,
                      "version: 1\nrefs:\n  SEQ_KEY: \"sk-seq\"\n");
            const auto seq = s.importLive("dsh");
            CHECK(seq.baseUrl == "https://seq.example.com/v1");
            CHECK(seq.model == "seq-model");
            CHECK(seq.contextWindow == 128000);
            CHECK(seq.maxTokens == 0);  // 没声明
            CHECK(seq.inputModalities ==
                  std::vector<std::string>({"text", "image"}));
            // 块序列读完后接着切换：写回的是同一组官方字段。
            s.switchTo("dsh", seq.id);
            const std::string ys = readTextFile(dshSettings);
            const std::string blockS =
                settingsEntryBlock(ys, "llmswitch-" + seq.id);
            CHECK(blockS.find("contextWindow: 128000") != std::string::npos);
            CHECK(blockS.find("input: [text, image]") != std::string::npos);
        }
        {
            writeFile(dshSettings,
                      "agent-default-model:\n"
                      "  provider: inline-flow\n"
                      "llm-pi-ai:\n"
                      "  providers:\n"
                      "    inline-flow:\n"
                      "      api: openai-completions\n"
                      "      baseURL: https://flow.example.com/v1\n"
                      "      apiKeyEnv: FLOW_KEY\n"
                      "      models:\n"
                      "        - id: flow-model\n"
                      "          maxTokens: 64K\n"
                      "          input: [text, image]\n"
                      "          reasoningEfforts: { \"off\": null, medium: medium }\n");
            writeFile(dshCredentials,
                      "version: 1\nrefs:\n  FLOW_KEY: \"sk-flow\"\n");
            const auto flow = s.importLive("dsh");
            CHECK(flow.baseUrl == "https://flow.example.com/v1");
            CHECK(flow.model == "flow-model");
            CHECK(flow.contextWindow == 0);  // 没声明
            CHECK(flow.maxTokens == 64000);  // K 后缀
            CHECK(flow.inputModalities ==
                  std::vector<std::string>({"text", "image"}));
            CHECK(flow.reasoningEfforts ==
                  std::vector<std::string>({"off", "medium"}));
            // 再切换写回：同行 flow 值按块风格落盘（摊平只改排版）。
            s.switchTo("dsh", flow.id);
            const std::string yf = readTextFile(dshSettings);
            const std::string blockF =
                settingsEntryBlock(yf, "llmswitch-" + flow.id);
            CHECK(blockF.find("maxTokens: 64000") != std::string::npos);
            CHECK(blockF.find("input: [text, image]") != std::string::npos);
            CHECK(blockF.find("\"medium\": medium") != std::string::npos);
        }
        // 8d7. 容量拼写（与 dsh 官方设置页同一套）：解析 / 回写往返 + 非法值。
        {
            CHECK(models::parseTokenCount("") == 0);  // 空 = 不声明
            CHECK(models::parseTokenCount("   ") == 0);
            CHECK(models::parseTokenCount("131072") == 131072);
            CHECK(models::parseTokenCount("256K") == 256000);
            CHECK(models::parseTokenCount("2m") == 2000000);
            CHECK(models::parseTokenCount("1.5K") == 1500);
            CHECK(models::parseTokenCount("0") == -1);    // 容量必须为正整数
            CHECK(models::parseTokenCount("-5") == -1);
            CHECK(models::parseTokenCount("abc") == -1);
            CHECK(models::parseTokenCount("1.5") == -1);  // 非整数
            CHECK(models::parseTokenCount("K") == -1);
            CHECK(models::formatTokenCount(1000000) == "1M");
            CHECK(models::formatTokenCount(256000) == "256K");
            CHECK(models::formatTokenCount(131072) == "131072");
            CHECK(models::formatTokenCount(0).empty());
            // 模态：text 是底座（非空清单必然带 text），空清单仍是「不声明」。
            CHECK(models::normalizeInputModalities({"image"}) ==
                  std::vector<std::string>({"text", "image"}));
            CHECK(models::normalizeInputModalities({}).empty());
            CHECK(models::inputModalitiesFromMask(
                      models::inputModalityMask({"image"})) ==
                  std::vector<std::string>({"text", "image"}));
            CHECK(models::inputModalityMask({}) == 0);
            CHECK(models::inputModalitiesFromMask(0).empty());
        }
        // 8d8. 增量同步的不变式与四个显式入口：
        //   - 组内每条供应商写成 llmswitch-<id>；内置路由与别家手写条目一字不动；
        //   - 不在组内的 llmswitch-* 视为孤儿，同步时清掉；
        //   - add/update/remove 即时同步 live（不等切换）；
        //   - adoptDshProvider 收编手写裸键（改名接管 + 默认路由跟着重指向）；
        //   - removeDshProvider 只删 live 那一条（含未纳管的 dsh 条目）。
        {
            // 先把前面测试攒下的 dsh 供应商全部删掉，让本块从空组开始。
            while (!s.group("dsh").providers.empty()) {
                s.removeProvider("dsh", s.group("dsh").providers.back().id);
            }
            const auto keyEnvOf = [](std::string_view id) {
                std::string out = "LLMSWITCH_";
                for (const unsigned char c : id) {
                    out += std::isalnum(c) ? static_cast<char>(std::toupper(c))
                                           : '_';
                }
                return out;
            };
            writeFile(dshSettings,
                      "# dsh 设置\ntheme: dark\n"
                      "llm-pi-ai:\n"
                      "  providers:\n"
                      "    deepseek-official:\n"
                      "      api: anthropic-messages\n"
                      "      baseURL: https://api.deepseek.com/anthropic\n"
                      "    hand-written:\n"
                      "      api: openai-completions\n"
                      "      baseURL: https://hand.example.com/v1\n"
                      "      apiKeyEnv: HAND_KEY\n"
                      "      models:\n"
                      "        - id: hand-model\n"
                      "    llmswitch-orphan:\n"
                      "      api: openai-completions\n"
                      "      baseURL: https://orphan.example.com\n"
                      "agent-presets:\n"
                      "  default: cordis\n");
            writeFile(dshCredentials,
                      "version: 1\nrefs:\n  HAND_KEY: \"sk-hand\"\n  ORPHAN_KEY: \"keep\"\n");
            // 空组同步：孤儿条目清掉，内置路由 / 别家条目 / 无关键 / 注释都留着，
            // 没有被引用的旧密钥也不代清（只增改，不删别人的）。
            s.syncDshProviders({});
            const std::string y0 = readTextFile(dshSettings);
            CHECK(y0.find("llmswitch-orphan") == std::string::npos);
            CHECK(y0.find("orphan.example.com") == std::string::npos);
            CHECK(y0.find("# dsh 设置") != std::string::npos);
            CHECK(y0.find("deepseek-official:") != std::string::npos);
            CHECK(y0.find("    hand-written:") != std::string::npos);
            CHECK(y0.find("https://hand.example.com/v1") != std::string::npos);
            CHECK(y0.find("agent-presets:") != std::string::npos);
            CHECK(y0.find("agent-default-model") == std::string::npos);
            CHECK(readTextFile(dshCredentials).find("ORPHAN_KEY") !=
                  std::string::npos);
            // dsh 实况列表：两条条目都在；hand-written 是未纳管（无 providerId）。
            {
                const auto live = s.dshLiveProviders();
                CHECK(live.size() == 2);
                for (const auto& entry : live) {
                    if (entry.key == "deepseek-official") {
                        // 没有 agent-default-model 块 → 实际默认就是内置官方
                        // 路由，所以这一行是「使用中」。
                        CHECK(entry.isDefault);
                        CHECK(entry.providerId.empty());
                        continue;
                    }
                    CHECK(!entry.isDefault);
                    if (entry.key == "hand-written") {
                        CHECK(entry.providerId.empty());
                        CHECK(entry.apiKey == "sk-hand");
                        CHECK(entry.model == "hand-model");
                    }
                }
            }
            // 手写条目正是 dsh 默认路由时收编：本地建一份，live 侧改名接管，
            // agent-default-model 同一次写入改指新键——行为不变。
            writeFile(dshSettings,
                      "agent-default-model:\n"
                      "  provider: hand-written\n"
                      "  model: \"hand-model\"\n" + readTextFile(dshSettings));
            const auto adopted = s.adoptDshProvider("hand-written");
            CHECK(adopted.id == "hand-written");
            CHECK(adopted.baseUrl == "https://hand.example.com/v1");
            CHECK(adopted.apiKey == "sk-hand");
            CHECK(adopted.model == "hand-model");
            CHECK(s.group("dsh").current == "hand-written");
            CHECK(s.detectCurrent("dsh") == "hand-written");
            const std::string yAd = readTextFile(dshSettings);
            CHECK(yAd.find("\n    hand-written:") == std::string::npos);
            CHECK(yAd.find("    llmswitch-hand-written:") != std::string::npos);
            CHECK(yAd.find("  provider: llmswitch-hand-written") !=
                  std::string::npos);
            CHECK(yAd.find("deepseek-official:") != std::string::npos);
            CHECK(readTextFile(dshCredentials).find("LLMSWITCH_HAND_WRITTEN") !=
                  std::string::npos);
            // 新增供应商即时落 live（不必等切换）：默认指向不变、两条并存。
            models::Provider pA{.name = "A",
                                .baseUrl = "https://a.example.com/v1",
                                .apiKey = "sk-a",
                                .model = "model-a"};
            const std::string idA = s.addProvider("dsh", pA);
            const std::string yA = readTextFile(dshSettings);
            CHECK(yA.find("    llmswitch-" + idA + ":") != std::string::npos);
            CHECK(yA.find("apiKeyEnv: " + keyEnvOf(idA)) != std::string::npos);
            CHECK(readTextFile(dshCredentials)
                      .find(keyEnvOf(idA) + ": \"sk-a\"") != std::string::npos);
            CHECK(yA.find("  provider: llmswitch-hand-written") !=
                  std::string::npos);
            CHECK(settingsEntryIndex(yA, "llmswitch-" + idA) >
                  settingsEntryIndex(yA, "llmswitch-hand-written"));
            CHECK(settingsEntryIndex(yA, "deepseek-official") <
                  settingsEntryIndex(yA, "llmswitch-hand-written"));
            // update 也即时同步（改 baseUrl 立刻反映到 live 条目）。
            models::Provider pA2 = pA;
            pA2.id = idA;
            pA2.baseUrl = "https://a2.example.com/v1";
            s.updateProvider("dsh", pA2);
            const std::string yA2 = readTextFile(dshSettings);
            CHECK(yA2.find("https://a2.example.com/v1") != std::string::npos);
            CHECK(yA2.find("a.example.com") == std::string::npos);
            // 删除默认供应商：条目消失、悬空的 agent-default-model 清回内置路由，
            // 其余条目（含未纳管的手写条目）不动。
            s.removeProvider("dsh", "hand-written");
            const std::string yRm = readTextFile(dshSettings);
            CHECK(yRm.find("llmswitch-hand-written") == std::string::npos);
            CHECK(yRm.find("agent-default-model") == std::string::npos);
            CHECK(yRm.find("    llmswitch-" + idA + ":") != std::string::npos);
            CHECK(yRm.find("deepseek-official:") != std::string::npos);
            // removeDshProvider：删的是一条 live 条目（这里是一条未纳管条目），
            // 组内其它条目照旧重建，默认指向它时同步清掉。
            writeFile(dshSettings,
                      "agent-default-model:\n"
                      "  provider: llmswitch-ext\n"
                      "  model: \"ext-model\"\n"
                      "llm-pi-ai:\n"
                      "  providers:\n"
                      "    deepseek-official:\n"
                      "      api: anthropic-messages\n"
                      "      baseURL: https://api.deepseek.com/anthropic\n"
                      "    llmswitch-ext:\n"
                      "      api: openai-completions\n"
                      "      baseURL: https://ext.example.com/v1\n"
                      "      apiKeyEnv: EXT_KEY\n"
                      "      models:\n"
                      "        - id: ext-model\n");
            writeFile(dshCredentials,
                      "version: 1\nrefs:\n  EXT_KEY: \"sk-ext\"\n");
            s.removeDshProvider("llmswitch-ext");
            const std::string yExt = readTextFile(dshSettings);
            CHECK(yExt.find("llmswitch-ext") == std::string::npos);
            CHECK(yExt.find("agent-default-model") == std::string::npos);
            // 增量删除只摘掉这一个键：组内那些还没写进 live 的供应商（这里的
            // idA 在重写文件时已经不在 live 里了）绝不能被顺手补写回来。
            CHECK(yExt.find("    llmswitch-" + idA + ":") == std::string::npos);
            CHECK(yExt.find("deepseek-official:") != std::string::npos);
            bool threw = false;
            try {
                s.removeDshProvider("nope");
            } catch (const std::exception&) {
                threw = true;
            }
            CHECK(threw);
        }
        // 8d9. 左列的内置官方行与「单条增量」的两条硬性质：
        //   - settings.yaml 里没有 deepseek-official（它是适配器注册的内置路由，
        //     通常不落文件）时，实况列表要补一条 builtin 合成行；没有
        //     agent-default-model 时它是「使用中」，默认指向别家裸键时则是
        //     那条裸键「使用中」；
        //   - 删除一个 live 里根本没有条目的供应商时，live 文件必须一字不动；
        //   - 删除一条 live 条目时，别的条目（含未纳管的）不受影响。
        {
            // 先把 dsh 组清空，让下面的增删只涉及本块自己造的供应商。
            while (!s.group("dsh").providers.empty()) {
                s.removeProvider("dsh", s.group("dsh").providers.back().id);
            }
            writeFile(dshSettings,
                      "llm-pi-ai:\n"
                      "  providers:\n"
                      "    hand-written:\n"
                      "      api: openai-completions\n"
                      "      baseURL: https://hand.example.com/v1\n");
            writeFile(dshCredentials, "version: 1\n");
            {
                const auto live = s.dshLiveProviders();
                CHECK(live.size() == 2);
                CHECK(live.front().key == "deepseek-official");
                CHECK(live.front().builtin);
                CHECK(live.front().providerId.empty());
                CHECK(live.front().isDefault);  // 没有 agent-default-model
                CHECK(live.back().key == "hand-written");
                CHECK(!live.back().builtin);
                CHECK(!live.back().isDefault);
            }
            // 实际用户文件的形状：默认指向一条裸键（未纳管的 c），文件里没有
            // deepseek-official → 官方行仍是 builtin 合成行，但「使用中」落在
            // c 上（不是官方行）。
            writeFile(dshSettings,
                      "agent-default-model:\n"
                      "  provider: c\n"
                      "  model: \"deepseek/deepseek-v4.1-flash\"\n"
                      "llm-pi-ai:\n"
                      "  providers:\n"
                      "    c:\n"
                      "      apiKeyEnv: C_API_KEY\n"
                      "      api: openai-responses\n"
                      "      baseURL: https://api.commandcode.ai/provider/v1\n");
            {
                const auto live = s.dshLiveProviders();
                CHECK(live.size() == 2);
                CHECK(live.front().key == "deepseek-official");
                CHECK(live.front().builtin);
                CHECK(!live.front().isDefault);
                CHECK(live.back().key == "c");
                CHECK(!live.back().builtin);
                CHECK(live.back().isDefault);
                CHECK(live.back().providerId.empty());
            }
            // 文件里真写了 deepseek-official 时不再合成（它是一条可收编/可删的
            // 手写条目，builtin 保持 false）。
            writeFile(dshSettings,
                      "llm-pi-ai:\n"
                      "  providers:\n"
                      "    deepseek-official:\n"
                      "      api: anthropic-messages\n"
                      "      baseURL: https://api.deepseek.com/anthropic\n");
            {
                const auto live = s.dshLiveProviders();
                CHECK(live.size() == 1);
                CHECK(live.front().key == "deepseek-official");
                CHECK(!live.front().builtin);
                CHECK(live.front().isDefault);
            }
            // 删一个 live 里没有的条目：文件一字不动（增量删除不写别的条目）。
            models::Provider pIdle{.name = "Idle",
                                   .baseUrl = "https://idle.example.com/v1",
                                   .apiKey = "sk-idle",
                                   .model = "idle-model"};
            const std::string idIdle = s.addProvider("dsh", pIdle);
            writeFile(dshSettings,
                      "llm-pi-ai:\n"
                      "  providers:\n"
                      "    deepseek-official:\n"
                      "      api: anthropic-messages\n"
                      "      baseURL: https://api.deepseek.com/anthropic\n");
            const std::string before = readTextFile(dshSettings);
            s.removeProvider("dsh", idIdle);
            CHECK(readTextFile(dshSettings) == before);
            // 删一条确实在 live 里的条目：只少这一条，未纳管的手写条目与内置
            // 路由原样保留。
            writeFile(dshSettings,
                      "llm-pi-ai:\n"
                      "  providers:\n"
                      "    deepseek-official:\n"
                      "      api: anthropic-messages\n"
                      "      baseURL: https://api.deepseek.com/anthropic\n"
                      "    hand-written:\n"
                      "      api: openai-completions\n"
                      "      baseURL: https://hand.example.com/v1\n");
            const std::string idLive = s.addProvider("dsh", pIdle);
            {
                const std::string yAdd = readTextFile(dshSettings);
                CHECK(yAdd.find("    llmswitch-" + idLive + ":") !=
                      std::string::npos);
                CHECK(yAdd.find("    hand-written:") != std::string::npos);
            }
            s.removeProvider("dsh", idLive);
            const std::string yOne = readTextFile(dshSettings);
            CHECK(yOne.find("llmswitch-" + idLive) == std::string::npos);
            CHECK(yOne.find("    hand-written:") != std::string::npos);
            CHECK(yOne.find("deepseek-official:") != std::string::npos);
            // eraseLive=false（右列删除确认的「只删本应用」）：只收回本地留存，
            // settings.yaml 一字不动，那条随即变成左列的未纳管手写路由；再收编
            // 回来仍能接管同一条，然后才轮到「连同 dsh 一起删」摘掉它。
            writeFile(dshSettings,
                      "llm-pi-ai:\n"
                      "  providers:\n"
                      "    deepseek-official:\n"
                      "      api: anthropic-messages\n"
                      "      baseURL: https://api.deepseek.com/anthropic\n");
            const std::string idKeep = s.addProvider("dsh", pIdle);
            const std::string beforeKeep = readTextFile(dshSettings);
            s.removeProvider("dsh", idKeep, /*eraseLive=*/false);
            CHECK(readTextFile(dshSettings) == beforeKeep);
            CHECK(s.group("dsh").providers.empty());
            {
                const auto live = s.dshLiveProviders();
                CHECK(live.size() == 2);
                CHECK(live.back().key == "llmswitch-" + idKeep);
                CHECK(live.back().providerId.empty());  // 已无本地对应条目
            }
            const auto readopted = s.adoptDshProvider("llmswitch-" + idKeep);
            CHECK(readopted.id == idKeep);
            s.removeProvider("dsh", idKeep, /*eraseLive=*/true);
            CHECK(readTextFile(dshSettings).find("llmswitch-" + idKeep) ==
                  std::string::npos);
        }

        // 8d10. .credentials.yaml 的文档布局。dsh 的凭据文档是 version 1 布局：
        // 顶层只允许 version / refs / records，引用名在 `refs:` 块内（缩进 2）。
        // 引用名写到版本 1 文档的顶层会让 dsh 拒绝**整份文档**
        // （unknown top-level key "…"），于是所有密钥一起失效、路由完全不可用
        // ——这里逐条钉住写/读的位置，以及旧残留的迁移。
        {
            while (!s.group("dsh").providers.empty()) {
                s.removeProvider("dsh", s.group("dsh").providers.back().id);
            }
            writeFile(dshSettings, "llm-pi-ai:\n  providers: {}\n");
            const auto envOf = [](std::string_view id) {
                std::string out = "LLMSWITCH_";
                for (const unsigned char c : id) {
                    out += std::isalnum(c) ? static_cast<char>(std::toupper(c))
                                           : '_';
                }
                return out;
            };
            const auto topKeys = [](const std::string& text) {
                std::vector<std::string> keys;
                std::size_t pos = 0;
                while (pos < text.size()) {
                    const std::size_t end = text.find('\n', pos);
                    const std::string line = text.substr(
                        pos, end == std::string::npos ? std::string::npos
                                                      : end - pos);
                    pos = end == std::string::npos ? text.size() : end + 1;
                    if (line.empty() || line.front() == ' ' ||
                        line.front() == '#') {
                        continue;
                    }
                    const auto colon = line.find(':');
                    if (colon == std::string::npos) continue;
                    keys.push_back(line.substr(0, colon));
                }
                return keys;
            };
            const auto addLayoutProvider = [&](const std::string& apiKey) {
                models::Provider p{.name = "布局",
                                   .baseUrl = "https://layout.example.com/v1",
                                   .apiKey = apiKey,
                                   .model = "layout-model"};
                return s.addProvider("dsh", p);
            };
            // (a) 版本 1 + refs 块：新引用名落在块内，顶层键与 records 原样。
            writeFile(dshCredentials,
                      "version: 1\nrefs:\n  KEEP_KEY: \"keep\"\n"
                      "records:\n  browser-session:\n    kind: grant\n");
            const std::string idA = addLayoutProvider("sk-a");
            {
                const std::string cred = readTextFile(dshCredentials);
                CHECK(cred.find("  " + envOf(idA) + ": \"sk-a\"") !=
                      std::string::npos);
                CHECK(cred.find("\n" + envOf(idA) + ":") == std::string::npos);
                CHECK(topKeys(cred) ==
                      std::vector<std::string>({"version", "refs", "records"}));
                CHECK(cred.find("  KEEP_KEY: \"keep\"") != std::string::npos);
                CHECK(cred.find("kind: grant") != std::string::npos);
            }
            // (b) 旧版本写错的顶层 LLMSWITCH_* 残留：迁回 refs 块（值保留），
            // refs 里已有的同名引用以 refs 为准。
            writeFile(dshCredentials,
                      "version: 1\nrefs:\n  LLMSWITCH_DUP: \"sk-refs\"\n"
                      "LLMSWITCH_OLD: \"sk-old\"\nLLMSWITCH_DUP: \"sk-stray\"\n");
            const std::string idB = addLayoutProvider("sk-b");
            {
                const std::string cred = readTextFile(dshCredentials);
                CHECK(topKeys(cred) ==
                      std::vector<std::string>({"version", "refs"}));
                CHECK(cred.find("  LLMSWITCH_OLD: \"sk-old\"") !=
                      std::string::npos);
                CHECK(cred.find("  LLMSWITCH_DUP: \"sk-refs\"") !=
                      std::string::npos);
                CHECK(cred.find("sk-stray") == std::string::npos);
                CHECK(cred.find("  " + envOf(idB) + ": \"sk-b\"") !=
                      std::string::npos);
            }
            // (c) 空 flow 块 refs: {} → 摊平成块风格再写。
            writeFile(dshCredentials, "version: 1\nrefs: {}\n");
            const std::string idC = addLayoutProvider("sk-c");
            {
                const std::string cred = readTextFile(dshCredentials);
                CHECK(cred.find("refs: {}") == std::string::npos);
                CHECK(cred.find("refs:\n  " + envOf(idC) + ": \"sk-c\"") !=
                      std::string::npos);
            }
            // (d) 预发布 flat 布局（没有 version）：仍写顶层，dsh 自己迁移。
            writeFile(dshCredentials, "FLAT_KEY: \"sk-flat\"\n");
            const std::string idD = addLayoutProvider("sk-d");
            {
                const std::string cred = readTextFile(dshCredentials);
                CHECK(cred.find("FLAT_KEY: \"sk-flat\"") != std::string::npos);
                CHECK(cred.find("\n" + envOf(idD) + ": \"sk-d\"") !=
                      std::string::npos);
                // flat 布局的引用名在顶层，读取也走顶层。
                const auto adopted = s.adoptDshProvider("llmswitch-" + idD);
                CHECK(adopted.apiKey == "sk-d");
            }
            // (e) 空文件：直接建版本 1 骨架（注释保留）。
            writeFile(dshCredentials, "# 我的密钥\n");
            const std::string idE = addLayoutProvider("sk-e");
            {
                const std::string cred = readTextFile(dshCredentials);
                CHECK(cred.find("# 我的密钥") != std::string::npos);
                CHECK(cred.find("version: 1\nrefs:\n  " + envOf(idE) +
                                ": \"sk-e\"") != std::string::npos);
                CHECK(topKeys(cred) ==
                      std::vector<std::string>({"version", "refs"}));
            }
        }
    }

    // 8e. hermes：config.yaml 的 custom_providers 列表 upsert（删旧
    // llmswitch-* 条目）+ 顶层 model 节指向，无关节/注释保留；detect/
    // import 往返；api_mode 三档映射；无官方态 restore 抛错。
    {
        writeFile(hermesConfig,
                  "# hermes 配置\n"
                  "model:\n"
                  "  default: \"old-model\"\n"
                  "  provider: some-native\n"
                  "  base_url: https://native.example.com\n"
                  "agent:\n"
                  "  max_turns: 50\n"
                  "custom_providers:\n"
                  "  - name: native-a\n"
                  "    base_url: https://a.example.com/v1\n"
                  "    api_key: sk-a\n"
                  "    api_mode: chat_completions\n"
                  "  - name: llmswitch-stale\n"
                  "    base_url: https://stale.example.com\n"
                  "    api_key: sk-stale\n");
        models::Provider ph{.name = "Hermes 中转",
                            .baseUrl = "https://relay.example.com/v1",
                            .apiKey = "sk-hermes",
                            .model = "test-model-1",
                            .apiFormat = "openai-responses"};
        s.addProvider("hermes", ph);
        const std::string idH = s.group("hermes").providers.back().id;
        s.switchTo("hermes", idH);
        {
            const std::string y = readTextFile(hermesConfig);
            CHECK(y.find("# hermes 配置") != std::string::npos);  // 注释保留
            CHECK(y.find("agent:\n  max_turns: 50") != std::string::npos);  // 无关节保留
            CHECK(y.find("- name: native-a") != std::string::npos);  // 原生条目保留
            CHECK(y.find("llmswitch-stale") == std::string::npos);   // 旧条目删除
            CHECK(y.find("stale.example.com") == std::string::npos);
            CHECK(y.find("- name: llmswitch-" + idH) != std::string::npos);
            CHECK(y.find("base_url: \"https://relay.example.com/v1\"") !=
                  std::string::npos);
            CHECK(y.find("api_key: \"sk-hermes\"") != std::string::npos);
            CHECK(y.find("api_mode: codex_responses") != std::string::npos);
            CHECK(y.find("\"test-model-1\": {}") != std::string::npos);
            CHECK(y.find("provider: llmswitch-" + idH) != std::string::npos);
            CHECK(y.find("default: \"test-model-1\"") != std::string::npos);
            // model 节其余键保留
            CHECK(y.find("base_url: https://native.example.com") !=
                  std::string::npos);
            CHECK(y.find("provider: some-native") == std::string::npos);
            CHECK(s.detectCurrent("hermes") == idH);
        }
        // 二次切换：条目替换而非堆积，model 节跟着改，原生条目不受影响。
        models::Provider ph2{.name = "GLM 直连",
                             .baseUrl = "https://open.bigmodel.cn/api/paas/v4",
                             .apiKey = "sk-hermes-2",
                             .model = "glm-5.1",
                             .apiFormat = "anthropic"};
        s.addProvider("hermes", ph2);
        const std::string idH2 = s.group("hermes").providers.back().id;
        s.switchTo("hermes", idH2);
        {
            const std::string y = readTextFile(hermesConfig);
            CHECK(y.find("- name: llmswitch-" + idH + "\n") == std::string::npos);
            CHECK(y.find("- name: llmswitch-" + idH2) != std::string::npos);
            CHECK(y.find("api_mode: anthropic_messages") != std::string::npos);
            CHECK(y.find("- name: native-a") != std::string::npos);
            CHECK(y.find("provider: llmswitch-" + idH2) != std::string::npos);
            CHECK(s.detectCurrent("hermes") == idH2);
        }
        // importLive 往返：同端点+密钥命中 idH2 复用。
        {
            const auto imported = s.importLive("hermes");
            CHECK(imported.id == idH2);
            CHECK(imported.apiKey == "sk-hermes-2");
            CHECK(imported.apiFormat == "anthropic");
            CHECK(imported.model == "glm-5.1");
            CHECK(s.detectCurrent("hermes") == idH2);
        }
        // 无官方默认状态：restoreOfficial 抛错。
        {
            bool threw = false;
            try {
                s.restoreOfficial("hermes");
            } catch (const std::exception&) {
                threw = true;
            }
            CHECK(threw);
        }
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
        // dsh 推理档位：规范全集、归一化（丢未知、去重、按规范升序）与位掩码往返。
        CHECK(models::reasoningLevels() ==
              std::vector<std::string>({"off", "minimal", "low", "medium",
                                        "high", "xhigh", "max"}));
        CHECK(models::normalizeReasoningEfforts(
                  {"high", "bogus", "off", "high", "low", ""}) ==
              std::vector<std::string>({"off", "low", "high"}));
        CHECK(models::normalizeReasoningEfforts({}).empty());
        CHECK(models::reasoningEffortMask({"high", "off"}) != 0);
        CHECK(models::reasoningEffortsFromMask(
                  models::reasoningEffortMask({"high", "off"})) ==
              std::vector<std::string>({"off", "high"}));
        CHECK(models::reasoningEffortsFromMask(0).empty());
        CHECK(models::reasoningEffortsFromMask(-1) == models::reasoningLevels());
        CHECK(models::reasoningLevelLabel("off") == "关闭思考 off");
        CHECK(models::reasoningLevelLabel("max") == "最高 max");
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
    }
    {
        // 官方默认用量模板表随资源包发布（resources/raw/usage_templates.json）：
        // 仓库内数据文件必须始终可解析，且四家已核实厂商可按 baseUrl 匹配。
        std::ifstream bundled(
            std::filesystem::path(LLMSWITCH_SOURCE_DIR) / "resources" / "raw" /
            "usage_templates.json");
        CHECK(bundled.is_open());
        const std::string text((std::istreambuf_iterator<char>(bundled)),
                               std::istreambuf_iterator<char>());
        const auto table = models::parseUsageTemplates(text);
        CHECK(table.size() == 8);
        const auto sug =
            models::suggestUsageQuery("https://api.deepseek.com/v1", table);
        CHECK(sug.has_value());
        CHECK(sug->url == "https://api.deepseek.com/user/balance");
        CHECK(sug->path == "balance_infos.0.total_balance");
        CHECK(sug->label == "CNY");
        const auto kimiCn =
            models::suggestUsageQuery("https://api.moonshot.cn", table);
        CHECK(kimiCn.has_value());
        CHECK(kimiCn->url == "https://api.moonshot.cn/v1/users/me/balance");
        CHECK(kimiCn->path == "data.available_balance");
        CHECK(kimiCn->label == "CNY");
        const auto kimiIntl =
            models::suggestUsageQuery("https://api.moonshot.ai/v1", table);
        CHECK(kimiIntl.has_value());
        CHECK(kimiIntl->url == "https://api.moonshot.ai/v1/users/me/balance");
        CHECK(kimiIntl->label == "USD");
        const auto openrouter =
            models::suggestUsageQuery("https://openrouter.ai/api/v1", table);
        CHECK(openrouter.has_value());
        CHECK(openrouter->url == "https://openrouter.ai/api/v1/key");
        CHECK(openrouter->path == "data.usage");
        // cc-switch 原生计费接口迁移：StepFun / SiliconFlow（国内与国际站
        // 按 host 区分，label 单位不同）。
        const auto stepfunCn =
            models::suggestUsageQuery("https://api.stepfun.com/step_plan", table);
        CHECK(stepfunCn.has_value());
        CHECK(stepfunCn->url == "https://api.stepfun.com/v1/accounts");
        CHECK(stepfunCn->path == "balance");
        CHECK(stepfunCn->label == "CNY");
        const auto stepfunIntl =
            models::suggestUsageQuery("https://api.stepfun.ai/step_plan", table);
        CHECK(stepfunIntl.has_value() && stepfunIntl->label == "USD");
        CHECK(stepfunIntl->url == "https://api.stepfun.ai/v1/accounts");
        const auto sfCn =
            models::suggestUsageQuery("https://api.siliconflow.cn", table);
        CHECK(sfCn.has_value());
        CHECK(sfCn->url == "https://api.siliconflow.cn/v1/user/info");
        CHECK(sfCn->path == "data.totalBalance");
        CHECK(sfCn->label == "CNY");
        const auto sfIntl =
            models::suggestUsageQuery("https://api.siliconflow.com/v1", table);
        CHECK(sfIntl.has_value() && sfIntl->label == "USD");
        // 不认识的 baseUrl 不建议（api.kimi.com 是订阅端点，无已核实模板）。
        CHECK(!models::suggestUsageQuery("https://x.example.com", table)
                    .has_value());
        CHECK(!models::suggestUsageQuery("https://api.kimi.com/coding/", table)
                    .has_value());
    }
    {
        // 解析容错：缺必填字段的条目跳过、顶层裸数组接受、坏 JSON 与缺
        // templates 数组抛错；便捷重载从原文直接解析匹配。
        const auto parsed = models::parseUsageTemplates(R"json(
            {"templates": [
                {"match": "api.example.com", "url": "https://api.example.com/u",
                 "path": "data.balance", "label": "CNY", "note": "未知字段忽略"},
                {"match": "broken.example.com"},
                {"url": "https://no.match.example"},
                "不是对象"
            ]}
        )json");
        CHECK(parsed.size() == 1);
        CHECK(parsed.front().match == "api.example.com");
        CHECK(parsed.front().label == "CNY");
        const auto bare = models::parseUsageTemplates(
            R"([{"match": "m", "url": "u", "path": "p"}])");
        CHECK(bare.size() == 1);
        const auto hit = models::suggestUsageQuery(
            "https://api.example.com/v1",
            R"json({"templates": [{"match": "api.example.com", "url": "u1", "path": "p1"}]})json");
        CHECK(hit.has_value() && hit->url == "u1");
        bool threw = false;
        try {
            static_cast<void>(models::parseUsageTemplates("{ 不是 JSON"));
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
        threw = false;
        try {
            static_cast<void>(models::parseUsageTemplates(R"({"a": 1})"));
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
    }
    {
        // 用户覆盖表：dataDir()/usage_templates.json 不存在返回空串；存在时
        // 原样读出（存在即整体替换内置表，UI 层负责选源）。
        CHECK(store::loadUsageTemplatesOverride().empty());
        const auto overrideFile = cfg::usageTemplatesFile();
        std::ofstream out(overrideFile, std::ios::binary);
        out << R"({"templates": [{"match": "relay.example.com", "url": "u",
                                   "path": "p", "label": "CNY"}]})";
        out.close();
        CHECK(!store::loadUsageTemplatesOverride().empty());
        const auto relay = models::suggestUsageQuery(
            "https://relay.example.com/v1",
            store::loadUsageTemplatesOverride());
        CHECK(relay.has_value() && relay->label == "CNY");
        std::error_code ec;
        std::filesystem::remove(overrideFile, ec);
        CHECK(store::loadUsageTemplatesOverride().empty());
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
        CHECK(models::findTool("codex") != nullptr &&
              models::findTool("codex")->displayName == "OpenAI");
        CHECK(models::officialVendorName("zcode") == "ZCode 官方");
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
        // cc-switch 预设迁移：claude-code 订阅组收录官方套餐计划；按量组
        // 收录 cn_official 与知名中转，有可表达计费接口的带好用量配置。
        const auto volc = std::ranges::find_if(cc.subscription,
            [](const models::Provider& p) { return p.name == "火山引擎 Coding Plan"; });
        CHECK(volc != cc.subscription.end() &&
              volc->baseUrl == "https://ark.cn-beijing.volces.com/api/coding" &&
              volc->model == "ark-code-latest" && volc->fullUrl);
        const auto ccOpenrouter = std::ranges::find_if(cc.metered,
            [](const models::Provider& p) { return p.name == "OpenRouter"; });
        CHECK(ccOpenrouter != cc.metered.end() && ccOpenrouter->usageEnabled &&
              ccOpenrouter->usageUrl == "https://openrouter.ai/api/v1/key" &&
              ccOpenrouter->usagePath == "data.usage");
        const auto ccSf = std::ranges::find_if(cc.metered,
            [](const models::Provider& p) { return p.name == "SiliconFlow"; });
        CHECK(ccSf != cc.metered.end() &&
              ccSf->usageUrl == "https://api.siliconflow.cn/v1/user/info" &&
              ccSf->usageLabel == "CNY");
        // GLM 计费接口需非 Bearer 鉴权，引擎无法表达 → 不带用量配置。
        const auto ccGlm = std::ranges::find_if(cc.metered,
            [](const models::Provider& p) { return p.name == "GLM（智谱）"; });
        CHECK(ccGlm != cc.metered.end() && !ccGlm->usageEnabled);
        // codex：新增条目带 TOML 模板，wire_api 与上游协议标注一致。
        const auto cxKfc = std::ranges::find_if(cx.subscription,
            [](const models::Provider& p) { return p.name == "Kimi For Coding"; });
        CHECK(cxKfc != cx.subscription.end() &&
              cxKfc->codexConfigToml.find(
                  "base_url = \"https://api.kimi.com/coding/v1\"") !=
                  std::string::npos &&
              cxKfc->codexConfigToml.find("wire_api = \"responses\"") !=
                  std::string::npos);
        const auto cxStepfun = std::ranges::find_if(cx.metered,
            [](const models::Provider& p) { return p.name == "StepFun（阶跃）"; });
        CHECK(cxStepfun != cx.metered.end() &&
              cxStepfun->codexConfigToml.find(
                  "base_url = \"https://api.stepfun.com/step_plan/v1\"") !=
                  std::string::npos &&
              cxStepfun->usageEnabled && cxStepfun->usagePath == "balance");
        // gemini：新增预设分支（cc-switch geminiProviderPresets 同源），
        // needsModel 工具的预设都带模型；qwen / zcode 仍无预设。
        const auto gm = models::builtinPresets("gemini");
        CHECK(!gm.subscription.empty() && gm.metered.empty());
        CHECK(std::ranges::all_of(gm.subscription,
            [](const models::Provider& p) { return !p.model.empty(); }));
        const auto qw = models::builtinPresets("qwen");
        CHECK(qw.subscription.empty() && qw.metered.empty());
        const auto zc = models::builtinPresets("zcode");
        CHECK(zc.subscription.empty() && zc.metered.empty());
        // dsh：按量组 4 家直连（DeepSeek / Kimi / GLM / 千问），都带模型；
        // 官方走常驻卡（内置 deepseek-official 路由）。
        CHECK(models::officialVendorName("dsh") == "DeepSeek 官方");
        const auto ds = models::builtinPresets("dsh");
        CHECK(ds.subscription.empty() && ds.metered.size() == 4);
        CHECK(ds.metered.front().name == "DeepSeek" &&
              ds.metered.front().apiFormat == "anthropic" &&
              ds.metered.front().usageEnabled);
        CHECK(std::ranges::all_of(ds.metered, [](const models::Provider& p) {
            return !p.baseUrl.empty() && !p.model.empty() && p.fullUrl;
        }));
        // hermes：cc-switch hermesProviderPresets 同源迁移，无官方常驻卡；
        // 预设都带模型，apiFormat 覆盖三档。
        CHECK(models::officialVendorName("hermes").empty());
        const auto hm = models::builtinPresets("hermes");
        CHECK(hm.subscription.empty() && !hm.metered.empty());
        CHECK(std::ranges::all_of(hm.metered, [](const models::Provider& p) {
            return !p.baseUrl.empty() && !p.model.empty() && p.fullUrl;
        }));
        CHECK(std::ranges::any_of(hm.metered, [](const models::Provider& p) {
            return p.apiFormat == "anthropic";
        }));
        const auto hmDs = std::ranges::find_if(hm.metered,
            [](const models::Provider& p) { return p.name == "DeepSeek"; });
        CHECK(hmDs != hm.metered.end() && hmDs->usageEnabled &&
              hmDs->apiFormat == "openai-chat");

        // 所有默认模板（两个计费组、所有 Agent）必须登记可复用的厂商图标。
        for (const auto& tool : models::toolRegistry()) {
            const auto presets = models::builtinPresets(tool.id);
            for (const auto& preset : presets.subscription) {
                CHECK(!models::builtinPresetIconName(preset.name).empty());
            }
            for (const auto& preset : presets.metered) {
                CHECK(!models::builtinPresetIconName(preset.name).empty());
            }
        }
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
        // 坏文件挪走后等价全新启动：live 文件仍在的组（claude-code/zcode 等）
        // 会被首次导入自动收编，组不再为空是预期行为。
        CHECK(broken.detectCurrent("zcode").empty() ||
              !broken.group("zcode").providers.empty());
        // 自动收编可能已重建 config.json（坏文件仍被挪走且不再被读回）。
        CHECK(!fs::exists(cfg::configFile()) ||
              readJson(cfg::configFile()).is_object());
        bool corruptMoved = false;
        for (const auto& entry : fs::directory_iterator(cfg::dataDir())) {
            if (entry.path().filename().string().starts_with("config.json.corrupt-")) {
                corruptMoved = true;
            }
        }
        CHECK(corruptMoved);
    }

    // 18. 本地环境检查：findExecutable 只在 PATH / 已知目录里找可执行文件，
    //     同名但没有可执行位的普通文件不算「已安装」。
    {
        const fs::path bin = root / "probe-bin";
        std::error_code ec;
        fs::create_directories(bin, ec);
#ifdef _WIN32
        // Windows 走 PATHEXT：探 llmswitch-probe-cli 命中同名 .cmd。
        const fs::path cli = bin / "llmswitch-probe-cli.cmd";
#else
        const fs::path cli = bin / "llmswitch-probe-cli";
#endif
        writeFile(cli, "#!/bin/sh\nexit 0\n");
#ifndef _WIN32
        fs::permissions(cli, fs::perms::owner_all, ec);
#endif
        const char* oldPath = std::getenv("PATH");
        const std::string savedPath = oldPath != nullptr ? oldPath : "";
        testenv::setenv("PATH", bin.string().c_str());
        const auto found = cfg::findExecutable("llmswitch-probe-cli");
        CHECK(!found.empty());
        if (!found.empty()) CHECK(fs::equivalent(found, cli));
        CHECK(cfg::findExecutable("llmswitch-definitely-missing").empty());
        CHECK(cfg::findExecutable("").empty());
#ifndef _WIN32
        const fs::path plain = bin / "llmswitch-not-exec";
        writeFile(plain, "not a program");
        fs::permissions(plain, fs::perms::owner_read | fs::perms::owner_write, ec);
        CHECK(cfg::findExecutable("llmswitch-not-exec").empty());
#endif
        testenv::setenv("PATH", savedPath.c_str());
    }

    // 19. 清理临时目录
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
