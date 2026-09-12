// net.cpp — llmswitch.net 实现单元（纯函数：地址推导 + 响应解析）。
module;

#include <cstddef>  // 裸名 size_t（原先由 curl/curl.h 传递引入）

module llmswitch.net;

import std;
import nlohmann.json;

namespace net {
namespace {

// base 末尾的 '/' 先 trim，端点拼接不产双斜杠。
std::string trimTrailingSlash(std::string_view base) {
    std::string s(base);
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

} // namespace

std::string modelListUrl(std::string_view baseUrl,
                         std::string_view upstreamFormat) {
    const std::string base = trimTrailingSlash(baseUrl);
    if (base.empty() || base.ends_with("/models")) return base;
    if (upstreamFormat == "anthropic" && base.ends_with("/v1")) {
        return base + "/models";
    }
    return base + (upstreamFormat == "anthropic" ? "/v1/models" : "/models");
}

std::vector<std::string> modelListUrlCandidates(
    std::string_view baseUrl, std::string_view upstreamFormat) {
    const std::string base = trimTrailingSlash(baseUrl);
    if (base.empty()) return {};

    std::vector<std::string> candidates;
    std::set<std::string> seen;
    const auto add = [&](std::string url) {
        if (!url.empty() && seen.insert(url).second) {
            candidates.push_back(std::move(url));
        }
    };
    const auto addPath = [&](std::string_view path) {
        if (base.ends_with(path)) {
            add(base);
        } else {
            add(base + std::string(path));
        }
    };

    // 首选当前格式；下面的候选处理真实世界中不同网关对 /v1 的差异。
    add(modelListUrl(base, upstreamFormat));
    addPath("/models");
    if (!base.ends_with("/v1") && !base.ends_with("/v1/models")) {
        addPath("/v1/models");
    }

    // Base URL 已带一个协议前缀时，也尝试去掉该前缀后的标准列表端点，
    // 例如 /gateway/v1 → /gateway/models，避免把同一段前缀重复拼接。
    if (base.ends_with("/v1")) {
        const std::string root = base.substr(0, base.size() - 3);
        if (!root.empty()) add(root + "/models");
    }
    if (base.ends_with("/anthropic/v1")) {
        const std::string root = base.substr(0, base.size() - 13);
        if (!root.empty()) add(root + "/v1/models");
    } else if (base.ends_with("/anthropic")) {
        const std::string root = base.substr(0, base.size() - 10);
        if (!root.empty()) add(root + "/v1/models");
    }

    return candidates;
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
