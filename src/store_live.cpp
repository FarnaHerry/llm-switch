// store_live.cpp — llmswitch.store 实现单元：live 配置文件同步。
//
// switchTo / detectCurrent / restoreOfficial / importLive / importFrom
// 与各工具的格式细节（env 行级读写、opencode / pi 的 provider 映射、
// codex 的 auth.json + config.toml、claude desktop 的 3p profile、
// zcode 的 config.json 条目、dsh 的 settings.yaml/.credentials.yaml 与
// hermes 的 config.yaml 行级改写）都在这里。跨单元共用的文件工具与 ZCode
// 条目助手以模块链接声明在 store.cppm、定义在 store.cpp / store_zcode.cpp。
module llmswitch.store;

import std;
import nlohmann.json;
import llmswitch.config;
import llmswitch.models;

namespace store {


// opencode 专用：解析失败抛中文错（提示 JSON5 注释暂不支持），
// 不挪文件不覆盖 —— 宁可拒绝切换也不能静默吞掉用户配置。
nlohmann::json readJsonStrict(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) return nullptr;
    const std::string text = readTextFile(file);
    if (text.empty()) return nullptr;
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded()) {
        throw std::runtime_error(std::format(
            "无法解析 {}：官方配置允许 JSON5 注释，暂不支持。"
            "请把注释去掉后重试（原文件未被修改）。",
            file.string()));
    }
    return j;
}

// 提取一行的 KEY；非赋值行（空/注释/无 =）返回空。
std::string envLineKey(std::string_view line) {
    std::string_view v(line);
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
    if (v.empty() || v.front() == '#') return "";
    if (v.starts_with("export ")) v.remove_prefix(7);
    const auto eq = v.find('=');
    if (eq == std::string_view::npos) return "";
    std::string key(v.substr(0, eq));
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
    return key;
}

std::string trimEnvValue(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
        value.back() == value.front()) {
        value = value.substr(1, value.size() - 2);
    }
    return std::string(value);
}

// 读单个键（文件/键缺失均为空串）。
std::string readEnvValue(const std::filesystem::path& file, std::string_view key) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return "";
    std::ifstream in(file, std::ios::binary);
    if (!in) return "";
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (envLineKey(line) == key) {
            const auto eq = line.find('=');
            return trimEnvValue(std::string_view(line).substr(eq + 1));
        }
    }
    return "";
}

// 行级 upsert：替换既有 KEY= 行（保留其余行、注释与顺序），缺失追加尾部；
// value 为空 = 删除该键的行。写前调用方负责 backupLiveFile。
void writeEnvValues(const std::filesystem::path& file,
                    const std::vector<std::pair<std::string, std::string>>& targets) {
    std::vector<std::string> lines;
    {
        std::ifstream in(file, std::ios::binary);
        if (in) {
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                lines.push_back(std::move(line));
            }
        }
    }
    for (const auto& [key, value] : targets) {
        bool found = false;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (envLineKey(lines[i]) != key) continue;
            found = true;
            if (value.empty()) {
                lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                lines[i] = key + "=" + value;
            }
            break;
        }
        if (!found && !value.empty()) {
            lines.push_back(key + "=" + value);
        }
    }
    std::string out;
    for (const auto& line : lines) {
        out += line;
        out += '\n';
    }
    atomicWrite(file, out);
}

// 对象里的字符串字段（缺失/非字符串 → 空串）。
std::string jsonStr(const nlohmann::json& j, std::string_view key) {
    if (!j.is_object()) return "";
    const auto it = j.find(std::string(key));
    if (it == j.end() || !it->is_string()) return "";
    return it->get<std::string>();
}

// 去掉前导空白。
std::string_view trimLeft(std::string_view s) {
    const auto it = std::ranges::find_if(
        s, [](unsigned char c) { return !std::isspace(c); });
    return s.substr(static_cast<std::size_t>(it - s.begin()));
}

// 行级解析顶层 model 键：去掉前导空白与可选的 `#` 注释前缀后，若余下文本以
// `model` + 空白/`=` 开头则返回等号右侧的原始值文本（trim 后，不去引号）。
// 不是 model 行返回空；commentedOut 传出该行是否为注释行。
std::string codexModelLineValue(std::string_view line, bool& commentedOut) {
    std::string_view s = trimLeft(line);
    commentedOut = false;
    if (s.starts_with('#')) {
        commentedOut = true;
        s = trimLeft(s.substr(1));
    }
    if (!s.starts_with("model")) return "";
    s.remove_prefix(5);
    if (!s.empty() && s.front() != '=' &&
        !std::isspace(static_cast<unsigned char>(s.front()))) {
        return "";  // model_provider 等前缀键不算
    }
    s = trimLeft(s);
    if (!s.starts_with('=')) return "";
    s = trimLeft(s.substr(1));
    return std::string(s);
}

// Claude Desktop 3p profile 路径（base 为空 = 平台不支持，返回空）。
std::filesystem::path claudeDesktopProfileFile() {
    const auto dir = cfg::claudeDesktop3pDir();
    if (dir.empty()) return {};
    return dir / "configLibrary" /
           (std::string(kClaudeDesktopProfileId) + ".json");
}

namespace {

// 深合并：patch 的对象递归并入 base，其余类型整体覆盖。
void deepMerge(nlohmann::json& base, const nlohmann::json& patch) {
    if (!patch.is_object()) {
        base = patch;
        return;
    }
    if (!base.is_object()) base = nlohmann::json::object();
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        if (it.value().is_object() && base.contains(it.key()) &&
            base[it.key()].is_object()) {
            deepMerge(base[it.key()], it.value());
        } else {
            base[it.key()] = it.value();
        }
    }
}

// pi 的 api 字段映射（三档，经 models::normalizeApiFormat 归一）：
// anthropic → anthropic-messages；openai-responses → openai-responses；
// 其余（openai-chat / 默认）→ openai-completions。
std::string piApiValue(std::string_view apiFormat) {
    const auto f = models::normalizeApiFormat(apiFormat);
    if (f == "anthropic") return "anthropic-messages";
    if (f == "openai-responses") return "openai-responses";
    return "openai-completions";
}

// opencode 的 npm 适配器映射（三档）：anthropic → @ai-sdk/anthropic；
// openai-responses → @ai-sdk/openai；其余（openai-chat / 默认）→
// @ai-sdk/openai-compatible。
std::string_view opencodeNpmValue(std::string_view apiFormat) {
    const auto f = models::normalizeApiFormat(apiFormat);
    if (f == "anthropic") return "@ai-sdk/anthropic";
    if (f == "openai-responses") return "@ai-sdk/openai";
    return "@ai-sdk/openai-compatible";
}

