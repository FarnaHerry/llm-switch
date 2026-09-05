// mcp.cpp —— llmswitch.mcp 实现单元。
//
// 文件安全约定对齐 llmswitch.store：
//   - 所有写走 atomicWrite（<file>.tmp → rename，失败回落 remove+rename）；
//   - mcp.json 读走 readJsonOrNull：损坏挪 <file>.corrupt-<毫秒> 再按空继续；
//   - live 文件读走 readJsonLive：解析失败抛中文错，不挪不覆盖用户文件
//     （opencode 的提示带「JSON5 注释暂不支持」）；
//   - 改写任何 live 文件前快照到 backupsDir()/mcp/，每文件保留最近 10 份。
// nlohmann::json 模块下禁用 .items() 结构化绑定，遍历用 it.key()/it.value()。
module llmswitch.mcp;

import std;
import nlohmann.json;
import llmswitch.config;

namespace mcp {
namespace {

// 每文件保留的备份份数上限。
constexpr std::size_t kMaxBackups = 10;

std::int64_t nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 原子写文本文件：先写 <file>.tmp 再 rename；rename 失败回落 remove + rename。
void atomicWrite(const std::filesystem::path& dest, std::string_view content) {
    std::error_code ec;
    if (dest.has_parent_path()) std::filesystem::create_directories(dest.parent_path(), ec);
    const std::filesystem::path tmp = dest.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error(std::format("无法写入文件：{}", dest.string()));
        }
        out << content;
        out.flush();
        if (!out) {
            throw std::runtime_error(std::format("写入文件失败：{}", dest.string()));
        }
    }
    std::filesystem::rename(tmp, dest, ec);
    if (ec) {
        ec.clear();
        std::filesystem::remove(dest, ec);
        ec.clear();
        std::filesystem::rename(tmp, dest, ec);
        if (ec) {
            throw std::runtime_error(
                std::format("保存文件失败：{}（{}）", dest.string(), ec.message()));
        }
    }
}

std::string readTextFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return "";
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

// 读 mcp.json：不存在 → null；损坏 → 挪到 <file>.corrupt-<毫秒> 并返回 null。
nlohmann::json readJsonOrNull(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) return nullptr;
    const std::string text = readTextFile(file);
    if (text.empty()) return nullptr;
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded()) {
        const auto corrupt =
            std::filesystem::path(file.string() + ".corrupt-" + std::to_string(nowMillis()));
        std::filesystem::rename(file, corrupt, ec);
        return nullptr;
    }
    return j;
}

// 读 live 文件：解析失败抛中文错，不挪文件不覆盖 —— 宁可拒绝也不能静默吞掉
// 用户配置。json5 = true 时按 opencode 口径提示「JSON5 注释暂不支持」。
nlohmann::json readJsonLive(const std::filesystem::path& file, bool json5) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) return nullptr;
    const std::string text = readTextFile(file);
    if (text.empty()) return nullptr;
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded()) {
        if (json5) {
            throw std::runtime_error(std::format(
                "无法解析 {}：官方配置允许 JSON5 注释，含 JSON5 注释暂不支持。"
                "请把注释去掉后重试（原文件未被修改）。",
                file.string()));
        }
        throw std::runtime_error(std::format(
            "无法解析 {}：JSON 已损坏。为避免覆盖用户数据操作已中止（原文件未被修改）。",
            file.string()));
    }
    return j;
}

// 备份清理：dir 下 <prefix>.<毫秒>.bak 只保留最新 kMaxBackups 份
// （毫秒时间戳定宽 13 位，文件名字典序即时间序）。
void pruneBackups(const std::filesystem::path& dir, const std::string& prefix) {
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.starts_with(prefix + ".") && name.ends_with(".bak")) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    while (files.size() > kMaxBackups) {
        std::filesystem::remove(files.front(), ec);
        files.erase(files.begin());
    }
}

