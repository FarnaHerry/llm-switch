// test_store.cpp — llmswitch.store 领域层测试（无框架：CHECK 失败计数，非零即败）。
// 全程隔离在临时目录：HOME / XDG_DATA_HOME / LLMSWITCH_* 指向
// temp_directory_path()/llmswitch-test-<pid>，live 文件与 dataDir 都不碰真实环境。
//
// 覆盖：空载默认值、首次导入收编、claude 切换深合并（保留非 env 字段）、
// codex 切换（auth.json + config.toml 整段替换）、备份生成、detectCurrent、
// 导出/导入回滚、损坏 config.json 挪走不崩溃。
#include <cstdio>    // stderr（std 模块不导出 stdout/stderr 宏）
#include <cstdlib>   // setenv
#include <unistd.h>  // getpid

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
        fs::temp_directory_path() / std::format("llmswitch-test-{}", ::getpid());
    {
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root);
    }
    const fs::path home = root / "home";
    ::setenv("HOME", home.c_str(), 1);
    ::setenv("XDG_DATA_HOME", (root / "xdg").c_str(), 1);
    const fs::path claudeSettings = home / ".claude" / "settings.json";
    const fs::path codexAuth = home / ".codex" / "auth.json";
    const fs::path codexConfig = home / ".codex" / "config.toml";
    ::setenv("LLMSWITCH_CLAUDE_SETTINGS", claudeSettings.c_str(), 1);
    ::setenv("LLMSWITCH_CODEX_AUTH", codexAuth.c_str(), 1);
    ::setenv("LLMSWITCH_CODEX_CONFIG", codexConfig.c_str(), 1);

    // 1. 空环境 load → 默认空配置
    {
        auto s = store::ProviderStore::load();
        CHECK(s.config().claude.providers.empty());
        CHECK(s.config().codex.providers.empty());
        CHECK(s.config().themeMode == "system");
        CHECK(s.detectCurrent(store::kToolClaude).empty());
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
    std::string liveId;
    {
        CHECK(s.config().claude.providers.size() == 1);
        const auto& p = s.config().claude.providers.front();
        CHECK(p.name == "当前配置");
        CHECK(p.baseUrl == "https://live.example.com/anthropic");
        CHECK(p.apiKey == "sk-live-token");
        CHECK(p.model == "claude-live");
        CHECK(s.config().claude.current == p.id);
        CHECK(s.detectCurrent(store::kToolClaude) == p.id);
        liveId = p.id;
        // 再次 load：已有收编项，不重复建
        auto again = store::ProviderStore::load();
        CHECK(again.config().claude.providers.size() == 1);
    }

    // 3. addProvider 两个 + switchTo：env 三字段写入、permissions 保留
    models::Provider pa{.name = "供应商A",
                        .baseUrl = "https://a.example.com",
                        .apiKey = "sk-a",
                        .model = "model-a"};
    models::Provider pb{.name = "供应商B",
                        .baseUrl = "https://b.example.com",
                        .apiKey = "sk-b"};  // model 留空
    s.addProvider(store::kToolClaude, pa);
    s.addProvider(store::kToolClaude, pb);
    CHECK(s.config().claude.providers.size() == 3);
    const std::string idA = s.config().claude.providers[1].id;
    const std::string idB = s.config().claude.providers[2].id;
    CHECK(!idA.empty() && !idB.empty() && idA != idB);

    s.switchTo(store::kToolClaude, idB);
    {
        const auto j = readJson(claudeSettings);
        CHECK(j["env"]["ANTHROPIC_BASE_URL"] == "https://b.example.com");
        CHECK(j["env"]["ANTHROPIC_AUTH_TOKEN"] == "sk-b");
        // model 为空 → 不动既有 ANTHROPIC_MODEL
        CHECK(j["env"]["ANTHROPIC_MODEL"] == "claude-live");
        // 非 env 字段原样保留
        CHECK(j["permissions"]["allow"][0] == "Bash(*)");
        CHECK(s.config().claude.current == idB);
        CHECK(s.detectCurrent(store::kToolClaude) == idB);
    }
    s.switchTo(store::kToolClaude, idA);
    {
        const auto j = readJson(claudeSettings);
        CHECK(j["env"]["ANTHROPIC_MODEL"] == "model-a");
        CHECK(s.config().claude.current == idA);
    }

    // 4. codex：switchTo 写 auth.json 的 OPENAI_API_KEY + config.toml 原文替换
    writeFile(codexAuth, R"json({"OPENAI_API_KEY": "sk-old"}
)json");
    writeFile(codexConfig, "# 旧的 codex 配置\nmodel = \"old\"\n");
    models::Provider pc{.name = "OpenRouter",
                        .baseUrl = "https://openrouter.ai/api/v1",
                        .apiKey = "sk-or-key",
                        .codexConfigToml = "model_provider = \"openrouter\"\n"};
    s.addProvider(store::kToolCodex, pc);
    const std::string idC = s.config().codex.providers.back().id;
    s.switchTo(store::kToolCodex, idC);
    {
        const auto j = readJson(codexAuth);
        CHECK(j["OPENAI_API_KEY"] == "sk-or-key");
        std::ifstream in(codexConfig, std::ios::binary);
        const std::string toml((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        CHECK(toml == "model_provider = \"openrouter\"\n");
        CHECK(s.config().codex.current == idC);
        CHECK(s.detectCurrent(store::kToolCodex) == idC);
    }

    // 5. 备份生成在 backups 目录（claude 切了两次 → ≥2 份；codex auth/config 各 1 份）
    {
        const auto backups = cfg::backupsDir();
        CHECK(countBackups(backups / "claude", "settings.json") >= 2);
        CHECK(countBackups(backups / "codex", "auth.json") == 1);
        CHECK(countBackups(backups / "codex", "config.toml") == 1);
    }

    // 6. detectCurrent：不匹配 → 空串；live 文件改回匹配值 → 恢复命中
    writeFile(claudeSettings, R"json({"env": {"ANTHROPIC_BASE_URL": "https://elsewhere.example.com", "ANTHROPIC_AUTH_TOKEN": "sk-unknown"}}
)json");
    CHECK(s.detectCurrent(store::kToolClaude).empty());
    writeFile(claudeSettings, R"json({"env": {"ANTHROPIC_BASE_URL": "https://a.example.com", "ANTHROPIC_AUTH_TOKEN": "sk-a"}}
)json");
    CHECK(s.detectCurrent(store::kToolClaude) == idA);

    // 7. exportTo → 改库 → importFrom 恢复（导入前自动备份 config.json）
    const fs::path exportPath = root / "export.json";
    s.exportTo(exportPath);
    CHECK(fs::exists(exportPath));
    s.removeProvider(store::kToolClaude, idA);
    CHECK(s.config().claude.providers.size() == 2);
    s.importFrom(exportPath);
    {
        CHECK(s.config().claude.providers.size() == 3);
        bool found = false;
        for (const auto& p : s.config().claude.providers) {
            if (p.id == idA) found = true;
        }
        CHECK(found);
        CHECK(countBackups(cfg::backupsDir(), "config.json") >= 1);
    }

    // 8. 损坏的 config.json → load 不崩溃，坏文件被挪到 .corrupt-<时间戳>
    fs::remove(claudeSettings);  // 避免首次导入干扰空配置断言
    fs::remove(codexAuth);
    writeFile(cfg::configFile(), "这不是 JSON {{{\n");
    {
        auto broken = store::ProviderStore::load();
        CHECK(broken.config().claude.providers.empty());
        CHECK(broken.config().codex.providers.empty());
        CHECK(!fs::exists(cfg::configFile()));  // 已被挪走且未重建
        bool corruptMoved = false;
        for (const auto& entry : fs::directory_iterator(cfg::dataDir())) {
            if (entry.path().filename().string().starts_with("config.json.corrupt-")) {
                corruptMoved = true;
            }
        }
        CHECK(corruptMoved);
    }

    // 9. 清理临时目录
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
