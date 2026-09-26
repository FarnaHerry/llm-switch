// usage.cpp — llmswitch.usage 实现单元。
//
// 各 agent 的日志格式（实测自真实文件）：
//   claude-code  ~/.claude/projects/**/*.jsonl
//                message.usage{input_tokens,output_tokens,cache_read_input_tokens,
//                              cache_creation_input_tokens}；同一条 message.id 会
//                被流式重复追加，按 id 取字段最大值。
//   codex        ~/.codex/sessions/**/rollout-*.jsonl
//                只取 type=="token_usage_record" 的逐条 payload.usage；同一文件里
//                的 event_msg/token_count.info.total_token_usage 是**会话累计值**，
//                按行累加会严重虚高，一律不用。
//   qwen         ~/.qwen/usage/token-usage-<年>-<月>.jsonl
//                本身就是一张用量账本，每条一个 id。
//   pi           ~/.pi/agent/sessions/**/*.jsonl
//                type=="message" 的 message.usage{input,output,cacheRead,cacheWrite}。
//   zcode        ~/.zcode/cli/rollout/model-io-*.jsonl
//                一行一次模型调用，response.usage{inputTokens,...,cacheReadTokens}。
//
// 归一化（关键）：各家 input 字段是否含缓存不一致，混着加会重复计数——
//   * claude / pi：input **不含**缓存 → 直接取用；
//   * codex / qwen / zcode：input **含**缓存 → 输入 = input − 缓存，缓存单独成列。
// 实测依据：codex/qwen/zcode 的 total = input + output；pi 的 total =
// input + cacheWrite + cacheRead + output。
module;

#include <ctime>  // localtime_r / localtime_s（当天 0 点划分，与 router 同款）

module llmswitch.usage;

import std;
import nlohmann.json;
import llmswitch.config;