// 把顶层 model = "<model>" 写进 config.toml 文本：有未注释 model 行就替换值；
// 否则替换第一条 `# model = ...` 注释行；都没有则在文件开头插入。model 为空
// 返回原文。其余内容（含所有 [table] 节）原样保留。
std::string applyCodexModel(std::string_view toml, std::string_view model) {
    if (model.empty()) return std::string(toml);
    std::string escaped;
    for (const char c : model) {
        if (c == '\\' || c == '"') escaped += '\\';
        escaped += c;
    }
    const std::string newLine = "model = \"" + escaped + "\"";
    std::string out;
    std::istringstream in{std::string(toml)};
    std::string line;
    bool inSection = false;    // 已进入 [table] 节区，停止 model 行匹配
    bool replaced = false;     // 已写入未注释 model 行
    bool sawCommented = false;
    std::string::size_type commentedPos = 0;
    while (std::getline(in, line)) {
        if (inSection || replaced) {
            out += line;
            out += '\n';
            continue;
        }
        if (trimLeft(line).starts_with('[')) {
            inSection = true;  // 进入节区，停止扫描
            out += line;
            out += '\n';
            continue;
        }
        bool commentedOut = false;
        if (!codexModelLineValue(line, commentedOut).empty()) {
            if (!commentedOut) {
                out += newLine;
                out += '\n';
                replaced = true;
                continue;
            }
            if (!sawCommented) {
                sawCommented = true;
                commentedPos = out.size();
            }
        }
        out += line;
        out += '\n';
    }
    if (!replaced) {  // 顶层没有未注释 model 行
        if (sawCommented) {
            // 整行替换第一条注释行（行末必带 \n）。
            const auto nl = out.find('\n', commentedPos);
            out.replace(commentedPos, nl - commentedPos, newLine);
        } else {
            out.insert(0, newLine + "\n");
        }
    }
    return out;
}

// 把供应商的实际访问 URL 写入 config.toml 的第一个未注释 base_url 键。
// codex 的模板可能包含多个节，但当前供应商模板只需要第一个上游地址；
// 没有 base_url 时保留原文，兼容只通过其他配置提供地址的模板。
std::string applyCodexBaseUrl(std::string_view toml, std::string_view baseUrl) {
    if (baseUrl.empty()) return std::string(toml);
    std::string escaped;
    for (const char c : baseUrl) {
        if (c == '\\' || c == '"') escaped += '\\';
        escaped += c;
    }
    std::string out;
    std::istringstream in{std::string(toml)};
    bool replaced = false;
    for (std::string line; std::getline(in, line);) {
        const std::string_view trimmed = trimLeft(line);
        std::string_view key = trimmed;
        const bool commented = key.starts_with('#');
        if (!commented && key.starts_with("base_url")) {
            key.remove_prefix(8);
            if (key.empty() || key.front() == '=' ||
                std::isspace(static_cast<unsigned char>(key.front()))) {
                const std::size_t indent = line.size() - trimmed.size();
                line = line.substr(0, indent) +
                       "base_url = \"" + escaped + "\"";
                replaced = true;
            }
        }
        out += line;
        out += '\n';
        if (replaced) {
            // 模板只替换当前供应商的第一条地址，避免误改其他 provider 节。
            for (std::string rest; std::getline(in, rest);) {
                out += rest;
                out += '\n';
            }
            break;
        }
    }
    return out;
}

// pi 目录/文件权限：目录 0700、文件 0600（对齐官方对凭据目录的约定）。
void restrictPiDir(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
#ifndef _WIN32
    std::filesystem::permissions(dir, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, ec);
#endif
}

void restrictPiFile(const std::filesystem::path& file) {
#ifndef _WIN32
    std::error_code ec;
    std::filesystem::permissions(file,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec);
#endif
}

// ---- dsh（DeepSeek Harness）YAML 行级助手（hermes 等其它 YAML 工具也复用 ----
// yamlLineOf / yamlScalar / yamlQuote）。
// 无 YAML 库，按缩进做行级读写；无关键、注释与顺序原样保留。

