// store_kimi.cpp — llmswitch.store 实现单元：Kimi Code 的 config.toml 读写。
//
// Kimi Code（CLI 与 Desktop 共用 ~/.kimi-code/config.toml，KIMI_CODE_HOME 可
// 覆盖）的供应商是两条并存的 TOML 表：
//   [providers."<key>"]  type / base_url / api_key（或 api_key_env / oauth 子表）
//   [models."<alias>"]   provider / model / max_context_size / capabilities ...
// 顶层 default_model 指向当前 model 别名。本应用只增改自己那条 provider 与
// 对应 model 别名，切换只改 default_model，绝不整组重写——原生条目、oauth
// 子表、注释和其它表都原样保留。
//
// TOML 处理是行级子集：识别表头、键值行与常见标量/字符串数组，支持跨行数组
// 与多行字符串的表头规避；不重建整份文档，因此未触碰的字节保持原样。这不是
// 通用 TOML 库，只为 Kimi 这份结构稳定的配置服务；遇到不认识的表/值就原样
// 保留，绝不猜写。
module llmswitch.store;

import std;
import llmswitch.config;
import llmswitch.models;

namespace store {
namespace {

// Kimi 的 max_context_size 必填。未声明过容量的旧卡按 Kimi 常见默认值写，
// 避免生成 Kimi 拒绝加载的配置；表单会要求新卡填写真实容量。
constexpr std::int64_t kDefaultContextSize = 262144;

using Lines = std::vector<std::string>;

std::string_view trimSpace(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

std::string_view lineBody(const std::string& line) {
    std::string_view body(line);
    if (body.ends_with('\n')) body.remove_suffix(1);
    if (body.ends_with('\r')) body.remove_suffix(1);
    return body;
}

Lines splitLines(std::string_view text) {
    Lines lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto pos = text.find('\n', start);
        if (pos == std::string_view::npos) {
            lines.emplace_back(text.substr(start));
            break;
        }
        lines.emplace_back(text.substr(start, pos - start + 1));
        start = pos + 1;
    }
    return lines;
}

std::string joinLines(const Lines& lines) {
    std::string text;
    for (const auto& line : lines) text += line;
    return text;
}

// 生成的行统一以 \n 结尾；插到没有结尾换行的旧文件前先补一个。
void ensureTrailingNewline(Lines& lines) {
    if (!lines.empty() && !lines.back().ends_with('\n')) lines.back() += '\n';
}

// 配置里有 API Key；新建文件按 0600 落盘（Windows 用 ACL，权限位无意义）。
void restrictKimiFile(const std::filesystem::path& file) {
#ifndef _WIN32
    std::error_code ec;
    std::filesystem::permissions(
        file,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ec);
#endif
}

void appendGenerated(Lines& lines, std::string_view content) {
    ensureTrailingNewline(lines);
    lines.emplace_back(std::string(content) + "\n");
}

std::string basicString(std::string_view text) {
    std::string out = "\"";
    for (const char ch : text) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += ch; break;
        }
    }
    out += '"';
    return out;
}

std::string keyPart(std::string_view text) {
    return basicString(text);
}

// TOML basic string（"..."）：返回 i 指向闭合引号之后。
bool parseBasicString(std::string_view text, std::size_t& i, std::string& out) {
    if (i >= text.size() || text[i] != '"') return false;
    ++i;
    while (i < text.size()) {
        const char ch = text[i++];
        if (ch == '"') return true;
        if (ch != '\\') {
            out += ch;
            continue;
        }
        if (i >= text.size()) return false;
        const char esc = text[i++];
        switch (esc) {
        case 'b': out += '\b'; break;
        case 't': out += '\t'; break;
        case 'n': out += '\n'; break;
        case 'f': out += '\f'; break;
        case 'r': out += '\r'; break;
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case 'u':
        case 'U': {
            const std::size_t count = esc == 'u' ? 4 : 8;
            if (i + count > text.size()) return false;
            unsigned code = 0;
            for (std::size_t k = 0; k < count; ++k) {
                const char hex = text[i++];
                code <<= 4;
                if (hex >= '0' && hex <= '9') code |= static_cast<unsigned>(hex - '0');
                else if (hex >= 'a' && hex <= 'f') code |= static_cast<unsigned>(hex - 'a' + 10);
                else if (hex >= 'A' && hex <= 'F') code |= static_cast<unsigned>(hex - 'A' + 10);
                else return false;
            }
            // 只收 BMP，够 Kimi 的键与值用；超出范围按替换符处理。
            if (code <= 0x7FF) {
                out += static_cast<char>(0xC0 | (code >> 6));
                out += static_cast<char>(0x80 | (code & 0x3F));
            } else {
                out += static_cast<char>(0xE0 | (code >> 12));
                out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (code & 0x3F));
            }
            break;
        }
        default: return false;
        }
    }
    return false;
}