namespace usage {
namespace {

// 原子写的半成品清理：析构即删除 <file>.tmp。成功 rename 后调 Release()
// 解除——提前 return 或改名失败都不留孤儿 .tmp。
class TempFileGuard final {
public:
    explicit TempFileGuard(std::filesystem::path path) : path_(std::move(path)) {}
    ~TempFileGuard() {
        if (!armed_) return;
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    TempFileGuard(const TempFileGuard&) = delete;
    TempFileGuard& operator=(const TempFileGuard&) = delete;
    void Release() noexcept { armed_ = false; }

private:
    std::filesystem::path path_;
    bool armed_ = true;
};

constexpr std::string_view kClaude = "claude-code";
constexpr std::string_view kCodex = "codex";
constexpr std::string_view kQwen = "qwen";
constexpr std::string_view kPi = "pi";
constexpr std::string_view kZcode = "zcode";

// ---- JSON 取值（缺字段/类型不符一律 0 或空串，绝不抛）----

std::int64_t JInt(const nlohmann::json& j, std::string_view key) {
    const auto it = j.find(std::string(key));
    if (it == j.end() || !it->is_number()) return 0;
    return it->get<std::int64_t>();
}

std::string JStr(const nlohmann::json& j, std::string_view key) {
    const auto it = j.find(std::string(key));
    if (it == j.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

// 取一个对象字段；不是对象就返回 nullptr。
const nlohmann::json* JObj(const nlohmann::json& j, std::string_view key) {
    const auto it = j.find(std::string(key));
    if (it == j.end() || !it->is_object()) return nullptr;
    return &*it;
}

// ---- 时间：自己解析 ISO 8601，避开 std::chrono::parse 的平台差异 ----

// 公历 → 1970-01-01 起的天数（Howard Hinnant days_from_civil）。
std::int64_t DaysFromCivil(std::int64_t y, std::int64_t m, std::int64_t d) {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;
    const std::int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

std::int64_t ParseIso(std::string_view s) {
    if (s.size() < 10) return 0;
    const auto num = [&s](std::size_t pos, std::size_t len) -> std::int64_t {
        std::int64_t v = 0;
        for (std::size_t i = 0; i < len; ++i) {
            const char c = s[pos + i];
            if (c < '0' || c > '9') return -1;
            v = v * 10 + (c - '0');
        }
        return v;
    };
    if (s[4] != '-' || s[7] != '-') return 0;
    const std::int64_t year = num(0, 4);
    const std::int64_t month = num(5, 2);
    const std::int64_t day = num(8, 2);
    if (year < 0 || month < 1 || month > 12 || day < 1 || day > 31) return 0;

    std::int64_t hour = 0;
    std::int64_t minute = 0;
    std::int64_t second = 0;
    std::int64_t millis = 0;
    if (s.size() >= 19 && (s[10] == 'T' || s[10] == ' ')) {
        const std::int64_t h = num(11, 2);
        const std::int64_t mi = num(14, 2);
        const std::int64_t sec = num(17, 2);
        if (h >= 0) hour = h;
        if (mi >= 0) minute = mi;
        if (sec >= 0) second = sec;
        if (s.size() >= 23 && s[19] == '.') {
            const std::int64_t frac = num(20, 3);
            if (frac > 0) millis = frac;
        }
    }
    const std::int64_t days = DaysFromCivil(year, month, day);
    return ((days * 24 + hour) * 60 + minute) * 60 * 1000 + second * 1000 + millis;
}

std::int64_t NowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 本地当天 0 点（与 router 的 today 口径一致）。
std::int64_t TodayStartMillis() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    ::localtime_s(&tm, &t);
#else
    ::localtime_r(&t, &tm);
#endif
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    return static_cast<std::int64_t>(std::mktime(&tm)) * 1000;
}

// ---- 合并：同一次调用的多次写入按字段取最大值 ----

void MergeMax(UsageRecord& into, const UsageRecord& from) {
    into.inputTokens = std::max(into.inputTokens, from.inputTokens);
    into.outputTokens = std::max(into.outputTokens, from.outputTokens);
    into.cacheReadTokens = std::max(into.cacheReadTokens, from.cacheReadTokens);
    into.cacheWriteTokens = std::max(into.cacheWriteTokens, from.cacheWriteTokens);
    into.tsMillis = std::max(into.tsMillis, from.tsMillis);
    if (into.model.empty()) into.model = from.model;
}

using RecordMap = std::map<std::string, UsageRecord>;

void Emit(RecordMap& out, UsageRecord r) {
    if (r.key.empty()) return;
    const auto it = out.find(r.key);
    if (it == out.end()) {
        out.emplace(r.key, std::move(r));
        return;
    }
    MergeMax(it->second, r);
}

std::vector<UsageRecord> ToVector(const RecordMap& map) {
    std::vector<UsageRecord> out;
    out.reserve(map.size());
    for (const auto& [key, rec] : map) out.push_back(rec);
    return out;
}

template <class Emit>
void ForEachLine(std::string_view text, Emit&& emit) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::size_t end = nl == std::string_view::npos ? text.size() : nl;
        std::string_view line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.empty()) emit(line);
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
}

nlohmann::json ParseLine(std::string_view line) {
    return nlohmann::json::parse(line, nullptr, false);
}

// ---- 扫描状态：每个文件已消费到的字节偏移 ----

struct FileState {
    std::uintmax_t offset = 0;
    std::uintmax_t size = 0;
    bool operator==(const FileState&) const = default;
};

using ScanState = std::map<std::string, FileState>;

ScanState ReadScanState(const std::filesystem::path& file) {
    ScanState state;
    std::ifstream in(file, std::ios::binary);
    if (!in) return state;
    const auto j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return state;
    const auto* files = JObj(j, "files");
    if (files == nullptr) return state;
    for (auto it = files->begin(); it != files->end(); ++it) {
        if (!it.value().is_object()) continue;
        FileState fs;
        fs.offset = static_cast<std::uintmax_t>(JInt(it.value(), "offset"));
        fs.size = static_cast<std::uintmax_t>(JInt(it.value(), "size"));
        state.emplace(it.key(), fs);
    }
    return state;
}

void WriteScanState(const std::filesystem::path& file, const ScanState& state) {
    nlohmann::json j;
    j["files"] = nlohmann::json::object();
    for (const auto& [path, fs] : state) {
        j["files"][path] = {{"offset", fs.offset}, {"size", fs.size}};
    }
    const auto tmp = file.string() + ".tmp";
    TempFileGuard tmp_guard(tmp);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << j.dump();
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        ec.clear();
        std::filesystem::remove(file, ec);
        ec.clear();
        std::filesystem::rename(tmp, file, ec);
        if (ec) return;
    }
    tmp_guard.Release();
}

// 读 [offset, size) 的字节；失败返回空。
std::string ReadRange(const std::filesystem::path& file, std::uintmax_t offset,
                      std::uintmax_t size) {
    if (size <= offset) return {};
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!in) return {};
    std::string out(static_cast<std::size_t>(size - offset), '\0');
    in.read(out.data(), static_cast<std::streamsize>(out.size()));
    out.resize(static_cast<std::size_t>(in.gcount()));
    return out;
}

// 递归收集 *.jsonl（排序保证结果稳定）。
std::vector<std::filesystem::path> CollectJsonl(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return files;
    // libc++ 21 起 recursive_directory_iterator 只剩 default_sentinel 比较
    // （迭代器对 != 已移除，range-for 编不过）；显式迭代 + 哨兵比较三标准库通吃。
    // 与 sessions.cpp 的 scanCodex 同一写法。
    for (auto it = std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::default_sentinel; it.increment(ec)) {
        if (ec) break;
        const auto& entry = *it;
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc) || entryEc) continue;
        if (entry.path().extension() != ".jsonl") continue;
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::vector<UsageRecord> ParseFor(std::string_view agent, std::string_view chunk) {
    if (agent == kClaude) return UsageStore::ParseClaude(chunk);
    if (agent == kCodex) return UsageStore::ParseCodex(chunk);
    if (agent == kQwen) return UsageStore::ParseQwen(chunk);
    if (agent == kPi) return UsageStore::ParsePi(chunk);
    if (agent == kZcode) return UsageStore::ParseZcode(chunk);
    return {};
}

} // namespace