std::string_view trimRight(std::string_view s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

std::string_view trimBoth(std::string_view s) { return trimRight(trimLeft(s)); }

// YAML 双引号标量（转义 \ 与 "）。
std::string yamlQuote(std::string_view v) {
    std::string out = "\"";
    for (const char c : v) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

// 读 YAML 标量：去引号（双引号解 \" \\，单引号解 ''）；裸值去掉 " #" 行尾
// 注释。读不到结构时 best-effort 返回原文。
std::string yamlScalar(std::string_view v) {
    v = trimBoth(v);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
        std::string out;
        for (std::size_t i = 1; i + 1 < v.size(); ++i) {
            if (v[i] == '\\' && i + 2 < v.size() &&
                (v[i + 1] == '"' || v[i + 1] == '\\')) {
                out += v[i + 1];
                ++i;
            } else {
                out += v[i];
            }
        }
        return out;
    }
    if (v.size() >= 2 && v.front() == '\'' && v.back() == '\'') {
        std::string out;
        for (std::size_t i = 1; i + 1 < v.size(); ++i) {
            if (v[i] == '\'' && i + 2 < v.size() && v[i + 1] == '\'') {
                out += '\'';
                ++i;
            } else {
                out += v[i];
            }
        }
        return out;
    }
    if (const auto p = v.find(" #"); p != std::string_view::npos) {
        v = trimRight(v.substr(0, p));
    }
    return std::string(v);
}

// 一行的 YAML 结构信息：indent = 前导空白列数；key = 首个 ': ' 前的键
// （列表项 "- id: x" 的 key 为 id；裸标量列表项与无冒号行 key 为空）。
struct YamlLine {
    int indent = 0;
    std::string_view key;
    std::string_view value;
    bool blank = false;
    bool comment = false;
    bool listItem = false;
};

YamlLine yamlLineOf(std::string_view line) {
    YamlLine r;
    const auto trimmed = trimLeft(line);
    r.indent = static_cast<int>(line.size() - trimmed.size());
    r.blank = trimmed.empty();
    r.comment = !r.blank && trimmed.front() == '#';
    if (r.blank || r.comment) return r;
    std::string_view body = trimmed;
    r.listItem = body.front() == '-';
    if (r.listItem) body = trimLeft(body.substr(1));
    const auto colon = body.find(':');
    if (colon == std::string_view::npos) {
        if (r.listItem) r.value = body;
        return r;
    }
    std::string_view value = body.substr(colon + 1);
    // 冒号后必须为空或空白分隔才是键值行（URL 等含冒号的标量不算）。
    if (!value.empty() && value.front() != ' ' && value.front() != '\t') {
        if (r.listItem) r.value = body;
        return r;
    }
    r.key = trimRight(body.substr(0, colon));
    r.value = value;
    return r;
}

// 凭据 env 名：LLMSWITCH_ + id 大写（非字母数字转 _）。
std::string dshApiKeyEnv(std::string_view id) {
    std::string out = "LLMSWITCH_";
    for (const unsigned char c : id) {
        out += std::isalnum(c) ? static_cast<char>(std::toupper(c)) : '_';
    }
    return out;
}

// 行级改写 settings.yaml：删顶层 agent-default-model 块与
// llm-pi-ai.providers 下的 llmswitch-* 条目；entryLines 非空时在 providers
// 块尾插入（缩进随实际 providers 行调整，entryLines 以 4 列基准缩进生成）；
// defaultProvider 非空时在文件头重建 agent-default-model 指向块。
// restore 模式 = entryLines 空 + defaultProvider 空（只删不增）。
std::string rewriteDshSettings(std::string_view text,
                               const std::vector<std::string>& entryLines,
                               std::string_view defaultProvider,
                               std::string_view defaultModel) {
    std::vector<std::string> lines;
    {
        std::istringstream in{std::string(text)};
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(std::move(line));
        }
    }
    std::vector<std::string> out;
    bool inserted = entryLines.empty();
    bool inPi = false, inProv = false, sawPi = false, sawProv = false;
    int piIndent = -1, provIndent = -1, entryIndent = -1;
    bool skipping = false;
    int skipIndent = -1;

    const auto insertEntries = [&](int baseIndent) {
        if (inserted) return;
        const int shift = baseIndent - 4;
        for (const auto& e : entryLines) {
            if (shift >= 0) {
                out.push_back(std::string(static_cast<std::size_t>(shift), ' ') +
                              e);
            } else {
                out.push_back(e.substr(static_cast<std::size_t>(-shift)));
            }
        }
        inserted = true;
    };

    for (const auto& line : lines) {
        const YamlLine dl = yamlLineOf(line);
        if (skipping) {
            // 块内（更深层、空行或注释行）继续丢弃；落到同级/外层结束丢弃。
            if (dl.blank || dl.comment || dl.indent > skipIndent) continue;
            skipping = false;
        }
        if (!dl.blank && !dl.comment) {
            if (inProv && dl.indent <= provIndent) {
                insertEntries(provIndent + 2);
                inProv = false;
                entryIndent = -1;
            }
            if (inPi && dl.indent <= piIndent) {
                if (sawPi && !sawProv) {
                    out.push_back(
                        std::string(static_cast<std::size_t>(piIndent + 2), ' ') +
                        "providers:");
                    insertEntries(piIndent + 4);
                }
                inPi = false;
            }
            if (inProv) {
                if (entryIndent == -1 || dl.indent <= entryIndent) {
                    entryIndent = dl.indent;
                    if (dl.key.starts_with("llmswitch-")) {
                        skipping = true;
                        skipIndent = dl.indent;
                        continue;
                    }
                }
            } else if (inPi) {
                if (dl.key == "providers" && trimBoth(dl.value).empty()) {
                    inProv = true;
                    sawProv = true;
                    provIndent = dl.indent;
                    entryIndent = -1;
                }
            } else if (dl.indent == 0) {
                if (dl.key == "agent-default-model") {
                    skipping = true;
                    skipIndent = 0;
                    continue;
                }
                if (dl.key == "llm-pi-ai") {
                    inPi = true;
                    sawPi = true;
                    piIndent = dl.indent;
                }
            }
        }
        out.push_back(line);
    }
    if (inProv) {
        insertEntries(provIndent + 2);
    } else if (sawPi && !sawProv) {
        out.push_back(std::string(static_cast<std::size_t>(piIndent + 2), ' ') +
                      "providers:");
        insertEntries(piIndent + 4);
    } else if (!sawPi && !inserted) {
        if (!out.empty() && !out.back().empty()) out.emplace_back();
        out.push_back("llm-pi-ai:");
        out.push_back("  providers:");
        insertEntries(4);
    }
    if (!defaultProvider.empty()) {
        std::vector<std::string> head;
        head.push_back("agent-default-model:");
        head.push_back("  provider: " + std::string(defaultProvider));
        if (!defaultModel.empty()) {
            head.push_back("  model: " + yamlQuote(defaultModel));
        }
        head.emplace_back();
        out.insert(out.begin(), head.begin(), head.end());
    }
    std::string result;
    for (const auto& l : out) {
        result += l;
        result += '\n';
    }
    return result;
}

// .credentials.yaml 行级 upsert `ENV: "key"`（顶层 map；version 等其它键、
// 注释与顺序原样保留，缺失追加尾部）。写前调用方负责 backupLiveFile。
void upsertDshCredential(const std::filesystem::path& file,
                         std::string_view envName, std::string_view apiKey) {
    std::vector<std::string> lines;
    {
        std::ifstream in(file, std::ios::binary);
        if (in) {
            for (std::string line; std::getline(in, line);) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                lines.push_back(std::move(line));
            }
        }
    }
    const std::string newLine =
        std::string(envName) + ": " + yamlQuote(apiKey);
    bool found = false;
    for (auto& line : lines) {
        const YamlLine dl = yamlLineOf(line);
        if (dl.listItem || dl.key != envName) continue;
        line = newLine;
        found = true;
        break;
    }
    if (!found) lines.push_back(newLine);
    std::string out;
    for (const auto& line : lines) {
        out += line;
        out += '\n';
    }
    atomicWrite(file, out);
}

// ---- hermes（Hermes Agent）config.yaml 行级助手 ----

// hermes 的 api_mode 字段映射（三档，经 models::normalizeApiFormat 归一）：
// anthropic → anthropic_messages；openai-responses → codex_responses；
// 其余（openai-chat / 默认）→ chat_completions。bedrock_converse 不在
// apiFormat 三档内，不生成。
std::string hermesApiMode(std::string_view apiFormat) {
    const auto f = models::normalizeApiFormat(apiFormat);
    if (f == "anthropic") return "anthropic_messages";
    if (f == "openai-responses") return "codex_responses";
    return "chat_completions";
}