// 改写 live 文件前的快照：复制到 backupsDir()/mcp/<文件名>.<毫秒>.bak。
// 文件不存在则无事发生；复制失败抛异常（宁可不写也不能无备份改写）。
void backupLiveFile(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) return;
    const auto dir = cfg::backupsDir() / "mcp";
    std::filesystem::create_directories(dir, ec);
    // 同毫秒内连续写入时递增时间戳避免覆盖。
    std::int64_t ts = nowMillis();
    std::filesystem::path dest;
    do {
        dest = dir / std::format("{}.{}.bak", file.filename().string(), ts++);
    } while (std::filesystem::exists(dest, ec));
    std::filesystem::copy_file(file, dest, ec);
    if (ec) {
        throw std::runtime_error(
            std::format("备份失败：{}（{}）", file.string(), ec.message()));
    }
    pruneBackups(dir, file.filename().string());
}

// ---- JSON 辅助 ----------------------------------------------------------------

// 对象里的字符串字段（缺失/非字符串 → 空串）。
std::string jsonStr(const nlohmann::json& j, std::string_view key) {
    if (!j.is_object()) return "";
    const auto it = j.find(std::string(key));
    if (it == j.end() || !it->is_string()) return "";
    return it->get<std::string>();
}

// 对象里的字符串数组字段（缺失/非数组 → 空）。
std::vector<std::string> jsonStrVec(const nlohmann::json& j, std::string_view key) {
    std::vector<std::string> out;
    if (!j.is_object()) return out;
    const auto it = j.find(std::string(key));
    if (it == j.end() || !it->is_array()) return out;
    for (const auto& v : *it) {
        if (v.is_string()) out.push_back(v.get<std::string>());
    }
    return out;
}

// 对象里的字符串 map 字段（缺失/非对象 → 空）。
std::map<std::string, std::string> jsonStrMap(const nlohmann::json& j,
                                              std::string_view key) {
    std::map<std::string, std::string> out;
    if (!j.is_object()) return out;
    const auto it = j.find(std::string(key));
    if (it == j.end() || !it->is_object()) return out;
    for (auto e = it->begin(); e != it->end(); ++e) {
        if (e.value().is_string()) out[e.key()] = e.value().get<std::string>();
    }
    return out;
}

// ---- mcp.json 序列化 -----------------------------------------------------------

nlohmann::json serverToJson(const McpServer& s) {
    auto j = nlohmann::json::object();
    j["name"] = s.name;
    j["type"] = s.type;
    j["command"] = s.command;
    j["args"] = s.args;
    j["env"] = s.env;
    j["url"] = s.url;
    j["headers"] = s.headers;
    j["enabledTools"] = s.enabledTools;
    return j;
}

McpServer serverFromJson(const nlohmann::json& j) {
    McpServer s;
    s.name = jsonStr(j, "name");
    s.type = jsonStr(j, "type");
    s.command = jsonStr(j, "command");
    s.args = jsonStrVec(j, "args");
    s.env = jsonStrMap(j, "env");
    s.url = jsonStr(j, "url");
    s.headers = jsonStrMap(j, "headers");
    s.enabledTools = jsonStrVec(j, "enabledTools");
    return s;
}

// ---- 各工具 live 条目构造 -------------------------------------------------------

// claude-code：~/.claude.json mcpServers.<name>。
nlohmann::json claudeEntry(const McpServer& s) {
    auto e = nlohmann::json::object();
    if (s.type == "stdio") {
        e["command"] = s.command;
        e["args"] = s.args;
        if (!s.env.empty()) e["env"] = s.env;
    } else {  // sse / http
        e["type"] = s.type;
        e["url"] = s.url;
        if (!s.headers.empty()) e["headers"] = s.headers;
    }
    return e;
}

// opencode：opencode.json mcp.<name>。
nlohmann::json opencodeEntry(const McpServer& s) {
    auto e = nlohmann::json::object();
    if (s.type == "stdio") {
        e["type"] = "local";
        auto cmd = nlohmann::json::array();
        cmd.push_back(s.command);
        for (const auto& a : s.args) cmd.push_back(a);
        e["command"] = std::move(cmd);
        if (!s.env.empty()) e["environment"] = s.env;
        e["enabled"] = true;
    } else {  // sse / http
        e["type"] = "remote";
        e["url"] = s.url;
        if (!s.headers.empty()) e["headers"] = s.headers;
        e["enabled"] = true;
    }
    return e;
}

