// test_router.cpp — llmswitch.router 领域层测试（无框架：CHECK 失败计数，非零即败）。
// 全程隔离在临时目录：HOME / XDG_DATA_HOME / LLMSWITCH_STATS_DIR 指向
// temp_directory_path()/llmswitch-test-router-<pid>，statsFile 不碰真实环境。
//
// 覆盖：转发 path/query/头替换（anthropic 双头与 openai 单头）、响应原样
// 透传、stats 记录与 usage token 提取、故障转移开关、逐 Agent 代理开关、
// 未知工具 404、无 current 502、clearStats、JSONL 落盘与重启回填。
// 假上游与客户端都用 cpp-httplib（测试目标已链 llmswitch_httplib）；出站
// 注入 httplib::Client 版 UpstreamSession 替身生产的平台 HttpClient 桥接。
#include <cstdio>    // stderr（std 模块不导出 stdout/stderr 宏）
#include "test_env.h"  // setenv/getpid/unsetenv 可移植封装
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

template <class Function>
bool throwsRuntimeError(Function&& function) {
    try {
        std::invoke(std::forward<Function>(function));
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

// 假上游：记录最近一次请求，返回可配置状态码 + 固定 JSON（含 usage 段）。
struct FakeUpstream {
    httplib::Server svr;
    std::thread thread;
    int port = 0;
    std::atomic<int> status{200};
    std::string body =
        R"({"ok":true,"usage":{"prompt_tokens":12,"completion_tokens":34}})";
    std::string contentType = "application/json";

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
                res.set_content(body, contentType);
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

// 测试上游会话：httplib::Client 直连 127.0.0.1 假上游，替身生产的
// 平台 HttpClient 桥接会话。content-type 从透传头里取出按 httplib 参数传递，
// 其余头原样透传；content-type 摘取由 router 侧统一整形，会话不负责。
class HttplibUpstreamSession final : public router::UpstreamSession {
public:
    router::UpstreamResponse Send(const router::UpstreamRequest& request) override {
        router::UpstreamResponse out;
        // 拆 scheme://host[:port]/path?query（假上游恒为 http 回环地址）。
        const std::string& url = request.url;
        const auto schemeEnd = url.find("://");
        if (schemeEnd == std::string::npos) {
            out.error = "测试会话：无效 URL";
            return out;
        }
        const auto pathStart = url.find('/', schemeEnd + 3);
        const std::string hostPort = url.substr(
            schemeEnd + 3,
            pathStart == std::string::npos ? std::string::npos
                                           : pathStart - schemeEnd - 3);
        const std::string path = pathStart == std::string::npos
                                     ? "/"
                                     : url.substr(pathStart);

        std::string contentType = "application/octet-stream";
        httplib::Headers headers;
        for (const auto& [name, value] : request.headers) {
            const std::string_view lower(name);
            if (lower.size() == 11 &&
                std::ranges::equal(lower, std::string_view("content-type"),
                                   [](char a, char b) {
                                       return std::tolower(static_cast<unsigned char>(a)) ==
                                              static_cast<unsigned char>(b);
                                   })) {
                contentType = value;
                continue;
            }
            headers.emplace(name, value);
        }

        httplib::Client cli("http://" + hostPort);
        const std::string& body = request.body;
        httplib::Result result = [&] {
            const std::string& m = request.method;
            if (m == "POST") return cli.Post(path, headers, body, contentType);
            if (m == "PUT") return cli.Put(path, headers, body, contentType);
            if (m == "PATCH") return cli.Patch(path, headers, body, contentType);
            if (m == "DELETE") return cli.Delete(path, headers, body, contentType);
            if (m == "HEAD") return cli.Head(path, headers);
            if (m == "OPTIONS") return cli.Options(path, headers);
            return cli.Get(path, headers);
        }();
        if (!result) {
            out.error = httplib::to_string(result.error());
            return out;
        }
        out.status = result->status;
        for (const auto& [name, value] : result->headers) {
            out.headers.emplace_back(name, value);
        }
        out.body = std::move(result->body);
        return out;
    }

    void AbortInFlight() override {}
};

} // namespace

int main() {
    namespace fs = std::filesystem;
    using namespace std::string_literals;

    // ---- 环境隔离 -----------------------------------------------------------
    const fs::path root =
        fs::temp_directory_path() / std::format("llmswitch-test-router-{}", testenv::getpid());
    {
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root);
    }
    testenv::setenv("HOME", (root / "home"));
    testenv::setenv("XDG_DATA_HOME", (root / "xdg"));
    testenv::setenv("LLMSWITCH_DATA_DIR", (root / "data"));
    testenv::setenv("LLMSWITCH_STATS_DIR", (root / "stats"));
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

    router::LocalRouter router1(resolver, std::make_shared<HttplibUpstreamSession>());
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

    // 2b. 逐 Agent 代理开关即时生效：禁用返回 403，不访问上游、不写统计；
    //     重新启用后同一路径恢复转发。
    {
        const auto requestsBefore = router1.snapshot().totalRequests;
        int hitsBefore = 0;
        {
            std::lock_guard lk(up1.mu);
            hitsBefore = up1.hits;
        }
        CHECK(router1.toolEnabled("codex"));
        router1.setToolEnabled("codex", false);
        CHECK(!router1.toolEnabled("codex"));
        auto res = cli.Post("/codex/chat/completions", "{}",
                            "application/json");
        CHECK(res && res->status == 403);
        CHECK(res->body.contains("未启用"));
        {
            std::lock_guard lk(up1.mu);
            CHECK(up1.hits == hitsBefore);
        }
        CHECK(router1.snapshot().totalRequests == requestsBefore);
        CHECK(!router1.toolEnabled("unknown"));
        CHECK(throwsRuntimeError(
            [&] { router1.setToolEnabled("unknown", true); }));

        router1.setToolEnabled("codex", true);
        res = cli.Post("/codex/chat/completions", "{}", "application/json");
        CHECK(res && res->status == 200);
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

    // 3b. 聚合供应商：按请求 model 优先匹配成员；成员 500 时继续下一条，
    //     即使组级 failover 关闭也保持聚合内兜底。
    {
        groups["codex"].providers.push_back(models::Provider{
            .id = "agg",
            .name = "聚合源",
            .upstreams = {
                models::ProviderUpstream{.name = "甲",
                                         .baseUrl = up1.baseUrl(),
                                         .apiKey = "sk-agg-1",
                                         .model = "model-a"},
                models::ProviderUpstream{.name = "乙",
                                         .baseUrl = up2.baseUrl(),
                                         .apiKey = "sk-agg-2",
                                         .model = "model-b"},
            }});
        groups["codex"].current = "agg";
        router1.setFailoverEnabled(false);
        router1.clearStats();
        up1.status = 500;
        up2.status = 200;
        up1.hits = 0;
        up2.hits = 0;
        auto res = cli.Post("/codex/chat/completions",
                            R"({"model":"model-b"})", "application/json");
        CHECK(res && res->status == 200);
        {
            std::lock_guard lk(up1.mu);
            CHECK(up1.hits == 0);  // model-b 命中乙，甲不应被先试
        }
        {
            std::lock_guard lk(up2.mu);
            CHECK(up2.hits == 1);
            CHECK(up2.lastAuth == "Bearer sk-agg-2");
            CHECK(up2.lastPath == "/chat/completions");
        }
        auto snap = router1.snapshot();
        CHECK(snap.totalRequests == 1);
        CHECK(perProviderCount(snap, "聚合源 · 乙") == 1);

        // 未匹配到 model 时按配置顺序：甲先 500，再兜底到乙 200。
        up1.hits = 0;
        up2.hits = 0;
        router1.clearStats();
        res = cli.Post("/codex/chat/completions",
                       R"({"model":"model-unknown"})", "application/json");
        CHECK(res && res->status == 200);
        {
            std::lock_guard lk(up1.mu);
            CHECK(up1.hits == 1);
        }
        {
            std::lock_guard lk(up2.mu);
            CHECK(up2.hits == 1);
        }
        snap = router1.snapshot();
        CHECK(snap.totalRequests == 2);
        CHECK(snap.successCount == 1);
        groups["codex"].current = "c1";
        up1.status = 200;
        up2.status = 200;
        router1.clearStats();
    }

    // 3c. 聚合成员引用同组现有 Provider：凭据/baseUrl 从被引用条目解析，
    //     model 匹配与兜底不变；引用聚合项或已删除项会被跳过。
    {
        groups["codex"].providers.push_back(models::Provider{
            .id = "ref-a",
            .name = "引用甲",
            .baseUrl = up1.baseUrl(),
            .apiKey = "sk-ref-a",
            .model = "model-ref-a",
            .fullUrl = true,
        });
        groups["codex"].providers.push_back(models::Provider{
            .id = "ref-b",
            .name = "引用乙",
            .baseUrl = up2.baseUrl(),
            .apiKey = "sk-ref-b",
            .model = "model-ref-b",
            .models = {"model-ref-b", "model-ref-extra"},
            .fullUrl = true,
        });
        groups["codex"].providers.push_back(models::Provider{
            .id = "agg-ref",
            .name = "引用聚合",
            .upstreams = {
                models::ProviderUpstream{.providerId = "ref-a",
                                         .model = "model-ref-a"},
                models::ProviderUpstream{.providerId = "ref-b",
                                         .model = "model-ref-b"},
            }});
        groups["codex"].current = "agg-ref";
        up1.status = 200;
        up2.status = 200;
        up1.hits = 0;
        up2.hits = 0;
        router1.clearStats();
        auto res = cli.Post("/codex/chat/completions",
                            R"({"model":"model-ref-b"})", "application/json");
        CHECK(res && res->status == 200);
        {
            std::lock_guard lk(up1.mu);
            CHECK(up1.hits == 0);
        }
        {
            std::lock_guard lk(up2.mu);
            CHECK(up2.hits == 1);
            CHECK(up2.lastAuth == "Bearer sk-ref-b");
        }
        // 未匹配 model 时按引用顺序，且首个 500 会兜底到第二个。
        up1.status = 500;
        up1.hits = 0;
        up2.hits = 0;
        router1.clearStats();
        res = cli.Post("/codex/chat/completions",
                       R"({"model":"unknown-ref"})", "application/json");
        CHECK(res && res->status == 200);
        {
            std::lock_guard lk(up1.mu);
            CHECK(up1.hits == 1);
        }
        {
            std::lock_guard lk(up2.mu);
            CHECK(up2.hits == 1);
        }
        const auto snap = router1.snapshot();
        CHECK(snap.totalRequests == 2);
        CHECK(perProviderCount(snap, "引用聚合 · 引用乙") == 1);
        groups["codex"].current = "c1";
        up1.status = 200;
        up2.status = 200;
        router1.clearStats();
    }

    // 3d. 引用聚合同一条供应商的两个模型：请求精确命中第二个模型时，
    //     不能退回供应商主模型或按配置顺序先打第一个成员。
    {
        groups["codex"].providers.push_back(models::Provider{
            .id = "ref-c",
            .name = "引用丙",
            .baseUrl = up2.baseUrl(),
            .apiKey = "sk-ref-c",
            .model = "primary-ref-c",
            .fullUrl = true,
        });
        groups["codex"].providers.push_back(models::Provider{
            .id = "agg-ref-multi",
            .name = "引用全模型",
            .upstreams = {
                models::ProviderUpstream{.providerId = "ref-a",
                                         .model = "model-ref-a"},
                models::ProviderUpstream{.providerId = "ref-c",
                                         .model = "model-ref-c-entry"},
            }});
        groups["codex"].current = "agg-ref-multi";
        up1.status = 200;
        up2.status = 200;
        up1.hits = 0;
        up2.hits = 0;
        router1.clearStats();
        auto res = cli.Post("/codex/chat/completions",
                            R"({"model":"model-ref-c-entry"})",
                            "application/json");
        CHECK(res && res->status == 200);
        {
            std::lock_guard lk(up1.mu);
            CHECK(up1.hits == 0);
        }
        {
            std::lock_guard lk(up2.mu);
            CHECK(up2.hits == 1);
            CHECK(up2.lastAuth == "Bearer sk-ref-c");
        }
        groups["codex"].current = "c1";
        auto& providers = groups["codex"].providers;
        std::erase_if(providers, [](const models::Provider& provider) {
            return provider.id == "agg-ref-multi" || provider.id == "ref-c";
        });
        router1.clearStats();
    }

    // 3e. 聚合对外模型带「供应商名/模型」前缀：命中成员后，转发给上游
    //     的请求体必须还原成原始模型 id，不能让上游看到前缀。
    {
        groups["codex"].providers.push_back(models::Provider{
            .id = "agg-ref-exposed",
            .name = "引用前缀",
            .models = {"引用乙/model-ref-b", "引用乙/model-ref-extra"},
            .upstreams = {
                models::ProviderUpstream{.providerId = "ref-a",
                                         .model = "model-ref-a"},
                models::ProviderUpstream{.providerId = "ref-b",
                                         .model = "model-ref-b"},
            }});
        groups["codex"].current = "agg-ref-exposed";
        up1.status = 200;
        up2.status = 200;
        up1.hits = 0;
        up2.hits = 0;
        router1.clearStats();
        auto res = cli.Post("/codex/chat/completions",
                            R"({"model":"引用乙/model-ref-b"})",
                            "application/json");
        CHECK(res && res->status == 200);
        {
            std::lock_guard lk(up1.mu);
            CHECK(up1.hits == 0);
        }
        {
            std::lock_guard lk(up2.mu);
            CHECK(up2.hits == 1);
            CHECK(up2.lastAuth == "Bearer sk-ref-b");
            CHECK(up2.lastBody == R"({"model":"model-ref-b"})");
        }
        // 清单里有、upstreams 没展开的模型也要能精确命中并剥前缀。
        up2.hits = 0;
        res = cli.Post("/codex/chat/completions",
                       R"({"model":"引用乙/model-ref-extra"})",
                       "application/json");
        CHECK(res && res->status == 200);
        {
            std::lock_guard lk(up1.mu);
            CHECK(up1.hits == 0);
        }
        {
            std::lock_guard lk(up2.mu);
            CHECK(up2.hits == 1);
            CHECK(up2.lastBody == R"({"model":"model-ref-extra"})");
        }
        groups["codex"].current = "c1";
        auto& providers = groups["codex"].providers;
        std::erase_if(providers, [](const models::Provider& provider) {
            return provider.id == "agg-ref-exposed";
        });
        router1.clearStats();
    }

    // 3f. 协议转换：Claude 目标聚合一个 OpenAI Chat 成员。请求体按
    //     Anthropic -> Chat 改写并走 /chat/completions，响应按 Chat ->
    //     Anthropic 回写；流式 SSE 也整体转换。
    {
        groups["claude-code"].providers.push_back(models::Provider{
            .id = "chat-member",
            .name = "Chat成员",
            .baseUrl = up1.baseUrl() + "/v1",
            .apiKey = "sk-chat",
            .upstreamFormat = "openai",
            .fullUrl = true,
        });
        groups["claude-code"].providers.push_back(models::Provider{
            .id = "agg-conv",
            .name = "转换聚合",
            .apiFormat = "anthropic",
            .upstreams = {
                models::ProviderUpstream{.providerId = "chat-member",
                                         .model = "chat-model",
                                         .apiFormat = "openai-chat"},
            }});
        groups["claude-code"].current = "agg-conv";
        up1.status = 200;
        up1.contentType = "application/json";
        up1.body =
            R"({"id":"chatcmpl-conv","model":"chat-model","choices":[{"index":0,"message":{"role":"assistant","content":"hello","tool_calls":[{"id":"call_1","type":"function","function":{"name":"read","arguments":"{\"path\":\"x\"}"}}]},"finish_reason":"tool_calls"}],"usage":{"prompt_tokens":7,"completion_tokens":3}})";
        up1.hits = 0;
        router1.clearStats();
        auto res = cli.Post(
            "/claude-code/v1/messages?beta=true",
            R"({"model":"Chat成员/chat-model","max_tokens":100,"system":"sys","messages":[{"role":"user","content":[{"type":"text","text":"hi"}]}]})",
            "application/json");
        CHECK(res && res->status == 200);
        {
            std::lock_guard lk(up1.mu);
            CHECK(up1.hits == 1);
            CHECK(up1.lastPath == "/v1/chat/completions");
            CHECK(up1.lastTarget == "/v1/chat/completions");
            CHECK(up1.lastAuth == "Bearer sk-chat");
            CHECK(up1.lastApiKey.empty());
            const auto body = nlohmann::json::parse(up1.lastBody, nullptr,
                                                    false);
            CHECK(body.is_object());
            CHECK(body.value("model", "") == "chat-model");
            CHECK(!body.contains("system"));
            CHECK(body["messages"].is_array());
            CHECK(body["messages"][0].value("role", "") == "system");
            CHECK(body["messages"][0].value("content", "") == "sys");
            CHECK(body["messages"][1].value("role", "") == "user");
            CHECK(body["messages"][1].value("content", "") == "hi");
        }
        const auto converted =
            nlohmann::json::parse(res->body, nullptr, false);
        CHECK(converted.is_object());
        CHECK(converted.value("type", "") == "message");
        CHECK(converted.value("stop_reason", "") == "tool_use");
        CHECK(converted["content"].size() == 2);
        CHECK(converted["content"][0].value("text", "") == "hello");
        CHECK(converted["content"][1].value("type", "") == "tool_use");
        CHECK(converted["content"][1].value("name", "") == "read");
        const auto snap = router1.snapshot();
        CHECK(snap.totalRequests == 1);
        CHECK(snap.totalPromptTokens == 7);
        CHECK(snap.totalCompletionTokens == 3);
        CHECK(perProviderCount(snap, "转换聚合 · Chat成员") == 1);

        // 流式：Chat SSE -> Anthropic SSE。
        up1.contentType = "text/event-stream";
        up1.body =
            "data: {\"id\":\"chatcmpl-stream\",\"model\":\"chat-model\","
            "\"choices\":[{\"delta\":{\"role\":\"assistant\","
            "\"content\":\"Hel\"}}]}\n\n"
            "data: {\"choices\":[{\"delta\":{\"content\":\"lo\"},"
            "\"finish_reason\":\"stop\"}],\"usage\":"
            "{\"prompt_tokens\":2,\"completion_tokens\":1}}\n\n"
            "data: [DONE]\n\n";
        up1.hits = 0;
        router1.clearStats();
        res = cli.Post(
            "/claude-code/v1/messages",
            R"({"model":"Chat成员/chat-model","max_tokens":100,"stream":true,"messages":[{"role":"user","content":"hi"}]})",
            "application/json");
        CHECK(res && res->status == 200);
        CHECK(res->get_header_value("Content-Type") ==
              "text/event-stream");
        CHECK(res->body.contains("event: message_start"));
        CHECK(res->body.contains("event: content_block_delta"));
        CHECK(res->body.contains("\"text\":\"Hel\""));
        CHECK(res->body.contains("\"text\":\"lo\""));
        CHECK(res->body.contains("\"stop_reason\":\"end_turn\""));
        CHECK(res->body.contains("event: message_stop"));
        {
            std::lock_guard lk(up1.mu);
            CHECK(up1.hits == 1);
            const auto body = nlohmann::json::parse(up1.lastBody, nullptr,
                                                    false);
            CHECK(body.value("stream", false));
            CHECK(body["stream_options"].value("include_usage", false));
        }

        groups["claude-code"].current = "p1";
        auto& claudeProviders = groups["claude-code"].providers;
        std::erase_if(claudeProviders, [](const models::Provider& provider) {
            return provider.id == "agg-conv" || provider.id == "chat-member";
        });
        up1.contentType = "application/json";
        up1.body =
            R"({"ok":true,"usage":{"prompt_tokens":12,"completion_tokens":34}})";
        router1.clearStats();
    }

    // 3g. 反向转换：Chat 目标（Codex）聚合 Anthropic 成员。请求按
    //     Chat -> Anthropic 改写并走 /v1/messages，响应按 Anthropic ->
    //     Chat 回写；流式 SSE 同样转换。
    {
        groups["codex"].providers.push_back(models::Provider{
            .id = "anthropic-member",
            .name = "Anthropic成员",
            .baseUrl = up2.baseUrl() + "/anthropic",
            .apiKey = "sk-anth",
            .upstreamFormat = "anthropic",
            .fullUrl = true,
        });
        groups["codex"].providers.push_back(models::Provider{
            .id = "agg-chat",
            .name = "Chat聚合",
            .upstreams = {
                models::ProviderUpstream{.providerId = "anthropic-member",
                                         .model = "claude-x",
                                         .apiFormat = "anthropic"},
            }});
        groups["codex"].current = "agg-chat";
        up2.status = 200;
        up2.contentType = "application/json";
        up2.body =
            R"({"id":"msg_1","type":"message","role":"assistant","model":"claude-x","content":[{"type":"text","text":"hello"},{"type":"tool_use","id":"toolu_1","name":"read","input":{"path":"z"}}],"stop_reason":"tool_use","usage":{"input_tokens":5,"output_tokens":2}})";
        up2.hits = 0;
        router1.clearStats();
        auto res = cli.Post(
            "/codex/chat/completions",
            R"({"model":"Anthropic成员/claude-x","max_tokens":100,"messages":[{"role":"user","content":"hi"}]})",
            "application/json");
        CHECK(res && res->status == 200);
        {
            std::lock_guard lk(up2.mu);
            CHECK(up2.hits == 1);
            CHECK(up2.lastPath == "/anthropic/v1/messages");
            CHECK(up2.lastAuth == "Bearer sk-anth");
            CHECK(up2.lastApiKey == "sk-anth");
            const auto body = nlohmann::json::parse(up2.lastBody, nullptr,
                                                    false);
            CHECK(body.is_object());
            CHECK(body.value("model", "") == "claude-x");
            CHECK(body.value("max_tokens", 0) == 100);
            CHECK(body["messages"].is_array());
            CHECK(body["messages"][0].value("role", "") == "user");
            CHECK(body["messages"][0]["content"][0].value("text", "") ==
                  "hi");
        }
        const auto converted =
            nlohmann::json::parse(res->body, nullptr, false);
        CHECK(converted.is_object());
        CHECK(converted.value("object", "") == "chat.completion");
        CHECK(converted["choices"].size() == 1);
        CHECK(converted["choices"][0].value("finish_reason", "") ==
              "tool_calls");
        const auto& message = converted["choices"][0]["message"];
        CHECK(message.value("content", "") == "hello");
        CHECK(message["tool_calls"].size() == 1);
        CHECK(message["tool_calls"][0]["function"].value("name", "") ==
              "read");
        const auto arguments =
            nlohmann::json::parse(
                message["tool_calls"][0]["function"].value("arguments", "{}"),
                nullptr, false);
        CHECK(arguments.value("path", "") == "z");
        const auto snap = router1.snapshot();
        CHECK(snap.totalRequests == 1);
        CHECK(snap.totalPromptTokens == 5);
        CHECK(snap.totalCompletionTokens == 2);
        CHECK(perProviderCount(snap, "Chat聚合 · Anthropic成员") == 1);

        // 流式：Anthropic SSE -> Chat SSE。
        up2.contentType = "text/event-stream";
        up2.body =
            "event: message_start\n"
            "data: {\"type\":\"message_start\",\"message\":{\"id\":"
            "\"msg_stream\",\"model\":\"claude-x\",\"usage\":"
            "{\"input_tokens\":2}}}\n\n"
            "event: content_block_start\n"
            "data: {\"type\":\"content_block_start\",\"index\":0,"
            "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
            "event: content_block_delta\n"
            "data: {\"type\":\"content_block_delta\",\"index\":0,"
            "\"delta\":{\"type\":\"text_delta\",\"text\":\"Hel\"}}\n\n"
            "event: content_block_delta\n"
            "data: {\"type\":\"content_block_delta\",\"index\":0,"
            "\"delta\":{\"type\":\"text_delta\",\"text\":\"lo\"}}\n\n"
            "event: message_delta\n"
            "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":"
            "\"end_turn\"},\"usage\":{\"output_tokens\":1}}\n\n"
            "event: message_stop\n"
            "data: {\"type\":\"message_stop\"}\n\n";
        up2.hits = 0;
        router1.clearStats();
        res = cli.Post(
            "/codex/chat/completions",
            R"({"model":"Anthropic成员/claude-x","max_tokens":100,"stream":true,"messages":[{"role":"user","content":"hi"}]})",
            "application/json");
        CHECK(res && res->status == 200);
        CHECK(res->get_header_value("Content-Type") ==
              "text/event-stream");
        CHECK(res->body.contains("\"object\":\"chat.completion.chunk\""));
        CHECK(res->body.contains("\"content\":\"Hel\""));
        CHECK(res->body.contains("\"content\":\"lo\""));
        CHECK(res->body.contains("\"finish_reason\":\"stop\""));
        CHECK(res->body.contains("\"prompt_tokens\":2"));
        CHECK(res->body.contains("\"completion_tokens\":1"));
        CHECK(res->body.contains("data: [DONE]"));
        {
            std::lock_guard lk(up2.mu);
            CHECK(up2.hits == 1);
            CHECK(up2.lastPath == "/anthropic/v1/messages");
        }

        groups["codex"].current = "c1";
        auto& codexProviders = groups["codex"].providers;
        std::erase_if(codexProviders, [](const models::Provider& provider) {
            return provider.id == "agg-chat" ||
                   provider.id == "anthropic-member";
        });
        up2.contentType = "application/json";
        up2.body =
            R"({"ok":true,"usage":{"prompt_tokens":12,"completion_tokens":34}})";
        router1.clearStats();
    }

    // 3h. Chat 目标聚合 Responses 成员：请求走 Chat -> Responses 桥，
    //     响应按 Responses -> Chat 桥回写；上游端点是 /responses。
    {
        groups["codex"].providers.push_back(models::Provider{
            .id = "responses-member",
            .name = "Responses成员",
            .baseUrl = up1.baseUrl() + "/v1",
            .apiKey = "sk-resp",
            .upstreamFormat = "openai",
            .fullUrl = true,
        });
        groups["codex"].providers.push_back(models::Provider{
            .id = "agg-chat-resp",
            .name = "Chat聚合Responses",
            .upstreams = {
                models::ProviderUpstream{.providerId = "responses-member",
                                         .model = "gpt-x",
                                         .apiFormat = "openai-responses"},
            }});
        groups["codex"].current = "agg-chat-resp";
        up1.status = 200;
        up1.contentType = "application/json";
        up1.body =
            R"({"id":"resp_1","object":"response","created_at":1,"model":"gpt-x","status":"completed","output":[{"id":"msg_0","type":"message","status":"completed","role":"assistant","content":[{"type":"output_text","text":"hello"}]}],"usage":{"input_tokens":5,"output_tokens":2,"total_tokens":7}})";
        up1.hits = 0;
        router1.clearStats();
        auto res = cli.Post(
            "/codex/chat/completions",
            R"({"model":"Responses成员/gpt-x","max_tokens":100,"messages":[{"role":"user","content":"hi"}]})",
            "application/json");
        CHECK(res && res->status == 200);
        {
            std::lock_guard lk(up1.mu);
            CHECK(up1.hits == 1);
            CHECK(up1.lastPath == "/v1/responses");
            CHECK(up1.lastAuth == "Bearer sk-resp");
            CHECK(up1.lastApiKey.empty());
            const auto body = nlohmann::json::parse(up1.lastBody, nullptr,
                                                    false);
            CHECK(body.is_object());
            CHECK(body.value("model", "") == "gpt-x");
            CHECK(body["input"].is_array());
        }
        const auto converted =
            nlohmann::json::parse(res->body, nullptr, false);
        CHECK(converted.is_object());
        CHECK(converted["choices"].size() == 1);
        CHECK(converted["choices"][0]["message"].value("content", "") ==
              "hello");
        CHECK(converted["usage"].value("prompt_tokens", 0) == 5);
        CHECK(converted["usage"].value("completion_tokens", 0) == 2);

        groups["codex"].current = "c1";
        auto& codexProviders = groups["codex"].providers;
        std::erase_if(codexProviders, [](const models::Provider& provider) {
            return provider.id == "agg-chat-resp" ||
                   provider.id == "responses-member";
        });
        up1.contentType = "application/json";
        up1.body =
            R"({"ok":true,"usage":{"prompt_tokens":12,"completion_tokens":34}})";
        router1.clearStats();
    }

    // 4. 未知工具 → 404；组无 current → 502（均为中文 JSON 错误体）。
    {
        auto res = cli.Get("/nope/x");
        CHECK(res && res->status == 404);
        CHECK(res->body.contains("error"));
        res = cli.Get("/pi/v1/chat");
        CHECK(res && res->status == 502);
        CHECK(res->body.contains("当前供应商"));
        // 错误响应不记入 stats（聚合测试结尾已 clearStats）。
        CHECK(router1.snapshot().totalRequests == 0);
    }

    // 4b. 非完整 URL 的 OpenAI 上游格式自动追加 /v1。
    {
        auto& pi = groups["pi"].providers.front();
        pi.baseUrl = up1.baseUrl();
        pi.upstreamFormat = "openai";
        pi.fullUrl = false;
        groups["pi"].current = pi.id;
        auto res = cli.Get("/pi/chat");
        CHECK(res && res->status == 200);
        {
            std::lock_guard lk(up1.mu);
            CHECK(up1.lastPath == "/v1/chat");
        }
        groups["pi"].current.clear();
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

    // 5b. 在途计数：通过校验的请求在转发期间 inflight()==1，完成/中止归零；
    //     校验拒绝（未知工具/未启用）不计入。可阻塞会话 + promise 门闩
    //     让断言能观察到「Send 进行中」的中间态。
    {
        class BlockingSession final : public router::UpstreamSession {
        public:
            std::promise<void> entered;
            std::mutex mu;
            std::condition_variable cv;
            bool released = false;
            bool aborted = false;

            router::UpstreamResponse Send(
                const router::UpstreamRequest&) override {
                entered.set_value();
                // 阻塞直至放行或中止（条件变量——AbortInFlight 换掉 promise
                // 成员唤不醒已进入 wait 的 Send，必须共享同一份状态）。
                std::unique_lock lk(mu);
                cv.wait(lk, [this] { return released || aborted; });
                router::UpstreamResponse out;
                if (aborted) out.error = "测试中止";
                return out;
            }
            void AbortInFlight() override {
                {
                    std::lock_guard lk(mu);
                    aborted = true;
                }
                cv.notify_all();
            }
            void Release() {
                {
                    std::lock_guard lk(mu);
                    released = true;
                }
                cv.notify_all();
            }
        };

        models::ProviderGroup group;
        group.providers.push_back(models::Provider{
            .id = "p1", .name = "供应商甲", .baseUrl = "http://127.0.0.1:1"});
        group.current = "p1";
        std::map<std::string, models::ProviderGroup> blockingGroups{
            {"claude-code", group}};
        const auto blockingResolver =
            [&blockingGroups](
                std::string_view tool) -> std::optional<models::ProviderGroup> {
            if (const auto it = blockingGroups.find(std::string(tool));
                it != blockingGroups.end()) {
                return it->second;
            }
            return std::nullopt;
        };

        // 5b-1：正常完成路径。
        {
            auto session = std::make_shared<BlockingSession>();
            router::LocalRouter rt(blockingResolver, session);
            rt.start(0);
            CHECK(rt.inflight() == 0);
            // 校验拒绝不计入：未知工具 404 走不到上游。
            {
                httplib::Client cli("127.0.0.1", rt.port());
                auto res = cli.Get("/no-such-tool/v1/x");
                CHECK(res && res->status == 404);
            }
            CHECK(rt.inflight() == 0);

            auto enteredFuture = session->entered.get_future();
            bool clientOk = false;
            std::thread client([&] {
                httplib::Client cli("127.0.0.1", rt.port());
                auto res = cli.Post("/claude-code/v1/messages", "{}",
                                    "application/json");
                clientOk = static_cast<bool>(res);  // 连接不被拒绝即认为请求完成
            });
            enteredFuture.wait();
            CHECK(rt.inflight() == 1);  // Send 进行中
            session->Release();
            client.join();
            CHECK(clientOk);  // 断言统一在 join 后做，避免跨线程写 g_failures
            CHECK(rt.inflight() == 0);  // 完成归零
            rt.stop();
        }

        // 5b-2：stop() 的 AbortInFlight 中止路径同样归零。
        {
            auto session = std::make_shared<BlockingSession>();
            router::LocalRouter rt(blockingResolver, session);
            rt.start(0);
            auto enteredFuture = session->entered.get_future();
            std::thread client([&] {
                httplib::Client cli("127.0.0.1", rt.port());
                static_cast<void>(cli.Post("/claude-code/v1/messages", "{}",
                                           "application/json"));
            });
            enteredFuture.wait();
            CHECK(rt.inflight() == 1);
            rt.stop();  // AbortInFlight 唤醒 Send → 失败出口 → 计数归零
            client.join();
            CHECK(rt.inflight() == 0);
        }
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