bool parseLiteralString(std::string_view text, std::size_t& i, std::string& out) {
    if (i >= text.size() || text[i] != '\'') return false;
    ++i;
    const auto end = text.find('\'', i);
    if (end == std::string_view::npos) return false;
    out = std::string(text.substr(i, end - i));
    i = end + 1;
    return true;
}

bool isBareKeyChar(char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
}

bool parseKeyPath(std::string_view text, std::vector<std::string>& path) {
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        if (i >= text.size() || text[i] == ']') break;
        std::string part;
        if (text[i] == '"') {
            if (!parseBasicString(text, i, part)) return false;
        } else if (text[i] == '\'') {
            if (!parseLiteralString(text, i, part)) return false;
        } else {
            const std::size_t begin = i;
            while (i < text.size() && isBareKeyChar(text[i])) ++i;
            if (i == begin) return false;
            part = std::string(text.substr(begin, i - begin));
        }
        path.push_back(std::move(part));
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        if (i < text.size() && text[i] == '.') {
            ++i;
            continue;
        }
        break;
    }
    return !path.empty();
}

struct Section {
    std::vector<std::string> path;
    std::size_t start = 0;  // 表头行下标
    std::size_t end = 0;    // 下一个表头/文件尾（不含）
};

struct MultilineState {
    bool active = false;
    char quote = '"';
};

void updateMultilineState(std::string_view text, MultilineState& state) {
    std::size_t i = 0;
    while (i < text.size()) {
        if (state.active) {
            if (state.quote == '"' && text[i] == '\\') {
                i += 2;
                continue;
            }
            if (text.compare(i, 3, state.quote == '"' ? "\"\"\"" : "'''") == 0) {
                state.active = false;
                i += 3;
                continue;
            }
            ++i;
            continue;
        }
        const char ch = text[i];
        if (ch == '#') {
            // 注释只到行尾；跨行数组的收尾括号可能在下一行。
            while (i < text.size() && text[i] != '\n') ++i;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            const char* triple = ch == '"' ? "\"\"\"" : "'''";
            if (text.compare(i, 3, triple) == 0) {
                state.active = true;
                state.quote = ch;
                i += 3;
                continue;
            }
            ++i;
            while (i < text.size()) {
                if (ch == '"' && text[i] == '\\') {
                    i += 2;
                    continue;
                }
                if (text[i] == ch) {
                    ++i;
                    break;
                }
                ++i;
            }
            continue;
        }
        ++i;
    }
}

std::vector<Section> scanSections(const Lines& lines) {
    std::vector<Section> sections;
    MultilineState state;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string_view body = lineBody(lines[i]);
        if (state.active) {
            updateMultilineState(body, state);
            continue;
        }
        const std::string_view trimmed = trimSpace(body);
        if (trimmed.starts_with('[') && !trimmed.starts_with("[[")) {
            const std::size_t close = trimmed.find(']', 1);
            if (close != std::string_view::npos) {
                std::vector<std::string> path;
                if (parseKeyPath(trimmed.substr(1, close - 1), path)) {
                    if (!sections.empty()) sections.back().end = i;
                    sections.push_back(Section{.path = std::move(path),
                                               .start = i,
                                               .end = lines.size()});
                    updateMultilineState(body, state);
                    continue;
                }
            }
        } else if (trimmed.starts_with("[[")) {
            // [[hooks]] 等数组表：作为普通分界，不参与 providers/models 匹配。
            const std::size_t close = trimmed.find("]]", 2);
            if (close != std::string_view::npos) {
                if (!sections.empty()) sections.back().end = i;
                sections.push_back(Section{.start = i, .end = lines.size()});
                updateMultilineState(body, state);
                continue;
            }
        }
        updateMultilineState(body, state);
    }
    return sections;
}

const Section* findSection(const std::vector<Section>& sections,
                           std::string_view table,
                           std::string_view key) {
    for (const auto& section : sections) {
        if (section.path.size() != 2) continue;
        if (section.path[0] == table && section.path[1] == key) return &section;
    }
    return nullptr;
}

// ---- 值 -----------------------------------------------------------------------

struct Value {
    enum class Kind { Other, String, Integer, Boolean, Array } kind = Kind::Other;
    std::string text;
    std::int64_t integer = 0;
    bool boolean = false;
    std::vector<Value> items;

    bool isString() const { return kind == Kind::String; }
    bool isInteger() const { return kind == Kind::Integer; }
    bool isArray() const { return kind == Kind::Array; }
};

void skipSpaces(std::string_view text, std::size_t& i) {
    while (i < text.size() &&
           (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' ||
            text[i] == '\r')) {
        ++i;
    }
}

