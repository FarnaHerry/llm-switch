// provider_network.cpp — 供应商页的网络请求与异步缓存辅助.
#include <huxerui/huxerui.h>

#include <chrono>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "providers_internal.h"

import llmswitch.config;
import llmswitch.models;
import llmswitch.net;
import llmswitch.store;

namespace llmswitch::ui::provider_detail {
std::string HttpBodyText(const huxerui::Bytes& body) {
    if (body.empty()) return {};
    return std::string(reinterpret_cast<const char*>(body.data()), body.size());
}

std::vector<huxerui::HttpHeader> ApiHeaders(std::string_view apiKey,
                                            bool anthropic) {
    std::vector<huxerui::HttpHeader> headers;
    if (apiKey.empty()) return headers;
    headers.push_back({"Authorization", "Bearer " + std::string(apiKey)});
    if (anthropic) {
        // 兼容 Anthropic 官方接口和同时接受 Bearer 的第三方网关。
        headers.push_back({"x-api-key", std::string(apiKey)});
        headers.push_back({"anthropic-version", "2023-06-01"});
    }
    return headers;
}

// HuxerUI HttpClient 在 Windows 走 WinHTTP/系统证书库，在 Linux/macOS 走
// 平台原生 HTTP 栈。响应体只在模型列表和用量查询中缓冲；连通检测使用流式
// 接口，只等待响应头，避免把供应商根 URL 返回的大页面读入内存。
huxerui::Task<std::string> FetchHttpText(
    std::shared_ptr<huxerui::HttpClient> http, std::string url,
    std::vector<huxerui::HttpHeader> headers, std::string_view context) {
    if (!http) {
        throw std::runtime_error(std::format("{}失败：HTTP 服务不可用", context));
    }
    const std::string requestUrl = url;
    auto result = co_await http->SendAsync(
        huxerui::HttpRequest{.url = std::move(url),
                             .headers = std::move(headers),
                             .timeout = std::chrono::seconds{10}});
    if (!result.Succeeded()) {
        throw std::runtime_error(std::format(
            "{}失败：{}", context, result.Error().message));
    }
    auto response = std::move(result).Value();
    if (response.status_code < 200 || response.status_code >= 300) {
        const std::string body = HttpBodyText(response.body);
        throw std::runtime_error(std::format(
            "{}失败：HTTP {}（{}）—— {}", context, response.status_code,
            response.url.empty() ? requestUrl : response.url,
            body.substr(0, 200)));
    }
    co_return HttpBodyText(response.body);
}

huxerui::Task<std::vector<std::string>> FetchModelIds(
    std::shared_ptr<huxerui::HttpClient> http, std::string url,
    std::string apiKey, std::string upstreamFormat) {
    const std::string body = co_await FetchHttpText(
        std::move(http), std::move(url),
        ApiHeaders(apiKey, upstreamFormat == "anthropic"), "拉取模型列表");
    co_return net::parseModelIds(body);
}

// 不同兼容网关的模型列表端点并不统一：同一 Base URL 可能使用 /models、
// /v1/models，或在 /anthropic 前缀后再挂 /v1/models。按候选顺序尝试，
// 只有拿到非空模型列表才结束；这样 404、网络错误、坏响应和空列表都会
// 自动进入下一个候选地址。
huxerui::Task<std::vector<std::string>> FetchModelIdsWithFallback(
    std::shared_ptr<huxerui::HttpClient> http,
    std::vector<std::string> urls,
    std::string apiKey,
    std::string upstreamFormat) {
    std::string lastError = "模型列表为空";
    for (const auto& url : urls) {
        try {
            auto models = co_await FetchModelIds(
                http, url, apiKey, upstreamFormat);
            if (!models.empty()) co_return models;
            lastError = std::format("模型列表为空（{}）", url);
        } catch (const std::exception& e) {
            lastError = e.what();
        }
    }
    throw std::runtime_error(
        std::format("{}；已尝试多个模型列表端点", lastError));
}

// 拉单个供应商的用量并格式化成展示文本；本函数不写 State。
huxerui::Task<std::string> FetchUsageText(
    std::shared_ptr<huxerui::HttpClient> http, models::Provider p) {
    try {
        const std::string body = co_await FetchHttpText(
            std::move(http), p.usageUrl, ApiHeaders(p.apiKey, false), "查询用量");
        const std::string value = net::extractByPath(body, p.usagePath);
        co_return p.usageLabel.empty() ? value
                                       : std::format("{} {}", value, p.usageLabel);
    } catch (const std::exception& e) {
        co_return std::format("查询失败：{}", e.what());
    }
}

huxerui::Task<double> FetchLatency(std::shared_ptr<huxerui::HttpClient> http,
                                   std::string url) {
    if (!http) {
        throw std::runtime_error("HTTP 服务不可用");
    }
    const auto started = std::chrono::steady_clock::now();
    auto result = co_await http->SendStreamAsync(
        huxerui::HttpRequest{.url = std::move(url),
                             .timeout = std::chrono::seconds{10}});
    if (!result.Succeeded()) {
        throw std::runtime_error(result.Error().message);
    }
    auto response = std::move(result).Value();
    // 任何 HTTP 响应（包括 401/404/500）都说明服务器已连通。
    static_cast<void>(response.StatusCode());
    const auto elapsed = std::chrono::steady_clock::now() - started;
    co_return std::chrono::duration<double, std::milli>(elapsed).count();
}

void WriteUsageCache(UsageCache cache, const std::string& id, std::string text) {
    auto m = cache.Get();
    m[id] = std::move(text);
    cache = std::move(m);
}

} // namespace llmswitch::ui::provider_detail