// ---- 各 agent 的解析器 -------------------------------------------------------

std::vector<UsageRecord> UsageStore::ParseClaude(std::string_view jsonl) {
    RecordMap out;
    ForEachLine(jsonl, [&out](std::string_view line) {
        const auto j = ParseLine(line);
        if (j.is_discarded() || !j.is_object()) return;
        const auto* message = JObj(j, "message");
        if (message == nullptr) return;
        const auto* u = JObj(*message, "usage");
        if (u == nullptr) return;
        std::string id = JStr(*message, "id");
        if (id.empty()) id = JStr(j, "uuid");
        if (id.empty()) return;
        UsageRecord r;
        r.agent = std::string(kClaude);
        r.key = r.agent + ":" + id;
        r.model = JStr(*message, "model");
        r.tsMillis = ParseIso(JStr(j, "timestamp"));
        // Anthropic：input_tokens 不含缓存，缓存两项单独成列。
        r.inputTokens = JInt(*u, "input_tokens");
        r.outputTokens = JInt(*u, "output_tokens");
        r.cacheReadTokens = JInt(*u, "cache_read_input_tokens");
        r.cacheWriteTokens = JInt(*u, "cache_creation_input_tokens");
        Emit(out, std::move(r));
    });
    return ToVector(out);
}

std::vector<UsageRecord> UsageStore::ParseCodex(std::string_view jsonl) {
    RecordMap out;
    ForEachLine(jsonl, [&out](std::string_view line) {
        const auto j = ParseLine(line);
        if (j.is_discarded() || !j.is_object()) return;
        if (JStr(j, "type") != "token_usage_record") return;  // 累计值事件一律不用
        const auto* p = JObj(j, "payload");
        if (p == nullptr) return;
        const auto* u = JObj(*p, "usage");
        if (u == nullptr) return;
        std::string id = JStr(*p, "response_id");
        if (id.empty()) {
            id = JStr(*p, "turn_id");
            const auto ordinal = JInt(j, "ordinal");
            if (id.empty() && ordinal == 0) return;
            id += "#" + std::to_string(ordinal);
        }
        UsageRecord r;
        r.agent = std::string(kCodex);
        r.key = r.agent + ":" + id;
        r.tsMillis = ParseIso(JStr(j, "timestamp"));
        // OpenAI 口径：input_tokens 含 cached_input_tokens，减掉避免重复计数。
        const std::int64_t cached = JInt(*u, "cached_input_tokens");
        r.inputTokens = std::max<std::int64_t>(0, JInt(*u, "input_tokens") - cached);
        r.cacheReadTokens = cached;
        r.cacheWriteTokens = JInt(*u, "cache_write_input_tokens");
        r.outputTokens = JInt(*u, "output_tokens");
        Emit(out, std::move(r));
    });
    return ToVector(out);
}

