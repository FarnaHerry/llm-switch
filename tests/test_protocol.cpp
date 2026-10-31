// test_protocol.cpp — llmswitch.protocol 纯函数测试（无网络/无 UI）。
//
// 覆盖第一刀：Anthropic Messages 客户端 -> OpenAI Chat 成员的请求映射，
// 以及 Chat 非流式/SSE 响应 -> Anthropic Messages 的响应映射。
#include <cstdio>

import std;
import nlohmann.json;
import llmswitch.protocol;

namespace {

int g_failures = 0;

#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            std::println(stderr, "FAIL {}: {}", __LINE__, #cond); \
            ++g_failures;                                        \
        }                                                        \
    } while (0)

using nlohmann::json;

json Parse(const std::string& text) {
    return json::parse(text, nullptr, false);
}

void TestAnthropicRequestToChat() {
    const std::string input = R"({
        "model":"claude-sonnet-4",
        "max_tokens":1024,
        "temperature":0.2,
        "top_p":0.9,
        "stop_sequences":["STOP"],
        "system":[{"type":"text","text":"sys"}],
        "messages":[
            {"role":"user","content":[{"type":"text","text":"hi"}]},
            {"role":"assistant","content":[
                {"type":"text","text":"call"},
                {"type":"tool_use","id":"t1","name":"read",
                 "input":{"path":"a"}}
            ]},
            {"role":"user","content":[
                {"type":"tool_result","tool_use_id":"t1","content":"ok"},
                {"type":"text","text":"next"}
            ]}
        ],
        "tools":[{"name":"read","description":"read file",
                  "input_schema":{"type":"object","properties":{}}}],
        "tool_choice":{"type":"auto"},
        "stream":true
    })";
    std::string error;
    auto converted = protocol::convertRequest(
        protocol::ApiFormat::anthropic, protocol::ApiFormat::openai_chat,
        input, error);
    CHECK(converted.has_value());
    CHECK(error.empty());
    if (!converted) return;
    const auto out = Parse(*converted);
    CHECK(out.is_object());
    CHECK(out.value("model", "") == "claude-sonnet-4");
    CHECK(out.value("max_tokens", 0) == 1024);
    CHECK(out.value("stream", false));
    CHECK(out["stream_options"].value("include_usage", false));
    CHECK(!out.contains("system"));
    CHECK(out["stop"].is_array());
    CHECK(out["stop"].front() == "STOP");

    const auto& messages = out["messages"];
    CHECK(messages.is_array());
    CHECK(messages.size() == 5);
    CHECK(messages[0].value("role", "") == "system");
    CHECK(messages[0].value("content", "") == "sys");
    CHECK(messages[1].value("role", "") == "user");
    CHECK(messages[1].value("content", "") == "hi");
    CHECK(messages[2].value("role", "") == "assistant");
    CHECK(messages[2].value("content", "") == "call");
    CHECK(messages[2]["tool_calls"].size() == 1);
    CHECK(messages[2]["tool_calls"][0].value("id", "") == "t1");
    CHECK(messages[2]["tool_calls"][0]["function"].value("name", "") ==
          "read");
    const auto arguments = Parse(
        messages[2]["tool_calls"][0]["function"].value("arguments", "{}"));
    CHECK(arguments.value("path", "") == "a");
    CHECK(messages[3].value("role", "") == "tool");
    CHECK(messages[3].value("tool_call_id", "") == "t1");
    CHECK(messages[3].value("content", "") == "ok");
    CHECK(messages[4].value("role", "") == "user");
    CHECK(messages[4].value("content", "") == "next");

    CHECK(out["tools"].size() == 1);
    CHECK(out["tools"][0].value("type", "") == "function");
    CHECK(out["tools"][0]["function"].value("name", "") == "read");
    CHECK(out["tools"][0]["function"]["parameters"].is_object());
    CHECK(out["tool_choice"] == "auto");
}

void TestChatResponseToAnthropic() {
    const std::string input = R"({
        "id":"chatcmpl-1",
        "model":"chat-model",
        "choices":[{
            "index":0,
            "message":{
                "role":"assistant",
                "content":"hello",
                "tool_calls":[{
                    "id":"call_1",
                    "type":"function",
                    "function":{"name":"read","arguments":"{\"path\":\"b\"}"}
                }]
            },
            "finish_reason":"tool_calls"
        }],
        "usage":{"prompt_tokens":10,"completion_tokens":4}
    })";
    std::string error;
    auto converted = protocol::convertResponse(
        protocol::ApiFormat::openai_chat, protocol::ApiFormat::anthropic,
        false, input, error);
    CHECK(converted.has_value());
    CHECK(error.empty());
    if (!converted) return;
    const auto out = Parse(*converted);
    CHECK(out.value("type", "") == "message");
    CHECK(out.value("role", "") == "assistant");
    CHECK(out.value("stop_reason", "") == "tool_use");
    CHECK(out["content"].size() == 2);
    CHECK(out["content"][0].value("type", "") == "text");
    CHECK(out["content"][0].value("text", "") == "hello");
    CHECK(out["content"][1].value("type", "") == "tool_use");
    CHECK(out["content"][1].value("name", "") == "read");
    CHECK(out["content"][1]["input"].value("path", "") == "b");
    CHECK(out["usage"].value("input_tokens", 0) == 10);
    CHECK(out["usage"].value("output_tokens", 0) == 4);
}

void TestChatSseToAnthropic() {
    const std::string input =
        "data: {\"id\":\"chatcmpl-x\",\"model\":\"m\",\"choices\":"
        "[{\"delta\":{\"role\":\"assistant\",\"content\":\"Hel\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_1\",\"type\":\"function\",\"function\":"
        "{\"name\":\"read\",\"arguments\":\"{\\\"path\\\":\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"function\":{\"arguments\":\"\\\"b\\\"}\"}}]},"
        "\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":10,"
        "\"completion_tokens\":4}}\n\n"
        "data: [DONE]\n\n";
    std::string error;
    auto converted = protocol::convertResponse(
        protocol::ApiFormat::openai_chat, protocol::ApiFormat::anthropic,
        true, input, error);
    CHECK(converted.has_value());
    CHECK(error.empty());
    if (!converted) return;
    const auto& out = *converted;
    CHECK(out.contains("event: message_start"));
    CHECK(out.contains("event: content_block_start"));
    CHECK(out.contains("\"text\":\"Hel\""));
    CHECK(out.contains("\"text\":\"lo\""));
    CHECK(out.contains("\"type\":\"tool_use\""));
    CHECK(out.contains("\"name\":\"read\""));
    CHECK(out.contains("\"partial_json\":\"{\\\"path\\\":\""));
    CHECK(out.contains("\"partial_json\":\"\\\"b\\\"}\""));
    CHECK(out.contains("\"stop_reason\":\"tool_use\""));
    CHECK(out.contains("\"output_tokens\":4"));
    CHECK(out.contains("event: message_stop"));
}

void TestUnsupportedDirection() {
    std::string error;
    auto converted = protocol::convertRequest(
        protocol::ApiFormat::openai_chat, protocol::ApiFormat::anthropic,
        "{}", error);
    CHECK(!converted.has_value());
    CHECK(!error.empty());
}

} // namespace

int main() {
    TestAnthropicRequestToChat();
    TestChatResponseToAnthropic();
    TestChatSseToAnthropic();
    TestUnsupportedDirection();
    if (g_failures != 0) {
        std::println(stderr, "{} protocol check(s) failed", g_failures);
        return 1;
    }
    std::println("protocol tests passed");
    return 0;
}
