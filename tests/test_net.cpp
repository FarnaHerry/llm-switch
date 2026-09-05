// test_net.cpp — llmswitch.net 测试（无框架：CHECK 失败计数，非零即败）。
// 只测 parseModelIds 纯函数：openai data 形状、models 键形状、字符串元素、
// 空列表、坏 JSON 抛错、缺键抛错、重复 id 去重保序。不测真实网络
// （fetchModels 由 UI 实操验证）。
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

} // namespace

int main() {
    // 1. OpenAI 兼容形状：{"data":[{"id":...}]}
    {
        const auto ids = net::parseModelIds(
            R"json({"object":"list","data":[{"id":"deepseek-chat","object":"model"},{"id":"deepseek-reasoner","object":"model"}]})json");
        CHECK(ids.size() == 2);
        CHECK(ids[0] == "deepseek-chat");
        CHECK(ids[1] == "deepseek-reasoner");
    }

    // 2. anthropic /v1/models 同为 data 形状（含无关字段）
    {
        const auto ids = net::parseModelIds(
            R"json({"data":[{"id":"claude-sonnet-4-6","type":"model","display_name":"Sonnet"}],"has_more":false})json");
        CHECK(ids.size() == 1);
        CHECK(ids[0] == "claude-sonnet-4-6");
    }

    // 3. {"models":[...]} 键形状 + 纯字符串元素
    {
        const auto ids = net::parseModelIds(
            R"json({"models":[{"id":"kimi-k2"},{"id":"k1.5"}]})json");
        CHECK(ids.size() == 2);
        CHECK(ids[0] == "kimi-k2");
        const auto strIds = net::parseModelIds(R"json({"models":["m1","m2"]})json");
        CHECK(strIds.size() == 2);
        CHECK(strIds[1] == "m2");
    }

    // 4. 空列表 → 空结果（不抛错）
    CHECK(net::parseModelIds(R"json({"data":[]})json").empty());

    // 5. 坏 JSON / 非对象 / 缺键 → 抛 std::runtime_error
    CHECK(throwsRuntimeError("这不是 JSON {{{"));
    CHECK(throwsRuntimeError(R"json(["a","b"])json"));
    CHECK(throwsRuntimeError(R"json({"models_map":{}})json"));

    // 6. 重复 id 去重保序；无 id 的元素跳过
    {
        const auto ids = net::parseModelIds(
            R"json({"data":[{"id":"a"},{"id":"b"},{"id":"a"},{"name":"无id"},{"id":"b"},{"id":"c"}]})json");
        CHECK(ids.size() == 3);
        CHECK(ids[0] == "a" && ids[1] == "b" && ids[2] == "c");
    }

    if (g_failures == 0) {
        std::println("test_net: ok");
        return 0;
    }
    std::println(stderr, "test_net: {} 项断言失败", g_failures);
    return 1;
}