std::vector<UsageRecord> UsageStore::ParseQwen(std::string_view jsonl) {
    RecordMap out;
    ForEachLine(jsonl, [&out](std::string_view line) {
        const auto j = ParseLine(line);
        if (j.is_discarded() || !j.is_object()) return;
        const std::string id = JStr(j, "id");
        if (id.empty()) return;
        UsageRecord r;
        r.agent = std::string(kQwen);
        r.key = r.agent + ":" + id;
        r.model = JStr(j, "model");
        r.tsMillis = ParseIso(JStr(j, "timestamp"));
        // 账本里 totalTokens = input + output，说明 input 含缓存（实测）。
        const std::int64_t cached = JInt(j, "cachedTokens");
        r.inputTokens = std::max<std::int64_t>(0, JInt(j, "inputTokens") - cached);
        r.cacheReadTokens = cached;
        r.outputTokens = JInt(j, "outputTokens");
        // thoughtsTokens 不并进 output：账本自己的 totalTokens 也没算它。
        Emit(out, std::move(r));
    });
    return ToVector(out);
}

std::vector<UsageRecord> UsageStore::ParsePi(std::string_view jsonl) {
    RecordMap out;
    ForEachLine(jsonl, [&out](std::string_view line) {
        const auto j = ParseLine(line);
        if (j.is_discarded() || !j.is_object()) return;
        if (JStr(j, "type") != "message") return;
        const auto* message = JObj(j, "message");
        if (message == nullptr) return;
        const auto* u = JObj(*message, "usage");
        if (u == nullptr) return;
        std::string id = JStr(j, "id");
        if (id.empty()) id = JStr(*message, "id");
        if (id.empty()) return;
        UsageRecord r;
        r.agent = std::string(kPi);
        r.key = r.agent + ":" + id;
        r.model = JStr(*message, "model");
        r.tsMillis = ParseIso(JStr(j, "timestamp"));
        // pi 的 totalTokens = input + cacheWrite + cacheRead + output，input 不含缓存。
        r.inputTokens = JInt(*u, "input");
        r.outputTokens = JInt(*u, "output");
        r.cacheReadTokens = JInt(*u, "cacheRead");
        r.cacheWriteTokens = JInt(*u, "cacheWrite");
        Emit(out, std::move(r));
    });
    return ToVector(out);
}

std::vector<UsageRecord> UsageStore::ParseZcode(std::string_view jsonl) {
    RecordMap out;
    ForEachLine(jsonl, [&out](std::string_view line) {
        const auto j = ParseLine(line);
        if (j.is_discarded() || !j.is_object()) return;
        const auto* response = JObj(j, "response");
        if (response == nullptr) return;
        const auto* u = JObj(*response, "usage");
        if (u == nullptr) return;
        const std::string id = JStr(j, "requestId");
        if (id.empty()) return;
        UsageRecord r;
        r.agent = std::string(kZcode);
        r.key = r.agent + ":" + id;
        if (const auto* model = JObj(j, "model"); model != nullptr) {
            r.model = JStr(*model, "modelId");
        }
        r.tsMillis = ParseIso(JStr(j, "completedAt"));
        // 实测 totalTokens = inputTokens + outputTokens ⇒ input 含 cacheReadTokens。
        const std::int64_t cached = JInt(*u, "cacheReadTokens");
        r.inputTokens = std::max<std::int64_t>(0, JInt(*u, "inputTokens") - cached);
        r.cacheReadTokens = cached;
        r.cacheWriteTokens = JInt(*u, "cacheWriteTokens");
        r.outputTokens = JInt(*u, "outputTokens");
        Emit(out, std::move(r));
    });
    return ToVector(out);
}

