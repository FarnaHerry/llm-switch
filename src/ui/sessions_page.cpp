// sessions_page.cpp — 会话管理页：扫描 claude-code / codex 的历史会话
// （sessions::listSessions），按 project 分组展示。顶部工具过滤
// SegmentedButton（全部 / Claude Code / Codex）+ 刷新按钮。每行：标题
// （store 层已截取 80 字符摘要）+ 相对时间 + 大小 + 消息数；行操作：
// 导出（exportSession 到 dataDir()/exports/，toast 显示导出路径）、
// 删除（确认框 → deleteSession）。列表整体滚动。
//
// 数据流：列表为纯函数扫描结果，页面用 UseState 持有一份，首组合加载；
// 过滤切换 / 刷新 / 删除 / 导出后重新扫描（微秒级本地 IO，UI 线程直接跑）。
#include <huxerui/huxerui.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "ui.h"

import llmswitch.config;
import llmswitch.models;
import llmswitch.sessions;

namespace llmswitch::ui {
namespace {

// 过滤下标 → 工具 id（0 = 全部）。
std::vector<sessions::SessionInfo> LoadSessions(int filter) {
    if (filter == 1) return sessions::listSessions("claude-code");
    if (filter == 2) return sessions::listSessions("codex");
    return sessions::listSessions();
}

// 简单相对时间：刚刚 / N 分钟前 / N 小时前 / N 天前 / N 个月前 / N 年前。
std::string RelativeTime(std::int64_t mtimeMillis) {
    const auto now = std::chrono::system_clock::now();
    const auto mtime = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{mtimeMillis}};
    const auto secs =
        std::chrono::duration_cast<std::chrono::seconds>(now - mtime).count();
    if (secs < 0) return "刚刚";
    if (secs < 60) return "刚刚";
    const auto mins = secs / 60;
    if (mins < 60) return std::format("{} 分钟前", mins);
    const auto hours = mins / 60;
    if (hours < 24) return std::format("{} 小时前", hours);
    const auto days = hours / 24;
    if (days < 30) return std::format("{} 天前", days);
    const auto months = days / 30;
    if (months < 12) return std::format("{} 个月前", months);
    return std::format("{} 年前", months / 12);
}

std::string FormatSize(std::uintmax_t bytes) {
    if (bytes < 1024) return std::format("{} B", bytes);
    if (bytes < 1024 * 1024) return std::format("{:.1f} KB", bytes / 1024.0);
    return std::format("{:.1f} MB", bytes / (1024.0 * 1024.0));
}

[[huxerui::composable]] huxerui::View SessionRow(
    const sessions::SessionInfo& session, huxerui::TaskScope tasks,
    huxerui::ToastHandle toast,
    huxerui::State<std::vector<sessions::SessionInfo>> list, int filter) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto dialog = huxerui::UseDialog();
    const std::string title = session.title;
    const std::filesystem::path path = session.path;

    auto reload = [list, filter] { list = LoadSessions(filter); };

    auto showDeleteConfirm = [dialog, toast, title, path, reload] {
        dialog.Show(
            "删除会话",
            std::format("确定删除会话「{}」？此操作不可撤销。", title),
            "删除", "取消",
            [toast, title, path, reload] {
                try {
                    sessions::deleteSession(path);
                    toast.Show(std::format("已删除 {}", title));
                } catch (const std::exception& e) {
                    toast.Show(e.what());
                }
                reload();
            },
            {});
    };

