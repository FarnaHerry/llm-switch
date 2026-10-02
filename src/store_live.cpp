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

// pi 的 models.json 模型清单条目取 id：裸标量（"m1"）与对象（{"id": "m1"} /
// {"name": "m1"}）两种形状都认。写侧要按原形状补条目，读侧只要 id。
std::string modelEntryId(const nlohmann::json& element) {
    if (element.is_string()) return element.get<std::string>();
    if (!element.is_object()) return "";
    const std::string id = jsonStr(element, "id");
    return id.empty() ? jsonStr(element, "name") : id;
}

bool modelListContains(const nlohmann::json& list, std::string_view id) {
    if (!list.is_array()) return false;
    for (const auto& item : list) {
        if (modelEntryId(item) == id) return true;
    }
    return false;
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

// 小节结束：上面的映射是模块内（匿名命名空间）的写侧助手；反映射
// piApiFormatValue 是模块链接的（store.cppm 声明、import 侧与 dsh 增量同步
// 共用），所以定义在匿名命名空间之外。
} // namespace

// pi/dsh 的 api 字段反映射（读侧：live 的 api 拼写 → apiFormat 三档）。
// 未知拼写返回空串（= 未知协议，不猜）。
std::string piApiFormatValue(std::string_view api) {
    if (api == "anthropic-messages") return "anthropic";
    if (api == "openai-responses") return "openai-responses";
    if (api == "openai-completions") return "openai-chat";
    return "";
}

namespace {

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

// ---- flow 风格 providers 块的摊平 -------------------------------------------
// settings.yaml 的 llm-pi-ai.providers 也可能是 flow 风格（用户手写或其它
// 工具写入）：
//     llm-pi-ai:
//       providers:
//         {
//           c: { apiKeyEnv: C_API_KEY, api: openai-responses, ... }
//         }
// 本文件的行级改写只理解块风格（靠缩进定层级、靠 `key:` 行删条目）：直接往
// flow 块之后插块条目会写出非法 YAML，DSH 的 watcher 会保留上一份好文档，
// 用户的切换于是静默失效。读写前先把这段 flow 值摊平成块风格——只改排版，
// 不改任何键值内容（flow 区域内部的注释会随之丢失，DSH 生成的文件没有注释）。

// flow 节点树：标量保留原文（含引号），键保留原文。
struct FlowValue {
    enum class Kind { Scalar, Map, Seq };
    Kind kind = Kind::Scalar;
    std::string scalar;
    std::vector<std::pair<std::string, FlowValue>> entries;  // Map
    std::vector<FlowValue> items;                            // Seq
};

// flow 文本的最小递归下降解析器：只处理 providers 这一类形状（标量 / 映射 /
// 序列，可任意嵌套），不做锚点、标签、多行折叠等完整 YAML 语义。
class FlowParser {
public:
    explicit FlowParser(std::string_view text) : text_(text) {}

    std::optional<FlowValue> Parse() {
        auto node = ParseNode();
        if (!node.has_value()) return std::nullopt;
        SkipSpace();
        if (pos_ != text_.size()) return std::nullopt;  // 尾部还有内容
        return node;
    }

private:
    std::optional<FlowValue> ParseNode() {
        SkipSpace();
        if (pos_ >= text_.size()) return std::nullopt;
        if (text_[pos_] == '{') return ParseMap();
        if (text_[pos_] == '[') return ParseSeq();
        return ParseScalar();
    }

    std::optional<FlowValue> ParseMap() {
        FlowValue node;
        node.kind = FlowValue::Kind::Map;
        ++pos_;  // '{'
        SkipSpace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return node;
        }
        for (;;) {
            SkipSpace();
            auto key = ParseKey();
            if (!key.has_value()) return std::nullopt;
            SkipSpace();
            if (pos_ >= text_.size() || text_[pos_] != ':') return std::nullopt;
            ++pos_;
            auto value = ParseNode();
            if (!value.has_value()) return std::nullopt;
            node.entries.emplace_back(std::move(*key), std::move(*value));
            SkipSpace();
            if (pos_ < text_.size() && text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < text_.size() && text_[pos_] == '}') {
                ++pos_;
                return node;
            }
            return std::nullopt;
        }
    }

    std::optional<FlowValue> ParseSeq() {
        FlowValue node;
        node.kind = FlowValue::Kind::Seq;
        ++pos_;  // '['
        SkipSpace();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return node;
        }
        for (;;) {
            auto item = ParseNode();
            if (!item.has_value()) return std::nullopt;
            node.items.push_back(std::move(*item));
            SkipSpace();
            if (pos_ < text_.size() && text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < text_.size() && text_[pos_] == ']') {
                ++pos_;
                return node;
            }
            return std::nullopt;
        }
    }

    // 键：引号原样保留，裸键读到 ':'（冒号前只有空白才算键）。
    std::optional<std::string> ParseKey() {
        if (pos_ >= text_.size()) return std::nullopt;
        if (text_[pos_] == '"' || text_[pos_] == '\'') return ParseQuoted();
        const std::size_t begin = pos_;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ':' || c == ',' || c == '}' || c == ']' || c == '\n') break;
            ++pos_;
        }
        if (pos_ >= text_.size() || text_[pos_] != ':') return std::nullopt;
        std::string key{trimBoth(text_.substr(begin, pos_ - begin))};
        if (key.empty()) return std::nullopt;
        return key;
    }

    // 标量：引号原样保留；裸标量读到 flow 分隔符，内部换行折成单个空格。
    std::optional<FlowValue> ParseScalar() {
        FlowValue node;
        node.kind = FlowValue::Kind::Scalar;
        if (text_[pos_] == '"' || text_[pos_] == '\'') {
            auto quoted = ParseQuoted();
            if (!quoted.has_value()) return std::nullopt;
            node.scalar = std::move(*quoted);
            return node;
        }
        bool space = false;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ',' || c == '}' || c == ']') break;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                space = !node.scalar.empty();
                ++pos_;
                continue;
            }
            if (space) {
                node.scalar += ' ';
                space = false;
            }
            node.scalar += c;
            ++pos_;
        }
        return node;
    }

    // 引号串：返回含首尾引号的原文。
    std::optional<std::string> ParseQuoted() {
        const char quote = text_[pos_];
        const std::size_t begin = pos_;
        ++pos_;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (quote == '"' && c == '\\' && pos_ + 1 < text_.size()) {
                pos_ += 2;
                continue;
            }
            if (quote == '\'' && c == '\'' && pos_ + 1 < text_.size() &&
                text_[pos_ + 1] == '\'') {
                pos_ += 2;  // 单引号串里的 '' 是转义的单引号
                continue;
            }
            ++pos_;
            if (c == quote) {
                return std::string(text_.substr(begin, pos_ - begin));
            }
        }
        return std::nullopt;
    }

    // 跳过空白与注释（flow 内的注释由 eemeli/yaml 写不出，这里只求不误判）。
    void SkipSpace() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
                continue;
            }
            if (c == '#') {
                while (pos_ < text_.size() && text_[pos_] != '\n') ++pos_;
                continue;
            }
            break;
        }
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

