// skills.cpp — llmswitch.skills 实现单元。
//
// 约定：
//   - 写 SKILL.md 走 atomicWrite（同 store.cpp 的 <file>.tmp → rename 风格）；
//   - 符号链接判断一律先看 symlink_status（dangling 链接也算"已链接"，以便
//     清理），再看 status；
//   - SKILL.md frontmatter 宽松解析：以 `---` 行开头、到下一个 `---` 行之间
//     找 name:/description: 键，找不到 name 用目录名，绝不抛。
module llmswitch.skills;

import std;
import llmswitch.config;

namespace skills {
namespace {

// Windows 上创建符号链接需要开发者模式或管理员权限，报错时带上提示。
#ifdef _WIN32
constexpr std::string_view kSymlinkHint =
    "（Windows 上创建符号链接可能需要开启开发者模式或以管理员身份运行）";
#else
constexpr std::string_view kSymlinkHint = "";
#endif

std::string trim(std::string_view s) {
    const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!s.empty() && isSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && isSpace(s.back())) s.remove_suffix(1);
    return std::string(s);
}

// skill 名字合法性：非空、非 . / ..、不含路径分隔符。
void validateName(std::string_view name) {
    if (name.empty() || name == "." || name == ".." ||
        name.find('/') != std::string_view::npos ||
        name.find('\\') != std::string_view::npos) {
        throw std::runtime_error(std::format("非法的 skill 名字：{}", name));
    }
}

std::filesystem::path toolSkillsDir(std::string_view toolId) {
    if (toolId == "claude-code") return cfg::claudeSkillsDir();
    if (toolId == "codex") return cfg::codexSkillsDir();
    throw std::runtime_error(std::format("未知工具：{}", toolId));
}

std::string readTextFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return "";
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
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

struct ParsedSkillMd {
    std::string name;
    std::string description;
    std::string body;
};

// 宽松解析 SKILL.md：首行是 `---` 才按 frontmatter 处理，否则整个文件当正文。
// frontmatter 里逐行找 `name:` / `description:` 键（值取冒号后 trim）。
ParsedSkillMd parseSkillMd(std::string_view text, std::string_view fallbackName) {
    ParsedSkillMd r;
    r.name = fallbackName;

    // 取首行
    const auto firstNl = text.find('\n');
    const std::string_view firstLine =
        firstNl == std::string_view::npos ? text : text.substr(0, firstNl);
    if (trim(firstLine) != "---") {
        r.body = text;
        return r;
    }

    // 逐行扫到下一个 `---`
    std::size_t pos = firstNl == std::string_view::npos ? text.size() : firstNl + 1;
    bool closed = false;
    while (pos <= text.size()) {
        const auto nl = text.find('\n', pos);
        const std::string_view line =
            nl == std::string_view::npos ? text.substr(pos) : text.substr(pos, nl - pos);
        const std::string t = trim(line);
        if (t == "---") {
            closed = true;
            pos = (nl == std::string_view::npos) ? text.size() : nl + 1;
            break;
        }
        if (const auto colon = t.find(':'); colon != std::string::npos) {
            const std::string_view key = std::string_view(t).substr(0, colon);
            const std::string value = trim(std::string_view(t).substr(colon + 1));
            if (key == "name" && !value.empty()) {
                r.name = value;
            } else if (key == "description") {
                r.description = value;
            }
        }
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }

    if (!closed) {  // 没有收尾的 ---：不当 frontmatter，整文件是正文
        r.name = fallbackName;
        r.description.clear();
        r.body = text;
        return r;
    }
    // 正文：收尾 --- 之后的内容；按 create 的写法剥掉一个前导空行以便往返一致。
    std::string_view body = text.substr(pos);
    if (body.starts_with('\n')) body.remove_prefix(1);
    r.body = body;
    return r;
}

std::string renderSkillMd(std::string_view name, std::string_view description,
                          std::string_view body) {
    return std::format("---\nname: {}\ndescription: {}\n---\n\n{}", name, description, body);
}

// symlink_status 判断（dangling 也是 true）。
bool isSymlink(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::is_symlink(std::filesystem::symlink_status(p, ec));
}

// 跟随链接后的存在性检查。
bool existsFollowed(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::status(p, ec));
}

} // namespace