Value parseValue(std::string_view text, std::size_t& i) {
    Value value;
    skipSpaces(text, i);
    if (i >= text.size()) return value;
    if (text[i] == '"') {
        if (parseBasicString(text, i, value.text)) value.kind = Value::Kind::String;
        return value;
    }
    if (text[i] == '\'') {
        if (parseLiteralString(text, i, value.text)) value.kind = Value::Kind::String;
        return value;
    }
    if (text[i] == '[') {
        value.kind = Value::Kind::Array;
        ++i;
        for (;;) {
            skipSpaces(text, i);
            // 数组元素间允许注释；注释延伸到行尾。
            while (i < text.size() && text[i] == '#') {
                while (i < text.size() && text[i] != '\n') ++i;
                skipSpaces(text, i);
            }
            if (i >= text.size()) return Value{};
            if (text[i] == ']') {
                ++i;
                return value;
            }
            Value item = parseValue(text, i);
            value.items.push_back(std::move(item));
            skipSpaces(text, i);
            if (i < text.size() && text[i] == ',') {
                ++i;
                continue;
            }
            if (i < text.size() && text[i] == ']') {
                ++i;
                return value;
            }
            return Value{};
        }
    }
    if (text.compare(i, 4, "true") == 0) {
        value.kind = Value::Kind::Boolean;
        value.boolean = true;
        i += 4;
        return value;
    }
    if (text.compare(i, 5, "false") == 0) {
        value.kind = Value::Kind::Boolean;
        value.boolean = false;
        i += 5;
        return value;
    }
    if (text[i] == '{') {
        int depth = 0;
        bool inString = false;
        char quote = 0;
        while (i < text.size()) {
            const char ch = text[i++];
            if (inString) {
                if (quote == '"' && ch == '\\') {
                    if (i < text.size()) ++i;
                } else if (ch == quote) {
                    inString = false;
                }
                continue;
            }
            if (ch == '"' || ch == '\'') {
                inString = true;
                quote = ch;
            } else if (ch == '{') {
                ++depth;
            } else if (ch == '}') {
                --depth;
                if (depth == 0) break;
            }
        }
        value.kind = Value::Kind::Other;
        return value;
    }
    if ((text[i] >= '0' && text[i] <= '9') || text[i] == '-' || text[i] == '+') {
        std::size_t begin = i;
        if (text[i] == '-' || text[i] == '+') ++i;
        std::int64_t number = 0;
        bool any = false;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            number = number * 10 + (text[i] - '0');
            ++i;
            any = true;
        }
        if (any && (i >= text.size() || text[i] == ' ' || text[i] == '\t' ||
                    text[i] == ',' || text[i] == ']' || text[i] == '\n' ||
                    text[i] == '#')) {
            value.kind = Value::Kind::Integer;
            value.integer = text[begin] == '-' ? -number : number;
            return value;
        }
        i = begin;
    }
    // 其它标量（浮点、日期等）：原样保留，调用方按 Other 忽略。
    while (i < text.size() && text[i] != ',' && text[i] != ']' &&
           text[i] != '#' && text[i] != '\n') {
        ++i;
    }
    value.kind = Value::Kind::Other;
    return value;
}

bool valueComplete(std::string_view text) {
    int brackets = 0;
    int braces = 0;
    bool inString = false;
    char quote = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (inString) {
            if (quote == '"' && ch == '\\') {
                ++i;
            } else if (ch == quote) {
                inString = false;
            }
            continue;
        }
        if (ch == '#') {
            // 注释只到行尾；跨行数组的收尾括号可能在下一行。
            while (i < text.size() && text[i] != '\n') ++i;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            inString = true;
            quote = ch;
        } else if (ch == '[') {
            ++brackets;
        } else if (ch == ']') {
            --brackets;
        } else if (ch == '{') {
            ++braces;
        } else if (ch == '}') {
            --braces;
        }
    }
    return !inString && brackets <= 0 && braces <= 0;
}

struct Entry {
    std::string key;
    std::size_t start = 0;
    std::size_t end = 0;
    Value value;
};

