// test_router.cpp — llmswitch.router 领域层测试（无框架：CHECK 失败计数，非零即败）。
// 全程隔离在临时目录：HOME / XDG_DATA_HOME / LLMSWITCH_STATS_DIR 指向
// temp_directory_path()/llmswitch-test-router-<pid>，statsFile 不碰真实环境。
//
// 覆盖：转发 path/query/头替换（anthropic 双头与 openai 单头）、响应原样
// 透传、stats 记录与 usage token 提取、故障转移开关、未知工具 404、
// 无 current 502、clearStats、JSONL 落盘与重启回填。
// 假上游与客户端都用 cpp-httplib（测试目标已链 llmswitch_httplib）。
#include <cstdio>    // stderr（std 模块不导出 stdout/stderr 宏）
#include <cstdlib>   // setenv
#include <unistd.h>  // getpid
#include <httplib.h>

import std;
import nlohmann.json;
import llmswitch.config;
import llmswitch.models;
import llmswitch.router;

namespace {

int g_failures = 0;

#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            std::println(stderr, "FAIL {}: {}", __LINE__, #cond); \
            ++g_failures;                                        \
        }                                                        \
    } while (0)

// 假上游：记录最近一次请求，返回可配置状态码 + 固定 JSON（含 usage 段）。
struct FakeUpstream {
    httplib::Server svr;
    std::thread thread;
    int port = 0;
    std::atomic<int> status{200};
    std::string body =
        R"({"ok":true,"usage":{"prompt_tokens":12,"completion_tokens":34}})";

    mutable std::mutex mu;
    int hits = 0;
    std::string lastMethod, lastPath, lastTarget, lastBody;
    std::string lastAuth, lastApiKey, lastCustom;

    void start() {
        // catch-all 正则（不用 set_pre_routing_handler：它在 body 读取前触发）。
        const auto handler =
            [this](const httplib::Request& req, httplib::Response& res) {
                {
                    std::lock_guard lk(mu);
                    ++hits;
                    lastMethod = req.method;
                    lastPath = req.path;
                    lastTarget = req.target;
                    lastBody = req.body;
                    lastAuth = req.has_header("Authorization")
                                   ? req.get_header_value("Authorization")
                                   : "";
                    lastApiKey = req.has_header("x-api-key")
                                     ? req.get_header_value("x-api-key")
                                     : "";
                    lastCustom = req.has_header("X-Custom")
                                     ? req.get_header_value("X-Custom")
                                     : "";
                }
                res.status = status.load();
                res.set_content(body, "application/json");
            };
        svr.Get(R"(/(.*))", handler);
        svr.Post(R"(/(.*))", handler);
        svr.Put(R"(/(.*))", handler);
        svr.Patch(R"(/(.*))", handler);
        svr.Delete(R"(/(.*))", handler);
        svr.Options(R"(/(.*))", handler);
        port = svr.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { svr.listen_after_bind(); });
    }

    void stop() {
        svr.stop();
        if (thread.joinable()) thread.join();
    }

    std::string baseUrl() const {
        return std::format("http://127.0.0.1:{}", port);
    }

    ~FakeUpstream() { stop(); }
};

int perProviderCount(const router::StatsSnapshot& s, std::string_view name) {
    for (const auto& [n, c] : s.perProvider) {
        if (n == name) return static_cast<int>(c);
    }
    return -1;
}

} // namespace

