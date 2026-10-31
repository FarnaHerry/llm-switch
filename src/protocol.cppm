// protocol.cppm — Agent 协议与上游协议之间的转换（纯函数，无网络/无 UI）。
//
// 第一刀先覆盖最常见的一对方向：
//   Agent 侧 Anthropic Messages（claude-code / claude desktop）
//   <-> 成员侧 OpenAI Chat Completions。
// 路由把客户端原始请求按「目标协议 -> 成员协议」转换，再把成员响应按
// 「成员协议 -> 目标协议」转换；同协议直通，不经过这里。
//
// 这里只做协议映射，不处理鉴权头、URL 前缀或统计；那些留在 router。
// 流式响应按整段 SSE 解析后重新编码（当前上游会话本身也是整段返回），
// 因此不需要增量状态机与跨 chunk 缓冲。
export module llmswitch.protocol;

import std;
import nlohmann.json;

namespace protocol {

export enum class ApiFormat {
    openai_chat,
    openai_responses,
    anthropic,
};

export ApiFormat parseApiFormat(std::string_view format) {
    if (format == "anthropic") return ApiFormat::anthropic;
    if (format == "openai-responses") return ApiFormat::openai_responses;
    return ApiFormat::openai_chat;
}

export std::string_view apiFormatName(ApiFormat format) {
    switch (format) {
    case ApiFormat::anthropic: return "anthropic";
    case ApiFormat::openai_responses: return "openai-responses";
    case ApiFormat::openai_chat: break;
    }
    return "openai-chat";
}

export std::string_view apiFormatLabel(ApiFormat format) {
    switch (format) {
    case ApiFormat::anthropic: return "Anthropic Messages";
    case ApiFormat::openai_responses: return "OpenAI Responses";
    case ApiFormat::openai_chat: break;
    }
    return "OpenAI Chat";
}

// 当前实现支持的方向：Anthropic 客户端 -> OpenAI Chat 成员。
// 反向（Chat 客户端 -> Anthropic 成员）留给下一刀；未支持时路由返回
// 502 并继续尝试聚合里的其他成员。
export bool supportsConversion(ApiFormat from, ApiFormat to) {
    return from == to ||
           (from == ApiFormat::anthropic && to == ApiFormat::openai_chat);
}

// 转换方向 from -> to 时，上游需要请求的路径（相对 effectiveBaseUrl）。
export std::string_view convertedRequestPath(ApiFormat from, ApiFormat to) {
    if (from == to) return {};
    if (from == ApiFormat::anthropic && to == ApiFormat::openai_chat) {
        return "chat/completions";
    }
    return {};
}

namespace {

using Json = nlohmann::json;

Json parseOrDiscard(std::string_view body) {
    return Json::parse(std::string(body), nullptr, false);
}

std::string textOf(const Json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (!value.is_array()) return {};
    std::string text;
    for (const auto& part : value) {
        if (part.is_string()) {
            text += part.get<std::string>();
        } else if (part.is_object()) {
            if (const auto type = part.value("type", "");
                type == "text" || type == "input_text") {
                text += part.value("text", "");
            }
        }
    }
    return text;
}

Json anthropicImageToChat(const Json& block) {
    const auto source = block.find("source");
    if (source == block.end() || !source->is_object()) return Json();
    const auto type = source->value("type", "");
    if (type == "base64") {
        const std::string media = source->value("media_type", "image/png");
        const std::string data = source->value("data", "");
        if (data.empty()) return Json();
        Json part;
        part["type"] = "image_url";
        part["image_url"]["url"] =
            "data:" + media + ";base64," + data;
        return part;
    }
    if (type == "url") {
        const std::string url = source->value("url", "");
        if (url.empty()) return Json();
        Json part;
        part["type"] = "image_url";
        part["image_url"]["url"] = url;
        return part;
    }
    return Json();
}

std::string toolResultText(const Json& content) {
    if (content.is_string()) return content.get<std::string>();
    if (!content.is_array()) return {};
    std::string text;
    for (const auto& block : content) {
        if (block.is_string()) {
            text += block.get<std::string>();
            continue;
        }
        if (!block.is_object()) continue;
        const auto type = block.value("type", "");
        if (type == "text") {
            text += block.value("text", "");
        } else if (type == "image") {
            text += "[image]";
        }
    }
    return text;
}

// 把 Anthropic 的 content blocks 拍成一条 Chat user 消息的内容。文本优先用
// 字符串，只有图片时才用 parts 数组，兼容不支持 content array 的网关。
Json chatUserContent(const std::vector<Json>& parts) {
    bool hasImage = false;
    std::string text;
    for (const auto& part : parts) {
        if (part.is_string()) {
            text += part.get<std::string>();
        } else if (part.is_object()) {
            if (part.value("type", "") == "text") {
                text += part.value("text", "");
            } else {
                hasImage = true;
            }
        }
    }
    if (!hasImage) return text;
    Json content = Json::array();
    if (!text.empty()) {
        Json block;
        block["type"] = "text";
        block["text"] = text;
        content.push_back(std::move(block));
    }
    for (const auto& part : parts) {
        if (part.is_object() && part.value("type", "") != "text") {
            content.push_back(part);
        }
    }
    return content;
}

Json anthropicToolChoiceToChat(const Json& choice) {
    if (!choice.is_object()) return Json();
    const auto type = choice.value("type", "");
    if (type == "auto") return "auto";
    if (type == "any") return "required";
    if (type == "none") return "none";
    if (type == "tool") {
        Json out;
        out["type"] = "function";
        out["function"]["name"] = choice.value("name", "");
        return out;
    }
    return Json();
}

Json chatRequestFromAnthropic(const Json& in) {
    Json out = Json::object();
    if (in.contains("model")) out["model"] = in["model"];

    std::string system = textOf(in.find("system") != in.end()
                                    ? in["system"]
                                    : Json());
    Json messages = Json::array();
    const auto sourceMessages = in.find("messages");
    if (sourceMessages != in.end() && sourceMessages->is_array()) {
        for (const auto& message : *sourceMessages) {
            if (!message.is_object()) continue;
            const std::string role = message.value("role", "");
            if (role == "system" || role == "developer") {
                const std::string extra = textOf(
                    message.find("content") != message.end()
                        ? message["content"]
                        : Json());
                if (!extra.empty()) {
                    if (!system.empty()) system += "\n\n";
                    system += extra;
                }
                continue;
            }
            if (role == "user") {
                const auto contentIt = message.find("content");
                if (contentIt == message.end()) continue;
                if (contentIt->is_string()) {
                    Json row;
                    row["role"] = "user";
                    row["content"] = *contentIt;
                    messages.push_back(std::move(row));
                    continue;
                }
                if (!contentIt->is_array()) continue;
                std::vector<Json> parts;
                const auto flushUser = [&] {
                    if (parts.empty()) return;
                    Json row;
                    row["role"] = "user";
                    row["content"] = chatUserContent(parts);
                    messages.push_back(std::move(row));
                    parts.clear();
                };
                for (const auto& block : *contentIt) {
                    if (!block.is_object()) continue;
                    const auto type = block.value("type", "");
                    if (type == "text") {
                        Json part;
                        part["type"] = "text";
                        part["text"] = block.value("text", "");
                        parts.push_back(std::move(part));
                    } else if (type == "image") {
                        Json part = anthropicImageToChat(block);
                        if (!part.is_null()) parts.push_back(std::move(part));
                    } else if (type == "tool_result") {
                        flushUser();
                        Json row;
                        row["role"] = "tool";
                        row["tool_call_id"] =
                            block.value("tool_use_id", "");
                        row["content"] =
                            toolResultText(block.find("content") != block.end()
                                               ? block["content"]
                                               : Json());
                        messages.push_back(std::move(row));
                    }
                }
                flushUser();
                continue;
            }
            if (role != "assistant") continue;
            const auto contentIt = message.find("content");
            std::string text;
            Json toolCalls = Json::array();
            if (contentIt != message.end()) {
                if (contentIt->is_string()) {
                    text = contentIt->get<std::string>();
                } else if (contentIt->is_array()) {
                    for (const auto& block : *contentIt) {
                        if (!block.is_object()) continue;
                        const auto type = block.value("type", "");
                        if (type == "text") {
                            text += block.value("text", "");
                        } else if (type == "tool_use") {
                            Json call;
                            call["id"] = block.value("id", "");
                            call["type"] = "function";
                            call["function"]["name"] =
                                block.value("name", "");
                            const auto input = block.find("input");
                            call["function"]["arguments"] =
                                input != block.end() && input->is_object()
                                    ? input->dump()
                                    : "{}";
                            toolCalls.push_back(std::move(call));
                        }
                    }
                }
            }
            Json row;
            row["role"] = "assistant";
            if (!text.empty()) row["content"] = text;
            if (!toolCalls.empty()) row["tool_calls"] = std::move(toolCalls);
            if (text.empty() && !row.contains("tool_calls")) row["content"] = "";
            messages.push_back(std::move(row));
        }
    }
    if (!system.empty()) {
        Json row;
        row["role"] = "system";
        row["content"] = std::move(system);
        messages.insert(messages.begin(), std::move(row));
    }
    if (!messages.empty()) out["messages"] = std::move(messages);

    const auto toolsIt = in.find("tools");
    if (toolsIt != in.end() && toolsIt->is_array() && !toolsIt->empty()) {
        Json tools = Json::array();
        for (const auto& tool : *toolsIt) {
            if (!tool.is_object()) continue;
            Json entry;
            entry["type"] = "function";
            entry["function"]["name"] = tool.value("name", "");
            if (tool.contains("description")) {
                entry["function"]["description"] = tool["description"];
            }
            entry["function"]["parameters"] =
                tool.contains("input_schema") && tool["input_schema"].is_object()
                    ? tool["input_schema"]
                    : Json{{"type", "object"}, {"properties", Json::object()}};
            tools.push_back(std::move(entry));
        }
        if (!tools.empty()) out["tools"] = std::move(tools);
    }

    if (const auto choice = in.find("tool_choice");
        choice != in.end()) {
        const Json mapped = anthropicToolChoiceToChat(*choice);
        if (!mapped.is_null()) out["tool_choice"] = mapped;
    }

    const auto copyNumber = [&](std::string_view key) {
        if (const auto it = in.find(key); it != in.end() && it->is_number()) {
            out[key] = *it;
        }
    };
    copyNumber("max_tokens");
    copyNumber("temperature");
    copyNumber("top_p");
    if (const auto it = in.find("stop_sequences");
        it != in.end() && it->is_array() && !it->empty()) {
        out["stop"] = *it;
    }
    if (const auto meta = in.find("metadata");
        meta != in.end() && meta->is_object()) {
        if (const auto user = meta->find("user_id");
            user != meta->end() && user->is_string()) {
            out["user"] = *user;
        }
    }
    const bool stream = in.value("stream", false);
    if (stream) {
        out["stream"] = true;
        out["stream_options"]["include_usage"] = true;
    }
    return out;
}

Json anthropicUsageFromChat(const Json& usage) {
    Json out = Json::object();
    if (!usage.is_object()) {
        out["input_tokens"] = 0;
        out["output_tokens"] = 0;
        return out;
    }
    out["input_tokens"] = usage.value("prompt_tokens", 0);
    out["output_tokens"] = usage.value("completion_tokens", 0);
    if (const auto details = usage.find("prompt_tokens_details");
        details != usage.end() && details->is_object()) {
        const auto cached = details->value("cached_tokens", 0);
        if (cached > 0) out["cache_read_input_tokens"] = cached;
    }
    return out;
}

std::string anthropicStopReason(std::string_view finishReason) {
    if (finishReason == "tool_calls" || finishReason == "function_call") {
        return "tool_use";
    }
    if (finishReason == "length") return "max_tokens";
    return "end_turn";
}

std::string anthropicMessageId(const Json& in) {
    const std::string raw = in.value("id", "");
    if (raw.empty()) return "msg_llmswitch";
    if (raw.starts_with("msg_")) return raw;
    return "msg_" + raw;
}

Json anthropicToolInput(const Json& call) {
    const auto function = call.find("function");
    if (function == call.end() || !function->is_object()) return Json::object();
    const auto args = function->find("arguments");
    if (args == function->end()) return Json::object();
    if (args->is_object()) return *args;
    if (!args->is_string()) return Json::object();
    const auto parsed = Json::parse(args->get<std::string>(), nullptr, false);
    return parsed.is_object() ? parsed : Json::object();
}

Json chatMessageToAnthropic(const Json& in, const Json& message,
                            std::string_view stopReason) {
    Json content = Json::array();
    const auto text = message.find("content") != message.end()
                          ? textOf(message["content"])
                          : std::string{};
    if (!text.empty()) {
        Json block;
        block["type"] = "text";
        block["text"] = text;
        content.push_back(std::move(block));
    }
    if (const auto calls = message.find("tool_calls");
        calls != message.end() && calls->is_array()) {
        for (const auto& call : *calls) {
            if (!call.is_object()) continue;
            Json block;
            block["type"] = "tool_use";
            std::string id = call.value("id", "");
            if (id.empty()) {
                id = "toolu_" + std::to_string(content.size());
            }
            block["id"] = std::move(id);
            const auto function = call.find("function");
            block["name"] = function != call.end() && function->is_object()
                                ? function->value("name", "")
                                : "";
            block["input"] = anthropicToolInput(call);
            content.push_back(std::move(block));
        }
    }
    if (content.empty()) {
        Json block;
        block["type"] = "text";
        block["text"] = "";
        content.push_back(std::move(block));
    }

    const auto usageIt = in.find("usage");
    Json out;
    out["id"] = anthropicMessageId(in);
    out["type"] = "message";
    out["role"] = "assistant";
    out["model"] = in.value("model", message.value("model", ""));
    out["content"] = std::move(content);
    out["stop_reason"] = std::string(stopReason);
    out["stop_sequence"] = nullptr;
    out["usage"] = anthropicUsageFromChat(
        usageIt != in.end() ? *usageIt : Json());
    return out;
}

Json chatResponseToAnthropic(const Json& in) {
    const auto choices = in.find("choices");
    if (choices == in.end() || !choices->is_array() || choices->empty()) {
        return Json();
    }
    const auto& choice = choices->front();
    if (!choice.is_object()) return Json();
    const auto message = choice.find("message");
    if (message == choice.end() || !message->is_object()) return Json();
    return chatMessageToAnthropic(
        in, *message,
        anthropicStopReason(choice.value("finish_reason", "")));
}

struct SseEvent {
    std::string event;
    std::string data;
};

std::vector<SseEvent> parseSse(std::string_view body) {
    std::vector<SseEvent> events;
    SseEvent current;
    std::size_t pos = 0;
    const auto flush = [&] {
        if (!current.data.empty() || !current.event.empty()) {
            events.push_back(std::move(current));
            current = SseEvent{};
        }
    };
    while (pos <= body.size()) {
        const auto end = body.find('\n', pos);
        std::string_view line =
            end == std::string_view::npos ? body.substr(pos)
                                          : body.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) {
            flush();
        } else if (line.starts_with("data:")) {
            line.remove_prefix(5);
            if (!line.empty() && line.front() == ' ') line.remove_prefix(1);
            if (!current.data.empty()) current.data += '\n';
            current.data += line;
        } else if (line.starts_with("event:")) {
            line.remove_prefix(6);
            if (!line.empty() && line.front() == ' ') line.remove_prefix(1);
            current.event = std::string(line);
        }
        if (end == std::string_view::npos) break;
        pos = end + 1;
    }
    flush();
    return events;
}

void emitSse(std::string& out, std::string_view event, const Json& data) {
    out += "event: ";
    out += event;
    out += "\ndata: ";
    out += data.dump();
    out += "\n\n";
}

struct ToolBlock {
    int chatIndex = 0;
    std::size_t index = 0;
    std::string id;
    std::string name;
    std::string args;
    bool started = false;
};

class ChatSseToAnthropic {
public:
    std::string run(std::string_view body) {
        const auto events = parseSse(body);
        for (const auto& event : events) {
            if (event.data == "[DONE]") break;
            const auto chunk = parseOrDiscard(event.data);
            if (chunk.is_discarded() || !chunk.is_object()) continue;
            consume(chunk);
        }
        finish();
        return std::move(out_);
    }

private:
    std::string out_;
    std::string id_ = "msg_llmswitch";
    std::string model_;
    Json usage_ = Json::object();
    bool started_ = false;
    bool textOpen_ = false;
    std::size_t nextIndex_ = 0;
    std::size_t textIndex_ = 0;
    std::vector<ToolBlock> tools_;
    std::string finishReason_;