// 解析 [start,end) 内的键值行；跨行数组会吃掉后续行。注释与空行跳过。
std::vector<Entry> parseEntries(const Lines& lines, std::size_t start,
                                std::size_t end) {
    std::vector<Entry> entries;
    std::size_t i = start;
    while (i < end) {
        const std::string_view body = lineBody(lines[i]);
        const std::string_view trimmed = trimSpace(body);
        if (trimmed.empty() || trimmed.starts_with('#')) {
            ++i;
            continue;
        }
        if (trimmed.starts_with('[')) break;
        // 找顶层 '='（忽略字符串里的 =）
        std::size_t eq = std::string_view::npos;
        bool inString = false;
        char quote = 0;
        for (std::size_t k = 0; k < body.size(); ++k) {
            const char ch = body[k];
            if (inString) {
                if (quote == '"' && ch == '\\') ++k;
                else if (ch == quote) inString = false;
                continue;
            }
            if (ch == '"' || ch == '\'') {
                inString = true;
                quote = ch;
            } else if (ch == '#') {
                break;
            } else if (ch == '=') {
                eq = k;
                break;
            }
        }
        if (eq == std::string_view::npos) {
            ++i;
            continue;
        }
        const std::string_view keyText = trimSpace(body.substr(0, eq));
        std::string key;
        std::size_t keyPos = 0;
        if (keyText.starts_with('"')) {
            if (!parseBasicString(keyText, keyPos, key) || keyPos != keyText.size()) {
                ++i;
                continue;
            }
        } else if (keyText.starts_with('\'')) {
            if (!parseLiteralString(keyText, keyPos, key) || keyPos != keyText.size()) {
                ++i;
                continue;
            }
        } else {
            key = std::string(keyText);
            if (key.find('.') != std::string::npos) {
                // 点分键（a.b = ...）不属于本文件的供应商字段，原样跳过。
                ++i;
                continue;
            }
        }
        std::string valueText(body.substr(eq + 1));
        std::size_t j = i + 1;
        while (j < end && !valueComplete(valueText)) {
            // 保留换行：数组里的 # 注释以行尾结束，不能把后续元素吞掉。
            valueText += "\n";
            valueText += lineBody(lines[j]);
            ++j;
        }
        std::size_t valuePos = 0;
        Value parsed = parseValue(valueText, valuePos);
        Entry entry{.key = std::move(key), .start = i, .end = j,
                    .value = std::move(parsed)};
        bool replaced = false;
        for (auto& cur : entries) {
            if (cur.key == entry.key) {
                cur = std::move(entry);
                replaced = true;
                break;
            }
        }
        if (!replaced) entries.push_back(std::move(entry));
        i = j;
    }
    return entries;
}

const Entry* findEntry(const std::vector<Entry>& entries, std::string_view key) {
    for (const auto& entry : entries) {
        if (entry.key == key) return &entry;
    }
    return nullptr;
}

std::string entryString(const std::vector<Entry>& entries, std::string_view key) {
    const Entry* entry = findEntry(entries, key);
    return entry != nullptr && entry->value.isString() ? entry->value.text : "";
}

std::int64_t entryInteger(const std::vector<Entry>& entries,
                          std::string_view key) {
    const Entry* entry = findEntry(entries, key);
    return entry != nullptr && entry->value.isInteger() ? entry->value.integer : 0;
}

std::vector<std::string> entryStrings(const std::vector<Entry>& entries,
                                      std::string_view key) {
    std::vector<std::string> out;
    const Entry* entry = findEntry(entries, key);
    if (entry == nullptr || !entry->value.isArray()) return out;
    for (const auto& item : entry->value.items) {
        if (item.isString()) out.push_back(item.text);
    }
    return out;
}

// ---- 写入辅助 -----------------------------------------------------------------

void replaceRange(Lines& lines, std::size_t start, std::size_t end,
                  const Lines& replacement) {
    lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(start),
                lines.begin() + static_cast<std::ptrdiff_t>(end));
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(start),
                 replacement.begin(), replacement.end());
}

// 在已有表内做增量键更新：保留未知键、注释和其它行。updates 是
// key → 完整 "key = value" 行；removes 是要摘掉的键。
void patchSection(Lines& lines, const Section& section,
                  const std::vector<std::pair<std::string, std::string>>& updates,
                  const std::vector<std::string>& removes) {
    const auto entries = parseEntries(lines, section.start + 1, section.end);
    std::vector<std::pair<std::size_t, std::size_t>> erased;
    std::vector<std::pair<std::string, std::string>> missing;
    for (const auto& [key, text] : updates) {
        const Entry* entry = findEntry(entries, key);
        if (entry != nullptr) {
            erased.emplace_back(entry->start, entry->end);
        } else {
            missing.push_back({key, text});
        }
    }
    for (const auto& key : removes) {
        const Entry* entry = findEntry(entries, key);
        if (entry != nullptr) erased.emplace_back(entry->start, entry->end);
    }
    std::sort(erased.begin(), erased.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first > rhs.first; });
    erased.erase(std::unique(erased.begin(), erased.end()), erased.end());

    // 缺键先补在表尾（下一个表头之前），再从后往前替换/删除已有键，避免
    // 行号漂移。
    if (!missing.empty()) {
        if (section.end == lines.size()) ensureTrailingNewline(lines);
        Lines generated;
        for (const auto& [key, text] : missing) {
            generated.emplace_back(text + "\n");
        }
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(section.end),
                     generated.begin(), generated.end());
    }
    for (const auto& [start, end] : erased) {
        const Entry* entry = nullptr;
        for (const auto& candidate : updates) {
            const Entry* found = findEntry(entries, candidate.first);
            if (found != nullptr && found->start == start) {
                entry = found;
                break;
            }
        }
        std::string replacement;
        if (entry != nullptr) {
            for (const auto& [key, text] : updates) {
                if (key == entry->key) {
                    replacement = text + "\n";
                    break;
                }
            }
        }
        Lines content;
        if (!replacement.empty()) content.push_back(std::move(replacement));
        replaceRange(lines, start, end, content);
    }
}

