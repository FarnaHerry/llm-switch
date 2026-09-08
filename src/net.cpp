// net.cpp — llmswitch.net 实现单元（curl）。
//
// 每次调用新建 easy handle（拉模型列表频率低，省去连接复用换线程安全）。
// 全程 CURLOPT_NOSIGNAL（多线程必须）；超时覆盖连接（5s）+ 全程（10s）；
// 跟随重定向；HTTPS 正常校验证书（不设任何 INSECURE 开关）。
module;

#include <curl/curl.h>

// 应用版本编译期常量由顶层 CMakeLists 只给 llm-switch 目标定义；测试目标
// （test_net）回落 "dev"。
#ifndef LLMSWITCH_VERSION
#define LLMSWITCH_VERSION "dev"
#endif

module llmswitch.net;

import std;
import nlohmann.json;

namespace net {
namespace {

size_t onBodyWrite(char* ptr, size_t size, size_t nmemb, void* userdata) noexcept {
    try {
        const size_t n = size * nmemb;
        auto* body = static_cast<std::string*>(userdata);
        body->append(ptr, n);
        return n;
    } catch (...) {
        return CURL_WRITEFUNC_ERROR;
    }
}

// base 末尾的 '/' 先 trim，端点拼接不产双斜杠。
std::string trimTrailingSlash(std::string_view base) {
    std::string s(base);
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

// 通用 GET：Bearer 鉴权，anthropicHeaders=true 时补 x-api-key / anthropic-version。
// context 用于拼错误消息（"{}失败：…"）。
std::string httpGet(const std::string& url, std::string_view apiKey,
                    bool anthropicHeaders, std::string_view context) {
    CURL* easy = curl_easy_init();
    if (easy == nullptr) {
        throw std::runtime_error(std::format("{}失败：curl 初始化失败", context));
    }
    struct Guard {
        CURL* h;
        ~Guard() { curl_easy_cleanup(h); }
    } guard{easy};

    struct curl_slist* headers = nullptr;
    struct HeaderGuard {
        curl_slist* l;
        ~HeaderGuard() { curl_slist_free_all(l); }
    } headerGuard{nullptr};

    if (!apiKey.empty()) {
        const std::string bearer = "Authorization: Bearer " + std::string(apiKey);
        headers = curl_slist_append(headers, bearer.c_str());
        if (anthropicHeaders) {
            // 网关两种鉴权都常见，x-api-key 与 Bearer 都给。
            const std::string xkey = "x-api-key: " + std::string(apiKey);
            headers = curl_slist_append(headers, xkey.c_str());
            headers = curl_slist_append(headers,
                                        "anthropic-version: 2023-06-01");
        }
    }
    headerGuard.l = headers;

    std::string body;
    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "https,http");
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &onBodyWrite);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_USERAGENT,
                     "llm-switch/" LLMSWITCH_VERSION);

    const CURLcode rc = curl_easy_perform(easy);
    if (rc != CURLE_OK) {
        throw std::runtime_error(std::format(
            "{}失败：{}（{}）", context, curl_easy_strerror(rc), url));
    }
    long status = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    if (status < 200 || status >= 300) {
        throw std::runtime_error(std::format(
            "{}失败：HTTP {}（{}）—— {}", context, status, url,
            body.substr(0, 200)));
    }
    return body;
}

} // namespace

std::string modelListUrl(std::string_view baseUrl,
                         std::string_view upstreamFormat) {
    const std::string base = trimTrailingSlash(baseUrl);
    return base + (upstreamFormat == "anthropic" ? "/v1/models" : "/models");
}

std::vector<std::string> fetchModels(std::string_view baseUrl,
                                     std::string_view apiKey,
                                     std::string_view upstreamFormat) {
    if (trimTrailingSlash(baseUrl).empty()) {
        throw std::runtime_error("拉取模型列表失败：Base URL 为空");
    }
    return fetchModelsFromUrl(modelListUrl(baseUrl, upstreamFormat), apiKey,
                              upstreamFormat);
}

std::vector<std::string> fetchModelsFromUrl(std::string_view url,
                                            std::string_view apiKey,
                                            std::string_view upstreamFormat) {
    if (trimTrailingSlash(url).empty()) {
        throw std::runtime_error("拉取模型列表失败：模型获取 URL 为空");
    }
    const bool anthropic = upstreamFormat == "anthropic";
    return parseModelIds(httpGet(std::string(url), apiKey, anthropic,
                                 "拉取模型列表"));
}

