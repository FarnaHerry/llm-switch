// router.cpp — llmswitch.router 实现单元（httplib 服务端 + UpstreamSession 出站）。
//
// 结构：LocalRouter::Impl 持有全部状态（httplib::Server / 后台线程 / 统计），
// LocalRouter 方法只做转发。服务端监听走 bind_to_port/bind_to_any_port +
// listen_after_bind（bind 阶段已完成 listen()，客户端连接进 backlog，无 accept
// 竞态）。出站转发交给注入的 UpstreamSession（生产=UI 层 HttpClient 平台桥接，
// 测试=httplib::Client），转发侧只负责 URL 拼接、鉴权注入与响应整形。
// 统计：内存环形缓冲（cap 1000，mutex 保护）+ JSONL 追加落盘
// （cfg::statsFile()；超过 5000 行重写截断保留最近 2500 行，防无限膨胀），
// 构造时从 JSONL 回填最近 1000 条。
// nlohmann::json 模块下禁用 .items() 结构化绑定，遍历用 it.key()/it.value()。
module;

#include <httplib.h>
#include <ctime>  // localtime_r / localtime_s（当天 0 点划分）

module llmswitch.router;

import std;
import nlohmann.json;
import llmswitch.config;
import llmswitch.models;

namespace router {
namespace {

// ---- 常量 --------------------------------------------------------------------

// 内存环形缓冲上限 / JSONL 压缩阈值（超过重写保留最近一半）。
constexpr std::size_t kMaxLogsInMemory = 1000;
constexpr std::int64_t kMaxJsonlLines = 5000;
constexpr std::int64_t kJsonlLinesAfterCompact = 2500;

// 转发时剥离的请求头（鉴权与 hop-by-hop；其余透传）。
const std::set<std::string, std::less<>> kStripRequestHeaders = {
    "authorization", "x-api-key", "host", "content-length",
};

// 回写客户端时剥离的响应头（hop-by-hop；Content-Length 由 httplib 按 body 重设，
// Transfer-Encoding: chunked 必须剥掉 —— 我们以原始 body 直接回）。
const std::set<std::string, std::less<>> kStripResponseHeaders = {
    "connection", "keep-alive", "proxy-authenticate", "proxy-authorization",
    "te",           "trailer",   "transfer-encoding", "upgrade",
    "content-length",
};

// ---- 小工具 ------------------------------------------------------------------

std::int64_t nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 本地当天 0 点的毫秒时间戳（snapshot 的 today 划分）。
std::int64_t todayStartMillis() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    ::localtime_s(&tm, &t);  // MSVC 安全版（参数对调）
#else
    ::localtime_r(&t, &tm);
#endif
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    return static_cast<std::int64_t>(std::mktime(&tm)) * 1000;
}

std::string toLower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) {
        c = static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string trimTrailingSlash(std::string_view base) {
    std::string s(base);
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

// 上游结果整形：从会话返回的头里摘出 content-type（usage 提取需要）。
UpstreamResponse ShapeUpstreamResponse(UpstreamResponse response) {
    std::vector<std::pair<std::string, std::string>> kept;
    kept.reserve(response.headers.size());
    for (auto& [name, value] : response.headers) {
        if (toLower(name) == "content-type") {
            response.contentType = std::move(value);
        } else {
            kept.emplace_back(std::move(name), std::move(value));
        }
    }
    response.headers = std::move(kept);
    return response;
}

// 出站转发：鉴权替换 + 其余头透传。anthropic 协议（apiFormat=="anthropic"
// 或 claude-code/claude 工具）注入 x-api-key + Authorization 双头，
// 其余只注入 Authorization: Bearer。
UpstreamResponse forwardToProvider(UpstreamSession& session,
                                   const models::Provider& provider,
                                   bool anthropic,
                                   const httplib::Request& req,
                                   const std::string& url) {
    UpstreamRequest request;
    request.method = req.method;
    request.url = url;
    // 透传非剥离头。
    for (const auto& [name, value] : req.headers) {
        if (kStripRequestHeaders.contains(toLower(name))) continue;
        request.headers.emplace_back(name, value);
    }
    // 按供应商协议注入鉴权。
    if (!provider.apiKey.empty()) {
        request.headers.emplace_back("Authorization",
                                     std::format("Bearer {}", provider.apiKey));
        if (anthropic) {
            // 网关两种鉴权都常见，x-api-key 与 Bearer 都给。
            request.headers.emplace_back("x-api-key", provider.apiKey);
        }
    }
    if (req.method == "POST" || req.method == "PUT" || req.method == "PATCH") {
        request.body = req.body;
    }
    return ShapeUpstreamResponse(session.Send(request));
}

// 从 JSON 响应体提取 token 用量（SSE 流式与非 JSON 不解析，保持 -1）。
// 兼容 OpenAI（prompt_tokens/completion_tokens）与 Anthropic
// （input_tokens/output_tokens）两种 usage 形状。
void extractUsage(const UpstreamResponse& up, RequestLog& log) {
    if (!toLower(up.contentType).contains("json")) return;
    const auto j = nlohmann::json::parse(up.body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;
    const auto uit = j.find("usage");
    if (uit == j.end() || !uit->is_object()) return;
    const auto pick = [&uit](std::string_view a, std::string_view b) {
        for (const std::string key : {std::string(a), std::string(b)}) {
            if (const auto it = uit->find(key);
                it != uit->end() && it->is_number()) {
                return it->get<std::int64_t>();
            }
        }
        return std::int64_t{-1};
    };
    log.promptTokens = pick("prompt_tokens", "input_tokens");
    log.completionTokens = pick("completion_tokens", "output_tokens");
}

nlohmann::json logToJson(const RequestLog& log) {
    nlohmann::json j;
    j["ts"] = log.tsMillis;
    j["tool"] = log.tool;
    j["providerId"] = log.providerId;
    j["providerName"] = log.providerName;
    j["method"] = log.method;
    j["path"] = log.path;
    j["status"] = log.status;
    j["latencyMs"] = log.latencyMs;
    j["promptTokens"] = log.promptTokens;
    j["completionTokens"] = log.completionTokens;
    return j;
}

RequestLog logFromJson(const nlohmann::json& j) {
    RequestLog log;
    if (!j.is_object()) return log;
    log.tsMillis = j.value("ts", std::int64_t{0});
    log.tool = j.value("tool", "");
    log.providerId = j.value("providerId", "");
    log.providerName = j.value("providerName", "");
    log.method = j.value("method", "");
    log.path = j.value("path", "");
    log.status = j.value("status", 0);
    log.latencyMs = j.value("latencyMs", std::int64_t{0});
    log.promptTokens = j.value("promptTokens", std::int64_t{-1});
    log.completionTokens = j.value("completionTokens", std::int64_t{-1});
    return log;
}

} // namespace

// ---- LocalRouter::Impl --------------------------------------------------------

struct LocalRouter::Impl {
    GroupProvider groupProvider;
    std::shared_ptr<UpstreamSession> session;
    mutable std::mutex sessionMu;  // 保护 session 的换绑与读取
    httplib::Server server;
    std::thread thread;
    std::atomic<bool> isRunning{false};
    std::atomic<int> boundPort{0};
    std::atomic<bool> failover{false};

    mutable std::mutex settingsMu;
    std::unordered_set<std::string> enabledTools;
    mutable std::mutex mu;
    std::deque<RequestLog> logs;      // 环形缓冲（旧 → 新）
    std::int64_t jsonlLines = 0;      // statsFile 当前行数（回填时初始化）

    Impl(GroupProvider gp, std::shared_ptr<UpstreamSession> upstream)
        : groupProvider(std::move(gp)), session(std::move(upstream)) {
        for (const auto& spec : models::toolRegistry()) {
            enabledTools.emplace(spec.id);
        }
        loadFromDisk();
        // 任意方法任意路径都进同一个处理器（catch-all 正则；不能用
        // set_pre_routing_handler —— 它在 read_content 之前触发，拿不到 body）。
        const auto handler =
            [this](const httplib::Request& req, httplib::Response& res) {
                handle(req, res);
            };
        server.Get(R"(/(.*))", handler);
        server.Post(R"(/(.*))", handler);
        server.Put(R"(/(.*))", handler);
        server.Patch(R"(/(.*))", handler);
        server.Delete(R"(/(.*))", handler);
        server.Options(R"(/(.*))", handler);
        // HEAD 无需注册：httplib 内部以 GET 处理器处理并自动剥掉响应体。
    }

    ~Impl() { stop(); }

    void start(int port) {
        if (isRunning.load()) return;  // 重复调用安全
        int bound = -1;
        if (port <= 0) {
            bound = server.bind_to_any_port("127.0.0.1");
        } else if (server.bind_to_port("127.0.0.1", port)) {
            bound = port;
        }
        if (bound <= 0) {
            throw std::runtime_error(
                std::format("路由服务绑定失败：127.0.0.1:{}", port));
        }
        boundPort = bound;
        isRunning = true;
        thread = std::thread([this] {
            server.listen_after_bind();
            isRunning = false;
            boundPort = 0;
        });
    }

    void stop() {
        // 先中止在途上游请求（防止 httplib 停止时 join 卡在长转发上），再停服务。
        std::shared_ptr<UpstreamSession> upstream;
        {
            std::lock_guard lk(sessionMu);
            upstream = session;
        }
        if (upstream) upstream->AbortInFlight();
        server.stop();
        if (thread.joinable()) thread.join();
        isRunning = false;
        boundPort = 0;
    }

    // ---- 请求处理 ------------------------------------------------------------

    void jsonError(httplib::Response& res, int status, const std::string& message) {
        nlohmann::json j;
        j["error"] = message;
        res.status = status;
        res.set_content(j.dump(), "application/json");
    }

    void handle(const httplib::Request& req, httplib::Response& res) {
        // 路由解析：/{tool}/{rest}（query string 保留原样转发）。
        std::string_view target(req.target);
        std::string_view query;
        if (const auto q = target.find('?'); q != std::string_view::npos) {
            query = target.substr(q + 1);
            target = target.substr(0, q);
        }
        std::string_view rest;
        std::string tool;
        {
            std::string_view p = target;
            if (p.starts_with('/')) p.remove_prefix(1);
            if (const auto slash = p.find('/'); slash != std::string_view::npos) {
                tool = std::string(p.substr(0, slash));
                rest = p.substr(slash + 1);
            } else {
                tool = std::string(p);
            }
        }
        if (models::findTool(tool) == nullptr) {
            jsonError(res, 404, std::format("未知工具：{}", tool));
            return;
        }
        {
            std::lock_guard lk(settingsMu);
            if (!enabledTools.contains(tool)) {
                jsonError(res, 403,
                          std::format("工具 {} 的本地路由未启用", tool));
                return;
            }
        }
        std::shared_ptr<UpstreamSession> upstream;
        {
            std::lock_guard lk(sessionMu);
            upstream = session;
        }
        if (!upstream) {
            // 未绑定会话（尚未进入组合期）：直接拒绝，不访问 resolver / 上游。
            jsonError(res, 502, "路由服务的上游传输尚未就绪");
            return;
        }
        const auto group = groupProvider(tool);
        if (!group.has_value()) {
            jsonError(res, 404, std::format("工具 {} 没有配置供应商", tool));
            return;
        }
        std::size_t curIdx = group->providers.size();
        for (std::size_t i = 0; i < group->providers.size(); ++i) {
            if (group->providers[i].id == group->current) {
                curIdx = i;
                break;
            }
        }
        if (curIdx == group->providers.size()) {
            jsonError(res, 502,
                      std::format("工具 {} 未设置当前供应商", tool));
            return;
        }

        // 尝试顺序：当前供应商优先；故障转移开时追加组内其余供应商
        // （每个只试一次，最多试全组）。
        std::vector<std::size_t> order{curIdx};
        if (failover.load()) {
            for (std::size_t i = 0; i < group->providers.size(); ++i) {
                if (i != curIdx) order.push_back(i);
            }
        }
        UpstreamResponse last;
        for (std::size_t k = 0; k < order.size(); ++k) {
            const auto& provider = group->providers[order[k]];
            std::string url =
                trimTrailingSlash(models::effectiveBaseUrl(provider)) + "/" +
                std::string(rest);
            if (!query.empty()) url += "?" + std::string(query);

            const auto t0 = std::chrono::steady_clock::now();
            const bool anthropic =
                tool == "claude-code" || tool == "claude" ||
                models::normalizeApiFormat(provider.apiFormat) == "anthropic";
            last = forwardToProvider(*upstream, provider, anthropic, req, url);
            const auto latency =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0)
                    .count();

            RequestLog log{.tsMillis = nowMillis(),
                           .tool = tool,
                           .providerId = provider.id,
                           .providerName = provider.name,
                           .method = req.method,
                           .path = std::string(target),
                           .status = static_cast<int>(last.status),
                           .latencyMs = latency};
            extractUsage(last, log);
            record(std::move(log));

            const bool retryable =
                last.status == 429 || last.status >= 500 || last.status == 0;
            if (k + 1 >= order.size() || !retryable) break;
        }

        if (last.status == 0) {
            jsonError(res, 502,
                      std::format("上游请求失败：{}", last.error));
            return;
        }
        // 原样透传状态码 + 头 + 体（剥 hop-by-hop 头）。
        res.status = static_cast<int>(last.status);
        for (const auto& [name, value] : last.headers) {
            if (kStripResponseHeaders.contains(toLower(name))) continue;
            res.headers.emplace(name, value);
        }
        res.set_content(last.body, last.contentType.empty()
                                       ? "application/octet-stream"
                                       : last.contentType.c_str());
    }

    // ---- 统计 ----------------------------------------------------------------

    // 记录一条日志：内存环形缓冲 + JSONL 追加（调用方不持锁）。
    void record(RequestLog log) {
        std::lock_guard lk(mu);
        logs.push_back(std::move(log));
        while (logs.size() > kMaxLogsInMemory) logs.pop_front();
        appendJsonl(logs.back());
    }

    void appendJsonl(const RequestLog& log) {
        const auto file = cfg::statsFile();  // 确保父目录存在
        {
            std::ofstream out(file, std::ios::binary | std::ios::app);
            if (!out) return;  // 落盘失败不拖垮转发
            out << logToJson(log).dump() << '\n';
        }
        ++jsonlLines;
        if (jsonlLines > kMaxJsonlLines) compactJsonl(file);
    }

    // 防无限膨胀：超过阈值时重写文件，只保留最近 kJsonlLinesAfterCompact 行。
    void compactJsonl(const std::filesystem::path& file) {
        std::ifstream in(file, std::ios::binary);
        if (!in) return;
        std::deque<std::string> lines;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty()) lines.push_back(std::move(line));
            while (static_cast<std::int64_t>(lines.size()) >
                   kJsonlLinesAfterCompact) {
                lines.pop_front();
            }
        }
        const auto tmp = file.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) return;
            for (const auto& l : lines) out << l << '\n';
        }
        std::error_code ec;
        std::filesystem::rename(tmp, file, ec);
        if (ec) {
            ec.clear();
            std::filesystem::remove(file, ec);
            ec.clear();
            std::filesystem::rename(tmp, file, ec);
        }
        jsonlLines = static_cast<std::int64_t>(lines.size());
    }

    // 启动时从 JSONL 回填内存（最多最近 1000 条）。
    void loadFromDisk() {
        const auto file = cfg::statsFile();
        std::ifstream in(file, std::ios::binary);
        if (!in) return;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            ++jsonlLines;
            const auto j = nlohmann::json::parse(line, nullptr, false);
            if (j.is_discarded()) continue;  // 坏行跳过
            logs.push_back(logFromJson(j));
            while (logs.size() > kMaxLogsInMemory) logs.pop_front();
        }
    }

    StatsSnapshot snapshot() const {
        std::lock_guard lk(mu);
        StatsSnapshot s;
        const std::int64_t todayStart = todayStartMillis();
        std::int64_t latencySum = 0;
        std::map<std::string, std::int64_t> per;
        for (const auto& log : logs) {
            ++s.totalRequests;
            if (log.tsMillis >= todayStart) ++s.todayRequests;
            if (log.status >= 200 && log.status < 300) ++s.successCount;
            latencySum += log.latencyMs;
            if (log.promptTokens > 0) s.totalPromptTokens += log.promptTokens;
            if (log.completionTokens > 0) {
                s.totalCompletionTokens += log.completionTokens;
            }
            ++per[log.providerName.empty() ? log.providerId
                                           : log.providerName];
        }
        if (!logs.empty()) {
            s.avgLatencyMs =
                static_cast<double>(latencySum) / static_cast<double>(logs.size());
        }
        s.perProvider.assign(per.begin(), per.end());
        return s;
    }

    std::vector<RequestLog> recentLogs(std::size_t limit) const {
        std::lock_guard lk(mu);
        const auto n = std::min(limit, logs.size());
        std::vector<RequestLog> out;
        out.reserve(n);
        for (auto it = logs.rbegin(); it != logs.rbegin() + static_cast<std::ptrdiff_t>(n); ++it) {
            out.push_back(*it);
        }
        return out;
    }

    void clearStats() {
        std::lock_guard lk(mu);
        logs.clear();
        jsonlLines = 0;
        std::error_code ec;
        std::filesystem::remove(cfg::statsFile(), ec);
    }
};