void replaceSectionBody(Lines& lines, const Section& section,
                        const std::vector<std::pair<std::string, std::string>>& body) {
    Lines generated;
    for (const auto& [key, text] : body) generated.emplace_back(text + "\n");
    replaceRange(lines, section.start + 1, section.end, generated);
}

// 找或建一个 [table."key"] 表；preserveUnknown 时只改 updates/removes 涉及
// 的键，否则整段表体替换成 updates。
void upsertSection(Lines& lines, std::string_view table, std::string_view key,
                   const std::vector<std::pair<std::string, std::string>>& updates,
                   const std::vector<std::string>& removes, bool preserveUnknown) {
    auto sections = scanSections(lines);
    const Section* section = findSection(sections, table, key);
    if (section != nullptr) {
        if (preserveUnknown) {
            patchSection(lines, *section, updates, removes);
        } else {
            replaceSectionBody(lines, *section, updates);
        }
        return;
    }
    // 新表追加到文件尾；表头用带引号的键，键里出现什么字符都不影响解析。
    std::string header = "[" + std::string(table) + "." + keyPart(key) + "]";
    appendGenerated(lines, header);
    for (const auto& entry : updates) appendGenerated(lines, entry.second);
}

void removeSection(Lines& lines, std::string_view table, std::string_view key) {
    const auto sections = scanSections(lines);
    const Section* section = findSection(sections, table, key);
    if (section == nullptr) return;
    replaceRange(lines, section->start, section->end, {});
}

void setTopLevelString(Lines& lines, std::string_view key, std::string_view value) {
    const auto sections = scanSections(lines);
    const std::size_t preambleEnd =
        sections.empty() ? lines.size() : sections.front().start;
    const auto entries = parseEntries(lines, 0, preambleEnd);
    const Entry* entry = findEntry(entries, key);
    const std::string generated =
        std::string(key) + " = " + basicString(value) + "\n";
    if (entry != nullptr) {
        replaceRange(lines, entry->start, entry->end, {generated});
        return;
    }
    Lines content{generated};
    if (preambleEnd == lines.size()) {
        appendGenerated(lines, std::string(key) + " = " + basicString(value));
    } else {
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(preambleEnd),
                     content.begin(), content.end());
    }
}

void removeTopLevelKey(Lines& lines, std::string_view key) {
    const auto sections = scanSections(lines);
    const std::size_t preambleEnd =
        sections.empty() ? lines.size() : sections.front().start;
    const auto entries = parseEntries(lines, 0, preambleEnd);
    const Entry* entry = findEntry(entries, key);
    if (entry != nullptr) replaceRange(lines, entry->start, entry->end, {});
}

// ---- 模型 ---------------------------------------------------------------------

std::string chooseModelAlias(const KimiConfigInfo& info, std::string_view key,
                             std::string_view model,
                             std::string_view preferredAlias) {
    const KimiModelEntry* fallback = nullptr;
    for (const auto& entry : info.models) {
        if (entry.provider != key || entry.model != model) continue;
        if (!preferredAlias.empty() && entry.alias == preferredAlias) {
            return entry.alias;
        }
        if (fallback == nullptr) fallback = &entry;
    }
    if (fallback != nullptr) return fallback->alias;

    std::string base;
    if (std::string_view(key).starts_with("llmswitch-")) {
        base = std::string(key);
    } else {
        base = std::string(key) + "/" + std::string(model);
    }
    std::string candidate = base;
    int suffix = 2;
    for (;;) {
        bool taken = false;
        for (const auto& entry : info.models) {
            if (entry.alias == candidate) {
                taken = true;
                break;
            }
        }
        if (!taken) return candidate;
        candidate = base + "-" + std::to_string(suffix++);
    }
}

std::string quoteArray(const std::vector<std::string>& values) {
    std::string out = "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out += ", ";
        out += basicString(values[i]);
    }
    out += "]";
    return out;
}

