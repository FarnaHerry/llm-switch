// test_sessions.cpp — llmswitch.sessions 领域层测试（无框架：CHECK 失败计数，非零即败）。
// 全程隔离在临时目录：HOME / LLMSWITCH_* 指向
// temp_directory_path()/llmswitch-test-sessions-<pid>。
//
// 覆盖：伪造 ~/.claude/projects/proj-x/*.jsonl（含 user 消息行）与
// ~/.codex/sessions/2026/09/06/*.jsonl → listSessions 数量/排序/元数据，
// summarizeSession 按需提取 title/preview；claude title 跳过命令样文本；codex 取不到回落
// 文件 stem；deleteSession 删除且越界路径（/etc/passwd）被拒绝；
// readSession 可读取完整消息；exportSession 复制成功。
#include <cstdio>    // stderr（std 模块不导出 stdout/stderr 宏）
#include "test_env.h"  // setenv/getpid/unsetenv 可移植封装

import std;
import llmswitch.config;
import llmswitch.sessions;

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
        fs::temp_directory_path() / std::format("llmswitch-test-sessions-{}", testenv::getpid());
    {
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root);
    }
    const fs::path home = root / "home";
    testenv::setenv("HOME", home);
    testenv::setenv("XDG_DATA_HOME", (root / "xdg"));
    testenv::setenv("LLMSWITCH_DATA_DIR", (root / "data"));
    const fs::path claudeProjects = home / ".claude" / "projects";
    const fs::path codexSessions = home / ".codex" / "sessions";
    testenv::setenv("LLMSWITCH_CLAUDE_PROJECTS", claudeProjects);
    testenv::setenv("LLMSWITCH_CODEX_SESSIONS", codexSessions);

    // ---- 伪造会话文件 -------------------------------------------------------
    // claude：content 为数组形式；mtime 最旧
    const fs::path claude1 = claudeProjects / "proj-x" / "sess-a.jsonl";
    writeFile(claude1,
              "{\"type\":\"summary\",\"summary\":\"旧摘要\"}\n"
              "{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"帮我修复登录页面的 bug\"}]}}\n"
              "{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"登录页面已修复\"}]}}\n");
    // claude：首条 user 是命令样文本，应跳过取第二条；content 为纯字符串
    const fs::path claude2 = claudeProjects / "proj-x" / "sess-b.jsonl";
    writeFile(claude2,
              "{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"<system-reminder>内部提示</system-reminder>\"}}\n"
              "{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"/clear\"}}\n"
              "{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"解释一下这段代码\"}}\n");
    // claude：另一个项目，完全没有 user 行 → 回落文件 stem
    const fs::path claude3 = claudeProjects / "proj-y" / "sess-c.jsonl";
    writeFile(claude3, "{\"type\":\"assistant\",\"message\":{}}\n");
    // codex：rollout 形式 payload user 消息
    const fs::path codex1 = codexSessions / "2026" / "09" / "06" / "rollout-1.jsonl";
    writeFile(codex1,
              "{\"timestamp\":\"2026-09-06T01:00:00Z\",\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"审查这个 PR\"}]}}\n"
              "{\"timestamp\":\"2026-09-06T01:00:01Z\",\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"PR 审查完成\"}]}}\n");
    // codex：结构取不到标题 → 回落文件 stem
    const fs::path codex2 = codexSessions / "2026" / "09" / "05" / "rollout-2.jsonl";
    writeFile(codex2, "{\"type\":\"turn_context\",\"payload\":{}}\n");

    // 控制排序：显式设置 mtime（sess-b 最新，rollout-2 最旧）
    const auto now = fs::file_time_type::clock::now();
    std::error_code ec;
    fs::last_write_time(claude1, now - std::chrono::hours(4), ec);
    fs::last_write_time(claude2, now - std::chrono::hours(1), ec);
    fs::last_write_time(claude3, now - std::chrono::hours(3), ec);
    fs::last_write_time(codex1, now - std::chrono::hours(2), ec);
    fs::last_write_time(codex2, now - std::chrono::hours(5), ec);

    // 1. 合并视图：数量 + 按 mtime 倒序
    const auto all = sessions::listSessions();
    CHECK(all.size() == 5);
    CHECK(std::ranges::is_sorted(all, [](const auto& a, const auto& b) {
        return a.mtimeMillis > b.mtimeMillis;
    }));
    if (all.size() == 5) {
        CHECK(all[0].id == "sess-b");
        CHECK(all[1].id == "rollout-1");
        CHECK(all[2].id == "sess-c");
        CHECK(all[3].id == "sess-a");
        CHECK(all[4].id == "rollout-2");
    }

    // 2. 字段：列表阶段只保留 tool / project / 文件 stem / 大小等元数据。
    {
        const auto claude = sessions::listSessions("claude-code");
        CHECK(claude.size() == 3);
        const auto itA = std::ranges::find(claude, "sess-a", &sessions::SessionInfo::id);
        CHECK(itA != claude.end());
        if (itA != claude.end()) {
            CHECK(itA->tool == "claude-code");
            CHECK(itA->project == "proj-x");
            CHECK(itA->title == "sess-a");
            CHECK(itA->preview.empty());
            CHECK(itA->sizeBytes > 0);
            CHECK(itA->path == claude1);
        }
        const auto itB = std::ranges::find(claude, "sess-b", &sessions::SessionInfo::id);
        CHECK(itB != claude.end());
        if (itB != claude.end()) {
            CHECK(itB->title == "sess-b");
        }
        const auto itC = std::ranges::find(claude, "sess-c", &sessions::SessionInfo::id);
        CHECK(itC != claude.end());
        if (itC != claude.end()) {
            CHECK(itC->project == "proj-y");
            CHECK(itC->title == "sess-c");  // 无 user 行回落 stem
        }
        const auto summaryA = sessions::summarizeSession("claude-code", claude1);
        CHECK(summaryA.title == "帮我修复登录页面的 bug");
        CHECK(summaryA.preview == "登录页面已修复");
        const auto summaryB = sessions::summarizeSession("claude-code", claude2);
        CHECK(summaryB.title == "解释一下这段代码");  // 跳过 reminder 和 /clear
    }

    // 3. codex：title 提取与回落、相对日期路径
    {
        const auto codex = sessions::listSessions("codex");
        CHECK(codex.size() == 2);
        const auto it1 = std::ranges::find(codex, "rollout-1", &sessions::SessionInfo::id);
        CHECK(it1 != codex.end());
        if (it1 != codex.end()) {
            CHECK(it1->tool == "codex");
            CHECK(it1->project == "2026/09/06");
            CHECK(it1->title == "rollout-1");
            CHECK(it1->preview.empty());
        }
        const auto it2 = std::ranges::find(codex, "rollout-2", &sessions::SessionInfo::id);
        CHECK(it2 != codex.end());
        if (it2 != codex.end()) {
            CHECK(it2->project == "2026/09/05");
            CHECK(it2->title == "rollout-2");  // 取不到回落 stem
        }
        const auto summary = sessions::summarizeSession("codex", codex1);
        CHECK(summary.title == "审查这个 PR");
        CHECK(summary.preview == "PR 审查完成");
    }

    // 4. 未知工具报错；空目录不炸
    CHECK(throwsRuntimeError([] { (void)sessions::listSessions("nope"); }));
    CHECK(throwsRuntimeError([&] {
        (void)sessions::readSession("nope", claude1);
    }));
    CHECK(throwsRuntimeError([&] {
        (void)sessions::summarizeSession("nope", claude1);
    }));

    // 5. 详情读取完整消息，不影响列表只读摘要的路径
    {
        const auto messages = sessions::readSession("claude-code", claude1);
        CHECK(messages.size() == 2);
        if (messages.size() == 2) {
            CHECK(messages[0].role == "user");
            CHECK(messages[0].text == "帮我修复登录页面的 bug");
            CHECK(messages[1].role == "assistant");
            CHECK(messages[1].text == "登录页面已修复");
        }
        const auto codexMessages = sessions::readSession("codex", codex1);
        CHECK(codexMessages.size() == 2);
        if (codexMessages.size() == 2) {
            CHECK(codexMessages[1].role == "assistant");
            CHECK(codexMessages[1].text == "PR 审查完成");
        }
    }

    // 6. deleteSession：正常删除 + 越界路径拒绝
    {
        sessions::deleteSession(claude3);
        CHECK(!fs::exists(claude3));
        CHECK(sessions::listSessions("claude-code").size() == 2);
        CHECK(throwsRuntimeError([] { sessions::deleteSession("/etc/passwd"); }));
        CHECK(throwsRuntimeError(
            [&] { sessions::deleteSession(root / "outside.jsonl"); }));
        CHECK(throwsRuntimeError(
            [&] { sessions::deleteSession(claudeProjects); }));  // 根目录本身也不行
        sessions::deleteSession(claude2);
        CHECK(throwsRuntimeError(
            [&] { sessions::deleteSession(claude2); }));  // 已删的文件再删报错
    }

    // 7. exportSession：复制成功返回目标路径
    {
        const fs::path destDir = root / "export";
        const fs::path dest = sessions::exportSession(claude1, destDir);
        CHECK(dest == destDir / "sess-a.jsonl");
        CHECK(fs::exists(dest));
        std::ifstream in(dest, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        CHECK(text.find("帮我修复登录页面的 bug") != std::string::npos);
        CHECK(throwsRuntimeError(
            [&] { (void)sessions::exportSession(root / "nope.jsonl", destDir); }));
    }

    if (g_failures == 0) {
        std::println("OK");
    }
    return g_failures == 0 ? 0 : 1;
}
