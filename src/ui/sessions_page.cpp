// sessions_page.cpp — 会话管理页：列表只显示轻量摘要，详情页按需加载完整会话。
// 列表扫描和详情读取都通过 RunWorker 离开 UI 线程；列表与详情使用
// IndexedPages 保留挂载状态，消息列表使用 StateList + VirtualList 虚拟化。
#include <huxerui/huxerui.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
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

template <class T>
void ReplaceStateList(const huxerui::StateList<T>& destination,
                      std::vector<T> values) {
    const std::size_t shared = std::min(destination.Size(), values.size());
    for (std::size_t i = 0; i < shared; ++i) {
        destination.Set(i, std::move(values[i]));
    }
    while (destination.Size() > values.size()) {
        destination.PopBack();
    }
    for (std::size_t i = shared; i < values.size(); ++i) {
        destination.PushBack(std::move(values[i]));
    }
}

// 简单相对时间：刚刚 / N 分钟前 / N 小时前 / N 天前 / N 个月前 / N 年前。
std::string RelativeTime(std::int64_t mtimeMillis) {
    const auto now = std::chrono::system_clock::now();
    const auto mtime = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{mtimeMillis}};
    const auto secs =
        std::chrono::duration_cast<std::chrono::seconds>(now - mtime).count();
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
    huxerui::ToastHandle toast, std::function<void()> onOpen,
    std::function<void()> onDeleted) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto dialog = huxerui::UseDialog();
    const std::string title = session.title;
    const std::string preview = session.preview;
    const std::filesystem::path path = session.path;

    auto showDeleteConfirm = [dialog, tasks, toast, title, path, onDeleted] {
        dialog.Show(
            "删除会话",
            std::format("确定删除会话「{}」？此操作不可撤销。", title),
            "删除", "取消",
            [tasks, toast, title, path, onDeleted] {
                tasks.Launch([toast, title, path, onDeleted]()
                                 -> huxerui::Task<void> {
                    try {
                        co_await huxerui::RunWorker(
                            [](std::filesystem::path target) {
                                sessions::deleteSession(target);
                            },
                            path);
                        toast.Show(std::format("已删除 {}", title));
                        onDeleted();
                    } catch (const std::exception& e) {
                        toast.Show(e.what());
                    }
                });
            },
            {});
    };

    auto open = [tasks, onOpen] {
        // 切换到保留的详情页也会改变当前命中节点，避开指针事件路径。
        tasks.Launch([onOpen]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            onOpen();
        });
    };

    huxerui::View content = huxerui::Column {
        huxerui::Text(title).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kBody), theme.colors.on_surface}),
        preview == title || preview.empty()
            ? huxerui::View{huxerui::Row{}}
            : huxerui::View{huxerui::Text(preview).Style(huxerui::TextStyle{
                  huxerui::Font::System(font_size::kCaption),
                  theme.colors.on_surface_variant})},
        huxerui::Text(std::format("{} · {} · {}",
                                  RelativeTime(session.mtimeMillis),
                                  FormatSize(session.sizeBytes),
                                  ToolName(session.tool)))
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
                                      theme.colors.on_surface_variant}),
        huxerui::Row {
            huxerui::Button("导出").OnClick([tasks, toast, path] {
                tasks.Launch([toast, path]() -> huxerui::Task<void> {
                    try {
                        const auto dest = co_await huxerui::RunWorker(
                            [](std::filesystem::path source,
                               std::filesystem::path destination) {
                                return sessions::exportSession(source,
                                                               destination);
                            },
                            path, cfg::dataDir() / "exports");
                        toast.Show("已导出到 " + dest.string());
                    } catch (const std::exception& e) {
                        toast.Show(e.what());
                    }
                });
            }),
            huxerui::Button("删除").OnClick([tasks, showDeleteConfirm] {
                tasks.Launch([showDeleteConfirm]() -> huxerui::Task<void> {
                    co_await huxerui::Delay(std::chrono::duration<double>{0});
                    showDeleteConfirm();
                });
            }),
        }.With(huxerui::Spacing(8.0F)),
    }.With(huxerui::Spacing(4.0F),
           huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 8.0F)),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
           huxerui::PointerCursor(huxerui::PointerCursorKind::Hand),
           huxerui::Tooltip("查看会话详情"))
        .OnClick(open)
        .Key(session.tool + "/" + session.id);
    return content;
}

