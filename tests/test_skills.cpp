// test_skills.cpp — llmswitch.skills 领域层测试（无框架：CHECK 失败计数，非零即败）。
// 全程隔离在临时目录：HOME / XDG_DATA_HOME / LLMSWITCH_* 指向
// temp_directory_path()/llmswitch-test-skills-<pid>。
//
// 覆盖：create（中央库文件 + frontmatter）、setLinked（符号链接出现/幂等/
// 实体目录冲突报错/unlink）、load 合并视图（inStore / linkedTools /
// installedOnlyTools）、importFromTool 收编、remove（删链接 + 库目录、实体目录
// 跳过）、dangling symlink（算已链接、可清理）、frontmatter 宽松解析
// （有/无 description、无 frontmatter 回落目录名）、readBody/updateBody 往返。
#include <cstdio>    // stderr（std 模块不导出 stdout/stderr 宏）
#include <cstdlib>   // setenv
#include <unistd.h>  // getpid

import std;
import llmswitch.config;
import llmswitch.skills;

namespace {

int g_failures = 0;

#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            std::println(stderr, "FAIL {}: {}", __LINE__, #cond); \
            ++g_failures;                                        \
        }                                                        \
    } while (0)

void writeFile(const std::filesystem::path& path, std::string_view content) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

bool isSymlink(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::is_symlink(std::filesystem::symlink_status(p, ec));
}

bool existsFollowed(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::status(p, ec));
}

const skills::SkillInfo* find(const skills::SkillsStore& s, std::string_view name) {
    for (const auto& info : s.skills()) {
        if (info.name == name) return &info;
    }
    return nullptr;
}

bool hasTool(const std::vector<std::string>& v, std::string_view tool) {
    return std::ranges::find(v, tool) != v.end();
}