    void ensureStarted() {
        if (started_) return;
        started_ = true;
        Json message;
        message["id"] = id_;
        message["type"] = "message";
        message["role"] = "assistant";
        message["model"] = model_;
        message["content"] = Json::array();
        message["stop_reason"] = nullptr;
        message["stop_sequence"] = nullptr;
        message["usage"] = Json::object();
        if (usage_.is_object()) {
            message["usage"]["input_tokens"] =
                usage_.value("prompt_tokens", 0);
            message["usage"]["output_tokens"] = 0;
        } else {
            message["usage"]["input_tokens"] = 0;
            message["usage"]["output_tokens"] = 0;
        }
        Json data;
        data["type"] = "message_start";
        data["message"] = std::move(message);
        emitSse(out_, "message_start", data);
    }

    void closeText() {
        if (!textOpen_) return;
        Json data;
        data["type"] = "content_block_stop";
        data["index"] = textIndex_;
        emitSse(out_, "content_block_stop", data);
        textOpen_ = false;
    }

    void closeTools() {
        for (auto& tool : tools_) {
            if (!tool.started) continue;
            Json data;
            data["type"] = "content_block_stop";
            data["index"] = tool.index;
            emitSse(out_, "content_block_stop", data);
            tool.started = false;
        }
    }

    void startText() {
        if (textOpen_) return;
        closeTools();
        ensureStarted();
        textIndex_ = nextIndex_++;
        Json block;
        block["type"] = "text";
        block["text"] = "";
        Json data;
        data["type"] = "content_block_start";
        data["index"] = textIndex_;
        data["content_block"] = std::move(block);
        emitSse(out_, "content_block_start", data);
        textOpen_ = true;
    }