std::string fetchUsage(std::string_view url, std::string_view apiKey,
                       std::string_view jsonPath) {
    const std::string body =
        httpGet(std::string(url), apiKey, false, "查询用量");
    return extractByPath(body, jsonPath);
}

double pingLatencyMs(std::string_view baseUrl) {
    const std::string url = trimTrailingSlash(baseUrl);
    if (url.empty()) {
        throw std::runtime_error("连通检测失败：Base URL 为空");
    }
    CURL* easy = curl_easy_init();
    if (easy == nullptr) {
        throw std::runtime_error("连通检测失败：curl 初始化失败");
    }
    struct Guard {
        CURL* h;
        ~Guard() { curl_easy_cleanup(h); }
    } guard{easy};

    std::string sink;  // 响应体丢弃，只关心可达性与耗时
    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "https,http");
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &onBodyWrite);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_USERAGENT,
                     "llm-switch/" LLMSWITCH_VERSION);

    const CURLcode rc = curl_easy_perform(easy);
    if (rc != CURLE_OK) {
        throw std::runtime_error(std::format(
            "连通检测失败：{}（{}）", curl_easy_strerror(rc), url));
    }
    // 收到任何 HTTP 状态（含 401/404）都算连通——服务器可达即可。
    double totalSec = 0;
    curl_easy_getinfo(easy, CURLINFO_TOTAL_TIME, &totalSec);
    return totalSec * 1000.0;
}

std::string extractByPath(std::string_view body, std::string_view dottedPath) {
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded()) {
        throw std::runtime_error("用量响应不是有效 JSON");
    }
    const nlohmann::json* cur = &j;
    size_t pos = 0;
    while (pos <= dottedPath.size()) {
        const size_t dot = dottedPath.find('.', pos);
        const std::string_view seg = dottedPath.substr(
            pos, dot == std::string_view::npos ? std::string_view::npos
                                               : dot - pos);
        if (seg.empty()) {
            throw std::runtime_error(
                std::format("用量路径无效：{}", dottedPath));
        }
        if (cur->is_object()) {
            const auto it = cur->find(std::string(seg));
            if (it == cur->end()) {
                throw std::runtime_error(
                    std::format("响应中找不到路径：{}", dottedPath));
            }
            cur = &*it;
        } else if (cur->is_array()) {
            if (!std::ranges::all_of(seg, [](unsigned char c) {
                    return std::isdigit(c) != 0;
                })) {
                throw std::runtime_error(
                    std::format("响应中找不到路径：{}", dottedPath));
            }
            const size_t idx = std::stoull(std::string(seg));
            if (idx >= cur->size()) {
                throw std::runtime_error(
                    std::format("响应中找不到路径：{}", dottedPath));
            }
            cur = &(*cur)[idx];
        } else {
            throw std::runtime_error(
                std::format("响应中找不到路径：{}", dottedPath));
        }
        if (dot == std::string_view::npos) break;
        pos = dot + 1;
    }
    if (cur->is_string()) {
        return cur->get<std::string>();
    }
    if (cur->is_number_integer() || cur->is_number_unsigned()) {
        return std::to_string(cur->get<long long>());
    }
    if (cur->is_number_float()) {
        // "{}" 走最短往返表示：10.0 → "10"，9.9 → "9.9"。
        return std::format("{}", cur->get<double>());
    }
    if (cur->is_boolean()) {
        return cur->get<bool>() ? "true" : "false";
    }
    throw std::runtime_error(
        std::format("路径 {} 指向的不是标量", dottedPath));
}

std::vector<std::string> parseModelIds(std::string_view body) {
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        throw std::runtime_error("模型列表响应不是有效的 JSON 对象");
    }
    const nlohmann::json* list = nullptr;
    if (j.contains("data") && j["data"].is_array()) {
        list = &j["data"];
    } else if (j.contains("models") && j["models"].is_array()) {
        list = &j["models"];
    } else {
        throw std::runtime_error("模型列表响应缺少 data / models 数组");
    }
    std::vector<std::string> ids;
    std::set<std::string> seen;
    for (const auto& item : *list) {
        std::string id;
        if (item.is_string()) {
            id = item.get<std::string>();
        } else if (item.is_object()) {
            if (const auto it = item.find("id");
                it != item.end() && it->is_string()) {
                id = it->get<std::string>();
            }
        }
        if (!id.empty() && seen.insert(id).second) {
            ids.push_back(std::move(id));
        }
    }
    return ids;
}

} // namespace net