SkillsStore SkillsStore::load() {
    SkillsStore s;
    s.refresh();
    return s;
}

void SkillsStore::refresh() {
    skills_.clear();
    std::error_code ec;

    // 中央库：每个实体目录是一个 skill（库里的链接不算，跳过）。
    for (const auto& e :
         std::filesystem::directory_iterator(cfg::skillsStoreDir(), ec)) {
        if (e.is_symlink(ec) || !e.is_directory(ec)) continue;
        SkillInfo info;
        info.name = e.path().filename().string();
        info.inStore = true;
        info.storePath = e.path();
        const auto parsed = parseSkillMd(readTextFile(e.path() / "SKILL.md"), info.name);
        info.description = parsed.description;
        skills_.push_back(std::move(info));
    }

    // 两工具目录：symlink（含 dangling）→ linkedTools；实体目录 → installedOnlyTools。
    for (const auto& [toolId, dir] : {
             std::pair<std::string_view, std::filesystem::path>{"claude-code", cfg::claudeSkillsDir()},
             std::pair<std::string_view, std::filesystem::path>{"codex", cfg::codexSkillsDir()},
         }) {
        ec.clear();
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            const bool link = e.is_symlink(ec);
            if (!link && !e.is_directory(ec)) continue;  // 普通文件不管
            const std::string name = e.path().filename().string();
            auto it = std::ranges::find(skills_, name, &SkillInfo::name);
            if (it == skills_.end()) {
                SkillInfo info;
                info.name = name;
                skills_.push_back(std::move(info));
                it = std::prev(skills_.end());
            }
            if (link) {
                it->linkedTools.emplace_back(toolId);
            } else {
                it->installedOnlyTools.emplace_back(toolId);
            }
        }
    }

    std::ranges::sort(skills_, {}, &SkillInfo::name);
}

void SkillsStore::create(std::string_view name, std::string_view description,
                         std::string_view body) {
    validateName(name);
    const std::filesystem::path dir = cfg::skillsStoreDir() / name;
    if (existsFollowed(dir) || isSymlink(dir)) {
        throw std::runtime_error(std::format("skill 已存在：{}", name));
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        throw std::runtime_error(std::format("创建目录失败：{}（{}）", dir.string(), ec.message()));
    }
    atomicWrite(dir / "SKILL.md", renderSkillMd(name, description, body));
    refresh();
}

void SkillsStore::importFromTool(std::string_view toolId, std::string_view name) {
    validateName(name);
    const std::filesystem::path src = toolSkillsDir(toolId) / name;
    std::filesystem::path realSrc = src;
    if (isSymlink(src)) {
        // 链接指向的内容才是要收编的；dangling 没东西可收。
        std::error_code ec;
        realSrc = std::filesystem::read_symlink(src, ec);
        if (ec) {
            throw std::runtime_error(std::format("读取链接失败：{}（{}）", src.string(), ec.message()));
        }
        if (realSrc.is_relative()) realSrc = src.parent_path() / realSrc;
    }
    if (!existsFollowed(realSrc)) {
        throw std::runtime_error(std::format("工具目录里没有可收编的 skill：{}/{}", toolId, name));
    }
    const std::filesystem::path dst = cfg::skillsStoreDir() / name;
    if (existsFollowed(dst) || isSymlink(dst)) {
        throw std::runtime_error(std::format("中央库已有同名 skill：{}", name));
    }
    std::error_code ec;
    std::filesystem::create_directories(cfg::skillsStoreDir(), ec);
    std::filesystem::copy(realSrc, dst, std::filesystem::copy_options::recursive, ec);
    if (ec) {
        throw std::runtime_error(
            std::format("收编失败：{} → {}（{}）", realSrc.string(), dst.string(), ec.message()));
    }
    refresh();
}