std::string flowIndent(int columns) {
    return std::string(static_cast<std::size_t>(columns), ' ');
}

// 裸标量是不是 YAML null（`null` / `Null` / `NULL` / `~`，以及本来就是空的
// 值）。null 与「没有值」是同一个值，所以摊平时统一写成空值：既不改语义，
// 又与本应用写入口径一致（推理档位里的 "off": 就是「不发思考参数」）。
// 带引号的 "null" 是字符串，按原样保留。
bool flowScalarIsNull(const std::string& scalar) {
    if (scalar.empty()) return true;
    if (scalar.front() == '"' || scalar.front() == '\'') return false;
    return scalar == "null" || scalar == "Null" || scalar == "NULL" ||
           scalar == "~";
}

void EmitFlowNode(const FlowValue& node, int indent,
                  std::vector<std::string>& out);

// 一条映射条目（dash = 作为序列项的首条，写成 "- key: value"）。
void EmitFlowEntry(const std::string& key, const FlowValue& value, int indent,
                   bool dash, std::vector<std::string>& out) {
    const std::string prefix =
        flowIndent(indent) + (dash ? std::string("- ") : std::string());
    if (value.kind == FlowValue::Kind::Scalar) {
        out.push_back(flowScalarIsNull(value.scalar)
                          ? prefix + key + ":"
                          : prefix + key + ": " + value.scalar);
        return;
    }
    if (value.entries.empty() && value.items.empty()) {
        out.push_back(prefix + key + ": " +
                      (value.kind == FlowValue::Kind::Map ? "{}" : "[]"));
        return;
    }
    out.push_back(prefix + key + ":");
    EmitFlowNode(value, indent + 2, out);
}

void EmitFlowNode(const FlowValue& node, int indent,
                  std::vector<std::string>& out) {
    if (node.kind == FlowValue::Kind::Map) {
        for (const auto& [key, value] : node.entries) {
            EmitFlowEntry(key, value, indent, false, out);
        }
        return;
    }
    for (const auto& item : node.items) {
        if (item.kind == FlowValue::Kind::Scalar) {
            out.push_back(flowScalarIsNull(item.scalar)
                              ? flowIndent(indent) + "-"
                              : flowIndent(indent) + "- " + item.scalar);
        } else if (item.kind == FlowValue::Kind::Map && !item.entries.empty()) {
            // 映射项的首条跟 "- " 同行，其余缩进两列，块风格列表的惯例写法。
            EmitFlowEntry(item.entries.front().first, item.entries.front().second,
                          indent, true, out);
            for (std::size_t i = 1; i < item.entries.size(); ++i) {
                EmitFlowEntry(item.entries[i].first, item.entries[i].second,
                              indent + 2, false, out);
            }
        } else if (item.entries.empty() && item.items.empty()) {
            out.push_back(flowIndent(indent) + "- " +
                          (item.kind == FlowValue::Kind::Map ? "{}" : "[]"));
        } else {
            out.push_back(flowIndent(indent) + "-");
            EmitFlowNode(item, indent + 2, out);
        }
    }
}

// 把 llm-pi-ai.providers 的 flow 值摊平成块风格；没有这种形状时原样返回。
std::string normalizeDshFlowProviders(std::string_view text) {
    std::vector<std::string> lines;
    {
        std::istringstream in{std::string(text)};
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(std::move(line));
        }
    }
    // 找顶层 llm-pi-ai: 之后的第一条 providers。
    std::size_t prov = lines.size();
    for (std::size_t i = 0; i < lines.size() && prov == lines.size(); ++i) {
        const YamlLine dl = yamlLineOf(lines[i]);
        if (dl.blank || dl.comment || dl.indent != 0) continue;
        if (dl.key != "llm-pi-ai") continue;
        for (std::size_t j = i + 1; j < lines.size(); ++j) {
            const YamlLine n = yamlLineOf(lines[j]);
            if (n.blank || n.comment) continue;
            if (n.indent == 0) break;  // 出了 llm-pi-ai 块
            if (n.key == "providers") {
                prov = j;
            }
            break;
        }
    }
    if (prov == lines.size()) return std::string(text);

    const YamlLine dl = yamlLineOf(lines[prov]);
    const std::string value = std::string(trimBoth(dl.value));
    std::size_t startLine = prov;
    std::size_t open = 0;
    if (value.empty()) {
        // providers: 之后另起一行写 flow 块。
        std::size_t k = prov + 1;
        while (k < lines.size()) {
            const YamlLine n = yamlLineOf(lines[k]);
            if (n.blank || n.comment) {
                ++k;
                continue;
            }
            break;
        }
        if (k >= lines.size()) return std::string(text);
        const auto trimmed = trimLeft(lines[k]);
        if (trimmed.empty() || trimmed.front() != '{') return std::string(text);
        startLine = k;
        open = lines[k].size() - trimmed.size();
    } else {
        if (value.front() != '{') return std::string(text);
        const auto colon = lines[prov].find(':');
        const auto brace = lines[prov].find('{', colon);
        if (brace == std::string::npos) return std::string(text);
        if (!trimBoth(lines[prov].substr(colon + 1, brace - colon - 1)).empty()) {
            return std::string(text);  // 冒号与 '{' 之间有别的记号，不认
        }
        open = brace;
    }

    // 收集整段 flow 文本并定位配对的 '}'。
    std::string flow;
    int depth = 0;
    std::size_t endLine = startLine;
    std::size_t afterClose = lines[startLine].size();
    char quote = '\0';
    bool done = false;
    for (std::size_t i = startLine; i < lines.size() && !done; ++i) {
        const std::string& line = lines[i];
        for (std::size_t c = (i == startLine ? open : 0); c < line.size(); ++c) {
            const char ch = line[c];
            if (quote != '\0') {
                flow += ch;
                if (quote == '"' && ch == '\\' && c + 1 < line.size()) {
                    flow += line[++c];
                    continue;
                }
                if (ch == quote) quote = '\0';
                continue;
            }
            if (ch == '"' || ch == '\'') {
                quote = ch;
                flow += ch;
                continue;
            }
            if (ch == '#') break;  // 行尾注释不进 flow 文本
            flow += ch;
            if (ch == '{' || ch == '[') ++depth;
            if (ch == '}' || ch == ']') {
                --depth;
                if (depth == 0) {
                    endLine = i;
                    afterClose = c + 1;
                    done = true;
                    break;
                }
            }
        }
        if (!done && i > startLine) flow += '\n';
    }
    if (!done || depth != 0) return std::string(text);

    FlowParser parser(flow);
    auto node = parser.Parse();
    if (!node.has_value() || node->kind != FlowValue::Kind::Map) {
        return std::string(text);
    }

    // 摊平：只替换这段 flow 文本，前后行原样保留。
    std::vector<std::string> out;
    out.reserve(lines.size());
    for (std::size_t i = 0; i < prov; ++i) out.push_back(lines[i]);
    const std::string head =
        startLine == prov
            ? std::string(trimRight(lines[prov].substr(0, open)))
            : std::string(trimRight(lines[prov]));
    out.push_back(head);
    EmitFlowNode(*node, dl.indent + 2, out);
    const std::string tail =
        std::string(trimRight(lines[endLine].substr(afterClose)));
    if (!trimBoth(tail).empty()) out.push_back(tail);
    for (std::size_t i = endLine + 1; i < lines.size(); ++i) {
        out.push_back(lines[i]);
    }

    std::string result;
    for (const auto& l : out) {
        result += l;
        result += '\n';
    }
    return result;
}