// 行级改写 config.yaml（对齐 cc-switch hermes_config.rs 的切换语义）：
// custom_providers 列表删掉 name 以 llmswitch- 开头的条目后在块尾追加
// entryLines（缩进固定 2/4/6，与 hermes 官方写法一致；块不存在则文件尾
// 补脚手架）；顶层 model 节原位写 provider（总是）与 default（非空时），
// 键缺失补进节尾，节不存在则文件头新建。其余节（agent / mcp_servers /
// v12+ providers dict 等）与注释原样保留。
std::string rewriteHermesConfig(std::string_view text,
                                const std::vector<std::string>& entryLines,
                                std::string_view providerName,
                                std::string_view defaultModel) {
    std::vector<std::string> lines;
    {
        std::istringstream in{std::string(text)};
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(std::move(line));
        }
    }
    const int n = static_cast<int>(lines.size());
    // 第一遍：定位 custom_providers / model 节区间与本应用托管条目区间。
    int customStart = -1, customEnd = -1;
    int modelStart = -1, modelEnd = -1;
    int providerIdx = -1, defaultIdx = -1;
    struct Range {
        int start;
        int end;
    };
    std::vector<Range> dropItems;
    bool inCustom = false, inModel = false, inItem = false, inModels = false;
    int itemIndent = -1, modelsIndent = -1, itemStart = -1;
    bool itemOurs = false;
    for (int i = 0; i < n; ++i) {
        const YamlLine dl = yamlLineOf(lines[static_cast<std::size_t>(i)]);
        if (dl.blank || dl.comment) continue;
        if (inModels && dl.indent <= modelsIndent) inModels = false;
        if (inItem && dl.indent <= itemIndent) {
            if (itemOurs) dropItems.push_back({itemStart, i});
            inItem = false;
            inModels = false;
        }
        if (inCustom && dl.indent == 0) {
            inCustom = false;
            customEnd = i;
        }
        if (inModel && dl.indent == 0) {
            inModel = false;
            modelEnd = i;
        }
        if (inModel) {
            if (dl.key == "provider") providerIdx = i;
            if (dl.key == "default") defaultIdx = i;
            continue;
        }
        if (inItem) {
            if (dl.key == "name" &&
                yamlScalar(dl.value).starts_with("llmswitch-")) {
                itemOurs = true;
            } else if (dl.key == "models" && trimBoth(dl.value).empty()) {
                inModels = true;
                modelsIndent = dl.indent;
            }
            continue;
        }
        if (inCustom) {
            if (dl.listItem) {
                inItem = true;
                itemIndent = dl.indent;
                itemStart = i;
                itemOurs = dl.key == "name" &&
                           yamlScalar(dl.value).starts_with("llmswitch-");
            }
            continue;
        }
        if (dl.indent == 0 && trimBoth(dl.value).empty()) {
            if (dl.key == "model") {
                inModel = true;
                modelStart = i;
            } else if (dl.key == "custom_providers") {
                inCustom = true;
                customStart = i;
            }
        }
    }
    if (inItem && itemOurs) dropItems.push_back({itemStart, n});
    if (inCustom && customEnd == -1) customEnd = n;
    if (inModel && modelEnd == -1) modelEnd = n;

    const std::string providerLine =
        "  provider: " + std::string(providerName);
    const std::string defaultLine =
        "  default: " + yamlQuote(defaultModel);
    std::vector<std::string> out;
    bool insertedEntries = false;
    bool wroteProvider = false, wroteDefault = false;
    for (int i = 0; i < n; ++i) {
        if (i == customEnd && !insertedEntries) {
            out.insert(out.end(), entryLines.begin(), entryLines.end());
            insertedEntries = true;
        }
        if (i == modelEnd) {
            if (!wroteProvider) {
                out.push_back(providerLine);
                wroteProvider = true;
            }
            if (!defaultModel.empty() && !wroteDefault) {
                out.push_back(defaultLine);
                wroteDefault = true;
            }
        }
        bool dropped = false;
        for (const auto& r : dropItems) {
            if (i >= r.start && i < r.end) {
                dropped = true;
                break;
            }
        }
        if (dropped) continue;
        if (i == providerIdx) {
            out.push_back(providerLine);
            wroteProvider = true;
            continue;
        }
        if (i == defaultIdx && !defaultModel.empty()) {
            out.push_back(defaultLine);
            wroteDefault = true;
            continue;
        }
        out.push_back(lines[static_cast<std::size_t>(i)]);
    }
    if (!insertedEntries) {
        if (customStart == -1) {
            if (!out.empty() && !out.back().empty()) out.emplace_back();
            out.push_back("custom_providers:");
        }
        out.insert(out.end(), entryLines.begin(), entryLines.end());
    }
    if (modelStart == -1) {
        std::vector<std::string> head;
        head.push_back("model:");
        head.push_back(providerLine);
        if (!defaultModel.empty()) head.push_back(defaultLine);
        head.emplace_back();
        out.insert(out.begin(), head.begin(), head.end());
    }
    std::string result;
    for (const auto& l : out) {
        result += l;
        result += '\n';
    }
    return result;
}

// 对齐 cc-switch 上游 is_claude_safe_model_id：Claude Desktop 的模型菜单只认
// claude-(sonnet|opus|haiku|fable)-* / anthropic/claude-* 前缀的 route id，
// 且角色前缀后必须有实际模型标识；其它名字（kimi-k2 等）写入 profile 会触发
// 桌面端 fail-all 拒收整组。
bool isClaudeSafeModelId(std::string model) {
    // trim + 小写归一
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    const auto first = std::ranges::find_if(model, notSpace);
    const auto last = std::find_if(model.rbegin(), model.rend(), notSpace).base();
    model = first < last ? std::string(first, last) : std::string{};
    std::ranges::transform(model, model.begin(),
                           [](unsigned char c) { return std::tolower(c); });
    std::string_view tail = model;
    if (tail.starts_with("anthropic/claude-")) {
        tail.remove_prefix(std::string_view("anthropic/claude-").size());
    } else if (tail.starts_with("claude-")) {
        tail.remove_prefix(std::string_view("claude-").size());
    } else {
        return false;
    }
    for (const std::string_view role : {"sonnet-", "opus-", "haiku-", "fable-"}) {
        if (tail.starts_with(role) && tail.size() > role.size()) return true;
    }
    return false;
}

// 组一条 inferenceModels 条目：actual 本身是白名单 route id 就直接写 name；
// 否则借用 borrowedId（该档的安全角色名）。labelOverride 只放菜单显示名，
// supports1m 只在勾选时声明；实际请求模型仍由 Provider 的 *Model 字段保存。
nlohmann::json claudeDesktopModelEntry(const std::string& actual,
                                       std::string_view borrowedId,
                                       std::string_view displayName = {},
                                       bool supports1m = false) {
    nlohmann::json m;
    const bool safe = isClaudeSafeModelId(actual);
    if (safe) {
        m["name"] = actual;
    } else {
        m["name"] = borrowedId;
    }
    if (!displayName.empty()) {
        m["labelOverride"] = displayName;
    } else if (!safe) {
        // 旧配置没有独立显示名时保持历史行为，避免菜单变成空白名称。
        m["labelOverride"] = actual;
    }
    if (supports1m) {
        m["supports1m"] = true;
    }
    return m;
}

} // namespace