    void emitText(std::string_view text) {
        if (text.empty()) return;
        startText();
        Json delta;
        delta["type"] = "text_delta";
        delta["text"] = std::string(text);
        Json data;
        data["type"] = "content_block_delta";
        data["index"] = textIndex_;
        data["delta"] = std::move(delta);
        emitSse(out_, "content_block_delta", data);
    }

    ToolBlock& toolBlock(int chatIndex) {
        for (auto& tool : tools_) {
            if (tool.chatIndex == chatIndex) return tool;
        }
        tools_.push_back(ToolBlock{.chatIndex = chatIndex});
        return tools_.back();
    }

    void startTool(ToolBlock& tool) {
        if (tool.started) return;
        closeText();
        ensureStarted();
        tool.index = nextIndex_++;
        tool.started = true;
        if (tool.id.empty()) {
            tool.id = "toolu_" + std::to_string(tool.index);
        }
        if (tool.name.empty()) tool.name = "tool";
        Json block;
        block["type"] = "tool_use";
        block["id"] = tool.id;
        block["name"] = tool.name;
        block["input"] = Json::object();
        Json data;
        data["type"] = "content_block_start";
        data["index"] = tool.index;
        data["content_block"] = std::move(block);
        emitSse(out_, "content_block_start", data);
        if (!tool.args.empty()) emitToolArgs(tool, tool.args);
    }

