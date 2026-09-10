// test_net.cpp — llmswitch.net 测试（无框架：CHECK 失败计数，非零即败）。
// 只测模型列表地址和响应解析纯函数：openai/anthropic 端点、data 形状、models
// 键形状、字符串元素、空列表、坏 JSON 抛错、缺键抛错、重复 id 去重保序。
// 不测真实网络（fetchModels 由 UI 实操验证）。
#include <cstdio>  // stderr（std 模块不导出 stdout/stderr 宏）

import std;
import llmswitch.net;

namespace {

int g_failures = 0;

#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            std::println(stderr, "FAIL {}: {}", __LINE__, #cond); \
            ++g_failures;                                        \
        }                                                        \
    } while (0)

bool throwsRuntimeError(std::string_view body) {
    try {
        (void)net::parseModelIds(body);
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

bool rejectsEmptyModelFetchUrl() {
    try {
        (void)net::fetchModelsFromUrl("", "", "openai");
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

} // namespace

int main() {
    // 1. 模型列表端点识别已存在的前缀/列表路径，不重复拼接。
    CHECK(net::modelListUrl("https://api.example.com/v1", "openai") ==
          "https://api.example.com/v1/models");
    CHECK(net::modelListUrl("https://api.example.com/anthropic", "anthropic") ==
          "https://api.example.com/anthropic/v1/models");
    CHECK(net::modelListUrl("https://api.example.com/v1/models", "openai") ==
          "https://api.example.com/v1/models");
    CHECK(net::modelListUrl("https://api.example.com/anthropic/v1", "anthropic") ==
          "https://api.example.com/anthropic/v1/models");
    {
        const auto candidates = net::modelListUrlCandidates(
            "https://api.example.com/gateway/v1", "openai");
        CHECK(!candidates.empty());
        CHECK(candidates.front() ==
              "https://api.example.com/gateway/v1/models");
        CHECK(std::ranges::find(candidates,
                                "https://api.example.com/gateway/models") !=
              candidates.end());
        for (const auto& candidate : candidates) {
            CHECK(candidate.find("/v1/v1/") == std::string::npos);
        }
    }
    CHECK(rejectsEmptyModelFetchUrl());

    // 2. OpenAI 兼容形状：{"data":[{"id":...}]}
    {
        const auto ids = net::parseModelIds(
            R"json({"object":"list","data":[{"id":"deepseek-chat","object":"model"},{"id":"deepseek-reasoner","object":"model"}]})json");
        CHECK(ids.size() == 2);
        CHECK(ids[0] == "deepseek-chat");
        CHECK(ids[1] == "deepseek-reasoner");
    }

    // 3. anthropic /v1/models 同为 data 形状（含无关字段）
    {
        const auto ids = net::parseModelIds(
            R"json({"data":[{"id":"claude-sonnet-4-6","type":"model","display_name":"Sonnet"}],"has_more":false})json");
        CHECK(ids.size() == 1);
        CHECK(ids[0] == "claude-sonnet-4-6");
    }

    // 4. {"models":[...]} 键形状 + 纯字符串元素
    {
        const auto ids = net::parseModelIds(
            R"json({"models":[{"id":"kimi-k2"},{"id":"k1.5"}]})json");
        CHECK(ids.size() == 2);
        CHECK(ids[0] == "kimi-k2");
        const auto strIds = net::parseModelIds(R"json({"models":["m1","m2"]})json");
        CHECK(strIds.size() == 2);
        CHECK(strIds[1] == "m2");
    }

    // 5. 空列表 → 空结果（不抛错）
    CHECK(net::parseModelIds(R"json({"data":[]})json").empty());

    // 6. 坏 JSON / 非对象 / 缺键 → 抛 std::runtime_error
    CHECK(throwsRuntimeError("这不是 JSON {{{"));
    CHECK(throwsRuntimeError(R"json(["a","b"])json"));
    CHECK(throwsRuntimeError(R"json({"models_map":{}})json"));

    // 7. 重复 id 去重保序；无 id 的元素跳过
    {
        const auto ids = net::parseModelIds(
            R"json({"data":[{"id":"a"},{"id":"b"},{"id":"a"},{"name":"无id"},{"id":"b"},{"id":"c"}]})json");
        CHECK(ids.size() == 3);
        CHECK(ids[0] == "a" && ids[1] == "b" && ids[2] == "c");
    }

    // 8. extractByPath：嵌套对象
    CHECK(net::extractByPath(R"json({"a":{"b":{"c":"deep"}}})json", "a.b.c") ==
          "deep");

    // 9. extractByPath：数组下标 + 字符串值原样返回（DeepSeek balance 形状）
    CHECK(net::extractByPath(
              R"json({"balance_infos":[{"total_balance":"9.90","currency":"CNY"}]})json",
              "balance_infos.0.total_balance") == "9.90");

    // 10. extractByPath：数字 / 布尔标量
    CHECK(net::extractByPath(R"json({"a":{"n":10}})json", "a.n") == "10");
    CHECK(net::extractByPath(R"json({"a":{"n":9.9}})json", "a.n") == "9.9");
    CHECK(net::extractByPath(R"json({"a":{"n":10.0}})json", "a.n") == "10");
    CHECK(net::extractByPath(R"json({"a":{"ok":true}})json", "a.ok") == "true");

    // 11. extractByPath：错误情形（缺键 / 越界 / 坏 JSON / 非标量终值）
    {
        const auto throws = [](std::string_view body, std::string_view path) {
            try {
                (void)net::extractByPath(body, path);
            } catch (const std::runtime_error&) {
                return true;
            }
            return false;
        };
        CHECK(throws(R"json({"a":{}})json", "a.missing"));
        CHECK(throws(R"json({"a":[1,2]})json", "a.5"));
        CHECK(throws(R"json({"a":[1,2]})json", "a.x"));  // 数组段非数字
        CHECK(throws("这不是 JSON {{{", "a.b"));
        CHECK(throws(R"json({"a":{"b":{"c":1}}})json", "a.b"));  // 终值是对象
        CHECK(throws(R"json({"a":[1]})json", "a"));  // 终值是数组
        CHECK(throws(R"json({"a":null})json", "a"));  // 终值是 null
    }

    if (g_failures == 0) {
        std::println("test_net: ok");
        return 0;
    }
    std::println(stderr, "test_net: {} 项断言失败", g_failures);
    return 1;
}