// 行级解析 settings.yaml：顶层 agent-default-model 的 provider/model，以及
// llm-pi-ai.providers 下每条手写路由的 baseURL / api / apiKeyEnv / 首个
// models 条目 id。best-effort；结构不符的字段留空。
DshSettingsInfo parseDshSettings(std::string_view text) {
    DshSettingsInfo info;
    std::istringstream in{std::string(text)};
    bool inAdm = false, inPi = false, inProv = false, inModels = false;
    int admIndent = -1, piIndent = -1, provIndent = -1;
    int entryIndent = -1, modelsIndent = -1;
    DshProviderEntry* cur = nullptr;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const YamlLine dl = yamlLineOf(line);
        if (dl.blank || dl.comment) continue;
        if (inModels && dl.indent <= modelsIndent) inModels = false;
        if (cur != nullptr && dl.indent <= entryIndent) {
            cur = nullptr;
            inModels = false;
        }
        if (inProv && dl.indent <= provIndent) inProv = false;
        if (inPi && dl.indent <= piIndent) inPi = false;
        if (inAdm && dl.indent <= admIndent) inAdm = false;
        if (inAdm) {
            if (dl.key == "provider") {
                info.defaultProvider = yamlScalar(dl.value);
            } else if (dl.key == "model") {
                info.defaultModel = yamlScalar(dl.value);
            }
            continue;
        }
        if (inModels && cur != nullptr) {
            if (dl.listItem) {
                // "- id: x" 或裸标量 "- x"
                const std::string id =
                    dl.key == "id" || dl.key.empty() ? yamlScalar(dl.value) : "";
                if (!id.empty() && cur->firstModel.empty()) {
                    cur->firstModel = id;
                }
            }
            continue;
        }
        if (cur != nullptr) {
            if (dl.key == "baseURL") {
                cur->baseUrl = yamlScalar(dl.value);
            } else if (dl.key == "api") {
                cur->api = yamlScalar(dl.value);
            } else if (dl.key == "apiKeyEnv") {
                cur->apiKeyEnv = yamlScalar(dl.value);
            } else if (dl.key == "models" && trimBoth(dl.value).empty()) {
                inModels = true;
                modelsIndent = dl.indent;
            }
            continue;
        }
        if (inProv) {
            if (!dl.listItem && !dl.key.empty() && trimBoth(dl.value).empty()) {
                info.providers.push_back(
                    DshProviderEntry{.key = std::string(dl.key)});
                cur = &info.providers.back();
                entryIndent = dl.indent;
            }
            continue;
        }
        if (inPi) {
            if (dl.key == "providers" && trimBoth(dl.value).empty()) {
                inProv = true;
                provIndent = dl.indent;
            }
            continue;
        }
        if (dl.indent == 0) {
            if (dl.key == "agent-default-model") {
                inAdm = true;
                admIndent = 0;
            } else if (dl.key == "llm-pi-ai") {
                inPi = true;
                piIndent = 0;
            }
        }
    }
    return info;
}

// 读 .credentials.yaml 顶层 map 里 envName 对应的密钥值（缺失返回空串）。
std::string readDshCredential(const std::filesystem::path& file,
                              std::string_view envName) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return "";
    std::ifstream in(file, std::ios::binary);
    if (!in) return "";
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const YamlLine dl = yamlLineOf(line);
        if (!dl.listItem && dl.key == envName) return yamlScalar(dl.value);
    }
    return "";
}

// 行级解析 hermes config.yaml：顶层 model 节的 provider/default，以及
// custom_providers 列表每条目的 name/base_url/api_key/api_mode/model 与
// models dict 首个键。best-effort；结构不符的字段留空。
HermesConfigInfo parseHermesConfig(std::string_view text) {
    HermesConfigInfo info;
    std::istringstream in{std::string(text)};
    bool inModel = false, inCustom = false, inModels = false;
    int itemIndent = -1, modelsIndent = -1;
    HermesProviderEntry* cur = nullptr;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const YamlLine dl = yamlLineOf(line);
        if (dl.blank || dl.comment) continue;
        if (inModels && dl.indent <= modelsIndent) inModels = false;
        if (cur != nullptr && dl.indent <= itemIndent) {
            cur = nullptr;
            inModels = false;
        }
        if (inCustom && dl.indent == 0) inCustom = false;
        if (inModel && dl.indent == 0) inModel = false;
        if (inModel) {
            if (dl.key == "provider") {
                info.modelProvider = yamlScalar(dl.value);
            } else if (dl.key == "default") {
                info.modelDefault = yamlScalar(dl.value);
            }
            continue;
        }
        if (cur != nullptr) {
            if (inModels) {
                // models dict 键（quoted 或裸键均可）
                if (cur->firstModel.empty() && !dl.key.empty()) {
                    cur->firstModel = yamlScalar(dl.key);
                }
                continue;
            }
            if (dl.key == "name") {
                cur->name = yamlScalar(dl.value);
            } else if (dl.key == "base_url") {
                cur->baseUrl = yamlScalar(dl.value);
            } else if (dl.key == "api_key") {
                cur->apiKey = yamlScalar(dl.value);
            } else if (dl.key == "api_mode") {
                cur->apiMode = yamlScalar(dl.value);
            } else if (dl.key == "model") {
                cur->model = yamlScalar(dl.value);
            } else if (dl.key == "models" && trimBoth(dl.value).empty()) {
                inModels = true;
                modelsIndent = dl.indent;
            }
            continue;
        }
        if (inCustom) {
            if (dl.listItem) {
                info.providers.push_back(HermesProviderEntry{});
                cur = &info.providers.back();
                itemIndent = dl.indent;
                if (dl.key == "name") cur->name = yamlScalar(dl.value);
            }
            continue;
        }
        if (dl.indent == 0 && trimBoth(dl.value).empty()) {
            if (dl.key == "model") {
                inModel = true;
            } else if (dl.key == "custom_providers") {
                inCustom = true;
            }
        }
    }
    return info;
}