    void emitToolArgs(ToolBlock& tool, std::string_view args) {
        if (args.empty()) return;
        Json delta;
        delta["type"] = "input_json_delta";
        delta["partial_json"] = std::string(args);
        Json data;
        data["type"] = "content_block_delta";
        data["index"] = tool.index;
        data["delta"] = std::move(delta);
        emitSse(out_, "content_block_delta", data);
    }

    void consume(const Json& chunk) {
        if (const auto id = chunk.find("id");
            id != chunk.end() && id->is_string()) {
            id_ = anthropicMessageId(chunk);
        }
        if (const auto model = chunk.find("model");
            model != chunk.end() && model->is_string()) {
            model_ = model->get<std::string>();
        }
        if (const auto usage = chunk.find("usage");
            usage != chunk.end() && usage->is_object()) {
            usage_ = *usage;
        }
        if (const auto error = chunk.find("error");
            error != chunk.end()) {
            std::string message = "上游返回错误";
            if (error->is_object()) {
                message = error->value("message", message);
            } else if (error->is_string()) {
                message = error->get<std::string>();
            }
            Json payload;
            payload["type"] = "error";
            payload["error"]["type"] = "api_error";
            payload["error"]["message"] = std::move(message);
            emitSse(out_, "error", payload);
            return;
        }
        const auto choices = chunk.find("choices");
        if (choices == chunk.end() || !choices->is_array() || choices->empty()) {
            return;
        }
        const auto& choice = choices->front();
        if (!choice.is_object()) return;
        if (const auto reason = choice.find("finish_reason");
            reason != choice.end() && reason->is_string()) {
            finishReason_ = reason->get<std::string>();
        }
        const auto delta = choice.find("delta");
        if (delta == choice.end() || !delta->is_object()) return;

        if (const auto content = delta->find("content");
            content != delta->end() && !content->is_null()) {
            emitText(textOf(*content));
        }
        if (const auto calls = delta->find("tool_calls");
            calls != delta->end() && calls->is_array()) {
            for (const auto& call : *calls) {
                if (!call.is_object()) continue;
                const int chatIndex = call.value("index", 0);
                ToolBlock& tool = toolBlock(chatIndex);
                if (const auto id = call.find("id");
                    id != call.end() && id->is_string()) {
                    tool.id = id->get<std::string>();
                }
                if (const auto function = call.find("function");
                    function != call.end() && function->is_object()) {
                    if (const auto name = function->find("name");
                        name != function->end() && name->is_string()) {
                        tool.name = name->get<std::string>();
                    }
                    if (const auto args = function->find("arguments");
                        args != function->end() && args->is_string()) {
                        const std::string piece = args->get<std::string>();
                        tool.args += piece;
                        if (tool.started) emitToolArgs(tool, piece);
                    }
                }
                if (!tool.started && !tool.name.empty()) startTool(tool);
            }
        }
    }

