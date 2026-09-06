// sessions_page.cpp — 会话管理页：扫描 claude-code / codex 的历史会话
// （sessions::listSessions），按 project 分组展示。顶部工具过滤图标组
// （全部 / Claude Code / Codex——与 Agent 管理页同一套图标资源，选中项
// 实心变体 + raised 底块）+ 刷新图标。每行：标题（store 层已截取 80 字符
// 摘要）+ 相对时间 + 大小 + 消息数；行操作：导出（exportSession 到
// dataDir()/exports/，toast 显示导出路径）、删除（确认框 → deleteSession）。
// 列表整体滚动。
//
// 性能：扫描含真实磁盘 IO（countLines 块读全文件数行、标题提取解析前
// 64KB JSONL），文件一多 UI 线程同步跑会明显卡顿——首载 / 切过滤 / 刷新 /
// 删除后重载一律经 huxerui::RunWorker 派到 worker 线程，恢复点回 UI 线程
// 写 State。重载期间旧列表保持显示不清空，刷新图标自转指示（无文字提示）。
#include <huxerui/huxerui.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "ui.h"
#include "app_resources.h"

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
    huxerui::ToastHandle toast, std::function<void()> onDeleted) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto dialog = huxerui::UseDialog();
    const std::string title = session.title;
    const std::filesystem::path path = session.path;

    auto showDeleteConfirm = [dialog, toast, title, path, onDeleted] {
        dialog.Show(
            "删除会话",
            std::format("确定删除会话「{}」？此操作不可撤销。", title),
            "删除", "取消",
            [toast, title, path, onDeleted] {
                try {
                    sessions::deleteSession(path);
                    toast.Show(std::format("已删除 {}", title));
                } catch (const std::exception& e) {
                    toast.Show(e.what());
                }
                onDeleted();
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
    const IslandTheme islands = ResolveIslandTheme(theme);
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto filter = huxerui::UseState(0);
    auto sessionsList =
        huxerui::UseState<std::vector<sessions::SessionInfo>>({});
    auto loading = huxerui::UseState(true);

    // 重载：扫描经 RunWorker 派到 worker 线程（磁盘 IO 不占 UI 线程），
    // 恢复点回 UI 线程写 State。
    auto reload = [tasks, sessionsList, loading](int f) {
        loading = true;
        tasks.Launch([sessionsList, loading, f]() -> huxerui::Task<void> {
            auto result = co_await huxerui::RunWorker(
                [](int f) { return LoadSessions(f); }, f);
            sessionsList = std::move(result);
            loading = false;
        });
    };
    // 首组合加载。
    huxerui::Lifecycle(
        [reload] {
            reload(0);
            return [] {};
        },
        0);

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
        projectRows[gi].push_back(SessionRow(
            s, tasks, toast, [reload, filter] { reload(filter.Get()); }));
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

    // 过滤图标组：全部（agents 图标）/ Claude Code / Codex，与 Agent 管理页
    // 同一套图标资源与选中态（实心变体 + raised 底块）。点击不重挂载本页，
    // 直接写 filter 并触发 worker 重载。
    struct FilterItem {
        int index;
        IconPair icons;
        const char* tooltip;
    };
    const std::array<FilterItem, 3> filterItems{{
        {0, {app::images::agents, app::images::agents_selected}, "全部"},
        {1, ToolIcon("claudecode"), "Claude Code"},
        {2, ToolIcon("codex"), "Codex"},
    }};
    std::vector<huxerui::View> headerItems;
    for (const auto& item : filterItems) {
        const bool selected = filter.Get() == item.index;
        const int idx = item.index;
        huxerui::View button =
            huxerui::IconButton(selected ? item.icons.selected
                                         : item.icons.normal,
                                item.tooltip)
                .OnClick([filter, reload, idx] {
                    if (filter.Get() == idx) return;
                    filter = idx;
                    reload(idx);
                })
                .With(huxerui::Tooltip(item.tooltip));
        if (selected) {
            button = std::move(button).With(
                huxerui::Background(islands.raised),
                huxerui::CornerRadius(islands.nested_radius));
        }
        headerItems.push_back(std::move(button));
    }
    // 加载指示：重载期间旧列表保持显示，刷新图标自转（无限 Tween 360°），
    // 不再使用文字提示。
    huxerui::View refreshButton =
        huxerui::IconButton(app::images::refresh, "刷新")
            .OnClick([filter, reload] { reload(filter.Get()); })
            .With(huxerui::Tooltip("刷新"));
    if (loading.Get()) {
        refreshButton = std::move(refreshButton).With(huxerui::Rotation(
            huxerui::AnimateTo(360.0F,
                               huxerui::TweenSpec{1.0, huxerui::Easing::Linear},
                               huxerui::AnimationPlayback{.iterations =
                                                              std::nullopt})));
    }
    headerItems.push_back(std::move(refreshButton));

    return PageScaffold(
        "会话管理",
        huxerui::Row(std::move(headerItems))
            .With(huxerui::Spacing(8.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        groups.empty()
            ? huxerui::View{
                  huxerui::Column {
                      // 空态：加载中显示自转刷新图标（无文字），否则提示无会话。
                      loading.Get()
                          ? huxerui::View{
                                huxerui::Image(app::images::refresh)
                                    .With(huxerui::Frame{.width = 24.0F,
                                                         .height = 24.0F},
                                          huxerui::Rotation(huxerui::AnimateTo(
                                              360.0F,
                                              huxerui::TweenSpec{
                                                  1.0, huxerui::Easing::Linear},
                                              huxerui::AnimationPlayback{
                                                  .iterations =
                                                      std::nullopt})))}
                          : huxerui::View{
                                huxerui::Text("未找到历史会话。")
                                    .Style(huxerui::TextStyle{
                                        huxerui::Font::System(font_size::kBody),
                                        theme.colors.on_surface_variant})},
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
