// sessions.cpp — llmswitch.sessions 实现单元。
//
// 解析约定：
//   - 列表扫描在后台批次中读取每个文件头 ~4KB 与尾部 ~8KB，提取标题和最近消息
//     摘要，不为列表统计整文件行数；
//   - 完整消息只在详情页调用 readSession，并由 UI 放到 RunWorker 中执行；
//   - mtime 用 file_clock → system_clock 近似换算，只用于排序与展示。
// nlohmann::json 模块下禁用 .items() 结构化绑定，遍历用 it.key()/it.value()。
module llmswitch.sessions;

import std;
import nlohmann.json;
import llmswitch.config;

namespace sessions {
namespace {

constexpr std::size_t kSummaryHeadBytes = 4 * 1024;
constexpr std::size_t kSummaryTailBytes = 8 * 1024;
constexpr std::size_t kTitleMaxLen = 80;
// 单条消息进入 UI 的文本上限：详情页的展开/滚动都要对它做原生文本排版，
// 不设界的话超大工具输出会冻结 UI（完整内容走导出，不经过该路径）。
constexpr std::size_t kMaxStoredMessageBytes = 16 * 1024;

// 会话文件里绝大多数记录是工具调用、增量事件和上下文快照，其中有些单行可达
// 数 MB。先做不分配内存的宽松筛选，避免为了最终必然丢弃的记录构造完整 JSON
// DOM；筛选允许假阳性，正确性仍由 parseMessageLine 的结构检查保证。
bool mayContainDisplayMessage(std::string_view tool, std::string_view line) {
    if (tool == "claude-code") {
        return line.find("\"type\"") != std::string_view::npos &&
               (line.find("\"user\"") != std::string_view::npos ||
                line.find("\"assistant\"") != std::string_view::npos) &&
               line.find("\"message\"") != std::string_view::npos;
    }
    if (tool == "codex") {
        return line.find("\"payload\"") != std::string_view::npos &&
               line.find("\"message\"") != std::string_view::npos &&
               line.find("\"role\"") != std::string_view::npos;
    }
    return true;
}

std::int64_t fileTimeToMillis(std::filesystem::file_time_type t) {
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        t - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::duration_cast<std::chrono::milliseconds>(sys.time_since_epoch()).count();
}

struct FileSummary {
    std::string head;
    std::string tail;
};

FileSummary readSummary(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    const auto end = in.tellg();
    if (end <= 0) return {};

    const auto headSize = std::min<std::streamoff>(
        end, static_cast<std::streamoff>(kSummaryHeadBytes));

    // 小文件同时作为头部和尾部摘要，单次读取即可，避免列表扫描为每个
    // 短会话重复打开/定位/读取同一份 JSONL。
    if (end <= static_cast<std::streamoff>(kSummaryHeadBytes + kSummaryTailBytes)) {
        in.seekg(0, std::ios::beg);
        FileSummary result;
        std::string all;
        all.resize(static_cast<std::size_t>(end));
        in.read(all.data(), static_cast<std::streamsize>(all.size()));
        all.resize(static_cast<std::size_t>(in.gcount()));
        result.head = all;
        result.tail = std::move(all);
        return result;
    }

    in.seekg(0, std::ios::beg);
    FileSummary result;
    result.head.resize(static_cast<std::size_t>(headSize));
    in.read(result.head.data(), static_cast<std::streamsize>(headSize));
    result.head.resize(static_cast<std::size_t>(in.gcount()));

    in.clear();
    const auto start = std::max<std::streamoff>(
        0, end - static_cast<std::streamoff>(kSummaryTailBytes));
    in.seekg(start, std::ios::beg);
    const auto available = end - start;
    result.tail.resize(static_cast<std::size_t>(available));
    in.read(result.tail.data(), static_cast<std::streamsize>(available));
    result.tail.resize(static_cast<std::size_t>(in.gcount()));
    return result;
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
        std::string result;
        for (const auto& item : content) {
            if (item.is_object() && item.contains("text") && item["text"].is_string()) {
                if (!result.empty()) result.push_back('\n');
                result += item["text"].get<std::string>();
            }
        }
        return result;
    }
    return "";
}

std::optional<SessionMessage> parseMessageLine(std::string_view tool,
                                                std::string_view line) {
    if (line.empty() || !mayContainDisplayMessage(tool, line)) return std::nullopt;
    const auto j = nlohmann::json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;

    const nlohmann::json* message = nullptr;
    std::string role;
    if (tool == "claude-code") {
        if (!j.contains("type") || (j["type"] != "user" && j["type"] != "assistant")) {
            return std::nullopt;
        }
        role = j["type"].get<std::string>();
        if (!j.contains("message") || !j["message"].is_object()) return std::nullopt;
        message = &j["message"];
        if (message->contains("role") && (*message)["role"].is_string()) {
            role = (*message)["role"].get<std::string>();
        }
    } else if (tool == "codex") {
        if (!j.contains("payload") || !j["payload"].is_object()) return std::nullopt;
        const auto& payload = j["payload"];
        if (!payload.contains("type") || payload["type"] != "message") {
            return std::nullopt;
        }
        if (!payload.contains("role") || !payload["role"].is_string()) {
            return std::nullopt;
        }
        role = payload["role"].get<std::string>();
        message = &payload;
    } else {
        throw std::runtime_error(std::format("未知工具：{}", tool));
    }

    if (role != "user" && role != "assistant" || !message->contains("content")) {
        return std::nullopt;
    }
    std::string text = trim(contentText((*message)["content"]));
    if (text.empty()) return std::nullopt;
    // 原生 Text 的排版成本随长度线性膨胀（Pango 逐段 itemize + shaping），
    // 几百 KB 的工具输出会让详情页在展开/滚动时冻结 UI。详情展示做有界
    // 截断；完整内容始终可通过导出（原文件）获取。
    if (text.size() > kMaxStoredMessageBytes) {
        std::size_t end = kMaxStoredMessageBytes;
        while (end > 0 &&
               (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
            --end;
        }
        text.resize(end);
        text += "\n……（内容过长已截断，完整内容请导出后查看）";
    }
    return SessionMessage{std::move(role), std::move(text)};
}

template <class Callback>
void forEachMessage(std::string_view tool, std::string_view text, Callback&& callback) {
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const auto nl = text.find('\n', pos);
        const std::string_view line =
            nl == std::string_view::npos ? text.substr(pos) : text.substr(pos, nl - pos);
        pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
        if (const auto message = parseMessageLine(tool, line)) callback(*message);
    }
}

std::string extractTitle(std::string_view tool, std::string_view head) {
    std::string title;
    forEachMessage(tool, head, [&title](const SessionMessage& message) {
        if (title.empty() && message.role == "user" && !looksLikeCommand(message.text)) {
            title = truncateTitle(message.text);
        }
    });
    return title;
}

std::string extractPreview(std::string_view tool, std::string_view tail) {
    std::string preview;
    forEachMessage(tool, tail, [&preview](const SessionMessage& message) {
        if (!looksLikeCommand(message.text)) preview = truncateTitle(message.text);
    });
    return preview;
}

SessionSummary buildSessionSummary(std::string_view tool, const FileSummary& file) {
    SessionSummary result;
    result.title = extractTitle(tool, file.head);
    result.preview = extractPreview(tool, file.tail);
    return result;
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
    // 在列表扫描 Worker 中一次完成固定大小摘要，避免每个 VirtualList 行再启动
    // 一个文件读取协程。读取失败仍保留可展示的轻量元数据。
    const SessionSummary summary = buildSessionSummary(tool, readSummary(path));
    info.title = summary.title.empty() ? info.id : summary.title;
    info.preview = summary.preview.empty() ? info.title : summary.preview;
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

SessionSummary summarizeSession(std::string_view toolId,
                                const std::filesystem::path& path) {
    if (toolId != "claude-code" && toolId != "codex") {
        throw std::runtime_error(std::format("未知工具：{}", toolId));
    }
    ensureKnownSessionPath(path);

    SessionSummary result = buildSessionSummary(toolId, readSummary(path));
    if (result.title.empty()) result.title = path.stem().string();
    if (result.preview.empty()) result.preview = result.title;
    return result;
}

std::vector<SessionMessage> readSession(std::string_view toolId,
                                        const std::filesystem::path& path) {
    if (toolId != "claude-code" && toolId != "codex") {
        throw std::runtime_error(std::format("未知工具：{}", toolId));
    }
    ensureKnownSessionPath(path);

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error(std::format("读取会话失败：{}", path.string()));
    }

    std::vector<SessionMessage> messages;
    std::string line;
    while (std::getline(in, line)) {
        if (const auto message = parseMessageLine(toolId, line)) {
            messages.push_back(*message);
        }
    }
    if (in.bad()) {
        throw std::runtime_error(std::format("读取会话失败：{}", path.string()));
    }
    return messages;
}

SessionMessagePage readSessionPage(std::string_view toolId,
                                   const std::filesystem::path& path,
                                   std::uintmax_t beforeOffset,
                                   std::size_t maxMessages) {
    if (toolId != "claude-code" && toolId != "codex") {
        throw std::runtime_error(std::format("未知工具：{}", toolId));
    }
    ensureKnownSessionPath(path);
    if (maxMessages == 0) return {};

    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error(std::format("读取会话失败：{}", path.string()));
    in.seekg(0, std::ios::end);
    const auto fileEnd = in.tellg();
    if (fileEnd <= 0) return {};
    std::uintmax_t position = beforeOffset == 0
                                  ? static_cast<std::uintmax_t>(fileEnd)
                                  : std::min(beforeOffset,
                                             static_cast<std::uintmax_t>(fileEnd));

    constexpr std::uintmax_t kChunkBytes = 64 * 1024;
    std::string carry;
    SessionMessagePage page;
    std::uintmax_t earliestMessageOffset = position;
    while (position > 0 && page.messages.size() < maxMessages) {
        const std::uintmax_t chunkStart = position > kChunkBytes
                                              ? position - kChunkBytes
                                              : 0;
        const std::size_t chunkSize = static_cast<std::size_t>(position - chunkStart);
        std::string data(chunkSize, '\0');
        in.clear();
        in.seekg(static_cast<std::streamoff>(chunkStart), std::ios::beg);
        in.read(data.data(), static_cast<std::streamsize>(chunkSize));
        data.resize(static_cast<std::size_t>(in.gcount()));
        data += carry;

        std::size_t completeStart = 0;
        if (chunkStart > 0) {
            const auto firstNewline = data.find('\n');
            if (firstNewline == std::string::npos) {
                carry = std::move(data);
                position = chunkStart;
                continue;
            }
            carry.assign(data, 0, firstNewline);
            completeStart = firstNewline + 1;
        }

        std::size_t lineEnd = data.size();
        while (lineEnd > completeStart && page.messages.size() < maxMessages) {
            if (data[lineEnd - 1] == '\n') --lineEnd;
            const auto newline = data.rfind('\n', lineEnd == 0 ? 0 : lineEnd - 1);
            const std::size_t lineStart =
                newline == std::string::npos || newline < completeStart
                    ? completeStart
                    : newline + 1;
            if (const auto message = parseMessageLine(
                    toolId, std::string_view(data).substr(lineStart, lineEnd - lineStart))) {
                auto positionedMessage = *message;
                positionedMessage.sourceOffset = chunkStart + lineStart;
                page.messages.push_back(std::move(positionedMessage));
                earliestMessageOffset = chunkStart + lineStart;
            }
            if (lineStart == completeStart) break;
            lineEnd = lineStart - 1;
        }
        position = chunkStart;
    }
    std::ranges::reverse(page.messages);
    page.nextBeforeOffset = earliestMessageOffset;
    // 只有因为达到页容量而提前停止，才可能还有更早的可显示消息；若已经扫到
    // 文件起点（即使前面全是非消息事件），不要让 UI 在顶部反复请求空页。
    page.hasMore = page.messages.size() == maxMessages &&
                   earliestMessageOffset > 0;
    return page;
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
