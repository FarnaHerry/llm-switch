// router.cppm — llmswitch.router：本地 HTTP 反向代理 + 请求统计（接口模块）。
//
// 用户把 CLI 的 base URL 指向 http://127.0.0.1:<port>/<tool>/，路由器把请求
// 转发到该工具当前激活供应商的 baseUrl，替换鉴权头，记录统计，可选故障转移。
// 服务器用 cpp-httplib（只在实现单元引用，接口不暴露 httplib 类型），
// 出站转发用 curl（参考 llmswitch.net）。
// 统计持久化为 JSONL（cfg::statsFile()），启动时回填内存环形缓冲。
export module llmswitch.router;

import std;
import llmswitch.models;

namespace router {

// 一次上游尝试的请求记录（故障转移时一次客户端请求可能产生多条）。
// promptTokens/completionTokens 为 -1 表示未提取（非 JSON / SSE 流式 / 无 usage）。
export struct RequestLog {
    std::int64_t tsMillis = 0;  // 毫秒时间戳
    std::string tool;           // 注册表工具 id
    std::string providerId;
    std::string providerName;
    std::string method;
    std::string path;           // 不含 query string
    int status = 0;             // 上游响应状态码；0 = 上游请求失败（curl 错误）
    std::int64_t latencyMs = 0;
    std::int64_t promptTokens = -1;
    std::int64_t completionTokens = -1;
};

// 统计聚合快照（基于内存环形缓冲，最多最近 1000 条）。
export struct StatsSnapshot {
    std::int64_t totalRequests = 0;
    std::int64_t todayRequests = 0;  // 按本地当天 0 点划分
    std::int64_t successCount = 0;   // 2xx
    double avgLatencyMs = 0.0;
    std::int64_t totalPromptTokens = 0;
    std::int64_t totalCompletionTokens = 0;
    std::vector<std::pair<std::string, std::int64_t>> perProvider;  // providerName -> 请求数
};

// 本地路由引擎。线程安全；httplib 服务跑在内部后台线程。
// groupProvider 在每次请求时调用（按工具 id 取 ProviderGroup 快照，含 current），
// 必须线程安全且快速返回（不要在里面做 IO 或持锁等待）。
export class LocalRouter {
public:
    using GroupProvider =
        std::function<std::optional<models::ProviderGroup>(std::string_view toolId)>;

    explicit LocalRouter(GroupProvider gp);
    ~LocalRouter();
    LocalRouter(const LocalRouter&) = delete;
    LocalRouter& operator=(const LocalRouter&) = delete;

    // 后台线程起服务，绑定 127.0.0.1；port=0 让系统分配（用 port() 取实际端口）。
    // 已在运行时重复调用为 no-op；绑定失败抛 std::runtime_error（中文消息）。
    void start(int port = 0);
    void stop();
    bool running() const;
    int port() const;  // 未运行返回 0

    // 故障转移开关（默认关）：上游 429/5xx/连接失败时按组内顺序试下一个供应商。
    void setFailoverEnabled(bool enabled);
    bool failoverEnabled() const;

    // 单工具代理开关，运行中可即时修改。默认所有注册工具启用；未知 id 抛异常。
    // 被禁用工具的请求返回 403，且不会访问 resolver / 上游或写入统计。
    void setToolEnabled(std::string_view toolId, bool enabled);
    bool toolEnabled(std::string_view toolId) const;

    StatsSnapshot snapshot() const;
    // 最近日志，新的在前，最多 limit 条。
    std::vector<RequestLog> recentLogs(std::size_t limit) const;
    // 清内存统计 + 删除 JSONL 日志文件。
    void clearStats();

private:
    struct Impl;  // 实现单元内定义（隐藏 httplib::Server / std::thread）
    std::unique_ptr<Impl> impl_;
};

} // namespace router