template <typename F>
bool throwsRuntimeError(F&& f) {
    try {
        f();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

} // namespace

int main() {
    namespace fs = std::filesystem;

    // ---- 环境隔离 -----------------------------------------------------------
    const fs::path root =
        fs::temp_directory_path() / std::format("llmswitch-test-skills-{}", ::getpid());
    {
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root);
    }
    const fs::path home = root / "home";
    ::setenv("HOME", home.c_str(), 1);
    ::setenv("XDG_DATA_HOME", (root / "xdg").c_str(), 1);
    const fs::path storeDir = root / "skills-store";
    const fs::path claudeSkills = home / ".claude" / "skills";
    const fs::path codexSkills = home / ".codex" / "skills";
    ::setenv("LLMSWITCH_SKILLS_STORE", storeDir.c_str(), 1);
    ::setenv("LLMSWITCH_CLAUDE_SKILLS", claudeSkills.c_str(), 1);
    ::setenv("LLMSWITCH_CODEX_SKILLS", codexSkills.c_str(), 1);

    // 1. create → 中央库有 SKILL.md，frontmatter 正确
    {
        auto s = skills::SkillsStore::load();
        CHECK(s.skills().empty());
        s.create("hello", "打个招呼", "当用户说 hi 时使用。");
        CHECK(existsFollowed(storeDir / "hello" / "SKILL.md"));

        const auto s2 = skills::SkillsStore::load();
        CHECK(s2.skills().size() == 1);
        const auto* info = find(s2, "hello");
        CHECK(info != nullptr);
        if (info) {
            CHECK(info->inStore);
            CHECK(info->description == "打个招呼");
            CHECK(info->linkedTools.empty());
            CHECK(info->installedOnlyTools.empty());
            CHECK(info->storePath == storeDir / "hello");
        }
        // 重名 create 报错
        CHECK(throwsRuntimeError(
            [&] { auto t = skills::SkillsStore::load(); t.create("hello", "x", "y"); }));
        // 非法名字报错
        CHECK(throwsRuntimeError(
            [] { auto t = skills::SkillsStore::load(); t.create("a/b", "x", "y"); }));
    }

    // 2. setLinked claude-code → 符号链接出现在假 .claude/skills；再 unlink
    {
        auto s = skills::SkillsStore::load();
        s.setLinked("hello", "claude-code", true);
        const fs::path link = claudeSkills / "hello";
        CHECK(isSymlink(link));
        std::error_code ec;
        CHECK(fs::read_symlink(link, ec) == storeDir / "hello");
        CHECK(existsFollowed(link));  // 链接有效

        const auto s2 = skills::SkillsStore::load();
        const auto* info = find(s2, "hello");
        CHECK(info && hasTool(info->linkedTools, "claude-code"));

        // 幂等：重复 link 不报错
        auto s3 = skills::SkillsStore::load();
        s3.setLinked("hello", "claude-code", true);
        CHECK(isSymlink(link));

        // unlink → 链接消失，库目录还在
        auto s4 = skills::SkillsStore::load();
        s4.setLinked("hello", "claude-code", false);
        CHECK(!isSymlink(link));
        CHECK(existsFollowed(storeDir / "hello" / "SKILL.md"));

        // 未知工具报错
        CHECK(throwsRuntimeError(
            [] { auto t = skills::SkillsStore::load(); t.setLinked("hello", "nope", true); }));
        // 不在中央库的 skill 不能 link
        CHECK(throwsRuntimeError(
            [] { auto t = skills::SkillsStore::load(); t.setLinked("ghost", "codex", true); }));
    }

    // 3. 实体目录冲突：工具侧有同名实体目录时 setLinked 报错且不覆盖
    {
        writeFile(codexSkills / "hello" / "SKILL.md", "用户自己装的\n");
        auto s = skills::SkillsStore::load();
        CHECK(throwsRuntimeError(
            [&] { s.setLinked("hello", "codex", true); }));
        CHECK(!isSymlink(codexSkills / "hello"));

        // load 合并视图：hello 在 codex 是 installedOnly
        const auto s2 = skills::SkillsStore::load();
        const auto* info = find(s2, "hello");
        CHECK(info && hasTool(info->installedOnlyTools, "codex"));
        CHECK(info && info->linkedTools.empty());

        // unlink 对实体目录不动
        auto s3 = skills::SkillsStore::load();
        s3.setLinked("hello", "codex", false);
        CHECK(existsFollowed(codexSkills / "hello" / "SKILL.md"));
    }

    // 4. importFromTool 收编只在工具目录的 skill
    {
        writeFile(codexSkills / "tool-only" / "SKILL.md",
                  "---\nname: tool-only\ndescription: 收编我\n---\n\n正文\n");
        writeFile(codexSkills / "tool-only" / "extra.txt", "附带文件\n");

        // 收编前：只在工具侧
        const auto before = skills::SkillsStore::load();
        const auto* b = find(before, "tool-only");
        CHECK(b && !b->inStore && hasTool(b->installedOnlyTools, "codex"));

        auto s = skills::SkillsStore::load();
        s.importFromTool("codex", "tool-only");
        CHECK(existsFollowed(storeDir / "tool-only" / "SKILL.md"));
        CHECK(existsFollowed(storeDir / "tool-only" / "extra.txt"));
        // 工具侧原目录保留
        CHECK(existsFollowed(codexSkills / "tool-only" / "SKILL.md"));

        const auto after = skills::SkillsStore::load();
        const auto* a = find(after, "tool-only");
        CHECK(a && a->inStore && a->description == "收编我");
        CHECK(a && hasTool(a->installedOnlyTools, "codex"));

        // 中央库已有同名 → 报错
        CHECK(throwsRuntimeError(
            [] { auto t = skills::SkillsStore::load(); t.importFromTool("codex", "tool-only"); }));
        // 不存在的 → 报错
        CHECK(throwsRuntimeError(
            [] { auto t = skills::SkillsStore::load(); t.importFromTool("codex", "missing"); }));
    }

    // 5. frontmatter 宽松解析：无 description / 无 frontmatter 回落目录名
    {
        writeFile(storeDir / "no-desc" / "SKILL.md",
                  "---\nname: no-desc\n---\n\n只有名字\n");
        writeFile(storeDir / "no-front" / "SKILL.md", "没有 frontmatter 的正文\n");
        const auto s = skills::SkillsStore::load();
        const auto* nd = find(s, "no-desc");
        CHECK(nd && nd->inStore && nd->description.empty());
        const auto* nf = find(s, "no-front");
        CHECK(nf && nf->inStore && nf->description.empty());
        CHECK(nf && nf->name == "no-front");
    }

    // 6. readBody / updateBody 往返
    {
        auto s = skills::SkillsStore::load();
        CHECK(s.readBody("hello") == "当用户说 hi 时使用。");
        s.updateBody("hello", "新描述", "新正文\n第二行");
        CHECK(s.readBody("hello") == "新正文\n第二行");
        const auto s2 = skills::SkillsStore::load();
        const auto* info = find(s2, "hello");
        CHECK(info && info->description == "新描述");
        // 无 frontmatter 的读出来是整文件
        CHECK(s2.readBody("no-front") == "没有 frontmatter 的正文\n");
        CHECK(throwsRuntimeError(
            [] { const auto t = skills::SkillsStore::load(); (void)t.readBody("missing"); }));
    }

    // 7. dangling symlink：算"已链接"，remove 能清理
    {
        auto s = skills::SkillsStore::load();
        s.setLinked("hello", "claude-code", true);
        const fs::path link = claudeSkills / "hello";
        CHECK(isSymlink(link));

        // 外部把库目录删了 → 链接 dangling
        std::error_code ec;
        fs::remove_all(storeDir / "hello", ec);
        CHECK(!existsFollowed(link) && isSymlink(link));

        const auto s2 = skills::SkillsStore::load();
        const auto* info = find(s2, "hello");
        CHECK(info != nullptr);
        if (info) {
            CHECK(!info->inStore);
            CHECK(hasTool(info->linkedTools, "claude-code"));  // dangling 也算已链接
        }

        // remove 清理 dangling 链接（库目录已不在也不报错）
        auto s3 = skills::SkillsStore::load();
        s3.remove("hello");
        CHECK(!isSymlink(link));
        const auto s4 = skills::SkillsStore::load();
        const auto* left = find(s4, "hello");
        CHECK(left && left->linkedTools.empty());  // 链接已清
        // codex 侧的实体目录（第 3 步造的）按约定跳过不删，仍显示为 installedOnly
        CHECK(left && !left->inStore && hasTool(left->installedOnlyTools, "codex"));
        CHECK(existsFollowed(codexSkills / "hello" / "SKILL.md"));
    }

    // 8. remove：删库目录 + 链接；工具侧实体目录跳过不删
    {
        auto s = skills::SkillsStore::load();
        s.create("bye", "再见", "正文");
        s.setLinked("bye", "claude-code", true);
        // codex 侧放一个同名实体目录（用户自己装的）
        writeFile(codexSkills / "bye" / "SKILL.md", "实体目录\n");

        auto s2 = skills::SkillsStore::load();
        s2.remove("bye");
        CHECK(!existsFollowed(storeDir / "bye"));
        CHECK(!isSymlink(claudeSkills / "bye"));
        CHECK(existsFollowed(codexSkills / "bye" / "SKILL.md"));  // 实体目录保留
    }

    if (g_failures == 0) {
        std::println("OK");
    }
    return g_failures == 0 ? 0 : 1;
}