[[huxerui::composable]] huxerui::View SessionsListPage(
    huxerui::State<sessions::SessionInfo> selectedSession) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto filter = huxerui::UseState(0);
    auto sessionsList = huxerui::UseStateList<sessions::SessionInfo>();
    auto loading = huxerui::UseState(true);
    auto loadError = huxerui::UseState(std::string{});
    auto requestGeneration = huxerui::UseState<std::uint64_t>(0);

    auto reload = [tasks, toast, sessionsList, loading, loadError,
                   requestGeneration](int f) {
        const std::uint64_t request = requestGeneration.Get() + 1;
        requestGeneration = request;
        loading = true;
        loadError = std::string{};
        tasks.Launch([toast, sessionsList, loading, loadError,
                      requestGeneration, f, request]() -> huxerui::Task<void> {
            try {
                auto result = co_await huxerui::RunWorker(
                    [](int selectedFilter) { return LoadSessions(selectedFilter); },
                    f);
                if (requestGeneration.Get() != request) co_return;
                ReplaceStateList(sessionsList, std::move(result));
                loading = false;
            } catch (const std::exception& e) {
                if (requestGeneration.Get() != request) co_return;
                const std::string message = e.what();
                loadError = message;
                loading = false;
                toast.Show("加载会话失败：" + message);
            }
        });
    };
    huxerui::Lifecycle(
        [reload] {
            reload(0);
            return [] {};
        },
        0);

    const std::size_t sessionCount = sessionsList.Size();
    const huxerui::Color projectTextColor = theme.colors.on_surface_variant;
    const auto buildSessionRow = [sessionsList, tasks, toast, reload, filter,
                                  selectedSession, projectTextColor](
                                     std::size_t index) {
        const auto& session = sessionsList.At(index);
        const bool firstInProject =
            index == 0 || sessionsList.At(index - 1).project != session.project;
        const huxerui::View projectHeader =
            firstInProject
                ? huxerui::View{huxerui::Text(session.project).Style(
                      huxerui::TextStyle{
                          huxerui::Font::System(font_size::kChip)
                              .WithWeight(huxerui::FontWeight::SemiBold),
                          projectTextColor})}
                : huxerui::View{huxerui::Row{}};
        const auto open = [selectedSession, session] {
            selectedSession = session;
        };
        return Card(huxerui::Column {
            projectHeader,
            SessionRow(session, tasks, toast, open,
                       [reload, filter] { reload(filter.Get()); }),
        }.With(huxerui::Spacing(firstInProject ? 6.0F : 0.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
            .Key(session.tool + "/" + session.id);
    };

    struct FilterItem {
        int index;
        huxerui::ImageResource icon;
        const char* tooltip;
    };
    const std::array<FilterItem, 3> filterItems{{
        {0, app::images::agents, "全部"},
        {1, ToolIcon("claudecode"), "Claude Code"},
        {2, ToolIcon("codex"), "Codex"},
    }};
    std::vector<huxerui::View> headerItems;
    for (const auto& item : filterItems) {
        const bool selected = filter.Get() == item.index;
        const int idx = item.index;
        huxerui::View button =
            huxerui::IconButton(item.icon, item.tooltip)
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
        sessionCount == 0
            ? huxerui::View{
                  huxerui::Column {
                      loading.Get()
                          ? huxerui::View{
                                huxerui::Image(app::images::refresh)
                                    .Tint(theme.colors.on_surface_variant)
                                    .With(huxerui::Frame{.width = 24.0F,
                                                         .height = 24.0F},
                                          huxerui::Rotation(huxerui::AnimateTo(
                                              360.0F,
                                              huxerui::TweenSpec{
                                                  1.0, huxerui::Easing::Linear},
                                              huxerui::AnimationPlayback{
                                                  .iterations = std::nullopt}))) }
                          : huxerui::View{
                                huxerui::Text(
                                    loadError.Get().empty()
                                        ? "未找到历史会话。"
                                        : "加载失败：" + loadError.Get())
                                    .Style(huxerui::TextStyle{
                                        huxerui::Font::System(font_size::kBody),
                                        theme.colors.on_surface_variant})},
                  }.With(huxerui::Padding(32.0F),
                         huxerui::Grow(1.0F),
                         huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                         huxerui::CrossAlign(
                             huxerui::CrossAxisAlignment::Center))}
            : huxerui::View{huxerui::VirtualList(sessionCount, buildSessionRow)
                                .EstimatedItemExtent(148.0F)
                                .CacheExtent(320.0F)
                                .With(huxerui::Grow(1.0F))});
}

[[huxerui::composable]] huxerui::View SessionDetailPage(
    huxerui::State<sessions::SessionInfo> selectedSession) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto messages = huxerui::UseStateList<sessions::SessionMessage>();
    auto loading = huxerui::UseState(false);
    auto loadError = huxerui::UseState(std::string{});
    auto requestGeneration = huxerui::UseState<std::uint64_t>(0);

    const sessions::SessionInfo session = selectedSession.Get();
    const std::string targetKey = session.path.generic_string();
    const std::string tool = session.tool;
    const std::filesystem::path path = session.path;

    auto load = [tasks, messages, loading, loadError, requestGeneration](
                    std::string targetTool, std::filesystem::path targetPath) {
        const std::uint64_t request = requestGeneration.Get() + 1;
        requestGeneration = request;
        loading = true;
        loadError = std::string{};
        tasks.Launch([messages, loading, loadError, requestGeneration,
                      targetTool = std::move(targetTool),
                      targetPath = std::move(targetPath), request]()
                         -> huxerui::Task<void> {
            try {
                auto result = co_await huxerui::RunWorker(
                    [](std::string selectedTool, std::filesystem::path file) {
                        return sessions::readSession(selectedTool, file);
                    },
                    targetTool, targetPath);
                if (requestGeneration.Get() != request) co_return;
                ReplaceStateList(messages, std::move(result));
                loading = false;
            } catch (const std::exception& e) {
                if (requestGeneration.Get() != request) co_return;
                loadError = e.what();
                loading = false;
            }
        });
    };
    huxerui::Lifecycle(
        [load, tool, path] {
            if (!path.empty()) load(tool, path);
            return [] {};
        },
        targetKey);

    auto goBack = [tasks, selectedSession] {
        tasks.Launch([selectedSession]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            selectedSession = sessions::SessionInfo{};
        });
    };
    auto retry = [load, tool, path] {
        if (!path.empty()) load(tool, path);
    };

    const auto buildMessageRow = [messages, theme](std::size_t index) {
        const auto& message = messages.At(index);
        const std::string role = message.role == "user" ? "用户" : "助手";
        return Card(huxerui::Column {
            huxerui::Text(role).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kChip)
                    .WithWeight(huxerui::FontWeight::SemiBold),
                theme.colors.on_surface_variant}),
            huxerui::Text(message.text).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody), theme.colors.on_surface}),
        }.With(huxerui::Spacing(6.0F),
               huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 8.0F)),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
            .Key(std::format("message-{}", index));
    };

    huxerui::View content;
    if (loading.Get()) {
        content = huxerui::Column {
            huxerui::Image(app::images::refresh)
                .Tint(theme.colors.on_surface_variant)
                .With(huxerui::Frame{.width = 24.0F, .height = 24.0F},
                      huxerui::Rotation(huxerui::AnimateTo(
                          360.0F,
                          huxerui::TweenSpec{1.0, huxerui::Easing::Linear},
                          huxerui::AnimationPlayback{.iterations = std::nullopt}))),
        }.With(huxerui::Grow(1.0F),
               huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    } else if (!loadError.Get().empty()) {
        content = huxerui::Column {
            huxerui::Text("加载会话失败：" + loadError.Get()).Style(
                huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                   theme.colors.on_surface_variant}),
            huxerui::Button("重试").OnClick(retry),
        }.With(huxerui::Spacing(12.0F), huxerui::Grow(1.0F),
               huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    } else if (messages.Size() == 0) {
        content = huxerui::Column {
            huxerui::Text("未找到可显示的文本消息。").Style(
                huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                   theme.colors.on_surface_variant}),
        }.With(huxerui::Grow(1.0F),
               huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    } else {
        content = huxerui::VirtualList(messages.Size(), buildMessageRow)
                      .EstimatedItemExtent(140.0F)
                      .CacheExtent(480.0F)
                      .With(huxerui::Grow(1.0F));
    }

    return PageScaffold(
        "会话详情",
        huxerui::Button("返回").OnClick(goBack),
        huxerui::Column {
            huxerui::Text(session.title).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody), theme.colors.on_surface}),
            huxerui::Text(std::format("{} · {} · {}",
                                      ToolName(session.tool),
                                      RelativeTime(session.mtimeMillis),
                                      FormatSize(session.sizeBytes)))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.on_surface_variant}),
            content,
        }.With(huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

} // namespace

[[huxerui::composable]] huxerui::View SessionsPage() {
    auto selectedSession = huxerui::UseState(sessions::SessionInfo{});
    const std::size_t selectedPage =
        selectedSession.Get().path.empty() ? 0U : 1U;
    return huxerui::IndexedPages(
               std::vector<huxerui::View>{
                   SessionsListPage(selectedSession).Key("sessions-list"),
                   SessionDetailPage(selectedSession).Key("session-detail"),
               },
               selectedPage)
        .With(huxerui::Grow(1.0F));
}

} // namespace llmswitch::ui