    void finish() {
        ensureStarted();
        closeText();
        for (auto& tool : tools_) {
            if (!tool.started) startTool(tool);
        }
        closeTools();
        Json delta;
        delta["stop_reason"] =
            anthropicStopReason(finishReason_);
        delta["stop_sequence"] = nullptr;
        Json usage;
        usage["output_tokens"] = usage_.is_object()
                                     ? usage_.value("completion_tokens", 0)
                                     : 0;
        Json data;
        data["type"] = "message_delta";
        data["delta"] = std::move(delta);
        data["usage"] = std::move(usage);
        emitSse(out_, "message_delta", data);
        Json stop;
        stop["type"] = "message_stop";
        emitSse(out_, "message_stop", stop);
    }
};

} // namespace

export std::optional<std::string> convertRequest(
    ApiFormat from, ApiFormat to, std::string_view body, std::string& error) {
    error.clear();
    if (from == to) return std::string(body);
    if (from != ApiFormat::anthropic || to != ApiFormat::openai_chat) {
        error = "暂不支持 " + std::string(apiFormatName(from)) + " -> " +
                std::string(apiFormatName(to)) + " 的请求转换";
        return std::nullopt;
    }
    const Json in = parseOrDiscard(body);
    if (in.is_discarded() || !in.is_object()) {
        error = "请求体不是 JSON 对象，无法转换为 OpenAI Chat";
        return std::nullopt;
    }
    return chatRequestFromAnthropic(in).dump();
}

export std::optional<std::string> convertResponse(
    ApiFormat from, ApiFormat to, bool stream, std::string_view body,
    std::string& error) {
    error.clear();
    if (from == to) return std::string(body);
    if (from != ApiFormat::openai_chat || to != ApiFormat::anthropic) {
        error = "暂不支持 " + std::string(apiFormatName(from)) + " -> " +
                std::string(apiFormatName(to)) + " 的响应转换";
        return std::nullopt;
    }
    if (stream || body.find("data:") != std::string_view::npos) {
        ChatSseToAnthropic converter;
        return converter.run(body);
    }
    const Json in = parseOrDiscard(body);
    if (in.is_discarded() || !in.is_object()) {
        error = "成员响应不是 JSON 对象，无法转换为 Anthropic Messages";
        return std::nullopt;
    }
    const Json out = chatResponseToAnthropic(in);
    if (out.is_null()) {
        error = "成员响应缺少 choices/message，无法转换为 Anthropic Messages";
        return std::nullopt;
    }
    return out.dump();
}

} // namespace protocol