void SkillsStore::setLinked(std::string_view name, std::string_view toolId, bool linked) {
    validateName(name);
    const std::filesystem::path toolDir = toolSkillsDir(toolId);
    const std::filesystem::path linkPath = toolDir / name;

    if (!linked) {
        // 只删 symlink（含 dangling），实体目录是用户自己装的，不动。
        if (isSymlink(linkPath)) {
            std::error_code ec;
            std::filesystem::remove(linkPath, ec);
            if (ec) {
                throw std::runtime_error(
                    std::format("删除链接失败：{}（{}）", linkPath.string(), ec.message()));
            }
        }
        refresh();
        return;
    }

    const std::filesystem::path storePath = cfg::skillsStoreDir() / name;
    if (!existsFollowed(storePath)) {
        throw std::runtime_error(std::format("skill 不在中央库：{}", name));
    }
    if (isSymlink(linkPath)) {
        std::error_code ec;
        const auto target = std::filesystem::read_symlink(linkPath, ec);
        if (!ec && target == storePath) {
            refresh();  // 已是正确的链接
            return;
        }
        std::filesystem::remove(linkPath, ec);  // 指错地方的旧链接，换掉
    } else if (existsFollowed(linkPath)) {
        throw std::runtime_error(std::format(
            "{} 的 skills 目录里已有同名实体目录，未覆盖：{}", toolId, linkPath.string()));
    }
    std::error_code ec;
    std::filesystem::create_directories(toolDir, ec);
    // Windows 上 create_symlink 按目标是否目录选择文件/目录型链接，判定失灵时
    // 产出文件型链接指向目录，跟随遍历直接失败（CI 实测）；显式选目录型。
    std::error_code typeEc;
    if (std::filesystem::is_directory(storePath, typeEc) && !typeEc) {
        std::filesystem::create_directory_symlink(storePath, linkPath, ec);
    } else {
        std::filesystem::create_symlink(storePath, linkPath, ec);
    }
    if (ec) {
        throw std::runtime_error(std::format("创建符号链接失败：{} → {}（{}）{}",
                                             linkPath.string(), storePath.string(), ec.message(),
                                             kSymlinkHint));
    }
    refresh();
}

void SkillsStore::remove(std::string_view name) {
    validateName(name);
    // 所有工具侧的同名 symlink 都删掉（含 dangling）；实体目录跳过不删。
    for (const std::string_view toolId : {"claude-code", "codex"}) {
        const std::filesystem::path p = toolSkillsDir(toolId) / name;
        if (isSymlink(p)) {
            std::error_code ec;
            std::filesystem::remove(p, ec);
        }
    }
    const std::filesystem::path storePath = cfg::skillsStoreDir() / name;
    if (existsFollowed(storePath)) {
        std::error_code ec;
        std::filesystem::remove_all(storePath, ec);
        if (ec) {
            throw std::runtime_error(
                std::format("删除目录失败：{}（{}）", storePath.string(), ec.message()));
        }
    }
    refresh();
}

std::string SkillsStore::readBody(std::string_view name) const {
    validateName(name);
    const auto it = std::ranges::find(skills_, name, &SkillInfo::name);
    if (it == skills_.end() || !it->inStore) {
        throw std::runtime_error(std::format("skill 不在中央库：{}", name));
    }
    return parseSkillMd(readTextFile(it->storePath / "SKILL.md"), it->name).body;
}

void SkillsStore::updateBody(std::string_view name, std::string_view description,
                             std::string_view body) {
    validateName(name);
    const auto it = std::ranges::find(skills_, name, &SkillInfo::name);
    if (it == skills_.end() || !it->inStore) {
        throw std::runtime_error(std::format("skill 不在中央库：{}", name));
    }
    atomicWrite(it->storePath / "SKILL.md", renderSkillMd(name, description, body));
    refresh();
}

} // namespace skills