// ---- 各工具 live 写入 -----------------------------------------------------------

// JSON live 文件（claude-code / opencode）共用：upsert 或删除 mapKey.<name>，
// 深合并保留其余字段与用户的其他条目。
void upsertJsonEntry(const std::filesystem::path& file, bool json5,
                     std::string_view mapKey, const std::string& name,
                     const nlohmann::json& entry) {
    auto j = readJsonLive(file, json5);
    if (!j.is_object()) j = nlohmann::json::object();
    const std::string mk(mapKey);
    if (!j.contains(mk) || !j[mk].is_object()) j[mk] = nlohmann::json::object();
    backupLiveFile(file);
    j[mk][name] = entry;
    atomicWrite(file, j.dump(2));
}

void removeJsonEntry(const std::filesystem::path& file, bool json5,
                     std::string_view mapKey, std::string_view name) {
    auto j = readJsonLive(file, json5);
    if (!j.is_object()) return;
    const std::string mk(mapKey);
    const auto it = j.find(mk);
    if (it == j.end() || !it->is_object() || !it->contains(std::string(name))) return;
    backupLiveFile(file);
    it->erase(std::string(name));
    atomicWrite(file, j.dump(2));
}

// TOML 基本字符串转义：\ " 与不可见字符。
std::string tomlEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20 ||
                static_cast<unsigned char>(c) == 0x7f) {
                out += std::format("\\u{:04X}", static_cast<unsigned int>(
                                                   static_cast<unsigned char>(c)));
            } else {
                out += c;
            }
        }
    }
    return out;
}

// codex：行级 section 重写 —— 丢弃所有 header 为 [mcp_servers.*] 的节，
// 其余内容（含首节前的 preamble）原样保留。
std::string stripCodexMcpSections(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    std::istringstream in(text);
    std::string line;
    bool dropping = false;
    while (std::getline(in, line)) {
        const auto pos = line.find_first_not_of(" \t");
        if (pos != std::string::npos && line[pos] == '[') {
            const auto close = line.find(']', pos);
            std::string header =
                close == std::string::npos ? line.substr(pos + 1)
                                           : line.substr(pos + 1, close - pos - 1);
            const auto b = header.find_first_not_of(" \t");
            const auto e2 = header.find_last_not_of(" \t");
            header = b == std::string::npos ? "" : header.substr(b, e2 - b + 1);
            dropping = header.starts_with("mcp_servers.");
        }
        if (!dropping) {
            out += line;
            out += '\n';
        }
    }
    return out;
}

// 生成 [mcp_servers.<name>] 节文本。
std::string codexSections(const std::vector<McpServer>& servers) {
    std::string out;
    for (const auto& s : servers) {
        out += std::format("[mcp_servers.{}]\n", s.name);
        if (s.type == "stdio") {
            out += "command = \"" + tomlEscape(s.command) + "\"\n";
            if (!s.args.empty()) {
                out += "args = [";
                bool first = true;
                for (const auto& a : s.args) {
                    if (!first) out += ", ";
                    out += "\"" + tomlEscape(a) + "\"";
                    first = false;
                }
                out += "]\n";
            }
        } else {  // sse / http
            out += "url = \"" + tomlEscape(s.url) + "\"\n";
        }
        if (!s.env.empty()) {
            out += std::format("[mcp_servers.{}.env]\n", s.name);
            for (const auto& kv : s.env) {
                out += kv.first + " = \"" + tomlEscape(kv.second) + "\"\n";
            }
        }
        out += '\n';
    }
    return out;
}