std::int64_t UsageStore::IsoToMillis(std::string_view iso) { return ParseIso(iso); }

// ---- UsageStore -------------------------------------------------------------

struct UsageStore::Impl {
    // mu 只保护 records：文件 IO 与解析都在锁外做，否则一轮全量扫描会把
    // UI 线程的 snapshot() 卡住（统计页每 5s 读一次）。
    mutable std::mutex mu;
    // syncMu 保证同一时刻只有一轮扫描在跑（状态文件不是并发安全的）。
    std::mutex syncMu;
    std::map<std::string, UsageRecord> records;
    bool loaded = false;
};

UsageStore::UsageStore() : impl_(std::make_unique<Impl>()) {}
UsageStore::~UsageStore() = default;

std::filesystem::path UsageStore::ledgerPath() const {
    return cfg::usageDir() / "usage.jsonl";
}

namespace {

std::filesystem::path ScanStatePath() { return cfg::usageDir() / "scan-state.json"; }

nlohmann::json RecordToJson(const UsageRecord& r) {
    return nlohmann::json{{"k", r.key},   {"a", r.agent},   {"m", r.model},
                          {"t", r.tsMillis}, {"i", r.inputTokens},
                          {"o", r.outputTokens}, {"cr", r.cacheReadTokens},
                          {"cw", r.cacheWriteTokens}};
}

UsageRecord RecordFromJson(const nlohmann::json& j) {
    UsageRecord r;
    r.key = JStr(j, "k");
    r.agent = JStr(j, "a");
    r.model = JStr(j, "m");
    r.tsMillis = JInt(j, "t");
    r.inputTokens = JInt(j, "i");
    r.outputTokens = JInt(j, "o");
    r.cacheReadTokens = JInt(j, "cr");
    r.cacheWriteTokens = JInt(j, "cw");
    return r;
}

} // namespace

void UsageStore::load() {
    std::lock_guard lk(impl_->mu);
    if (impl_->loaded) return;
    impl_->loaded = true;
    impl_->records.clear();
    std::ifstream in(ledgerPath(), std::ios::binary);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const auto j = nlohmann::json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.is_object()) continue;
        auto rec = RecordFromJson(j);
        if (rec.key.empty()) continue;
        const auto it = impl_->records.find(rec.key);
        if (it == impl_->records.end()) {
            impl_->records.emplace(rec.key, std::move(rec));
        } else {
            MergeMax(it->second, rec);
        }
    }
}