std::vector<std::string> kimiCapabilities(const models::Provider& target) {
    // Kimi 的模型编辑器默认声明 tool_use + thinking；只写 tool_use 会让
    // Kimi 把这条模型当成不支持思考。输入模态再按需加 image_in。
    std::vector<std::string> capabilities{"tool_use", "thinking"};
    const bool image = std::ranges::find(target.inputModalities, "image") !=
                       target.inputModalities.end();
    if (image) capabilities.push_back("image_in");
    return capabilities;
}

std::vector<std::pair<std::string, std::string>> providerBody(
    const models::Provider& target, bool newSection) {
    std::vector<std::pair<std::string, std::string>> body;
    body.emplace_back("type",
                      "type = " + basicString(kimiProviderTypeValue(target.apiFormat)));
    body.emplace_back("base_url",
                      "base_url = " + basicString(models::effectiveBaseUrl(target)));
    if (!target.apiKey.empty()) {
        body.emplace_back("api_key", "api_key = " + basicString(target.apiKey));
    } else if (newSection) {
        // Kimi 的 provider 表要求有密钥字段；留空比缺字段更接近官方骨架，
        // 用户之后在 Kimi 里登录/补 key 也不会被本应用挡住。
        body.emplace_back("api_key", "api_key = \"\"");
    }
    return body;
}

std::vector<std::pair<std::string, std::string>> modelBody(
    const models::Provider& target, std::string_view key, bool full) {
    std::vector<std::pair<std::string, std::string>> body;
    body.emplace_back("provider", "provider = " + basicString(key));
    body.emplace_back("model", "model = " + basicString(target.model));
    const std::int64_t context =
        target.contextWindow > 0 ? target.contextWindow : kDefaultContextSize;
    body.emplace_back("max_context_size",
                      "max_context_size = " + std::to_string(context));
    if (full) {
        if (!target.name.empty()) {
            body.emplace_back("display_name",
                              "display_name = " + basicString(target.name));
        }
        body.emplace_back("capabilities",
                          "capabilities = " + quoteArray(kimiCapabilities(target)));
        if (!target.reasoningEfforts.empty()) {
            body.emplace_back("support_efforts",
                              "support_efforts = " +
                                  quoteArray(target.reasoningEfforts));
        }
    }
    return body;
}

} // namespace

// ---- 公共读取 -----------------------------------------------------------------

KimiConfigInfo parseKimiConfig(std::string_view text) {
    KimiConfigInfo info;
    const Lines lines = splitLines(text);
    const auto sections = scanSections(lines);
    const std::size_t preambleEnd =
        sections.empty() ? lines.size() : sections.front().start;
    const auto preamble = parseEntries(lines, 0, preambleEnd);
    info.defaultModel = entryString(preamble, "default_model");

    for (const auto& section : sections) {
        if (section.path.size() != 2) continue;
        const auto entries = parseEntries(lines, section.start + 1, section.end);
        if (section.path[0] == "providers") {
            KimiProviderEntry provider;
            provider.key = section.path[1];
            provider.type = entryString(entries, "type");
            provider.baseUrl = entryString(entries, "base_url");
            const Entry* apiKey = findEntry(entries, "api_key");
            if (apiKey != nullptr && apiKey->value.isString()) {
                provider.apiKey = apiKey->value.text;
                provider.hasApiKey = true;
            }
            const Entry* apiKeyEnv = findEntry(entries, "api_key_env");
            if (apiKeyEnv != nullptr && apiKeyEnv->value.isString()) {
                provider.apiKeyEnv = apiKeyEnv->value.text;
                provider.hasApiKeyEnv = true;
            }
            info.providers.push_back(std::move(provider));
        } else if (section.path[0] == "models") {
            KimiModelEntry model;
            model.alias = section.path[1];
            model.provider = entryString(entries, "provider");
            model.model = entryString(entries, "model");
            model.displayName = entryString(entries, "display_name");
            model.maxContextSize = entryInteger(entries, "max_context_size");
            model.maxOutputSize = entryInteger(entries, "max_output_size");
            model.capabilities = entryStrings(entries, "capabilities");
            model.supportEfforts = entryStrings(entries, "support_efforts");
            model.defaultEffort = entryString(entries, "default_effort");
            info.models.push_back(std::move(model));
        }
    }
    return info;
}

KimiConfigInfo readKimiConfig(const std::filesystem::path& file) {
    return parseKimiConfig(readTextFile(file));
}

std::string kimiProviderTypeValue(std::string_view apiFormat) {
    if (apiFormat == "kimi") return "kimi";
    if (apiFormat == "anthropic") return "anthropic";
    if (apiFormat == "openai-responses") return "openai_responses";
    return "openai";
}

