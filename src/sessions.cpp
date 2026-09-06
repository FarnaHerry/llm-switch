// sessions.cpp — llmswitch.sessions 实现单元。
//
// 解析约定（全 best-effort，任何失败都回落，绝不抛）：
//   - title 只读文件前 ~64KB；claude 取第一条 type=="user" 且文本非命令/系统样
//     （不以 '/' 或 '<' 开头）的行的 text，截取 80 字符（按 UTF-8 边界截断）；
//     codex rollout 结构不同，尽力从 payload 里取 user 文本，取不到用文件 stem；
//   - messageCount 是 jsonl 行数（64KB 块读数换行，比 getline 逐行快一个
//     量级；末行无换行符也算一行），数到 10000 封顶；
//   - mtime 用 file_clock → system_clock 近似换算，只用于排序与展示。
// nlohmann::json 模块下禁用 .items() 结构化绑定，遍历用 it.key()/it.value()。
module llmswitch.sessions;

import std;
import nlohmann.json;
import llmswitch.config;

namespace sessions {
namespace {

constexpr std::size_t kMaxCountLines = 10000;
constexpr std::size_t kTitleReadBytes = 64 * 1024;
constexpr std::size_t kTitleMaxLen = 80;

std::int64_t fileTimeToMillis(std::filesystem::file_time_type t) {
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        t - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::duration_cast<std::chrono::milliseconds>(sys.time_since_epoch()).count();
}

std::string readHead(const std::filesystem::path& file, std::size_t maxBytes) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return "";
    std::string buf(maxBytes, '\0');
    in.read(buf.data(), static_cast<std::streamsize>(maxBytes));
    buf.resize(static_cast<std::size_t>(in.gcount()));
    return buf;
}

std::size_t countLines(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return 0;
    std::size_t n = 0;
    char last = '\n';
    std::array<char, 64 * 1024> buf;
    while (n < kMaxCountLines) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto got = static_cast<std::size_t>(in.gcount());
        if (got == 0) break;
        last = buf[got - 1];
        n += static_cast<std::size_t>(
            std::count(buf.data(), buf.data() + got, '\n'));
    }
    if (n > kMaxCountLines) n = kMaxCountLines;
    // 末行无换行符也算一行（与原 getline 计数口径一致）。
    if (n < kMaxCountLines && last != '\n') ++n;
    return n;
}

// 截取 80 字符：按字节截到 80 后回退到 UTF-8 边界，不切碎多字节字符。
std::string truncateTitle(std::string_view text) {
    if (text.size() <= kTitleMaxLen) return std::string(text);
    std::size_t cut = kTitleMaxLen;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    // text[cut] 是某个多字节字符的首字节但被截断时，丢弃这个不完整字符。
    const auto lead = static_cast<unsigned char>(text[cut]);
    const int len = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : 4;
    if (cut + static_cast<std::size_t>(len) > kTitleMaxLen) return std::string(text.substr(0, cut));
    return std::string(text.substr(0, kTitleMaxLen));
}

std::string trim(std::string_view s) {
    const auto isSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (!s.empty() && isSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && isSpace(s.back())) s.remove_suffix(1);
    return std::string(s);
}

// 命令/系统样的文本不作标题（slash 命令、<system-reminder> 之类）。
bool looksLikeCommand(std::string_view text) {
    return text.starts_with('/') || text.starts_with('<');
}

// 从 content 字段取文本：纯字符串或 [{"type":"text"/"input_text","text":...}] 数组。
std::string contentText(const nlohmann::json& content) {
    if (content.is_string()) return content.get<std::string>();
    if (content.is_array()) {
        for (const auto& item : content) {
            if (item.is_object() && item.contains("text") && item["text"].is_string()) {
                return item["text"].get<std::string>();
            }
        }
    }
    return "";
}

// claude jsonl：{"type":"user","message":{"role":"user","content":[...]}}。
std::string extractClaudeTitle(std::string_view head) {
    std::size_t pos = 0;
    while (pos <= head.size()) {
        const auto nl = head.find('\n', pos);
        const std::string_view line =
            nl == std::string_view::npos ? head.substr(pos) : head.substr(pos, nl - pos);
        pos = nl == std::string_view::npos ? head.size() + 1 : nl + 1;
        if (line.empty()) continue;
        const auto j = nlohmann::json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.is_object()) continue;
        if (!j.contains("type") || j["type"] != "user") continue;
        if (!j.contains("message") || !j["message"].is_object()) continue;
        const auto& msg = j["message"];
        if (!msg.contains("content")) continue;
        const std::string text = trim(contentText(msg["content"]));
        if (text.empty() || looksLikeCommand(text)) continue;
        return truncateTitle(text);
    }
    return "";
}

// codex rollout jsonl：{"type":"response_item","payload":{"type":"message",
// "role":"user","content":[{"type":"input_text","text":"..."}]}}。取不到就回落。
std::string extractCodexTitle(std::string_view head) {
    std::size_t pos = 0;
    while (pos <= head.size()) {
        const auto nl = head.find('\n', pos);
        const std::string_view line =
            nl == std::string_view::npos ? head.substr(pos) : head.substr(pos, nl - pos);
        pos = nl == std::string_view::npos ? head.size() + 1 : nl + 1;
        if (line.empty()) continue;
        const auto j = nlohmann::json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.is_object() || !j.contains("payload")) continue;
        const auto& payload = j["payload"];
        if (!payload.is_object()) continue;
        if (!payload.contains("role") || payload["role"] != "user") continue;
        if (!payload.contains("content")) continue;
        const std::string text = trim(contentText(payload["content"]));
        if (text.empty() || looksLikeCommand(text)) continue;
        return truncateTitle(text);
    }
    return "";
}

