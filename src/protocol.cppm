// protocol.cppm — Agent 协议与上游协议之间的转换（纯函数，无网络/无 UI）。
//
// 覆盖第一对方向：
//   Agent 侧 Anthropic Messages（claude-code / claude desktop）
//   <-> 成员侧 OpenAI Chat Completions，双向转换。
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

// 当前实现支持的方向：Anthropic Messages <-> OpenAI Chat Completions。
// OpenAI Responses 仍是直通档，暂不做协议转换。
export bool supportsConversion(ApiFormat from, ApiFormat to) {
    return from == to ||
           (from == ApiFormat::anthropic && to == ApiFormat::openai_chat) ||
           (from == ApiFormat::openai_chat && to == ApiFormat::anthropic);
}

// 转换方向 from -> to 时，上游需要请求的路径（相对 effectiveBaseUrl）。
export std::string_view convertedRequestPath(ApiFormat from, ApiFormat to) {
    if (from == to) return {};
    if (from == ApiFormat::anthropic && to == ApiFormat::openai_chat) {
        return "chat/completions";
    }
    if (from == ApiFormat::openai_chat && to == ApiFormat::anthropic) {
        return "v1/messages";
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


std::int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string chatCompletionId(std::string_view id) {
    std::string suffix(id);
    if (suffix.starts_with("msg_")) suffix.erase(0, 4);
    if (suffix.empty()) suffix = "llmswitch";
    return "chatcmpl-" + suffix;
}

Json chatImageToAnthropic(const Json& part) {
    const auto image = part.find("image_url");
    if (image == part.end() || !image->is_object()) return Json();
    const std::string url = image->value("url", "");
    if (url.empty()) return Json();
    Json block;
    block["type"] = "image";
    if (url.starts_with("data:")) {
        const auto marker = url.find(";base64,");
        if (marker != std::string::npos) {
            const std::string media = url.substr(5, marker - 5);
            const std::string data = url.substr(marker + 8);
            if (!data.empty()) {
                block["source"]["type"] = "base64";
                block["source"]["media_type"] = media;
                block["source"]["data"] = data;
                return block;
            }
        }
    }
    block["source"]["type"] = "url";
    block["source"]["url"] = url;
    return block;
}

Json chatContentToAnthropicBlocks(const Json& content) {
    Json blocks = Json::array();
    const auto addText = [&blocks](const std::string& text) {
        if (text.empty()) return;
        Json block;
        block["type"] = "text";
        block["text"] = text;
        blocks.push_back(std::move(block));
    };
    if (content.is_string()) {
        addText(content.get<std::string>());
        return blocks;
    }
    if (!content.is_array()) return blocks;
    for (const auto& part : content) {
        if (part.is_string()) {
            addText(part.get<std::string>());
            continue;
        }
        if (!part.is_object()) continue;
        const auto type = part.value("type", "");
        if (type == "text" || type == "input_text") {
            addText(part.value("text", ""));
        } else if (type == "image_url") {
            Json image = chatImageToAnthropic(part);
            if (!image.is_null()) blocks.push_back(std::move(image));
        } else if (type == "input_audio") {
            addText("[audio]");
        }
    }
    return blocks;
}

void appendAnthropicBlocks(std::vector<Json>& messages, std::string_view role,
                           Json blocks) {
    if (!blocks.is_array() || blocks.empty()) return;
    if (!messages.empty() && messages.back().value("role", "") == role) {
        auto& existing = messages.back()["content"];
        for (auto& block : blocks) existing.push_back(std::move(block));
        return;
    }
    Json message;
    message["role"] = std::string(role);
    message["content"] = std::move(blocks);
    messages.push_back(std::move(message));
}

Json chatRequestToAnthropic(const Json& in) {
    Json out = Json::object();
    if (in.contains("model")) out["model"] = in["model"];

    std::string system = textOf(in.find("system") != in.end()
                                    ? in["system"]
                                    : Json());
    std::vector<Json> messages;
    std::vector<Json> pendingToolResults;
    const auto flushToolResults = [&] {
        if (pendingToolResults.empty()) return;
        Json blocks = Json::array();
        for (auto& block : pendingToolResults) {
            blocks.push_back(std::move(block));
        }
        pendingToolResults.clear();
        appendAnthropicBlocks(messages, "user", std::move(blocks));
    };

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
            if (role == "tool") {
                Json block;
                block["type"] = "tool_result";
                block["tool_use_id"] = message.value("tool_call_id", "");
                block["content"] =
                    message.find("content") != message.end()
                        ? textOf(message["content"])
                        : std::string{};
                if (message.value("is_error", false)) block["is_error"] = true;
                pendingToolResults.push_back(std::move(block));
                continue;
            }
            flushToolResults();
            if (role == "user") {
                Json blocks = chatContentToAnthropicBlocks(
                    message.find("content") != message.end()
                        ? message["content"]
                        : Json());
                appendAnthropicBlocks(messages, "user", std::move(blocks));
                continue;
            }
            if (role != "assistant") continue;

            Json blocks = Json::array();
            const std::string text = textOf(
                message.find("content") != message.end()
                    ? message["content"]
                    : Json());
            if (!text.empty()) {
                Json block;
                block["type"] = "text";
                block["text"] = text;
                blocks.push_back(std::move(block));
            }
            if (const auto calls = message.find("tool_calls");
                calls != message.end() && calls->is_array()) {
                std::size_t toolIndex = 0;
                for (const auto& call : *calls) {
                    if (!call.is_object()) continue;
                    Json block;
                    block["type"] = "tool_use";
                    std::string id = call.value("id", "");
                    if (id.empty()) {
                        id = "toolu_" + std::to_string(toolIndex);
                    }
                    ++toolIndex;
                    block["id"] = std::move(id);
                    const auto function = call.find("function");
                    block["name"] = function != call.end() &&
                                            function->is_object()
                                        ? function->value("name", "")
                                        : "";
                    Json input = Json::object();
                    if (function != call.end() && function->is_object()) {
                        const auto args = function->find("arguments");
                        if (args != function->end()) {
                            if (args->is_object()) {
                                input = *args;
                            } else if (args->is_string()) {
                                const auto parsed = Json::parse(
                                    args->get<std::string>(), nullptr, false);
                                if (parsed.is_object()) input = parsed;
                            }
                        }
                    }
                    block["input"] = std::move(input);
                    blocks.push_back(std::move(block));
                }
            }
            if (blocks.empty()) {
                Json block;
                block["type"] = "text";
                block["text"] = "";
                blocks.push_back(std::move(block));
            }
            appendAnthropicBlocks(messages, "assistant", std::move(blocks));
        }
    }
    flushToolResults();
    if (!system.empty()) out["system"] = std::move(system);
    Json messageArray = Json::array();
    for (auto& message : messages) messageArray.push_back(std::move(message));
    if (!messageArray.empty()) out["messages"] = std::move(messageArray);

    bool allowTools = true;
    std::string choiceType;
    std::string choiceName;
    if (const auto choice = in.find("tool_choice"); choice != in.end()) {
        if (choice->is_string()) {
            choiceType = choice->get<std::string>();
        } else if (choice->is_object()) {
            choiceType = choice->value("type", "");
            const auto function = choice->find("function");
            if (function != choice->end() && function->is_object()) {
                choiceName = function->value("name", "");
            }
            if (choiceType == "function") choiceType = "tool";
        }
    }
    if (choiceType == "none") allowTools = false;
    if (allowTools) {
        if (const auto toolsIt = in.find("tools");
            toolsIt != in.end() && toolsIt->is_array() && !toolsIt->empty()) {
            Json tools = Json::array();
            for (const auto& tool : *toolsIt) {
                if (!tool.is_object()) continue;
                const auto function = tool.find("function");
                if (function == tool.end() || !function->is_object()) continue;
                Json entry;
                entry["name"] = function->value("name", "");
                if (function->contains("description")) {
                    entry["description"] = (*function)["description"];
                }
                entry["input_schema"] =
                    function->contains("parameters") &&
                            (*function)["parameters"].is_object()
                        ? (*function)["parameters"]
                        : Json{{"type", "object"},
                               {"properties", Json::object()}};
                tools.push_back(std::move(entry));
            }
            if (!tools.empty()) out["tools"] = std::move(tools);
        }
    }
    if (allowTools && out.contains("tools")) {
        Json choice;
        if (choiceType == "auto") {
            choice["type"] = "auto";
        } else if (choiceType == "required") {
            choice["type"] = "any";
        } else if (choiceType == "tool") {
            choice["type"] = "tool";
            choice["name"] = std::move(choiceName);
        }
        if (!choice.is_null()) {
            if (in.contains("parallel_tool_calls") &&
                !in.value("parallel_tool_calls", true)) {
                choice["disable_parallel_tool_use"] = true;
            }
            out["tool_choice"] = std::move(choice);
        }
    }

    std::int64_t maxTokens = 0;
    for (const std::string_view key :
         {"max_completion_tokens", "max_tokens"}) {
        if (const auto it = in.find(std::string(key));
            it != in.end() && it->is_number()) {
            maxTokens = it->get<std::int64_t>();
            if (maxTokens > 0) break;
        }
    }
    out["max_tokens"] = maxTokens > 0 ? maxTokens : 4096;
    const auto copyNumber = [&](std::string_view key) {
        if (const auto it = in.find(std::string(key));
            it != in.end() && it->is_number()) {
            out[key] = *it;
        }
    };
    copyNumber("temperature");
    copyNumber("top_p");
    if (const auto stop = in.find("stop"); stop != in.end()) {
        if (stop->is_string()) {
            out["stop_sequences"] = Json::array({*stop});
        } else if (stop->is_array()) {
            out["stop_sequences"] = *stop;
        }
    }
    std::string userId;
    if (const auto user = in.find("user");
        user != in.end() && user->is_string()) {
        userId = user->get<std::string>();
    }
    if (const auto meta = in.find("metadata");
        meta != in.end() && meta->is_object()) {
        if (const auto id = meta->find("user_id");
            id != meta->end() && id->is_string()) {
            userId = id->get<std::string>();
        }
    }
    if (!userId.empty()) out["metadata"]["user_id"] = std::move(userId);
    if (in.value("stream", false)) out["stream"] = true;
    return out;
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

Json anthropicUsageToChat(const Json& usage) {
    Json out = Json::object();
    std::int64_t input = 0;
    std::int64_t output = 0;
    if (usage.is_object()) {
        input = usage.value("input_tokens", 0);
        output = usage.value("output_tokens", 0);
        if (const auto cache = usage.find("cache_read_input_tokens");
            cache != usage.end() && cache->is_number() &&
            cache->get<std::int64_t>() > 0) {
            out["prompt_tokens_details"]["cached_tokens"] =
                cache->get<std::int64_t>();
        }
    }
    out["prompt_tokens"] = input;
    out["completion_tokens"] = output;
    out["total_tokens"] = input + output;
    return out;
}

std::string chatFinishReason(std::string_view reason) {
    if (reason == "tool_use") return "tool_calls";
    if (reason == "max_tokens") return "length";
    if (reason == "refusal") return "content_filter";
    return "stop";
}

Json anthropicResponseToChat(const Json& in) {
    std::string text;
    std::string reasoning;
    Json toolCalls = Json::array();
    const auto content = in.find("content");
    if (content != in.end()) {
        const auto appendBlock = [&](const Json& block) {
            if (block.is_string()) {
                text += block.get<std::string>();
                return;
            }
            if (!block.is_object()) return;
            const auto type = block.value("type", "");
            if (type == "text") {
                text += block.value("text", "");
            } else if (type == "thinking") {
                reasoning += block.value("thinking", "");
            } else if (type == "tool_use") {
                Json call;
                std::string id = block.value("id", "");
                if (id.empty()) {
                    id = "toolu_" + std::to_string(toolCalls.size());
                }
                call["id"] = std::move(id);
                call["type"] = "function";
                call["function"]["name"] = block.value("name", "");
                const auto input = block.find("input");
                if (input != block.end() && input->is_object()) {
                    call["function"]["arguments"] = input->dump();
                } else {
                    call["function"]["arguments"] = "{}";
                }
                toolCalls.push_back(std::move(call));
            }
        };
        if (content->is_array()) {
            for (const auto& block : *content) appendBlock(block);
        } else if (content->is_string()) {
            text = content->get<std::string>();
        }
    }

    Json message;
    message["role"] = "assistant";
    if (!text.empty()) {
        message["content"] = text;
    } else {
        message["content"] = nullptr;
    }
    if (!toolCalls.empty()) message["tool_calls"] = std::move(toolCalls);
    if (!reasoning.empty()) message["reasoning_content"] = std::move(reasoning);

    Json choice;
    choice["index"] = 0;
    choice["message"] = std::move(message);
    choice["finish_reason"] = chatFinishReason(
        in.value("stop_reason", ""));

    Json out;
    out["id"] = chatCompletionId(in.value("id", ""));
    out["object"] = "chat.completion";
    out["created"] = nowSeconds();
    out["model"] = in.value("model", "");
    out["choices"] = Json::array();
    out["choices"].push_back(std::move(choice));
    out["usage"] = anthropicUsageToChat(
        in.find("usage") != in.end() ? in["usage"] : Json());
    return out;
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

// Anthropic SSE -> OpenAI Chat SSE。上游会话已整段返回，所以这里同样
// 先解析完整事件流再重新编码；Chat 客户端按增量 delta 消费结果。
class AnthropicSseToChat {
public:
    std::string run(std::string_view body) {
        for (const auto& event : parseSse(body)) {
            if (event.data.empty()) continue;
            if (event.data == "[DONE]") break;
            const auto data = parseOrDiscard(event.data);
            if (data.is_discarded() || !data.is_object()) continue;
            consume(event, data);
        }
        finish();
        return std::move(out_);
    }

private:
    std::string out_;
    std::string id_ = "chatcmpl-llmswitch";
    std::string model_;
    std::int64_t created_ = nowSeconds();
    std::int64_t inputTokens_ = 0;
    std::int64_t outputTokens_ = 0;
    bool started_ = false;
    bool finished_ = false;
    bool hasUsage_ = false;
    std::string finishReason_ = "stop";
    std::size_t toolIndex_ = 0;
    std::map<int, std::size_t> blockToolIndex_;

    void emitChunk(Json delta, Json finishReason,
                   bool includeChoices = true) {
        Json chunk;
        chunk["id"] = id_;
        chunk["object"] = "chat.completion.chunk";
        chunk["created"] = created_;
        chunk["model"] = model_;
        chunk["choices"] = Json::array();
        if (includeChoices) {
            Json choice;
            choice["index"] = 0;
            choice["delta"] = std::move(delta);
            choice["finish_reason"] = std::move(finishReason);
            chunk["choices"].push_back(std::move(choice));
        }
        out_ += "data: ";
        out_ += chunk.dump();
        out_ += "\n\n";
    }

    void ensureStarted() {
        if (started_) return;
        started_ = true;
        Json delta;
        delta["role"] = "assistant";
        delta["content"] = "";
        emitChunk(std::move(delta), nullptr);
    }

    void emitUsage() {
        Json chunk;
        chunk["id"] = id_;
        chunk["object"] = "chat.completion.chunk";
        chunk["created"] = created_;
        chunk["model"] = model_;
        chunk["choices"] = Json::array();
        Json usage;
        usage["input_tokens"] = inputTokens_;
        usage["output_tokens"] = outputTokens_;
        chunk["usage"] = anthropicUsageToChat(usage);
        out_ += "data: ";
        out_ += chunk.dump();
        out_ += "\n\n";
    }

    void finish() {
        if (finished_) return;
        ensureStarted();
        emitChunk(Json::object(), finishReason_);
        if (hasUsage_) emitUsage();
        out_ += "data: [DONE]\n\n";
        finished_ = true;
    }

    void consume(const SseEvent& event, const Json& data) {
        const std::string type = data.value("type", "");
        if (type == "message_start") {
            const auto message = data.find("message");
            if (message != data.end() && message->is_object()) {
                if (const auto id = message->find("id");
                    id != message->end() && id->is_string()) {
                    id_ = chatCompletionId(id->get<std::string>());
                }
                if (const auto model = message->find("model");
                    model != message->end() && model->is_string()) {
                    model_ = model->get<std::string>();
                }
                if (const auto usage = message->find("usage");
                    usage != message->end() && usage->is_object()) {
                    inputTokens_ = usage->value("input_tokens", 0);
                    hasUsage_ = true;
                }
            }
            ensureStarted();
            return;
        }
        if (type == "content_block_start") {
            ensureStarted();
            const int blockIndex = data.value("index", 0);
            const auto block = data.find("content_block");
            if (block == data.end() || !block->is_object()) return;
            const auto blockType = block->value("type", "");
            if (blockType == "text") {
                const std::string text = block->value("text", "");
                if (!text.empty()) {
                    Json delta;
                    delta["content"] = text;
                    emitChunk(std::move(delta), nullptr);
                }
                return;
            }
            if (blockType != "tool_use") return;
            const std::size_t toolIndex = toolIndex_++;
            blockToolIndex_[blockIndex] = toolIndex;
            Json call;
            call["index"] = toolIndex;
            std::string id = block->value("id", "");
            if (id.empty()) id = "toolu_" + std::to_string(toolIndex);
            call["id"] = std::move(id);
            call["type"] = "function";
            call["function"]["name"] = block->value("name", "");
            const auto input = block->find("input");
            call["function"]["arguments"] =
                input != block->end() && input->is_object() &&
                        !input->empty()
                    ? input->dump()
                    : std::string{};
            Json delta;
            delta["tool_calls"] = Json::array();
            delta["tool_calls"].push_back(std::move(call));
            emitChunk(std::move(delta), nullptr);
            return;
        }
        if (type == "content_block_delta") {
            ensureStarted();
            const int blockIndex = data.value("index", 0);
            const auto delta = data.find("delta");
            if (delta == data.end() || !delta->is_object()) return;
            const auto deltaType = delta->value("type", "");
            if (deltaType == "text_delta") {
                const std::string text = delta->value("text", "");
                if (text.empty()) return;
                Json out;
                out["content"] = text;
                emitChunk(std::move(out), nullptr);
            } else if (deltaType == "input_json_delta") {
                const auto tool = blockToolIndex_.find(blockIndex);
                if (tool == blockToolIndex_.end()) return;
                const std::string partial = delta->value("partial_json", "");
                if (partial.empty()) return;
                Json call;
                call["index"] = tool->second;
                call["function"]["arguments"] = partial;
                Json out;
                out["tool_calls"] = Json::array();
                out["tool_calls"].push_back(std::move(call));
                emitChunk(std::move(out), nullptr);
            } else if (deltaType == "thinking_delta") {
                const std::string thinking = delta->value("thinking", "");
                if (thinking.empty()) return;
                Json out;
                out["reasoning_content"] = thinking;
                emitChunk(std::move(out), nullptr);
            }
            return;
        }
        if (type == "message_delta") {
            if (const auto delta = data.find("delta");
                delta != data.end() && delta->is_object()) {
                if (const auto reason = delta->find("stop_reason");
                    reason != delta->end() && reason->is_string()) {
                    finishReason_ =
                        chatFinishReason(reason->get<std::string>());
                }
            }
            if (const auto usage = data.find("usage");
                usage != data.end() && usage->is_object()) {
                outputTokens_ = usage->value("output_tokens", 0);
                hasUsage_ = true;
            }
            return;
        }
        if (type == "message_stop") {
            finish();
            return;
        }
        if (type == "error" || event.event == "error") {
            std::string message = "上游返回错误";
            if (const auto error = data.find("error");
                error != data.end()) {
                if (error->is_object()) {
                    message = error->value("message", message);
                } else if (error->is_string()) {
                    message = error->get<std::string>();
                }
            }
            Json payload;
            payload["error"]["message"] = std::move(message);
            payload["error"]["type"] = "api_error";
            out_ += "data: ";
            out_ += payload.dump();
            out_ += "\n\n";
            out_ += "data: [DONE]\n\n";
            finished_ = true;
        }
    }
};

} // namespace

export std::optional<std::string> convertRequest(
    ApiFormat from, ApiFormat to, std::string_view body, std::string& error) {
    error.clear();
    if (from == to) return std::string(body);
    const bool chatFromAnthropic =
        from == ApiFormat::anthropic && to == ApiFormat::openai_chat;
    const bool anthropicFromChat =
        from == ApiFormat::openai_chat && to == ApiFormat::anthropic;
    if (!chatFromAnthropic && !anthropicFromChat) {
        error = "暂不支持 " + std::string(apiFormatName(from)) + " -> " +
                std::string(apiFormatName(to)) + " 的请求转换";
        return std::nullopt;
    }
    const Json in = parseOrDiscard(body);
    if (in.is_discarded() || !in.is_object()) {
        error = "请求体不是 JSON 对象，无法转换协议";
        return std::nullopt;
    }
    return chatFromAnthropic ? chatRequestFromAnthropic(in).dump()
                             : chatRequestToAnthropic(in).dump();
}

export std::optional<std::string> convertResponse(
    ApiFormat from, ApiFormat to, bool stream, std::string_view body,
    std::string& error) {
    error.clear();
    if (from == to) return std::string(body);
    const bool chatFromAnthropic =
        from == ApiFormat::openai_chat && to == ApiFormat::anthropic;
    const bool anthropicFromChat =
        from == ApiFormat::anthropic && to == ApiFormat::openai_chat;
    if (!chatFromAnthropic && !anthropicFromChat) {
        error = "暂不支持 " + std::string(apiFormatName(from)) + " -> " +
                std::string(apiFormatName(to)) + " 的响应转换";
        return std::nullopt;
    }
    if (stream || body.find("data:") != std::string_view::npos) {
        if (chatFromAnthropic) {
            ChatSseToAnthropic converter;
            return converter.run(body);
        }
        AnthropicSseToChat converter;
        return converter.run(body);
    }
    const Json in = parseOrDiscard(body);
    if (in.is_discarded() || !in.is_object()) {
        error = "成员响应不是 JSON 对象，无法转换协议";
        return std::nullopt;
    }
    const Json out = chatFromAnthropic ? chatResponseToAnthropic(in)
                                       : anthropicResponseToChat(in);
    if (out.is_null()) {
        error = "成员响应缺少必要字段，无法转换协议";
        return std::nullopt;
    }
    return out.dump();
}

} // namespace protocol