std::string kimiApiFormatValue(std::string_view type) {
    if (type == "kimi") return "kimi";
    if (type == "anthropic") return "anthropic";
    if (type == "openai_responses" || type == "openai-responses") {
        return "openai-responses";
    }
    return "";
}

std::string kimiEntryKeyFor(const std::vector<KimiProviderEntry>& providers,
                            const models::Provider& target) {
    const std::string ours = "llmswitch-" + target.id;
    for (const auto& provider : providers) {
        if (provider.key == ours) return ours;
    }
    const std::string baseUrl = models::effectiveBaseUrl(target);
    for (const auto& provider : providers) {
        // managed:* 是 Kimi 自己的 OAuth 账号，永远不能被当成本应用的
        // 条目；空密钥也不参与端点匹配（OAuth provider 的 api_key 就是
        // 空串，不挡住会把官方条目误认成目标、进而改写它）。
        if (provider.key.starts_with("managed:") ||
            provider.key.starts_with("llmswitch-")) {
            continue;
        }
        if (provider.baseUrl == baseUrl && !provider.apiKey.empty() &&
            provider.apiKey == target.apiKey) {
            return provider.key;
        }
    }
    for (const auto& provider : providers) {
        if (provider.key == target.id && !provider.key.starts_with("managed:")) {
            return provider.key;
        }
    }
    return ours;
}

// ---- 写入 ---------------------------------------------------------------------

void writeKimiEntry(const models::Provider& target, bool overwrite,
                    bool makeDefault) {
    if (target.model.empty()) {
        throw std::runtime_error("Kimi Code 需要填写模型");
    }
    const auto file = cfg::kimiConfigFile();
    const std::string original = readTextFile(file);
    Lines lines = splitLines(original);
    KimiConfigInfo info = parseKimiConfig(original);
    std::string key = kimiEntryKeyFor(info.providers, target);
    const std::string ours = "llmswitch-" + target.id;
    // 双保险：任何路径都不改写 managed:*（Kimi 官方 OAuth 账号）。
    if (key.starts_with("managed:")) key = ours;
    const bool ownEntry = key == ours;

    bool providerExists = false;
    for (const auto& provider : info.providers) {
        if (provider.key == key) providerExists = true;
    }

    if (!providerExists || overwrite) {
        const auto body = providerBody(target, !providerExists);
        std::vector<std::string> removes;
        if (!target.apiKey.empty()) {
            removes.push_back("api_key_env");
        } else if (ownEntry && overwrite) {
            removes.push_back("api_key");
        }
        upsertSection(lines, "providers", key, body, removes,
                      providerExists && !ownEntry);
    }

    // 重新解析：上面可能刚插入了 provider 表。
    info = parseKimiConfig(joinLines(lines));
    const bool aggregate = models::isAggregateProvider(target);
    std::vector<std::string> desiredModels = target.models;
    if (!target.model.empty() &&
        std::ranges::find(desiredModels, target.model) ==
            desiredModels.end()) {
        desiredModels.insert(desiredModels.begin(), target.model);
    }
    if (aggregate && !desiredModels.empty()) {
        // 聚合条目由本应用独占：把合并后的完整模型清单写成一组模型别名，
        // 让 Kimi 的模型菜单看到所有模型；默认模型仍指向 target.model。
        std::vector<std::string> oldAliases;
        for (const auto& model : info.models) {
            if (model.provider == key) oldAliases.push_back(model.alias);
        }
        const std::string previousDefault = info.defaultModel;
        std::vector<std::string> desiredAliases;
        std::string defaultAlias;
        for (const auto& model : desiredModels) {
            const std::string alias =
                chooseModelAlias(info, key, model,
                                 model == target.model ? info.defaultModel
                                                       : std::string{});
            bool modelExists = false;
            for (const auto& existing : info.models) {
                if (existing.alias == alias) modelExists = true;
            }
            if (!modelExists || overwrite) {
                models::Provider modelTarget = target;
                modelTarget.model = model;
                modelTarget.name = model;
                const auto body =
                    modelBody(modelTarget, key, ownEntry || !modelExists);
                upsertSection(lines, "models", alias, body, {},
                              modelExists && !ownEntry);
            }
            info = parseKimiConfig(joinLines(lines));
            desiredAliases.push_back(alias);
            if (model == target.model) defaultAlias = alias;
        }
        if (overwrite) {
            for (const auto& alias : oldAliases) {
                if (std::ranges::find(desiredAliases, alias) ==
                    desiredAliases.end()) {
                    removeSection(lines, "models", alias);
                }
            }
        }
        const bool defaultRemoved =
            !previousDefault.empty() &&
            std::ranges::find(oldAliases, previousDefault) != oldAliases.end() &&
            std::ranges::find(desiredAliases, previousDefault) ==
                desiredAliases.end();
        if ((makeDefault || defaultRemoved) && !defaultAlias.empty()) {
            setTopLevelString(lines, "default_model", defaultAlias);
        }
    } else {
        const std::string alias =
            chooseModelAlias(info, key, target.model, info.defaultModel);
        bool modelExists = false;
        for (const auto& model : info.models) {
            if (model.alias == alias) modelExists = true;
        }
        if (!modelExists || overwrite) {
            const auto body = modelBody(target, key, ownEntry || !modelExists);
            upsertSection(lines, "models", alias, body, {},
                          modelExists && !ownEntry);
        }

        if (makeDefault) {
            setTopLevelString(lines, "default_model", alias);
        }
    }

    std::string updated = joinLines(lines);
    if (updated == original) return;
    if (!updated.ends_with('\n')) updated += '\n';
    if (!original.empty()) backupLiveFile("kimi-code", file);
    atomicWrite(file, updated);
    restrictKimiFile(file);
}