    return huxerui::Column {
        huxerui::Text(title).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kBody),
            theme.colors.on_surface}),
        huxerui::Text(std::format("{} · {} · {} 条消息 · {}",
                                  RelativeTime(session.mtimeMillis),
                                  FormatSize(session.sizeBytes),
                                  session.messageCount,
                                  ToolName(session.tool)))
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
                                      theme.colors.on_surface_variant}),
        huxerui::Row {
            huxerui::Button("导出").OnClick([toast, path] {
                try {
                    const auto destDir = cfg::dataDir() / "exports";
                    std::error_code ec;
                    std::filesystem::create_directories(destDir, ec);
                    const auto dest = sessions::exportSession(path, destDir);
                    toast.Show("已导出到 " + dest.string());
                } catch (const std::exception& e) {
                    toast.Show(e.what());
                }
            }),
            huxerui::Button("删除").OnClick([tasks, showDeleteConfirm] {
                // 弹窗会卸载点击路径上的节点：推迟出指针事件路径。
                tasks.Launch([=]() -> huxerui::Task<void> {
                    co_await huxerui::Delay(std::chrono::duration<double>{0});
                    showDeleteConfirm();
                });
            }),
        }.With(huxerui::Spacing(8.0F)),
    }.With(huxerui::Spacing(4.0F),
           huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 8.0F)),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
        .Key(session.tool + "/" + session.id);
}

} // namespace

[[huxerui::composable]] huxerui::View SessionsPage() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto filter = huxerui::UseState(0);
    auto sessionsList =
        huxerui::UseState<std::vector<sessions::SessionInfo>>({});
    auto loaded = huxerui::UseState(false);
    if (!loaded.Get()) {
        loaded = true;
        sessionsList = LoadSessions(filter.Get());
    }

    // 按 project 分组（保持首现顺序；listSessions 已按 mtime 倒序）。
    std::vector<huxerui::View> groups;
    std::vector<std::string> projectOrder;
    std::vector<std::vector<huxerui::View>> projectRows;
    for (const auto& s : sessionsList.Get()) {
        std::size_t gi = projectOrder.size();
        for (std::size_t i = 0; i < projectOrder.size(); ++i) {
            if (projectOrder[i] == s.project) {
                gi = i;
                break;
            }
        }
        if (gi == projectOrder.size()) {
            projectOrder.push_back(s.project);
            projectRows.emplace_back();
        }
        projectRows[gi].push_back(
            SessionRow(s, tasks, toast, sessionsList, filter.Get()));
    }
    for (std::size_t i = 0; i < projectOrder.size(); ++i) {
        groups.push_back(Card(huxerui::Column {
            huxerui::Text(projectOrder[i]).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kChip)
                    .WithWeight(huxerui::FontWeight::SemiBold),
                theme.colors.on_surface_variant}),
            huxerui::Column(std::move(projectRows[i]))
                .With(huxerui::Spacing(2.0F),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        }.With(huxerui::Spacing(6.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
            .Key(projectOrder[i]));
    }

    return PageScaffold(
        "会话管理",
        huxerui::Row {
            huxerui::SegmentedButton({"全部", "Claude Code", "Codex"},
                                     static_cast<std::size_t>(filter.Get()))
                .OnChanged([filter, sessionsList](std::size_t index) {
                    filter = static_cast<int>(index);
                    sessionsList = LoadSessions(static_cast<int>(index));
                }),
            huxerui::Button("刷新").OnClick([filter, sessionsList] {
                sessionsList = LoadSessions(filter.Get());
            }),
        }.With(huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        groups.empty()
            ? huxerui::View{
                  huxerui::Column {
                      huxerui::Text("未找到历史会话。")
                          .Style(huxerui::TextStyle{
                              huxerui::Font::System(font_size::kBody),
                              theme.colors.on_surface_variant}),
                  }.With(huxerui::Padding(32.0F),
                         huxerui::Grow(1.0F),
                         huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                         huxerui::CrossAlign(
                             huxerui::CrossAxisAlignment::Center))}
            : huxerui::View{huxerui::ScrollView(
                                huxerui::Column(std::move(groups))
                                    .With(huxerui::Spacing(10.0F),
                                          huxerui::CrossAlign(
                                              huxerui::CrossAxisAlignment::Stretch)))
                                .With(huxerui::Grow(1.0F))});
}

} // namespace llmswitch::ui
