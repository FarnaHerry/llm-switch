// router_transport.cpp — 本地路由的生产上游会话：桥接 HuxerUI 平台 HttpClient。
//
// 路由器的 httplib 工作线程同步调用 Send，而 HttpClient 是 UI 线程 Task 异步
// 模型：每次 Send 用 TaskScope::Post 把请求抛到 UI 线程，Launch 协程任务
// co_await SendAsync（平台原生异步网络：Linux libsoup / Windows WinHTTP /
// macOS NSURLSession，TLS 由平台栈负责），工作线程在条件变量上等结果。
// 等待带截止时间（请求上限 + 10s 富余）；LocalRouter::stop() 触发
// AbortInFlight 快速中止在途请求——保证任何退出路径都不悬挂工作线程。
//
// 注意：本文件没有 composable；BindRouterUpstreamSession 是普通函数（可安全
// 持有 static 守卫），必须在组合期调用（app.cpp 根组合）。
#include <huxerui/huxerui.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <utility>

#include "ui.h"

import llmswitch.router;

namespace llmswitch::ui {
namespace {

using router::UpstreamRequest;
using router::UpstreamResponse;

huxerui::HttpMethod HttpMethodFromString(const std::string& method) {
    if (method == "POST") return huxerui::HttpMethod::Post;
    if (method == "PUT") return huxerui::HttpMethod::Put;
    if (method == "PATCH") return huxerui::HttpMethod::Patch;
    if (method == "DELETE") return huxerui::HttpMethod::Delete;
    if (method == "OPTIONS") return huxerui::HttpMethod::Options;
    if (method == "HEAD") return huxerui::HttpMethod::Head;
    return huxerui::HttpMethod::Get;
}

huxerui::Bytes BodyToBytes(const std::string& body) {
    huxerui::Bytes bytes(body.size());
    if (!body.empty()) {
        std::memcpy(bytes.data(), body.data(), body.size());
    }
    return bytes;
}

std::string BytesToBody(const huxerui::Bytes& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

UpstreamResponse Failed(std::string message) {
    UpstreamResponse out;
    out.error = std::move(message);
    return out;
}

// 一次上游交换在 worker 线程与 UI 线程任务之间的共享状态。
struct PendingExchange {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;     // response 有效
    bool aborted = false;  // 被 AbortInFlight 中止
    UpstreamResponse response;
};

class PlatformUpstreamSession final
    : public router::UpstreamSession,
      public std::enable_shared_from_this<PlatformUpstreamSession> {
public:
    PlatformUpstreamSession(std::shared_ptr<huxerui::HttpClient> http,
                            huxerui::TaskScope tasks)
        : http_(std::move(http)), tasks_(std::move(tasks)) {}

    UpstreamResponse Send(const UpstreamRequest& request) override {
        auto pending = std::make_shared<PendingExchange>();
        {
            std::lock_guard lk(mu_);
            inFlight_.insert(pending);
        }
        // 外部线程 → UI 线程：TaskScope::Post 专为非 Task 来源的回调设计；
        // 捕 shared_from_this 保证换绑/销毁后在途任务仍安全。作用域已关闭时
        // 回调被丢弃，由下方截止时间兜底释放 worker。
        const auto self = shared_from_this();
        tasks_.Post([self, pending, request]() mutable {
            auto factory = [self, pending,
                            request = std::move(request)]() mutable -> huxerui::Task<void> {
                UpstreamResponse response = co_await Perform(self->http_, request);
                self->Finish(pending, std::move(response));
            };
            try {
                self->tasks_.Launch(std::move(factory));
            } catch (const std::exception&) {
                // 作用域关闭导致 Launch 拒绝（应用退出路径）。
                self->Finish(pending, Failed("路由服务的任务作用域已关闭"));
            }
        });

        std::unique_lock lk(pending->mu);
        const bool settled = pending->cv.wait_until(
            lk,
            std::chrono::steady_clock::now() + request.timeout +
                std::chrono::seconds{10},
            [&] { return pending->done || pending->aborted; });
        if (pending->aborted) {
            return Failed("本地路由已停止，上游请求中止");
        }
        if (!settled) {
            Forget(pending);
            return Failed("上游请求未在截止时间内完成");
        }
        return std::move(pending->response);
    }

    void AbortInFlight() override {
        // 只中止当时在途的交换，不清空会话：stop 后重新 start 继续可用。
        decltype(inFlight_) snapshot;
        {
            std::lock_guard lk(mu_);
            snapshot.swap(inFlight_);
        }
        for (const auto& pending : snapshot) {
            {
                std::lock_guard lk(pending->mu);
                pending->aborted = true;
            }
            pending->cv.notify_all();
        }
    }

private:
    // 实际出站（UI 线程协程内执行）。
    static huxerui::Task<UpstreamResponse> Perform(
        const std::shared_ptr<huxerui::HttpClient>& http,
        const UpstreamRequest& request) {
        UpstreamResponse out;
        huxerui::HttpRequest hr;
        hr.url = request.url;
        hr.method = HttpMethodFromString(request.method);
        hr.headers.reserve(request.headers.size());
        for (const auto& [name, value] : request.headers) {
            hr.headers.push_back({name, value});
        }
        if (!request.body.empty()) {
            hr.body = BodyToBytes(request.body);
        }
        hr.timeout = request.timeout;
        auto result = co_await http->SendAsync(std::move(hr));
        if (!result.Succeeded()) {
            out.error = result.Error().message;
            co_return out;
        }
        auto response = std::move(result).Value();
        out.status = response.status_code;
        out.body = BytesToBody(response.body);
        out.headers.reserve(response.headers.size());
        for (const auto& header : response.headers) {
            out.headers.emplace_back(header.name, header.value);
        }
        co_return out;
    }

    void Finish(const std::shared_ptr<PendingExchange>& pending,
                UpstreamResponse response) {
        {
            std::lock_guard lk(mu_);
            inFlight_.erase(pending);
        }
        {
            std::lock_guard lk(pending->mu);
            pending->response = std::move(response);
            pending->done = true;
        }
        pending->cv.notify_all();
    }

    void Forget(const std::shared_ptr<PendingExchange>& pending) {
        std::lock_guard lk(mu_);
        inFlight_.erase(pending);
    }

    std::shared_ptr<huxerui::HttpClient> http_;
    huxerui::TaskScope tasks_;
    std::mutex mu_;
    std::set<std::shared_ptr<PendingExchange>> inFlight_;
};

} // namespace

void BindRouterUpstreamSession(std::shared_ptr<huxerui::HttpClient> http,
                               huxerui::TaskScope tasks) {
    // 进程内一次：把组合期捕获的平台 HttpClient 与 TaskScope 绑定到路由单例。
    // 本函数是普通函数（非 composable），static 守卫安全；UseService/
    // UseTaskScope 由调用点（app.cpp 根组合）求值后传入，且该调用点必须
    // 早于一切托盘/页面/事件对 routerInstance() 的调用。
    static const bool bound = [&] {
        routerInstance().setUpstreamSession(
            std::make_shared<PlatformUpstreamSession>(std::move(http),
                                                      std::move(tasks)));
        return true;
    }();
    (void)bound;
}

} // namespace llmswitch::ui