void ProviderStore::switchTo(std::string_view tool, const std::string& id) {
    auto& g = groupRef(tool);
    const models::Provider* target = nullptr;
    for (const auto& p : g.providers) {
        if (p.id == id) target = &p;
    }
    if (target == nullptr) {
        throw std::runtime_error(std::format("供应商不存在：{}", id));
    }
    const std::string baseUrl = models::effectiveBaseUrl(*target);

    if (tool == "claude-code") {
        // 深合并 env 三字段，permissions 等其余字段原样保留。
        const auto file = cfg::claudeSettingsFile();
        nlohmann::json settings = readJsonOrNull(file);
        if (!settings.is_object()) settings = nlohmann::json::object();
        backupLiveFile(tool, file);
        nlohmann::json patch;
        patch["env"]["ANTHROPIC_BASE_URL"] = baseUrl;
        patch["env"]["ANTHROPIC_AUTH_TOKEN"] = target->apiKey;
        if (!target->model.empty()) patch["env"]["ANTHROPIC_MODEL"] = target->model;
        // 三档模型映射（官方 env，均选填）。
        if (!target->haikuModel.empty()) {
            patch["env"]["ANTHROPIC_DEFAULT_HAIKU_MODEL"] = target->haikuModel;
        }
        if (!target->sonnetModel.empty()) {
            patch["env"]["ANTHROPIC_DEFAULT_SONNET_MODEL"] = target->sonnetModel;
        }
        if (!target->opusModel.empty()) {
            patch["env"]["ANTHROPIC_DEFAULT_OPUS_MODEL"] = target->opusModel;
        }
        deepMerge(settings, patch);
        atomicWrite(file, settings.dump(2) + "\n");
    } else if (tool == "codex") {
        // auth.json 只深合并 OPENAI_API_KEY；codexConfigToml 非空时
        // config.toml 整段替换（TOML 不做结构化合并，原文即模板）；model
        // 非空时再行级重写顶层 model 键（其余内容原样保留）。
        const auto authFile = cfg::codexAuthFile();
        nlohmann::json auth = readJsonOrNull(authFile);
        if (!auth.is_object()) auth = nlohmann::json::object();
        backupLiveFile(tool, authFile);
        nlohmann::json patch;
        patch["OPENAI_API_KEY"] = target->apiKey;
        deepMerge(auth, patch);
        atomicWrite(authFile, auth.dump(2) + "\n");
        if (!target->codexConfigToml.empty()) {
            const auto tomlFile = cfg::codexConfigFile();
            backupLiveFile(tool, tomlFile);
            atomicWrite(tomlFile,
                        applyCodexModel(
                            applyCodexBaseUrl(target->codexConfigToml, baseUrl),
                            target->model));
        }
    } else if (tool == "opencode") {
        // additive 模式：往顶层 provider map upsert 本工具条目，其余顶层字段
        // （theme、agent 等）原样保留；model 非空写顶层 model="<id>/<model>"。
        const auto file = cfg::opencodeConfigFile();
        nlohmann::json doc = readJsonStrict(file);  // JSON5 注释 → 抛错，不碰文件
        if (!doc.is_object()) doc = nlohmann::json::object();
        backupLiveFile(tool, file);
        nlohmann::json entry;
        entry["npm"] = opencodeNpmValue(target->apiFormat);
        entry["options"]["baseURL"] = baseUrl;
        entry["options"]["apiKey"] = target->apiKey;
        if (!target->model.empty()) {
            entry["models"][target->model] = nlohmann::json::object();
        }
        if (!doc.contains("provider") || !doc["provider"].is_object()) {
            doc["provider"] = nlohmann::json::object();
        }
        doc["provider"][target->id] = entry;
        if (!target->model.empty()) {
            doc["model"] = target->id + "/" + target->model;
        }
        atomicWrite(file, doc.dump(2) + "\n");
    } else if (tool == "pi") {
        // models.json upsert providers[id]；settings.json 深合并
        // defaultProvider(+defaultModel)。凭据文件权限收紧：目录 0700、文件 0600。
        const auto dir = cfg::piAgentDir();
        restrictPiDir(dir);
        const auto modelsFile = cfg::piModelsFile();
        nlohmann::json models = readJsonOrNull(modelsFile);
        if (!models.is_object()) models = nlohmann::json::object();
        backupLiveFile(tool, modelsFile);
        nlohmann::json entry;
        entry["baseUrl"] = baseUrl;
        entry["apiKey"] = target->apiKey;
        entry["api"] = piApiValue(target->apiFormat);
        if (!target->model.empty()) {
            entry["models"] = nlohmann::json::array({target->model});
        }
        if (!models.contains("providers") || !models["providers"].is_object()) {
            models["providers"] = nlohmann::json::object();
        }
        models["providers"][target->id] = entry;
        atomicWrite(modelsFile, models.dump(2) + "\n");
        restrictPiFile(modelsFile);

        const auto settingsFile = cfg::piSettingsFile();
        nlohmann::json settings = readJsonOrNull(settingsFile);
        if (!settings.is_object()) settings = nlohmann::json::object();
        backupLiveFile(tool, settingsFile);
        nlohmann::json patch;
        patch["defaultProvider"] = target->id;
        if (!target->model.empty()) patch["defaultModel"] = target->model;
        deepMerge(settings, patch);
        atomicWrite(settingsFile, settings.dump(2) + "\n");
        restrictPiFile(settingsFile);
    } else if (tool == "dsh") {
        // settings.yaml：删旧 agent-default-model 与 llmswitch-* 路由后 upsert
        // llmswitch-<id> 条目，并在文件头重建 agent-default-model 指向；密钥
        // 只写 .credentials.yaml（apiKeyEnv 引用）。两份文件都被 dsh 热监听
        // → 切换即时生效，无需重启。
        const auto settingsFile = cfg::dshSettingsFile();
        restrictPiDir(settingsFile.parent_path());
        backupLiveFile(tool, settingsFile);
        std::string text;
        {
            std::error_code ec;
            if (std::filesystem::exists(settingsFile, ec)) {
                text = readTextFile(settingsFile);
            }
        }
        const std::string envName = dshApiKeyEnv(target->id);
        std::vector<std::string> entry;
        entry.push_back("    llmswitch-" + target->id + ":");
        entry.push_back("      displayName: " + yamlQuote(target->name));
        entry.push_back("      api: " + piApiValue(target->apiFormat));
        entry.push_back("      baseURL: " + yamlQuote(baseUrl));
        entry.push_back("      apiKeyEnv: " + envName);
        if (!target->model.empty()) {
            entry.push_back("      models:");
            entry.push_back("        - id: " + yamlQuote(target->model));
        }
        atomicWrite(settingsFile,
                    rewriteDshSettings(text, entry, "llmswitch-" + target->id,
                                       target->model));
        if (!target->apiKey.empty()) {
            const auto credFile = cfg::dshCredentialsFile();
            backupLiveFile(tool, credFile);
            upsertDshCredential(credFile, envName, target->apiKey);
            restrictPiFile(credFile);
        }
    } else if (tool == "hermes") {
        // config.yaml：custom_providers 列表删旧 llmswitch-* 条目后追加新
        // 条目（api_mode 三档映射），顶层 model 节写 provider（总是）与
        // default（model 非空时）；agent / mcp_servers / v12+ providers
        // dict 等其余节原样保留。配置含密钥，目录 0700 / 文件 0600。
        const auto file = cfg::hermesConfigFile();
        restrictPiDir(file.parent_path());
        backupLiveFile(tool, file);
        std::string text;
        {
            std::error_code ec;
            if (std::filesystem::exists(file, ec)) {
                text = readTextFile(file);
            }
        }
        std::vector<std::string> entry;
        entry.push_back("  - name: llmswitch-" + target->id);
        entry.push_back("    base_url: " + yamlQuote(baseUrl));
        entry.push_back("    api_key: " + yamlQuote(target->apiKey));
        entry.push_back("    api_mode: " + hermesApiMode(target->apiFormat));
        if (!target->model.empty()) {
            entry.push_back("    model: " + yamlQuote(target->model));
            entry.push_back("    models:");
            entry.push_back("      " + yamlQuote(target->model) + ": {}");
        }
        atomicWrite(file,
                    rewriteHermesConfig(text, entry, "llmswitch-" + target->id,
                                        target->model));
        restrictPiFile(file);
    } else if (tool == "gemini" || tool == "qwen") {
        // gemini-cli 系（Gemini CLI / Qwen Code）：认证与端点写 <dir>/.env
        // （行级 upsert，其余变量与注释原样保留），auth 类型写 settings.json
        // 深合并。baseUrl 为空 = 回到官方端点（删除覆盖行）。.env 含密钥，
        // 目录 0700 / 文件 0600。
        const bool isGemini = tool == "gemini";
        const auto dir = isGemini ? cfg::geminiDir() : cfg::qwenDir();
        const auto envFile = isGemini ? cfg::geminiEnvFile() : cfg::qwenEnvFile();
        const auto settingsFile =
            isGemini ? cfg::geminiSettingsFile() : cfg::qwenSettingsFile();
        const std::string keyVar = isGemini ? "GEMINI_API_KEY" : "OPENAI_API_KEY";
        const std::string baseVar =
            isGemini ? "GOOGLE_GEMINI_BASE_URL" : "OPENAI_BASE_URL";
        const std::string modelVar = isGemini ? "GEMINI_MODEL" : "OPENAI_MODEL";
        const std::string authType = isGemini ? "gemini-api-key" : "openai";
        restrictPiDir(dir);
        backupLiveFile(tool, envFile);
        writeEnvValues(envFile,
                       {{keyVar, target->apiKey},
                        {baseVar, baseUrl},
                        {modelVar, target->model}});
        restrictPiFile(envFile);
        nlohmann::json settings = readJsonOrNull(settingsFile);
        if (!settings.is_object()) settings = nlohmann::json::object();
        backupLiveFile(tool, settingsFile);
        nlohmann::json patch;
        patch["security"]["auth"]["selectedType"] = authType;
        deepMerge(settings, patch);
        atomicWrite(settingsFile, settings.dump(2) + "\n");
    } else if (tool == "zcode") {
        // 启用目标条目（原生条目原位合并后置 true），并停用其余本应用
        // 托管（llmswitch:*）条目。builtin:* 与 ZCode 原生自建条目不动：
        // ZCode 允许多条目同时启用，其自有条目的启停由用户在 ZCode 侧
        // 管理，本应用不得代管。
        const auto file = cfg::zcodeConfigFile();
        nlohmann::json doc = readJsonOrNull(file);
        if (!doc.is_object()) doc = nlohmann::json::object();
        if (!doc.contains("provider") || !doc["provider"].is_object()) {
            doc["provider"] = nlohmann::json::object();
        }
        backupLiveFile(tool, file);
        auto& providers = doc["provider"];
        const std::string entryKey = zcodeEntryKeyFor(providers, target->id);
        nlohmann::json entry =
            buildZcodeEntry(*target, baseUrl);
        const auto existing = providers.find(entryKey);
        if (existing != providers.end() && existing->is_object()) {
            entry = mergeZcodeEntry(*existing, entry);
        }
        entry["enabled"] = true;
        providers[entryKey] = std::move(entry);
        for (auto it = providers.begin(); it != providers.end(); ++it) {
            if (it.key() != entryKey && it.key().starts_with("llmswitch:") &&
                it.value().is_object()) {
                it.value()["enabled"] = false;
            }
        }
        atomicWrite(file, doc.dump(2) + "\n");
    } else if (tool == "claude") {
        // Claude Desktop 3p 直连（对齐 cc-switch）：Linux 不支持。
        const auto baseDir = cfg::claudeDesktopDir();
        if (baseDir.empty()) {
            throw std::runtime_error(
                "Claude Desktop 不支持 Linux（仅 macOS / Windows）");
        }
        const auto threepDir = cfg::claudeDesktop3pDir();
        // 两份 claude_desktop_config.json（正常目录 + 3p 目录）都置
        // deploymentMode=3p，其余字段保留。
        for (const auto& file :
             {baseDir / "claude_desktop_config.json",
              threepDir / "claude_desktop_config.json"}) {
            nlohmann::json doc = readJsonOrNull(file);
            if (!doc.is_object()) doc = nlohmann::json::object();
            backupLiveFile(tool, file);
            nlohmann::json patch;
            patch["deploymentMode"] = "3p";
            deepMerge(doc, patch);
            atomicWrite(file, doc.dump(2) + "\n");
        }
        // configLibrary 下固定 id 的 profile（网关字段 + 可选模型标签）。
        const auto profileFile = claudeDesktopProfileFile();
        nlohmann::json profile;
        profile["coworkEgressAllowedHosts"] = nlohmann::json::array({"*"});
        profile["disableDeploymentModeChooser"] = true;
        profile["inferenceGatewayApiKey"] = target->apiKey;
        profile["inferenceGatewayAuthScheme"] = "bearer";
        profile["inferenceGatewayBaseUrl"] = baseUrl;
        profile["inferenceProvider"] = "gateway";
        // inferenceModels：主模型（直连官方时通常就这一条）+ 三档映射（每档
        // 映射到供应商真实模型名）。name 必须是桌面端白名单 route id（见
        // claudeDesktopModelEntry / isClaudeSafeModelId）。
        nlohmann::json inferenceModels = nlohmann::json::array();
        if (!target->model.empty()) {
            inferenceModels.push_back(
                claudeDesktopModelEntry(target->model, "claude-sonnet-4-6", {},
                                        target->modelSupports1m));
        }
        if (!target->haikuModel.empty()) {
            inferenceModels.push_back(
                claudeDesktopModelEntry(target->haikuModel, "claude-haiku-4-5",
                                        target->haikuDisplayName,
                                        target->haikuSupports1m));
        }
        if (!target->sonnetModel.empty()) {
            inferenceModels.push_back(
                claudeDesktopModelEntry(target->sonnetModel, "claude-sonnet-4-6",
                                        target->sonnetDisplayName,
                                        target->sonnetSupports1m));
        }
        if (!target->opusModel.empty()) {
            inferenceModels.push_back(
                claudeDesktopModelEntry(target->opusModel, "claude-opus-4-8",
                                        target->opusDisplayName,
                                        target->opusSupports1m));
        }
        if (!inferenceModels.empty()) {
            profile["inferenceModels"] = std::move(inferenceModels);
        }
        backupLiveFile(tool, profileFile);
        atomicWrite(profileFile, profile.dump(2) + "\n");
        // _meta.json：注册条目（同名去重）并指为 appliedId。
        const auto metaFile = profileFile.parent_path() / "_meta.json";
        nlohmann::json meta = readJsonOrNull(metaFile);
        if (!meta.is_object()) meta = nlohmann::json::object();
        backupLiveFile(tool, metaFile);
        nlohmann::json entries = nlohmann::json::array();
        if (meta.contains("entries") && meta["entries"].is_array()) {
            for (const auto& e : meta["entries"]) {
                if (jsonStr(e, "id") != kClaudeDesktopProfileId) {
                    entries.push_back(e);
                }
            }
        }
        nlohmann::json self;
        self["id"] = kClaudeDesktopProfileId;
        self["name"] = kClaudeDesktopProfileName;
        entries.push_back(self);
        meta["entries"] = entries;
        meta["appliedId"] = kClaudeDesktopProfileId;
        atomicWrite(metaFile, meta.dump(2) + "\n");
    } else {
        throw std::runtime_error(std::format("未知的工具：{}", tool));
    }

    g.current = id;
    save();
}