SessionInfo makeInfo(std::string_view tool, std::string_view project,
                     const std::filesystem::path& path) {
    SessionInfo info;
    info.tool = tool;
    info.project = project;
    info.path = path;
    info.id = path.stem().string();
    std::error_code ec;
    if (const auto t = std::filesystem::last_write_time(path, ec); !ec) {
        info.mtimeMillis = fileTimeToMillis(t);
    }
    if (const auto s = std::filesystem::file_size(path, ec); !ec) {
        info.sizeBytes = s;
    }
    info.messageCount = countLines(path);
    const std::string head = readHead(path, kTitleReadBytes);
    info.title = tool == "claude-code" ? extractClaudeTitle(head) : extractCodexTitle(head);
    if (info.title.empty()) info.title = info.id;
    return info;
}

void scanClaude(std::vector<SessionInfo>& out) {
    std::error_code ec;
    const std::filesystem::path root = cfg::claudeProjectsDir();
    for (const auto& proj : std::filesystem::directory_iterator(root, ec)) {
        if (!proj.is_directory(ec)) continue;
        const std::string project = proj.path().filename().string();
        std::error_code ec2;
        for (const auto& e : std::filesystem::directory_iterator(proj.path(), ec2)) {
            if (!e.is_regular_file(ec2) || e.path().extension() != ".jsonl") continue;
            out.push_back(makeInfo("claude-code", project, e.path()));
        }
    }
}

void scanCodex(std::vector<SessionInfo>& out) {
    std::error_code ec;
    const std::filesystem::path root = cfg::codexSessionsDir();
    // libc++ 21 起 recursive_directory_iterator 只剩 default_sentinel 比较
    // （迭代器对 != 已移除，range-for 编不过）；显式迭代 + 哨兵比较三标准库通吃。
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         it != std::default_sentinel; it.increment(ec)) {
        const auto& e = *it;
        if (!e.is_regular_file(ec) || e.path().extension() != ".jsonl") continue;
        std::error_code ec2;
        std::string project =
            std::filesystem::relative(e.path().parent_path(), root, ec2).generic_string();
        if (ec2) project.clear();
        out.push_back(makeInfo("codex", project, e.path()));
    }
}

void sortByMtimeDesc(std::vector<SessionInfo>& v) {
    std::ranges::stable_sort(v, [](const SessionInfo& a, const SessionInfo& b) {
        return a.mtimeMillis > b.mtimeMillis;
    });
}

// p 是否严格位于 root 之下（lexically_normal 后的前缀比较）。
bool isUnder(const std::filesystem::path& p, const std::filesystem::path& root) {
    if (root.empty()) return false;
    const auto np = p.lexically_normal();
    const auto nr = root.lexically_normal();
    auto [itP, itR] = std::mismatch(np.begin(), np.end(), nr.begin(), nr.end());
    return itR == nr.end() && itP != np.end();
}

void ensureKnownSessionPath(const std::filesystem::path& p) {
    if (!isUnder(p, cfg::claudeProjectsDir()) && !isUnder(p, cfg::codexSessionsDir())) {
        throw std::runtime_error(
            std::format("拒绝操作：路径不在已知会话目录之下：{}", p.string()));
    }
}

} // namespace

std::vector<SessionInfo> listSessions() {
    std::vector<SessionInfo> out;
    scanClaude(out);
    scanCodex(out);
    sortByMtimeDesc(out);
    return out;
}

std::vector<SessionInfo> listSessions(std::string_view toolId) {
    std::vector<SessionInfo> out;
    if (toolId == "claude-code") {
        scanClaude(out);
    } else if (toolId == "codex") {
        scanCodex(out);
    } else {
        throw std::runtime_error(std::format("未知工具：{}", toolId));
    }
    sortByMtimeDesc(out);
    return out;
}

void deleteSession(const std::filesystem::path& p) {
    ensureKnownSessionPath(p);
    std::error_code ec;
    if (!std::filesystem::remove(p, ec) || ec) {
        throw std::runtime_error(std::format("删除会话失败：{}（{}）", p.string(), ec.message()));
    }
}

std::filesystem::path exportSession(const std::filesystem::path& p,
                                    const std::filesystem::path& destDir) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec) || ec) {
        throw std::runtime_error(std::format("会话文件不存在：{}", p.string()));
    }
    std::filesystem::create_directories(destDir, ec);
    if (ec) {
        throw std::runtime_error(
            std::format("创建目录失败：{}（{}）", destDir.string(), ec.message()));
    }
    const std::filesystem::path dest = destDir / p.filename();
    std::filesystem::copy_file(p, dest, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        throw std::runtime_error(
            std::format("导出会话失败：{} → {}（{}）", p.string(), dest.string(), ec.message()));
    }
    return dest;
}

} // namespace sessions