// ---- LocalRouter 转发 ---------------------------------------------------------

LocalRouter::LocalRouter(GroupProvider gp, std::shared_ptr<UpstreamSession> session)
    : impl_(std::make_unique<Impl>(std::move(gp), std::move(session))) {}

LocalRouter::~LocalRouter() = default;

void LocalRouter::setUpstreamSession(std::shared_ptr<UpstreamSession> session) {
    std::lock_guard lk(impl_->sessionMu);
    impl_->session = std::move(session);
}

void LocalRouter::start(int port) { impl_->start(port); }
void LocalRouter::stop() { impl_->stop(); }
bool LocalRouter::running() const { return impl_->isRunning.load(); }
int LocalRouter::port() const { return impl_->boundPort.load(); }
void LocalRouter::setFailoverEnabled(bool enabled) { impl_->failover = enabled; }
bool LocalRouter::failoverEnabled() const { return impl_->failover.load(); }
void LocalRouter::setToolEnabled(std::string_view toolId, bool enabled) {
    if (models::findTool(toolId) == nullptr) {
        throw std::runtime_error(std::format("未知工具：{}", toolId));
    }
    std::lock_guard lk(impl_->settingsMu);
    if (enabled) {
        impl_->enabledTools.emplace(toolId);
    } else {
        impl_->enabledTools.erase(std::string(toolId));
    }
}
bool LocalRouter::toolEnabled(std::string_view toolId) const {
    if (models::findTool(toolId) == nullptr) return false;
    std::lock_guard lk(impl_->settingsMu);
    return impl_->enabledTools.contains(std::string(toolId));
}
StatsSnapshot LocalRouter::snapshot() const { return impl_->snapshot(); }
std::vector<RequestLog> LocalRouter::recentLogs(std::size_t limit) const {
    return impl_->recentLogs(limit);
}
void LocalRouter::clearStats() { impl_->clearStats(); }

} // namespace router