void ProviderStore::restoreOfficial(std::string_view tool) {
    auto& g = groupRef(tool);  // 未注册工具抛错

    if (tool == "claude-code") {
        // 回到官方 OAuth 登录：撤掉 env 块里本应用写入的三个覆盖键，其余
        // env 键与 permissions 等字段原样保留。
        const auto file = cfg::claudeSettingsFile();
        std::error_code ec;
        if (std::filesystem::exists(file, ec)) {
            nlohmann::json settings = readJsonOrNull(file);
            if (settings.is_object()) {
                backupLiveFile(tool, file);
                if (settings.contains("env") && settings["env"].is_object()) {
                    auto& env = settings["env"];
                    env.erase("ANTHROPIC_BASE_URL");
                    env.erase("ANTHROPIC_AUTH_TOKEN");
                    env.erase("ANTHROPIC_MODEL");
                    env.erase("ANTHROPIC_DEFAULT_HAIKU_MODEL");
                    env.erase("ANTHROPIC_DEFAULT_SONNET_MODEL");
                    env.erase("ANTHROPIC_DEFAULT_OPUS_MODEL");
                }
                atomicWrite(file, settings.dump(2) + "\n");
            }
        }
    } else if (tool == "codex") {
        // 回到官方 ChatGPT 登录：auth.json 删 OPENAI_API_KEY（tokens 等其余
        // 字段保留；删完为空对象则删文件）。config.toml 仅当内容与组内某
        // provider 的 codexConfigToml 完全一致（本应用写入且未被手改）才删，
        // 用户手改过的文件不动。
        const auto authFile = cfg::codexAuthFile();
        std::error_code ec;
        if (std::filesystem::exists(authFile, ec)) {
            nlohmann::json auth = readJsonOrNull(authFile);
            if (auth.is_object()) {
                backupLiveFile(tool, authFile);
                auth.erase("OPENAI_API_KEY");
                if (auth.empty()) {
                    std::filesystem::remove(authFile, ec);
                } else {
                    atomicWrite(authFile, auth.dump(2) + "\n");
                }
            }
        }
        const auto tomlFile = cfg::codexConfigFile();
        if (std::filesystem::exists(tomlFile, ec)) {
            const std::string current = readTextFile(tomlFile);
            for (const auto& p : g.providers) {
                if (!p.codexConfigToml.empty() &&
                    (current == p.codexConfigToml ||
                     current == applyCodexModel(p.codexConfigToml, p.model))) {
                    backupLiveFile(tool, tomlFile);
                    std::filesystem::remove(tomlFile, ec);
                    break;
                }
            }
        }
    } else if (tool == "gemini" || tool == "qwen") {
        // 回到官方认证：.env 删本应用写入的三行（其余变量与注释原样保留），
        // settings.json 删 security.auth.selectedType（其余字段保留）。
        const auto envFile = tool == "gemini" ? cfg::geminiEnvFile() : cfg::qwenEnvFile();
        const std::string baseVar =
            tool == "gemini" ? "GOOGLE_GEMINI_BASE_URL" : "OPENAI_BASE_URL";
        const std::string keyVar = tool == "gemini" ? "GEMINI_API_KEY" : "OPENAI_API_KEY";
        const std::string modelVar = tool == "gemini" ? "GEMINI_MODEL" : "OPENAI_MODEL";
        std::error_code ec;
        if (std::filesystem::exists(envFile, ec)) {
            backupLiveFile(tool, envFile);
            writeEnvValues(envFile, {{keyVar, ""}, {baseVar, ""}, {modelVar, ""}});
        }
        const auto settingsFile =
            tool == "gemini" ? cfg::geminiSettingsFile() : cfg::qwenSettingsFile();
        if (std::filesystem::exists(settingsFile, ec)) {
            nlohmann::json settings = readJsonOrNull(settingsFile);
            if (settings.is_object() && settings.contains("security") &&
                settings["security"].is_object() &&
                settings["security"].contains("auth") &&
                settings["security"]["auth"].is_object()) {
                backupLiveFile(tool, settingsFile);
                settings["security"]["auth"].erase("selectedType");
                atomicWrite(settingsFile, settings.dump(2) + "\n");
            }
        }
    } else if (tool == "zcode") {
        // 关闭本应用写入的 llmswitch:* 条目，重新启用第一个内置（builtin:*）
        // provider；无内置条目时保持现状，用户可在 ZCode 内自选。
        const auto file = cfg::zcodeConfigFile();
        std::error_code ec;
        if (std::filesystem::exists(file, ec)) {
            nlohmann::json doc = readJsonOrNull(file);
            if (doc.is_object() && doc.contains("provider") &&
                doc["provider"].is_object()) {
                backupLiveFile(tool, file);
                bool enabledBuiltin = false;
                for (auto it = doc["provider"].begin();
                     it != doc["provider"].end(); ++it) {
                    if (!it.value().is_object()) continue;
                    if (it.key().starts_with("llmswitch:")) {
                        it.value()["enabled"] = false;
                    } else if (!enabledBuiltin &&
                               it.key().starts_with("builtin:")) {
                        it.value()["enabled"] = true;
                        enabledBuiltin = true;
                    }
                }
                atomicWrite(file, doc.dump(2) + "\n");
            }
        }
    } else if (tool == "dsh") {
        // 回到内置 deepseek-official 路由：删 settings.yaml 的
        // agent-default-model 块与 llmswitch-* 手写路由，其余键保留。
        // .credentials.yaml 里的 LLMSWITCH_* 密钥无引用即无害，不代清。
        const auto settingsFile = cfg::dshSettingsFile();
        std::error_code ec;
        if (std::filesystem::exists(settingsFile, ec)) {
            backupLiveFile(tool, settingsFile);
            atomicWrite(settingsFile,
                        rewriteDshSettings(readTextFile(settingsFile), {}, "",
                                           ""));
        }
    } else if (tool == "claude") {
        // 撤掉 3p 直连：两份 claude_desktop_config.json 删 deploymentMode 键；
        // _meta.json 移除本应用 profile 条目并清 appliedId（profile 文件本体
        // 保留，未被引用即无害）。Linux 不支持。
        const auto baseDir = cfg::claudeDesktopDir();
        if (baseDir.empty()) {
            throw std::runtime_error(
                "Claude Desktop 不支持 Linux（仅 macOS / Windows）");
        }
        const auto threepDir = cfg::claudeDesktop3pDir();
        std::error_code ec;
        for (const auto& file :
             {baseDir / "claude_desktop_config.json",
              threepDir / "claude_desktop_config.json"}) {
            if (!std::filesystem::exists(file, ec)) continue;
            nlohmann::json doc = readJsonOrNull(file);
            if (!doc.is_object()) continue;
            backupLiveFile(tool, file);
            doc.erase("deploymentMode");
            atomicWrite(file, doc.dump(2) + "\n");
        }
        const auto profileFile = claudeDesktopProfileFile();
        const auto metaFile = profileFile.parent_path() / "_meta.json";
        if (std::filesystem::exists(metaFile, ec)) {
            nlohmann::json meta = readJsonOrNull(metaFile);
            if (meta.is_object()) {
                backupLiveFile(tool, metaFile);
                if (meta.contains("entries") && meta["entries"].is_array()) {
                    nlohmann::json entries = nlohmann::json::array();
                    for (const auto& e : meta["entries"]) {
                        if (jsonStr(e, "id") != kClaudeDesktopProfileId) {
                            entries.push_back(e);
                        }
                    }
                    meta["entries"] = entries;
                }
                if (jsonStr(meta, "appliedId") == kClaudeDesktopProfileId) {
                    meta.erase("appliedId");
                }
                atomicWrite(metaFile, meta.dump(2) + "\n");
            }
        }
    } else {
        const auto* spec = models::findTool(tool);
        throw std::runtime_error(std::format(
            "{} 没有官方默认状态可恢复",
            spec != nullptr ? spec->displayName : tool));
    }

    g.current.clear();
    save();
}

} // namespace store