// codex live 同步：mcp_servers.* 段整体按「当前对该工具启用的服务器清单」重生成。
void syncCodexLive(const std::vector<McpServer>& servers) {
    const auto file = cfg::codexConfigFile();
    std::error_code ec;
    const bool exists = std::filesystem::exists(file, ec) && !ec;
    std::vector<McpServer> enabled;
    for (const auto& s : servers) {
        if (std::ranges::find(s.enabledTools, std::string_view("codex")) !=
            s.enabledTools.end()) {
            enabled.push_back(s);
        }
    }
    if (!exists && enabled.empty()) return;  // 没有文件也没有条目：不动
    const std::string stripped = stripCodexMcpSections(readTextFile(file));
    const std::string generated = codexSections(enabled);
    std::string content = stripped;
    if (!content.empty() && !generated.empty() &&
        !content.ends_with("\n\n")) {
        content += '\n';
    }
    content += generated;
    backupLiveFile(file);
    atomicWrite(file, content);
}

// ---- 工具分发 -------------------------------------------------------------------

bool isSupportedTool(std::string_view toolId) {
    return toolId == "claude-code" || toolId == "codex" || toolId == "opencode";
}

void requireSupported(std::string_view toolId) {
    if (!isSupportedTool(toolId)) {
        throw std::runtime_error(
            std::format("该工具暂不支持 MCP 管理：{}", toolId));
    }
}

void writeLiveEntry(std::string_view toolId, const McpServer& s,
                    const std::vector<McpServer>& all) {
    requireSupported(toolId);
    if (toolId == "claude-code") {
        upsertJsonEntry(cfg::claudeJsonFile(), false, "mcpServers", s.name,
                        claudeEntry(s));
    } else if (toolId == "opencode") {
        upsertJsonEntry(cfg::opencodeConfigFile(), true, "mcp", s.name,
                        opencodeEntry(s));
    } else {  // codex：整体重生成
        syncCodexLive(all);
    }
}

void removeLiveEntry(std::string_view toolId, std::string_view name,
                     const std::vector<McpServer>& all) {
    requireSupported(toolId);
    if (toolId == "claude-code") {
        removeJsonEntry(cfg::claudeJsonFile(), false, "mcpServers", name);
    } else if (toolId == "opencode") {
        removeJsonEntry(cfg::opencodeConfigFile(), true, "mcp", name);
    } else {  // codex：整体重生成
        syncCodexLive(all);
    }
}

// ---- importFromTool 回读 ---------------------------------------------------------

// claude-code：~/.claude.json mcpServers。
std::vector<McpServer> parseClaudeMcp() {
    std::vector<McpServer> out;
    const auto j = readJsonLive(cfg::claudeJsonFile(), false);
    if (!j.is_object()) return out;
    const auto it = j.find("mcpServers");
    if (it == j.end() || !it->is_object()) return out;
    for (auto e = it->begin(); e != it->end(); ++e) {
        if (!e.value().is_object()) continue;
        McpServer s;
        s.name = e.key();
        s.type = jsonStr(e.value(), "type");
        if (s.type.empty()) {
            s.type = e.value().contains("url") ? "sse" : "stdio";
        }
        if (s.type == "stdio") {
            s.command = jsonStr(e.value(), "command");
            s.args = jsonStrVec(e.value(), "args");
            s.env = jsonStrMap(e.value(), "env");
        } else {
            s.url = jsonStr(e.value(), "url");
            s.headers = jsonStrMap(e.value(), "headers");
        }
        out.push_back(std::move(s));
    }
    return out;
}

// opencode：opencode.json mcp（local → stdio；remote 不区分 sse/http，按 http 收编）。
std::vector<McpServer> parseOpencodeMcp() {
    std::vector<McpServer> out;
    const auto j = readJsonLive(cfg::opencodeConfigFile(), true);
    if (!j.is_object()) return out;
    const auto it = j.find("mcp");
    if (it == j.end() || !it->is_object()) return out;
    for (auto e = it->begin(); e != it->end(); ++e) {
        if (!e.value().is_object()) continue;
        McpServer s;
        s.name = e.key();
        if (jsonStr(e.value(), "type") == "local") {
            s.type = "stdio";
            const auto cmd = jsonStrVec(e.value(), "command");
            if (!cmd.empty()) {
                s.command = cmd.front();
                s.args.assign(cmd.begin() + 1, cmd.end());
            }
            s.env = jsonStrMap(e.value(), "environment");
        } else {
            s.type = "http";
            s.url = jsonStr(e.value(), "url");
            s.headers = jsonStrMap(e.value(), "headers");
        }
        out.push_back(std::move(s));
    }
    return out;
}

} // namespace