bool eraseKimiEntry(const std::string& id) {
    const auto file = cfg::kimiConfigFile();
    const std::string original = readTextFile(file);
    KimiConfigInfo info = parseKimiConfig(original);
    // 先找本应用条目 llmswitch-<id>；没有则按 id 找收编的原生 provider
    // （收编不改名，id 就是原生键）。managed:* 是 Kimi 官方账号，任何情况
    // 下都不删。
    std::string key = "llmswitch-" + id;
    bool exists = false;
    for (const auto& provider : info.providers) {
        if (provider.key == key) exists = true;
    }
    if (!exists && !id.starts_with("managed:")) {
        for (const auto& provider : info.providers) {
            if (provider.key == id) {
                key = id;
                exists = true;
                break;
            }
        }
    }
    if (!exists) return false;

    Lines lines = splitLines(original);
    removeSection(lines, "providers", key);
    std::string removedDefault;
    for (const auto& model : info.models) {
        if (model.provider == key) {
            removeSection(lines, "models", model.alias);
            if (model.alias == info.defaultModel) removedDefault = model.alias;
        }
    }
    if (!removedDefault.empty()) {
        // default_model 不能悬空：指向剩下的任意一个模型别名；一个都不剩
        // 就删掉这个顶层键（Kimi 首次启动会自行重建）。
        std::string replacement;
        for (const auto& model : info.models) {
            if (model.provider == key) continue;
            replacement = model.alias;
            break;
        }
        if (replacement.empty()) {
            removeTopLevelKey(lines, "default_model");
        } else {
            setTopLevelString(lines, "default_model", replacement);
        }
    }
    const std::string updated = joinLines(lines);
    if (updated == original) return true;
    backupLiveFile("kimi-code", file);
    atomicWrite(file, updated);
    restrictKimiFile(file);
    return true;
}

void restoreKimiOfficial() {
    const auto file = cfg::kimiConfigFile();
    const std::string original = readTextFile(file);
    const KimiConfigInfo info = parseKimiConfig(original);
    const std::string managed = "managed:kimi-code";
    bool hasManaged = false;
    for (const auto& provider : info.providers) {
        if (provider.key == managed) hasManaged = true;
    }
    if (!hasManaged) {
        throw std::runtime_error(
            "Kimi Code 尚未登录官方账号（~/.kimi-code/config.toml 中没有 "
            "managed:kimi-code）；请先在 Kimi Code 里执行 /login");
    }
    std::string alias;
    if (!info.defaultModel.empty()) {
        for (const auto& model : info.models) {
            if (model.alias == info.defaultModel && model.provider == managed) {
                alias = model.alias;
                break;
            }
        }
    }
    if (alias.empty()) {
        for (const auto& model : info.models) {
            if (model.provider != managed) continue;
            if (model.alias == "kimi-code/kimi-for-coding") {
                alias = model.alias;
                break;
            }
            if (alias.empty()) alias = model.alias;
        }
    }
    if (alias.empty()) {
        throw std::runtime_error("Kimi Code 官方账号没有可用模型");
    }

    Lines lines = splitLines(original);
    for (const auto& provider : info.providers) {
        if (provider.key.starts_with("llmswitch-")) {
            removeSection(lines, "providers", provider.key);
        }
    }
    // provider 表已被手工删掉、只剩 model 别名的残留也一并清掉。
    for (const auto& model : info.models) {
        if (model.provider.starts_with("llmswitch-")) {
            removeSection(lines, "models", model.alias);
        }
    }
    setTopLevelString(lines, "default_model", alias);
    const std::string updated = joinLines(lines);
    if (updated == original) return;
    backupLiveFile("kimi-code", file);
    atomicWrite(file, updated);
    restrictKimiFile(file);
}

} // namespace store