SyncReport UsageStore::sync() {
    load();
    // 一轮扫描串行：状态文件与"已见记录"的推进不能并发。
    std::lock_guard syncLock(impl_->syncMu);
    SyncReport report;

    // agent → 根目录（每个根递归找 *.jsonl）。
    const std::vector<std::pair<std::string, std::filesystem::path>> roots{
        {std::string(kClaude), cfg::claudeProjectsDir()},
        {std::string(kCodex), cfg::codexSessionsDir()},
        {std::string(kQwen), cfg::qwenUsageDir()},
        {std::string(kPi), cfg::piSessionsDir()},
        {std::string(kZcode), cfg::zcodeRolloutDir()},
    };

    ScanState state = ReadScanState(ScanStatePath());
    // 每轮的 (文件, 解析结果) —— 解析在锁外做，合并才进锁。
    std::vector<std::pair<std::string, std::vector<UsageRecord>>> perFile;

    for (const auto& [agent, root] : roots) {
        for (const auto& path : CollectJsonl(root)) {
            std::error_code ec;
            const auto size = std::filesystem::file_size(path, ec);
            if (ec) continue;
            const std::string key = path.generic_string();

            std::uintmax_t offset = 0;
            if (const auto it = state.find(key); it != state.end()) {
                if (it->second.size == size) {
                    ++report.skippedFiles;
                    continue;
                }
                // 文件被截断/轮转 → 从头重读（去重键会把它并回同一批记录）。
                offset = it->second.size > size ? 0 : it->second.offset;
            }
            ++report.scannedFiles;

            const std::string chunk = ReadRange(path, offset, size);
            if (chunk.empty()) {
                state[key] = FileState{offset, size};
                continue;
            }
            // 只消费到最后一个换行：末尾半行留给下一轮，避免解析到写入中的行。
            const auto lastNewline = chunk.rfind('\n');
            if (lastNewline == std::string::npos) continue;
            const std::size_t consumed = lastNewline + 1;

            auto parsed =
                ParseFor(agent, std::string_view(chunk).substr(0, consumed));
            report.parsedRecords += parsed.size();
            perFile.emplace_back(key, std::move(parsed));
            state[key] = FileState{offset + consumed, size};
        }
    }

    std::vector<UsageRecord> fresh;
    {
        std::lock_guard lk(impl_->mu);
        for (auto& [key, parsed] : perFile) {
            for (const auto& rec : parsed) {
                if (rec.key.empty()) continue;
                const auto it = impl_->records.find(rec.key);
                if (it == impl_->records.end()) {
                    impl_->records.emplace(rec.key, rec);
                    fresh.push_back(rec);
                    continue;
                }
                const UsageRecord before = it->second;
                MergeMax(it->second, rec);
                if (!(it->second == before)) fresh.push_back(it->second);
            }
        }
    }

    if (!fresh.empty()) {
        std::ofstream out(ledgerPath(), std::ios::binary | std::ios::app);
        if (out) {
            for (const auto& rec : fresh) out << RecordToJson(rec).dump() << '\n';
        }
    }
    report.storedRecords = fresh.size();

    // 掉队的文件（被删/改名）从状态里清掉，状态文件不无限膨胀。
    if (report.scannedFiles > 0) {
        std::erase_if(state, [](const auto& entry) {
            std::error_code e;
            return !std::filesystem::exists(entry.first, e) || e;
        });
    }
    WriteScanState(ScanStatePath(), state);
    return report;
}

void UsageStore::clear() {
    std::lock_guard lk(impl_->mu);
    impl_->records.clear();
    impl_->loaded = true;
    std::error_code ec;
    std::filesystem::remove(ledgerPath(), ec);
    ec.clear();
    std::filesystem::remove(ScanStatePath(), ec);
}

std::int64_t UsageStore::recordCount() const {
    std::lock_guard lk(impl_->mu);
    return static_cast<std::int64_t>(impl_->records.size());
}

UsageSnapshot UsageStore::snapshot() const {
    std::lock_guard lk(impl_->mu);
    UsageSnapshot snap;
    const std::int64_t todayStart = TodayStartMillis();
    std::map<std::string, UsageTotals> perAgent;
    for (const auto& [key, rec] : impl_->records) {
        const auto accumulate = [&rec](UsageTotals& t) {
            ++t.requests;
            t.inputTokens += rec.inputTokens;
            t.outputTokens += rec.outputTokens;
            t.cacheReadTokens += rec.cacheReadTokens;
            t.cacheWriteTokens += rec.cacheWriteTokens;
        };
        accumulate(snap.total);
        accumulate(perAgent[rec.agent]);
        if (rec.tsMillis >= todayStart && rec.tsMillis > 0) {
            ++snap.todayRequests;
            snap.todayTokens += rec.inputTokens + rec.outputTokens +
                                rec.cacheReadTokens + rec.cacheWriteTokens;
        }
    }
    snap.records = static_cast<std::int64_t>(impl_->records.size());
    snap.byAgent.reserve(perAgent.size());
    for (const auto& [agent, totals] : perAgent) {
        snap.byAgent.push_back(AgentUsage{agent, totals});
    }
    std::sort(snap.byAgent.begin(), snap.byAgent.end(),
              [](const AgentUsage& a, const AgentUsage& b) {
                  if (a.totals.TotalTokens() != b.totals.TotalTokens()) {
                      return a.totals.TotalTokens() > b.totals.TotalTokens();
                  }
                  return a.agent < b.agent;
              });
    return snap;
}

} // namespace usage