int main() {
    namespace fs = std::filesystem;
    using namespace std::string_literals;

    // ---- 环境隔离 -----------------------------------------------------------
    const fs::path root =
        fs::temp_directory_path() / std::format("llmswitch-test-router-{}", ::getpid());
    {
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root);
    }
    ::setenv("HOME", (root / "home").c_str(), 1);
    ::setenv("XDG_DATA_HOME", (root / "xdg").c_str(), 1);
    ::setenv("LLMSWITCH_STATS_DIR", (root / "stats").c_str(), 1);
    CHECK(cfg::statsFile() == root / "stats" / "requests.jsonl");

    FakeUpstream up1;
    up1.start();
    FakeUpstream up2;
    up2.start();
    CHECK(up1.port > 0 && up2.port > 0);

    // 静态 resolver：三个组 —— claude-code（单供应商，baseUrl 带子路径）、
    // codex（openai 协议单供应商）、pi（有供应商但无 current）。
    models::ProviderGroup gClaude{
        .providers = {models::Provider{.id = "p1",
                                       .name = "供应商甲",
                                       .baseUrl = up1.baseUrl() + "/base",
                                       .apiKey = "sk-provider-1"}},
        .current = "p1"};
    models::ProviderGroup gCodex{
        .providers = {models::Provider{.id = "c1",
                                       .name = "Codex源",
                                       .baseUrl = up1.baseUrl() + "/v1",
                                       .apiKey = "sk-codex-1"}},
        .current = "c1"};
    models::ProviderGroup gPi{
        .providers = {models::Provider{.id = "pi1",
                                       .name = "Pi源",
                                       .baseUrl = up1.baseUrl(),
                                       .apiKey = "sk-pi"}},
        .current = ""};  // 无 current

    std::map<std::string, models::ProviderGroup> groups = {
        {"claude-code", gClaude}, {"codex", gCodex}, {"pi", gPi}};

    router::LocalRouter::GroupProvider resolver =
        [&groups](std::string_view tool) -> std::optional<models::ProviderGroup> {
        if (const auto it = groups.find(std::string(tool)); it != groups.end()) {
            return it->second;
        }
        return std::nullopt;
    };

    router::LocalRouter router1(resolver);
    router1.start(0);
    CHECK(router1.running());
    CHECK(router1.port() > 0);
    router1.start(0);  // 重复调用安全（no-op）
    CHECK(router1.running());

    httplib::Client cli("127.0.0.1", router1.port());

    // 1. 基本转发：path/query 拼接、鉴权替换（claude-code → anthropic 双头）、
    //    其余头透传、响应原样返回、stats 记录且 token 被解析。
    {
        httplib::Headers headers = {
            {"Authorization", "Bearer sk-client"},
            {"x-api-key", "client-x-key"},
            {"X-Custom", "keep-me"},
        };
        auto res = cli.Post("/claude-code/v1/messages?stream=0", headers,
                            R"({"model":"m","messages":[]})",
                            "application/json");
        CHECK(res && res->status == 200);
        CHECK(res->body.contains("\"ok\":true"));
        {
            std::lock_guard lk(up1.mu);
            CHECK(up1.lastMethod == "POST");
            CHECK(up1.lastPath == "/base/v1/messages");  // baseUrl 子路径拼接
            CHECK(up1.lastTarget.contains("stream=0"));  // query 保留
            CHECK(up1.lastAuth == "Bearer sk-provider-1");  // 鉴权替换
            CHECK(up1.lastApiKey == "sk-provider-1");       // anthropic 双头
            CHECK(up1.lastCustom == "keep-me");             // 其余头透传
            CHECK(up1.lastBody == R"({"model":"m","messages":[]})");
        }
        const auto snap = router1.snapshot();
        CHECK(snap.totalRequests == 1);
        CHECK(snap.todayRequests == 1);
        CHECK(snap.successCount == 1);
        CHECK(snap.avgLatencyMs >= 0.0);
        CHECK(snap.totalPromptTokens == 12);
        CHECK(snap.totalCompletionTokens == 34);
        CHECK(perProviderCount(snap, "供应商甲") == 1);
        const auto logs = router1.recentLogs(10);
        CHECK(logs.size() == 1);
        CHECK(logs[0].tool == "claude-code");
        CHECK(logs[0].providerId == "p1");
        CHECK(logs[0].method == "POST");
        CHECK(logs[0].path == "/claude-code/v1/messages");
        CHECK(logs[0].status == 200);
        CHECK(logs[0].promptTokens == 12);
        CHECK(logs[0].completionTokens == 34);
    }

    // 2. openai 协议（codex）：只注入 Authorization，不注入 x-api-key；
    //    客户端自带的 x-api-key 被剥掉。
    {
        httplib::Headers headers = {
            {"Authorization", "Bearer sk-client-2"},
            {"x-api-key", "client-x-2"},
        };
        auto res = cli.Post("/codex/chat/completions", headers, "{}", "application/json");
        CHECK(res && res->status == 200);
        std::lock_guard lk(up1.mu);
        CHECK(up1.lastPath == "/v1/chat/completions");
        CHECK(up1.lastAuth == "Bearer sk-codex-1");
        CHECK(up1.lastApiKey.empty());
    }

    // 3. 故障转移：p1（500）→ p2（200）。开 failover 拿到 200 且 stats 两条；
    //    关 failover 拿到 500 且 stats 一条。
    groups["claude-code"].providers.push_back(
        models::Provider{.id = "p2",
                         .name = "供应商乙",
                         .baseUrl = up2.baseUrl(),
                         .apiKey = "sk-provider-2"});
    {
        up1.status = 500;
        const int hits1Before = up1.hits;

        router1.setFailoverEnabled(true);
        router1.clearStats();
        auto res = cli.Post("/claude-code/v1/messages", "{}", "application/json");
        CHECK(res && res->status == 200);
        {
            std::lock_guard lk(up1.mu);
            CHECK(up1.hits == hits1Before + 1);  // p1 只试一次
            std::lock_guard lk2(up2.mu);
            CHECK(up2.hits == 1);
            CHECK(up2.lastAuth == "Bearer sk-provider-2");
        }
        auto snap = router1.snapshot();
        CHECK(snap.totalRequests == 2);
        CHECK(snap.successCount == 1);
        CHECK(perProviderCount(snap, "供应商甲") == 1);
        CHECK(perProviderCount(snap, "供应商乙") == 1);
        const auto logs = router1.recentLogs(10);
        CHECK(logs.size() == 2);
        CHECK(logs[0].status == 200);  // 新的在前
        CHECK(logs[1].status == 500);

        router1.setFailoverEnabled(false);
        router1.clearStats();
        res = cli.Post("/claude-code/v1/messages", "{}", "application/json");
        CHECK(res && res->status == 500);
        snap = router1.snapshot();
        CHECK(snap.totalRequests == 1);
        CHECK(snap.successCount == 0);
        up1.status = 200;
    }

    // 4. 未知工具 → 404；组无 current → 502（均为中文 JSON 错误体）。
    {
        auto res = cli.Get("/nope/x");
        CHECK(res && res->status == 404);
        CHECK(res->body.contains("error"));
        res = cli.Get("/pi/v1/chat");
        CHECK(res && res->status == 502);
        CHECK(res->body.contains("当前供应商"));
        // 错误响应不记入 stats
        CHECK(router1.snapshot().totalRequests == 1);
    }

    // 5. clearStats / JSONL 落盘与重启回填。
    {
        router1.clearStats();
        CHECK(router1.snapshot().totalRequests == 0);
        CHECK(!fs::exists(cfg::statsFile()));

        auto res = cli.Post("/claude-code/v1/messages", "{}", "application/json");
        CHECK(res && res->status == 200);
        CHECK(router1.snapshot().totalRequests == 1);
        CHECK(fs::exists(cfg::statsFile()));  // 已落盘

        router1.stop();
        CHECK(!router1.running());
        CHECK(router1.port() == 0);

        // 新实例从 JSONL 回填（不启动服务也能查统计）。
        router::LocalRouter router2(resolver);
        const auto snap = router2.snapshot();
        CHECK(snap.totalRequests == 1);
        CHECK(snap.successCount == 1);
        CHECK(perProviderCount(snap, "供应商甲") == 1);
        const auto logs = router2.recentLogs(10);
        CHECK(logs.size() == 1);
        CHECK(logs[0].promptTokens == 12);

        router2.clearStats();
        CHECK(router2.snapshot().totalRequests == 0);
        CHECK(!fs::exists(cfg::statsFile()));
    }

    // 6. 清理临时目录
    up1.stop();
    up2.stop();
    {
        std::error_code ec;
        fs::remove_all(root, ec);
        CHECK(!ec);
    }

    if (g_failures == 0) {
        std::println("test_router: ok");
        return 0;
    }
    std::println(stderr, "test_router: {} 项断言失败", g_failures);
    return 1;
}