// ---- McpStore -------------------------------------------------------------------

McpStore McpStore::load() {
    McpStore st;
    const auto j = readJsonOrNull(cfg::mcpStoreFile());
    if (!j.is_object()) return st;
    const auto it = j.find("servers");
    if (it == j.end() || !it->is_array()) return st;
    for (const auto& e : *it) {
        auto s = serverFromJson(e);
        if (!s.name.empty()) st.servers_.push_back(std::move(s));
    }
    return st;
}

void McpStore::save() const {
    auto j = nlohmann::json::object();
    auto arr = nlohmann::json::array();
    for (const auto& s : servers_) arr.push_back(serverToJson(s));
    j["servers"] = std::move(arr);
    atomicWrite(cfg::mcpStoreFile(), j.dump(2));
}

void McpStore::upsert(McpServer srv) {
    if (srv.name.empty()) {
        throw std::runtime_error("MCP 服务器 name 不能为空");
    }
    if (srv.type.empty()) srv.type = "stdio";
    for (const auto& t : srv.enabledTools) requireSupported(t);
    for (auto& cur : servers_) {
        if (cur.name != srv.name) continue;
        // 旧启用面里被摘掉的工具：也要从 live 移除条目，避免残留。
        std::vector<std::string> removed;
        for (const auto& t : cur.enabledTools) {
            if (std::ranges::find(srv.enabledTools, t) == srv.enabledTools.end()) {
                removed.push_back(t);
            }
        }
        cur = srv;  // 先更新内存态，codex 的整段重生成才能读到最新清单
        for (const auto& t : removed) removeLiveEntry(t, cur.name, servers_);
        for (const auto& t : cur.enabledTools) writeLiveEntry(t, cur, servers_);
        save();
        return;
    }
    servers_.push_back(std::move(srv));
    for (const auto& t : servers_.back().enabledTools) {
        writeLiveEntry(t, servers_.back(), servers_);
    }
    save();
}

void McpStore::remove(std::string_view name) {
    for (std::size_t i = 0; i < servers_.size(); ++i) {
        if (servers_[i].name != name) continue;
        const auto tools = servers_[i].enabledTools;
        servers_.erase(servers_.begin() + static_cast<std::ptrdiff_t>(i));
        for (const auto& t : tools) removeLiveEntry(t, name, servers_);
        save();
        return;
    }
}

void McpStore::setEnabled(std::string_view name, std::string_view toolId,
                          bool enabled) {
    requireSupported(toolId);
    for (auto& s : servers_) {
        if (s.name != name) continue;
        const auto it = std::ranges::find(s.enabledTools, toolId);
        const bool has = it != s.enabledTools.end();
        if (enabled == has) return;  // 已是目标状态
        if (enabled) {
            s.enabledTools.push_back(std::string(toolId));
            writeLiveEntry(toolId, s, servers_);
        } else {
            s.enabledTools.erase(it);
            removeLiveEntry(toolId, name, servers_);
        }
        save();
        return;
    }
    throw std::runtime_error(std::format("MCP 服务器不存在：{}", name));
}

std::size_t McpStore::importFromTool(std::string_view toolId) {
    requireSupported(toolId);
    if (toolId == "codex") return 0;  // TOML 回读太脆，跳过
    const auto found =
        toolId == "claude-code" ? parseClaudeMcp() : parseOpencodeMcp();
    std::size_t added = 0;
    bool changed = false;
    for (auto s : found) {
        bool merged = false;
        for (auto& cur : servers_) {
            if (cur.name != s.name) continue;
            if (std::ranges::find(cur.enabledTools, toolId) ==
                cur.enabledTools.end()) {
                cur.enabledTools.push_back(std::string(toolId));
                changed = true;
            }
            merged = true;
            break;
        }
        if (!merged) {
            s.enabledTools = {std::string(toolId)};
            servers_.push_back(std::move(s));
            ++added;
            changed = true;
        }
    }
    if (changed) save();
    return added;
}

} // namespace mcp