// 行级改写 settings.yaml（dsh 增量多供应商）：
//   - removeKeys 里的条目从 llm-pi-ai.providers 删除（调用方负责算全：
//     本应用将重建的 llmswitch-* 键、被接管的裸键、孤儿 llmswitch-*）；
//   - entries 非空时在 providers 块尾按顺序插入（每条以 4 列基准缩进生成，
//     缩进随实际 providers 行调整）；
//   - touchDefault 为真时删掉顶层 agent-default-model 块并在文件头重建
//     （defaultProvider 为空 = 只删不建，回到内置官方路由）。
// 其余键、手写条目、注释与顺序原样保留；flow 风格的 providers 值先摊平成
// 块风格（行级逻辑只懂块风格）。
std::string rewriteDshSettings(
    std::string_view text,
    const std::vector<std::vector<std::string>>& entries,
    const std::set<std::string>& removeKeys, bool touchDefault,
    std::string_view defaultProvider, std::string_view defaultModel,
    std::string_view defaultReasoningEffort) {
    // flow 风格的 providers 值先摊平成块风格，后面的行级逻辑才成立。
    const std::string blockText = normalizeDshFlowProviders(text);
    text = blockText;
    std::vector<std::string> lines;
    {
        std::istringstream in{std::string(text)};
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(std::move(line));
        }
    }
    std::vector<std::string> out;
    bool inserted = entries.empty();
    bool inPi = false, inProv = false, sawPi = false, sawProv = false;
    int piIndent = -1, provIndent = -1, entryIndent = -1;
    bool skipping = false;
    int skipIndent = -1;

    const auto insertEntries = [&](int baseIndent) {
        if (inserted) return;
        const int shift = baseIndent - 4;
        for (const auto& e : entries) {
            for (const auto& line : e) {
                if (shift >= 0) {
                    out.push_back(
                        std::string(static_cast<std::size_t>(shift), ' ') + line);
                } else {
                    out.push_back(line.substr(static_cast<std::size_t>(-shift)));
                }
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
                    if (removeKeys.find(std::string(dl.key)) !=
                        removeKeys.end()) {
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
                if (touchDefault && dl.key == "agent-default-model") {
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
    if (touchDefault && !defaultProvider.empty()) {
        std::vector<std::string> head;
        head.push_back("agent-default-model:");
        head.push_back("  provider: " + std::string(defaultProvider));
        if (!defaultModel.empty()) {
            head.push_back("  model: " + yamlQuote(defaultModel));
        }
        // dsh 的推理等级是模型选择状态的一部分（官方键 reasoningEffort）：
        // 本应用不建模它，但切换只是换 provider/model，用户选的等级要原样
        // 带回去，否则每切一次就被清掉。
        if (!defaultReasoningEffort.empty()) {
            head.push_back("  reasoningEffort: " +
                           std::string(defaultReasoningEffort));
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

// 一条 llmswitch-<id> 条目的行（4 列基准缩进，写侧唯一来源）：模型条目写
// dsh 官方能力字段（与官方设置页编辑的是同一组）——contextWindow / maxTokens
// 容量、input 请求模态、reasoningEfforts 推理档位。档位键固定加引号
// （off/low 在部分解析器里会被当成布尔字面量），"off" 留空 = 不发思考参数，
// 与 dsh 官方文档示例一致。
std::vector<std::string> dshEntryLines(const std::string& id,
                                       const models::Provider& p,
                                       const std::string& baseUrl) {
    std::vector<std::string> entry;
    entry.push_back("    llmswitch-" + id + ":");
    entry.push_back("      displayName: " + yamlQuote(p.name));
    entry.push_back("      api: " + piApiValue(p.apiFormat));
    entry.push_back("      baseURL: " + yamlQuote(baseUrl));
    entry.push_back("      apiKeyEnv: " + dshApiKeyEnv(id));
    if (!p.model.empty()) {
        entry.push_back("      models:");
        entry.push_back("        - id: " + yamlQuote(p.model));
        if (p.contextWindow > 0) {
            entry.push_back("          contextWindow: " +
                            std::to_string(p.contextWindow));
        }
        if (p.maxTokens > 0) {
            entry.push_back("          maxTokens: " +
                            std::to_string(p.maxTokens));
        }
        if (const auto modalities =
                models::normalizeInputModalities(p.inputModalities);
            !modalities.empty()) {
            std::string list;
            for (const auto& modality : modalities) {
                if (!list.empty()) list += ", ";
                list += modality;
            }
            entry.push_back("          input: [" + list + "]");
        }
        if (const auto efforts =
                models::normalizeReasoningEfforts(p.reasoningEfforts);
            !efforts.empty()) {
            entry.push_back("          reasoningEfforts:");
            for (const auto& level : efforts) {
                entry.push_back("            " + yamlQuote(level) +
                                (level == "off" ? ":" : ": " + level));
            }
        }
    }
    return entry;
}

// ---- .credentials.yaml（dsh 的凭据文档）-------------------------------------
//
// 这份文档有两种布局，引用名该写在哪一层完全不同：
//   - 版本 1（当前 build 读写的布局）：顶层只有 `version` / `refs` /
//     `records` 三个键，引用名在 `refs:` **块内**（缩进 2）；
//   - 预发布 flat 布局（没有 `version` 键，引用名直接就是顶层键）：dsh 自己
//     首次写入时会迁移成版本 1。
// 引用名写错层级——往版本 1 文档的顶层追加 `LLMSWITCH_<ID>: "..."`——会让 dsh
// 直接拒绝**整份文档**（`credentials-local: unknown top-level key "..."`），
// 于是所有密钥一起失效，连用户自己存的 C_API_KEY 都读不出来，路由因此完全
// 不可用。所以这里先认布局再落笔，并把旧版本写错的 `LLMSWITCH_*` 顶层残留
// 迁回 `refs:` 块（值原样搬，不丢凭据）。写前调用方负责 backupLiveFile。

// 顶层键扫描结果。
struct CredDocShape {
    bool hasVersion = false;
    bool hasRecords = false;
    int refsIdx = -1;  // `refs:` 行下标（缩进必须为 0），无则 -1
};

CredDocShape scanCredDocShape(const std::vector<std::string>& lines) {
    CredDocShape shape;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const YamlLine dl = yamlLineOf(lines[i]);
        if (dl.blank || dl.comment || dl.listItem || dl.indent != 0) continue;
        if (dl.key == "version") shape.hasVersion = true;
        if (dl.key == "records") shape.hasRecords = true;
        if (dl.key == "refs") shape.refsIdx = static_cast<int>(i);
    }
    return shape;
}

// `refs:` 块的结束下标：块内之后的第一个顶层（缩进 0）非空行，或文件尾。
std::size_t credRefsBlockEnd(const std::vector<std::string>& lines, int refsIdx) {
    for (std::size_t i = static_cast<std::size_t>(refsIdx) + 1; i < lines.size(); ++i) {
        const YamlLine dl = yamlLineOf(lines[i]);
        if (dl.blank || dl.comment) continue;
        if (dl.indent == 0) return i;
    }
    return lines.size();
}

// 行级 upsert 一条凭据：按文档布局写进 `refs:` 块（版本 1）或顶层（flat）。
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
    const std::string newLine = std::string(envName) + ": " + yamlQuote(apiKey);

    // 空文档（文件不存在或全是空行/注释）：直接写成 dsh 当前的版本 1 布局，
    // 不留 flat 布局让 dsh 事后迁移。
    bool hasContent = false;
    for (const auto& line : lines) {
        const YamlLine dl = yamlLineOf(line);
        if (!dl.blank && !dl.comment) {
            hasContent = true;
            break;
        }
    }
    if (!hasContent) {
        // 纯注释/空文件：注释原样留下，再补版本 1 的骨架。
        std::string out;
        for (const auto& line : lines) {
            out += line;
            out += '\n';
        }
        out += "version: 1\nrefs:\n  " + newLine + "\n";
        atomicWrite(file, out);
        return;
    }

    // 旧版本把引用名写到了版本 1 文档的顶层——先摘出来（有 `refs:` 块才摘，
    // flat 布局的顶层引用名本来就是对的），随后搬进 refs 块。
    std::vector<std::pair<std::string, std::string>> stray;
    if (scanCredDocShape(lines).refsIdx >= 0) {
        std::vector<std::string> kept;
        kept.reserve(lines.size());
        for (auto& line : lines) {
            const YamlLine dl = yamlLineOf(line);
            if (!dl.blank && !dl.comment && !dl.listItem && dl.indent == 0 &&
                dl.key.starts_with("LLMSWITCH_")) {
                stray.emplace_back(std::string(dl.key), yamlScalar(dl.value));
                continue;
            }
            kept.push_back(std::move(line));
        }
        lines = std::move(kept);
    }

    const CredDocShape shape = scanCredDocShape(lines);
    if (shape.refsIdx < 0 && !shape.hasVersion) {
        // 预发布 flat 布局：引用名就是顶层键，按顶层 upsert（dsh 迁移时会把
        // 它整段挪进 refs 块）。
        bool found = false;
        for (auto& line : lines) {
            const YamlLine dl = yamlLineOf(line);
            if (dl.listItem || dl.indent != 0 || dl.key != envName) continue;
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
        return;
    }

    if (shape.refsIdx < 0) {
        // 版本 1 但还没有 refs 块：文件尾补一块。
        lines.push_back("refs:");
        lines.push_back("  " + newLine);
        for (const auto& [name, value] : stray) {
            if (name == envName) continue;
            lines.push_back("  " + name + ": " + yamlQuote(value));
        }        std::string out;
        for (const auto& line : lines) {
            out += line;
            out += '\n';
        }
        atomicWrite(file, out);
        return;
    }

    const int refsIdx = shape.refsIdx;
    const int refsIndent = yamlLineOf(lines[static_cast<std::size_t>(refsIdx)]).indent;
    const int childIndent = refsIndent + 2;
    // `refs:` 行上带 flow 值：`{}` 是 dsh 写的空块，摊平成块风格继续；其它
    // flow 内容本应用的行级改写看不懂，宁可报错也不写坏文档。
    if (std::string_view rv = trimLeft(
            yamlLineOf(lines[static_cast<std::size_t>(refsIdx)]).value);
        !rv.empty()) {
        if (rv != "{}") {
            throw std::runtime_error(
                "dsh 的 .credentials.yaml 里 refs 是 flow 风格，本应用暂不支持"
                "改写；请手工整理成块风格");
        }
        lines[static_cast<std::size_t>(refsIdx)] = "refs:";
    }
    const std::string childPad(static_cast<std::size_t>(childIndent), ' ');
    const std::size_t blockEnd = credRefsBlockEnd(lines, refsIdx);
    const auto findInBlock = [&](std::string_view name) -> std::size_t {
        for (std::size_t i = static_cast<std::size_t>(refsIdx) + 1; i < blockEnd; ++i) {
            const YamlLine dl = yamlLineOf(lines[i]);
            if (dl.blank || dl.comment || dl.listItem) continue;
            if (dl.indent == childIndent && dl.key == name) return i;
        }
        return std::string::npos;
    };
    // 要写的这条 + 搬回来的旧残留（refs 里已存在的以 refs 为准，不拿旧值覆盖）。
    std::vector<std::string> additions;
    const auto stage = [&](std::string_view name, const std::string& text) {
        const std::size_t at = findInBlock(name);
        if (at != std::string::npos) {
            lines[at] = text;
        } else {
            additions.push_back(text);
        }
    };
    stage(envName, childPad + newLine);
    for (const auto& [name, value] : stray) {
        if (name == envName) continue;
        const std::size_t at = findInBlock(name);
        if (at != std::string::npos) continue;  // 块里已有这条引用
        additions.push_back(childPad + name + ": " + yamlQuote(value));
    }
    if (!additions.empty()) {
        // 插在块尾（块内尾随空行之前），保持块内条目连成一片。
        std::size_t insertAt = blockEnd;
        while (insertAt > static_cast<std::size_t>(refsIdx) + 1 &&
               yamlLineOf(lines[insertAt - 1]).blank) {
            --insertAt;
        }
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insertAt),
                     additions.begin(), additions.end());
    }
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

namespace {

// flow 值的标量清单：序列取各项、映射取各键（同行写法
// `reasoningEfforts: { off:, low: low }` 的档位就是键）。形状不符时返回空清单。
std::vector<std::string> flowScalarList(std::string_view value) {
    std::vector<std::string> out;
    FlowParser parser(value);
    auto node = parser.Parse();
    if (!node.has_value()) return out;
    for (const auto& [key, item] : node->entries) {
        const std::string name = yamlScalar(key);
        if (!name.empty()) out.push_back(name);
    }
    for (const auto& item : node->items) {
        if (item.kind != FlowValue::Kind::Scalar) continue;
        const std::string scalar = yamlScalar(item.scalar);
        if (!scalar.empty()) out.push_back(scalar);
    }
    return out;
}

} // namespace

// 行级解析 settings.yaml：顶层 agent-default-model 的 provider/model，以及
// llm-pi-ai.providers 下每条手写路由的 baseURL / api / apiKeyEnv / 首个
// models 条目 id 与它声明的官方字段（reasoningEfforts 的键、contextWindow /
// maxTokens 容量、input 输入模态）。best-effort；结构不符的字段留空。
// flow 风格的 providers 值先摊平成块风格再解析（所以 flow 里的 `input: [a, b]`
// 与 `reasoningEfforts: { … }` 到这里已经是块风格；同行 flow 写法也直接认）。
DshSettingsInfo parseDshSettings(std::string_view text) {
    DshSettingsInfo info;
    const std::string blockText = normalizeDshFlowProviders(text);
    std::istringstream in{blockText};
    bool inAdm = false, inPi = false, inProv = false, inModels = false;
    bool inEfforts = false, inInput = false;
    int admIndent = -1, piIndent = -1, provIndent = -1;
    int entryIndent = -1, modelsIndent = -1, effortsIndent = -1;
    int inputIndent = -1;
    int firstItemIndent = -1;
    bool firstItemSeen = false, secondItemSeen = false;
    DshProviderEntry* cur = nullptr;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const YamlLine dl = yamlLineOf(line);
        if (dl.blank || dl.comment) continue;
        if (inModels && dl.indent <= modelsIndent) inModels = false;
        if (inEfforts && dl.indent <= effortsIndent) inEfforts = false;
        // input 的块序列项（"input:" 之后的 "- text"）要在模型条目分支之前收，
        // 否则会被当成一个新的模型条目。
        if (inInput && dl.indent <= inputIndent) inInput = false;
        if (cur != nullptr && dl.indent <= entryIndent) {
            cur = nullptr;
            inModels = false;
            inEfforts = false;
            inInput = false;
        }
        if (inInput && cur != nullptr && dl.listItem && !secondItemSeen) {
            const std::string value =
                yamlScalar(dl.key.empty() ? dl.value : dl.key);
            if (!value.empty()) cur->inputModalities.push_back(value);
            continue;
        }
        if (inProv && dl.indent <= provIndent) inProv = false;
        if (inPi && dl.indent <= piIndent) inPi = false;
        if (inAdm && dl.indent <= admIndent) inAdm = false;
        if (inAdm) {
            if (dl.key == "provider") {
                info.defaultProvider = yamlScalar(dl.value);
            } else if (dl.key == "model") {
                info.defaultModel = yamlScalar(dl.value);
            } else if (dl.key == "reasoningEffort") {
                info.defaultReasoningEffort = yamlScalar(dl.value);
            }
            continue;
        }
        if (inModels && cur != nullptr) {
            if (dl.listItem) {
                // "- id: x" 或裸标量 "- x"
                if (!firstItemSeen) {
                    firstItemSeen = true;
                    firstItemIndent = dl.indent;
                } else if (dl.indent == firstItemIndent) {
                    secondItemSeen = true;  // 只认首个模型条目的能力声明
                }
                const std::string id =
                    dl.key == "id" || dl.key.empty() ? yamlScalar(dl.value) : "";
                if (!id.empty() && cur->firstModel.empty()) {
                    cur->firstModel = id;
                }
                continue;
            }
            if (!secondItemSeen) {
                if (dl.key == "contextWindow" || dl.key == "maxTokens") {
                    const std::int64_t count =
                        models::parseTokenCount(yamlScalar(dl.value));
                    if (count > 0) {
                        if (dl.key == "contextWindow") {
                            cur->contextWindow = count;
                        } else {
                            cur->maxTokens = count;
                        }
                    }
                    continue;
                }
                if (dl.key == "input" || dl.key == "reasoningEfforts") {
                    const std::string inlineValue =
                        std::string(trimBoth(dl.value));
                    if (!inlineValue.empty()) {
                        // 同行 flow 写法（input: [text, image] /
                        // reasoningEfforts: { off:, low: low }）直接收。
                        for (auto& value : flowScalarList(inlineValue)) {
                            if (dl.key == "input") {
                                cur->inputModalities.push_back(std::move(value));
                            } else {
                                cur->reasoningEfforts.push_back(std::move(value));
                            }
                        }
                    } else if (dl.key == "input") {
                        inInput = true;
                        inputIndent = dl.indent;
                    } else {
                        inEfforts = true;
                        effortsIndent = dl.indent;
                    }
                    continue;
                }
            }
            if (inEfforts && !secondItemSeen && dl.indent > effortsIndent &&
                !dl.key.empty()) {
                cur->reasoningEfforts.push_back(yamlScalar(dl.key));
            }
            continue;
        }
        if (cur != nullptr) {
            if (dl.key == "displayName") {
                cur->displayName = yamlScalar(dl.value);
            } else if (dl.key == "baseURL") {
                cur->baseUrl = yamlScalar(dl.value);
            } else if (dl.key == "api") {
                cur->api = yamlScalar(dl.value);
            } else if (dl.key == "apiKeyEnv") {
                cur->apiKeyEnv = yamlScalar(dl.value);
            } else if (dl.key == "models" && trimBoth(dl.value).empty()) {
                inModels = true;
                modelsIndent = dl.indent;
                // 只认每条路由首个模型条目的能力声明，所以进入新的 models 块
                // 必须重置「第几个模型条目」的计数——否则第二条及以后的
                // llmswitch-* 路由会被上一条的计数影响，字段被静默丢掉。
                firstItemSeen = false;
                secondItemSeen = false;
                firstItemIndent = -1;
                inEfforts = false;
                inInput = false;
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
    for (auto& entry : info.providers) {
        entry.reasoningEfforts =
            models::normalizeReasoningEfforts(entry.reasoningEfforts);
        entry.inputModalities =
            models::normalizeInputModalities(entry.inputModalities);
    }
    return info;
}

// 读 .credentials.yaml 里 envName 对应的密钥值（缺失返回空串）。版本 1 布局
// 的引用在 `refs:` 块内，预发布 flat 布局的引用才在顶层：文档里有 `refs:`
// 块时只认块内——顶层的同名行是旧版本写错的残留，不能当成有效值。
std::string readDshCredential(const std::filesystem::path& file,
                              std::string_view envName) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return "";
    std::ifstream in(file, std::ios::binary);
    if (!in) return "";
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    const int refsIdx = scanCredDocShape(lines).refsIdx;
    if (refsIdx >= 0) {
        const int refsIndent =
            yamlLineOf(lines[static_cast<std::size_t>(refsIdx)]).indent;
        for (std::size_t i = static_cast<std::size_t>(refsIdx) + 1;
             i < lines.size(); ++i) {
            const YamlLine dl = yamlLineOf(lines[i]);
            if (dl.blank || dl.comment) continue;
            if (dl.indent <= refsIndent) break;  // 块结束
            if (!dl.listItem && dl.key == envName) return yamlScalar(dl.value);
        }
        return "";
    }
    for (const auto& line : lines) {
        const YamlLine dl = yamlLineOf(line);
        if (!dl.listItem && dl.indent == 0 && dl.key == envName) {
            return yamlScalar(dl.value);
        }
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
        if (!models.contains("providers") || !models["providers"].is_object()) {
            models["providers"] = nlohmann::json::object();
        }
        const auto previous = models["providers"].find(target->id);
        const bool hadEntry = previous != models["providers"].end() &&
                              previous->is_object();
        nlohmann::json entry;
        entry["baseUrl"] = baseUrl;
        entry["apiKey"] = target->apiKey;
        entry["api"] = piApiValue(target->apiFormat);
        // 模型清单：该条目里已有的其它模型必须保留——本应用只声明主模型，
        // 把清单裁成一条会让 pi 的模型菜单静默少掉几条（用户在该 provider 下
        // 配的其它模型是 pi 自己的数据）。元素形状原样沿用：裸标量或
        // {"id": ...} 对象都可能，主模型缺失时按同一形状补一条。
        if (hadEntry) {
            const auto old = previous->find("models");
            if (old != previous->end() && old->is_array()) {
                if (target->model.empty()) {
                    entry["models"] = *old;  // 没有主模型：清单原样保留
                } else if (!modelListContains(*old, target->model)) {
                    bool objectShape = false;
                    for (const auto& item : *old) {
                        if (item.is_object()) {
                            objectShape = true;
                            break;
                        }
                    }
                    nlohmann::json list = *old;
                    list.push_back(objectShape
                                       ? nlohmann::json{{"id", target->model}}
                                       : nlohmann::json(target->model));
                    entry["models"] = std::move(list);
                } else {
                    entry["models"] = *old;  // 主模型已在清单里
                }
            } else if (!target->model.empty()) {
                entry["models"] = nlohmann::json::array({target->model});
            }
        } else if (!target->model.empty()) {
            entry["models"] = nlohmann::json::array({target->model});
        }
        models["providers"][target->id] = std::move(entry);
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
        // settings.yaml：切换 = 只改 agent-default-model 指向；外加一条保险——
        // 目标条目还没写进 live 时补写一条（否则指针指向不存在的键就是坏配置）。
        // 已在 live 里的条目内容原样保留（用户在 dsh 侧的手改由页面上的
        // 「写入 / 更新」显式覆盖），别的条目一个字都不动。密钥只写
        // .credentials.yaml（apiKeyEnv 引用）。两份文件都被 dsh 热监听 →
        // 切换即时生效，无需重启。
        writeDshEntry(*target, /*overwrite=*/false, /*makeDefault=*/true);
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
        // agent-default-model 块与 llmswitch-* 手写路由（本应用条目再写就以
        // 后一次同步为准），其余键与别家条目保留。
        // .credentials.yaml 里的 LLMSWITCH_* 密钥无引用即无害，不代清。
        const auto settingsFile = cfg::dshSettingsFile();
        std::error_code ec;
        if (std::filesystem::exists(settingsFile, ec)) {
            const std::string text = readTextFile(settingsFile);
            std::set<std::string> removeKeys;
            for (const auto& entry : parseDshSettings(text).providers) {
                if (entry.key.starts_with("llmswitch-")) {
                    removeKeys.insert(entry.key);
                }
            }
            backupLiveFile(tool, settingsFile);
            atomicWrite(settingsFile,
                        rewriteDshSettings(text, {}, removeKeys, true, "", "",
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

// ---- dsh 增量多供应商 ---------------------------------------------------------
// dsh 的 settings.yaml 里 llm-pi-ai.providers 是「多条手写路由并存」的 map：
// 本应用把组内供应商逐条写成 llmswitch-<id> 条目（累积、不删别家条目），
// 「切换」只改 agent-default-model 指向。下面这些 API 是这张表的全部入口；
// 供应商页的左（live 实况）右（本地留存）两列对照就建立在这上面。
//
// 写侧的铁律：**每个入口只动自己那一条**。新增/编辑/复制/收编/删除/切换/单条
// 「写入 / 更新」都走 writeDshEntry / eraseDshEntry 的单条增量，绝不整组重建——
// 整组重建会把本地全部供应商一次性推给 dsh（用户在左列删掉/整理过的那些会
// 立刻被补回来），只有「全部写入 dsh」按钮才显式触发 syncDshProviders。

// 单条增量写入（私有；声明见 store.cppm）。
void ProviderStore::writeDshEntry(const models::Provider& p, bool overwrite,
                                 bool makeDefault) {
    const auto settingsFile = cfg::dshSettingsFile();
    restrictPiDir(settingsFile.parent_path());
    std::string text;
    {
        std::error_code ec;
        if (std::filesystem::exists(settingsFile, ec)) {
            text = readTextFile(settingsFile);
        }
    }
    const auto info = parseDshSettings(text);
    const std::string key = "llmswitch-" + p.id;
    bool hasEntry = false;    // llmswitch-<id> 已在 live 里
    bool hasBareKey = false;  // 裸键 <id>（收编前的形状 / 用户手写的同名键）
    for (const auto& entry : info.providers) {
        if (entry.key == key) hasEntry = true;
        if (entry.key == p.id) hasBareKey = true;
    }
    const bool rebuild = overwrite || !hasEntry;
    bool touchDefault = makeDefault;
    std::string defaultKey = makeDefault ? key : std::string{};
    if (!touchDefault && hasBareKey && info.defaultProvider == p.id) {
        // 原默认正好指向这个裸键：它被改名接管成 llmswitch-<id>，指针跟着
        // 改指同一个路由，dsh 侧行为不变。
        touchDefault = true;
        defaultKey = key;
    }
    // 条目已是最新形状（例如「设为默认」）：内容一字不动，只在指针需要改指时
    // 写一次；两者都不需要时连文件都不碰。
    if (!rebuild && !touchDefault) return;
    std::vector<std::vector<std::string>> entries;
    std::set<std::string> removeKeys;
    if (rebuild) {
        entries.push_back(dshEntryLines(p.id, p, models::effectiveBaseUrl(p)));
        removeKeys.insert(key);
        if (hasBareKey) removeKeys.insert(p.id);
    }
    std::string defaultModel;
    if (!defaultKey.empty()) defaultModel = p.model;
    backupLiveFile("dsh", settingsFile);
    atomicWrite(settingsFile,
                rewriteDshSettings(text, entries, removeKeys, touchDefault,
                                   defaultKey, defaultModel,
                                   info.defaultReasoningEffort));
    if (!p.apiKey.empty()) {
        const auto credFile = cfg::dshCredentialsFile();
        backupLiveFile("dsh", credFile);
        upsertDshCredential(credFile, dshApiKeyEnv(p.id), p.apiKey);
        restrictPiFile(credFile);
    }
}

// 单条增量删除（私有；声明见 store.cppm）。
bool ProviderStore::eraseDshEntry(const std::string& key) {
    const auto settingsFile = cfg::dshSettingsFile();
    std::error_code ec;
    if (!std::filesystem::exists(settingsFile, ec) || ec) return false;
    const std::string text = readTextFile(settingsFile);
    const auto info = parseDshSettings(text);
    bool found = false;
    for (const auto& entry : info.providers) {
        if (entry.key == key) found = true;
    }
    // 只有它正是默认路由时才动 agent-default-model（清掉整块 = 回到内置官方
    // 路由）；别的条目、默认指向、无关键与注释全部原样。
    const bool clearDefault = info.defaultProvider == key;
    if (!found && !clearDefault) return false;
    backupLiveFile("dsh", settingsFile);
    atomicWrite(settingsFile,
                rewriteDshSettings(text, {}, {key}, clearDefault, "", "", ""));
    return found;
}

void ProviderStore::writeDshProvider(const std::string& id) {
    const auto& g = group("dsh");
    for (const auto& p : g.providers) {
        if (p.id != id) continue;
        writeDshEntry(p, /*overwrite=*/true, /*makeDefault=*/false);
        return;
    }
    throw std::runtime_error(std::format("供应商不存在：{}", id));
}

std::vector<DshLiveProvider> ProviderStore::dshLiveProviders() const {
    std::vector<DshLiveProvider> out;
    const auto file = cfg::dshSettingsFile();
    std::error_code ec;
    // 文件不存在不是「什么都没有」：dsh 仍然跑在组合里的内置默认路由上，合成
    // 官方行照样要出现（见下）。所以这里不提前返回。
    const bool hasFile = std::filesystem::exists(file, ec) && !ec;
    const auto info =
        hasFile ? parseDshSettings(readTextFile(file)) : DshSettingsInfo{};
    const auto& g = group("dsh");
    const auto credFile = cfg::dshCredentialsFile();
    out.reserve(info.providers.size() + 1);
    bool hasOfficial = false;
    for (const auto& entry : info.providers) {
        if (entry.key == "deepseek-official") hasOfficial = true;
        DshLiveProvider live;
        live.key = entry.key;
        live.displayName = entry.displayName;
        live.baseUrl = entry.baseUrl;
        live.api = entry.api;
        live.apiFormat = piApiFormatValue(entry.api);
        live.apiKeyEnv = entry.apiKeyEnv;
        live.model = entry.firstModel;
        // 没有 agent-default-model 块时 dsh 用的是组合里的内置默认路由
        // （provider: deepseek-official）——左列的「使用中」要落在官方行上。
        const bool officialRoute = entry.key == "deepseek-official";
        live.isDefault = info.defaultProvider.empty()
                             ? officialRoute
                             : entry.key == info.defaultProvider;
        if (!entry.apiKeyEnv.empty()) {
            live.apiKey = readDshCredential(credFile, entry.apiKeyEnv);
        }
        if (entry.key.starts_with("llmswitch-")) {
            const std::string id = entry.key.substr(10);
            for (const auto& p : g.providers) {
                if (p.id == id) {
                    live.providerId = id;
                    break;
                }
            }
        }
        out.push_back(std::move(live));
    }
    // dsh 内置的 deepseek-official 由适配器注册，通常不落在 settings.yaml 里，
    // 但默认选择完全可能指向它（没有 agent-default-model 块时就是它）。左列要
    // 能回答「现在到底用哪条」，所以文件里没有这条时补一个只读的合成行：
    // 它不在文件里，既不能收编也不能删除。
    if (!hasOfficial) {
        DshLiveProvider official;
        official.key = "deepseek-official";
        official.displayName = "DeepSeek 官方";
        official.builtin = true;
        official.isDefault = info.defaultProvider.empty() ||
                             info.defaultProvider == official.key;
        if (official.isDefault) official.model = info.defaultModel;
        out.insert(out.begin(), std::move(official));
    }
    return out;
}

void ProviderStore::syncDshProviders(const std::string& defaultProviderId,
                                     bool clearDefault) {
    // 显式整体重建（供应商页「全部写入 dsh」按钮 + restoreOfficial 的收尾）：
    // 组内全部供应商逐条重建、孤儿 llmswitch-* 清掉。增删改的即时同步一律走
    // writeDshEntry / eraseDshEntry 的单条增量，不走这里。
    const auto& g = group("dsh");
    const auto settingsFile = cfg::dshSettingsFile();
    restrictPiDir(settingsFile.parent_path());
    std::string text;
    {
        std::error_code ec;
        if (std::filesystem::exists(settingsFile, ec)) {
            text = readTextFile(settingsFile);
        }
    }
    const auto info = parseDshSettings(text);

    std::set<std::string> groupIds;
    std::vector<std::vector<std::string>> entries;
    entries.reserve(g.providers.size());
    for (const auto& p : g.providers) {
        groupIds.insert(p.id);
        entries.push_back(dshEntryLines(p.id, p, models::effectiveBaseUrl(p)));
    }
    // 要删的键：全部 llmswitch-*（本应用条目一律重建，不在组内的即孤儿）、
    // 被收编过来的裸键（键恰好等于组内 id → 接管成 llmswitch-<id>）。其它
    // 手写条目（dsh 内置路由、别家工具写的键）一律不动。
    std::set<std::string> removeKeys;
    for (const auto& entry : info.providers) {
        if (entry.key.starts_with("llmswitch-") ||
            groupIds.find(entry.key) != groupIds.end()) {
            removeKeys.insert(entry.key);
        }
    }
    // agent-default-model 的处置：显式指定 / 显式清除 / 裸键接管后重指向 /
    // 指向已被删除供应商时清回内置官方路由。
    bool touchDefault = clearDefault || !defaultProviderId.empty();
    std::string defaultKey =
        clearDefault ? std::string{} : "llmswitch-" + defaultProviderId;
    if (!touchDefault && !info.defaultProvider.empty()) {
        if (info.defaultProvider.starts_with("llmswitch-")) {
            const std::string id = info.defaultProvider.substr(10);
            if (groupIds.find(id) == groupIds.end()) {
                touchDefault = true;  // 悬空：指向的供应商已不在组内
                defaultKey.clear();
            }
        } else if (groupIds.find(info.defaultProvider) != groupIds.end()) {
            touchDefault = true;  // 裸键被接管改名 → 默认跟着改指新键
            defaultKey = "llmswitch-" + info.defaultProvider;
        }
    }
    std::string defaultModel;
    if (!defaultKey.empty()) {
        const std::string id = defaultKey.substr(10);
        for (const auto& p : g.providers) {
            if (p.id == id) defaultModel = p.model;
        }
    }
    backupLiveFile("dsh", settingsFile);
    atomicWrite(settingsFile,
                rewriteDshSettings(text, entries, removeKeys, touchDefault,
                                   defaultKey, defaultModel,
                                   info.defaultReasoningEffort));
    // 密钥：组内每个带密钥的供应商 upsert 进 .credentials.yaml（只增改，不代
    // 清无引用的旧键；备份每轮只做一次）。
    bool anyKey = false;
    for (const auto& p : g.providers) {
        if (!p.apiKey.empty()) anyKey = true;
    }
    if (anyKey) {
        const auto credFile = cfg::dshCredentialsFile();
        backupLiveFile("dsh", credFile);
        for (const auto& p : g.providers) {
            if (p.apiKey.empty()) continue;
            upsertDshCredential(credFile, dshApiKeyEnv(p.id), p.apiKey);
        }
        restrictPiFile(credFile);
    }
}

models::Provider ProviderStore::adoptDshProvider(const std::string& key) {
    auto& g = groupRef("dsh");
    const auto file = cfg::dshSettingsFile();
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) {
        throw std::runtime_error("dsh 配置文件不存在，无法收编条目");
    }
    const std::string text = readTextFile(file);
    const auto info = parseDshSettings(text);
    const DshProviderEntry* entry = nullptr;
    for (const auto& candidate : info.providers) {
        if (candidate.key == key) entry = &candidate;
    }
    if (entry == nullptr) {
        throw std::runtime_error(std::format("dsh 配置里没有条目：{}", key));
    }
    const std::string id =
        key.starts_with("llmswitch-") ? key.substr(10) : key;
    const bool isDefault = key == info.defaultProvider;
    models::Provider p;
    p.id = id;
    p.name = entry->displayName.empty() ? key : entry->displayName;
    p.baseUrl = entry->baseUrl;
    p.apiFormat = piApiFormatValue(entry->api);
    p.model = isDefault && !info.defaultModel.empty() ? info.defaultModel
                                                      : entry->firstModel;
    p.reasoningEfforts = entry->reasoningEfforts;
    p.contextWindow = entry->contextWindow;
    p.maxTokens = entry->maxTokens;
    p.inputModalities = entry->inputModalities;
    if (!entry->apiKeyEnv.empty()) {
        p.apiKey = readDshCredential(cfg::dshCredentialsFile(), entry->apiKeyEnv);
    }
    bool replaced = false;
    for (auto& cur : g.providers) {
        if (cur.id != id) continue;
        const std::int64_t created = cur.createdAt;
        cur = p;
        cur.createdAt = created;
        replaced = true;
        break;
    }
    if (!replaced) {
        p.createdAt = nowMillis();
        g.providers.push_back(p);
    }
    if (isDefault) g.current = id;
    // 先落盘本地列表（live 接管失败也不丢收编结果，页面会显示「未写入」），
    // 再把这一条接管成 llmswitch-<id>：它正是默认路由时 agent-default-model
    // 同步改指新键，dsh 侧行为不变。只动这一条——收编不该顺手把本地其它
    // 供应商也推给 dsh。
    save();
    writeDshEntry(p, /*overwrite=*/true, /*makeDefault=*/isDefault);
    return p;
}

void ProviderStore::removeDshProvider(const std::string& key) {
    const auto file = cfg::dshSettingsFile();
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) {
        throw std::runtime_error("dsh 配置文件不存在，没有可删除的条目");
    }
    // 存在性检查先做（找不到就整体不动），真正落盘交给单条增量删除：只删这个
    // 键，别的条目一字不动——包括组内那些还没写进 live 的供应商（左列删一条
    // 不该把右列全部推过去）。
    const auto info = parseDshSettings(readTextFile(file));
    bool found = false;
    for (const auto& entry : info.providers) {
        if (entry.key == key) found = true;
    }
    if (!found) {
        throw std::runtime_error(std::format("dsh 配置里没有条目：{}", key));
    }
    eraseDshEntry(key);
}

} // namespace store
