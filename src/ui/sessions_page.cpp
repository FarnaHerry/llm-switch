// sessions_page.cpp — 会话管理页：按 Agent 分开的列表显示后台批量生成的轻量摘要，
// 详情页按需加载完整会话。列表扫描和详情读取都通过 RunWorker 离开 UI 线程；
// 列表与详情使用 IndexedPages 保留挂载状态，消息列表使用 StateList +
// VirtualList 虚拟化。右上角 Agent 图标与 Agent 管理页使用同一注册表，
// 每个 Pager 页面只在选中时扫描对应历史；会话行仍保留导出和删除操作。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
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

std::vector<sessions::SessionInfo> LoadAgentSessions(std::string tool) {
    return sessions::listSessions(tool);
}

constexpr std::int64_t kSessionCacheTtlMillis = 30'000;

std::int64_t CurrentTimeMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

constexpr std::size_t kStateListCommitBatchSize = 64;
constexpr std::size_t kMessageCollapseThresholdBytes = 3'000;
constexpr std::size_t kMessageCollapsedBytes = 1'500;

// StateList 的每次写入都会使观察它的组合失效。大列表若在一个 UI 回调里一次性
// 逐项提交，会把 worker 中省下来的时间又变成主线程长任务。分批提交并在批次间
// 让出事件循环，保证滚动、窗口拖动和加载动画仍能及时响应。
template <class T>
huxerui::Task<void> ReplaceStateListInBatches(
    const huxerui::StateList<T>& destination, std::vector<T> values) {
    const std::size_t shared = std::min(destination.Size(), values.size());
    std::size_t writesSinceYield = 0;
    const auto yieldIfNeeded = [&writesSinceYield]() -> huxerui::Task<void> {
        if (++writesSinceYield < kStateListCommitBatchSize) co_return;
        writesSinceYield = 0;
        co_await huxerui::Delay(std::chrono::duration<double>{0});
    };
    for (std::size_t i = 0; i < shared; ++i) {
        destination.Set(i, std::move(values[i]));
        co_await yieldIfNeeded();
    }
    while (destination.Size() > values.size()) {
        destination.PopBack();
        co_await yieldIfNeeded();
    }
    for (std::size_t i = shared; i < values.size(); ++i) {
        destination.PushBack(std::move(values[i]));
        co_await yieldIfNeeded();
    }
}

std::string CollapseMessageText(std::string_view text) {
    if (text.size() <= kMessageCollapsedBytes) return std::string(text);
    std::size_t end = kMessageCollapsedBytes;
    while (end > 0 &&
           (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
        --end;
    }
    return std::string(text.substr(0, end)) + "…";
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
    const sessions::SessionInfo& session, huxerui::ToastHandle toast,
    std::function<void(sessions::SessionInfo)> onOpen,
    std::function<void()> onDeleted) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto dialog = huxerui::UseDialog();
    const std::filesystem::path path = session.path;
    const std::string title = session.title;

    // 摘要已经由 Agent 列表扫描 Worker 批量生成；行本身只负责展示和交互，
    // 不在 VirtualList 的行生命周期内重复读取会话文件。
    const std::string rowKey = session.path.generic_string();

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

    auto open = [tasks, onOpen, session] {
        // 切换到保留的详情页也会改变当前命中节点，避开指针事件路径。
        tasks.Launch([onOpen, session]() mutable
                         -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            onOpen(session);
        });
    };

    huxerui::View content = huxerui::Column {
        huxerui::Text(session.title).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kBody), theme.colors.on_surface}),
        session.preview == session.title || session.preview.empty()
            ? huxerui::View{huxerui::Row{}}
            : huxerui::View{huxerui::Text(session.preview).Style(huxerui::TextStyle{
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
        .Key(rowKey);
    return content;
}

[[huxerui::composable]] huxerui::View SessionMessageRow(
    const sessions::SessionMessage& message, std::size_t index) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto expanded = huxerui::UseState(false);
    const bool isUser = message.role == "user";
    const bool isLong = message.text.size() > kMessageCollapseThresholdBytes;
    const std::string role = isUser ? "用户" : "助手";
    const std::string displayText = isLong && !expanded.Get()
                                        ? CollapseMessageText(message.text)
                                        : message.text;

    std::vector<huxerui::View> cardChildren;
    cardChildren.reserve(isLong ? 3U : 2U);
    cardChildren.push_back(
        huxerui::Text(role)
            .Align(isUser ? huxerui::TextAlign::Trailing
                          : huxerui::TextAlign::Leading)
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kChip)
                    .WithWeight(huxerui::FontWeight::SemiBold),
                theme.colors.on_surface_variant}));
    cardChildren.push_back(
        huxerui::Text(displayText)
            .Align(huxerui::TextAlign::Leading)
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                      theme.colors.on_surface}));
    if (isLong) {
        cardChildren.push_back(
            huxerui::Button(expanded.Get() ? "收起" : "展开完整内容")
                .OnClick([expanded] { expanded = !expanded.Get(); }));
    }

    const huxerui::View messageCard =
        Card(huxerui::Column(std::move(cardChildren))
                 .With(huxerui::Spacing(6.0F),
                       huxerui::Padding(
                           huxerui::EdgeInsets::Symmetric(10.0F, 8.0F)),
                       huxerui::CrossAlign(
                           huxerui::CrossAxisAlignment::Stretch)))
            .Key(std::format("message-{}", index));
    huxerui::View messageSlot =
        huxerui::Column {messageCard}.With(
            huxerui::Grow(4.0F),
            huxerui::CrossAlign(isUser ? huxerui::CrossAxisAlignment::End
                                       : huxerui::CrossAxisAlignment::Start));
    huxerui::View row =
        isUser ? huxerui::View{huxerui::Row{huxerui::Spacer(), messageSlot}}
               : huxerui::View{huxerui::Row{messageSlot, huxerui::Spacer()}};
    return std::move(row).Key(std::format("message-row-{}", index));
}

[[huxerui::composable]] huxerui::View AgentSessionsPanel(
    std::string tool, huxerui::State<sessions::SessionInfo> selectedSession,
    huxerui::State<std::size_t> selectedAgent, std::size_t agentIndex) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool supported = tool == "claude-code" || tool == "codex";
    const bool active = selectedAgent.Get() == agentIndex;
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    // 列表扫描是整批替换，不需要逐项可变语义。使用不可变共享快照可让 worker
    // 结果在 UI 线程以一次 O(1) 状态写入完成，避免数百次 StateList 通知和重组。
    auto sessionsSnapshot = huxerui::UseState(
        std::make_shared<const std::vector<sessions::SessionInfo>>());
    auto loading = huxerui::UseState(supported);
    auto loadError = huxerui::UseState(std::string{});
    auto requestGeneration = huxerui::UseState<std::uint64_t>(0);
    auto hasLoaded = huxerui::UseState(false);
    auto loadedAtMillis = huxerui::UseState<std::int64_t>(0);

    auto reload = [tasks, toast, sessionsSnapshot, loading, loadError,
                   requestGeneration, hasLoaded, loadedAtMillis,
                   selectedAgent, agentIndex, tool] {
        const std::uint64_t request = requestGeneration.Get() + 1;
        requestGeneration = request;
        loading = true;
        loadError = std::string{};
        hasLoaded = false;
        loadedAtMillis = 0;
        tasks.Launch([toast, sessionsSnapshot, loading, loadError,
                      requestGeneration, hasLoaded, loadedAtMillis,
                      selectedAgent, agentIndex, tool,
                      request]() -> huxerui::Task<void> {
            try {
                auto result = co_await huxerui::RunWorker(
                    [](std::string selectedTool) {
                        return LoadAgentSessions(std::move(selectedTool));
                    },
                    tool);
                if (requestGeneration.Get() != request ||
                    selectedAgent.Get() != agentIndex) {
                    co_return;
                }
                sessionsSnapshot =
                    std::make_shared<const std::vector<sessions::SessionInfo>>(
                        std::move(result));
                hasLoaded = true;
                loadedAtMillis = CurrentTimeMillis();
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
    auto loadIfNeeded = [reload, hasLoaded, loadedAtMillis] {
        const std::int64_t now = CurrentTimeMillis();
        const std::int64_t loaded = loadedAtMillis.Get();
        const bool fresh = hasLoaded.Get() && loaded > 0 && now >= loaded &&
                           now - loaded < kSessionCacheTtlMillis;
        if (!fresh) reload();
    };
    huxerui::Lifecycle(
        [loadIfNeeded, requestGeneration, active, supported] {
            if (active && supported) loadIfNeeded();
            return [requestGeneration] {
                // Pager 会保留未选中的页面；让切走时尚未完成的 Worker 结果失效，
                // 避免隐藏页面在后台提交列表更新。
                ++requestGeneration;
            };
        },
        std::format("{}:{}", tool, active));

    const auto sessionItems = sessionsSnapshot.Get();
    const std::size_t sessionCount = sessionItems->size();
    const huxerui::Color projectTextColor = theme.colors.on_surface_variant;
    const auto buildSessionRow = [sessionItems, toast, reload, selectedSession,
                                  projectTextColor](
                                     std::size_t index) {
        const auto& session = sessionItems->at(index);
        const bool firstInProject =
            index == 0 || sessionItems->at(index - 1).project != session.project;
        const huxerui::View projectHeader =
            firstInProject
                ? huxerui::View{huxerui::Text(session.project).Style(
                      huxerui::TextStyle{
                          huxerui::Font::System(font_size::kChip)
                              .WithWeight(huxerui::FontWeight::SemiBold),
                          projectTextColor})}
                : huxerui::View{huxerui::Row{}};
        const auto open = [selectedSession](sessions::SessionInfo session) {
            selectedSession = std::move(session);
        };
        return Card(huxerui::Column {
            projectHeader,
            SessionRow(session, toast, open, [reload] { reload(); }),
        }.With(huxerui::Spacing(firstInProject ? 6.0F : 0.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
            .Key(session.path.generic_string());
    };

    huxerui::View listContent;
    if (sessionCount == 0) {
        listContent = huxerui::Column {
            !supported
                ? huxerui::View{
                      huxerui::Text("该 Agent 暂无可读取的历史会话")
                          .Style(huxerui::TextStyle{
                              huxerui::Font::System(font_size::kBody),
                              theme.colors.on_surface_variant})}
                : loading.Get()
                ? huxerui::View{
                      huxerui::Image(app::images::refresh)
                          .Tint(theme.colors.on_surface_variant)
                          .With(huxerui::Frame{.width = 24.0F,
                                               .height = 24.0F},
                                huxerui::Rotation(huxerui::AnimateTo(
                                    360.0F,
                                    huxerui::TweenSpec{1.0,
                                                       huxerui::Easing::Linear},
                                    huxerui::AnimationPlayback{
                                        .iterations = std::nullopt}))) }
                : huxerui::View{
                      huxerui::Text(loadError.Get().empty()
                                        ? "暂无历史会话"
                                        : "加载失败：" + loadError.Get())
                          .Style(huxerui::TextStyle{
                              huxerui::Font::System(font_size::kBody),
                              theme.colors.on_surface_variant})},
        }.With(huxerui::Grow(1.0F),
               huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    } else {
        listContent = huxerui::VirtualList(sessionCount, buildSessionRow)
                          .EstimatedItemExtent(148.0F)
                          .CacheExtent(320.0F)
                          .With(huxerui::Grow(1.0F));
    }

    return Card(huxerui::Column {
        huxerui::Text(std::string(ToolName(tool))).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kTitle)
                .WithWeight(huxerui::FontWeight::SemiBold),
            theme.colors.on_surface}),
        listContent,
    }.With(huxerui::Spacing(8.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

[[huxerui::composable]] huxerui::View SessionsListPage(
    huxerui::State<sessions::SessionInfo> selectedSession) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    const auto& registry = models::toolRegistry();
    auto selectedAgent = huxerui::UseState<std::size_t>(0);

    std::vector<huxerui::View> agentButtons;
    std::vector<huxerui::View> agentPages;
    agentButtons.reserve(registry.size());
    agentPages.reserve(registry.size());
    for (std::size_t index = 0; index < registry.size(); ++index) {
        const auto& spec = registry[index];
        const std::string id(spec.id);
        const std::string label(spec.displayName);
        huxerui::View button =
            huxerui::IconButton(ToolIcon(spec.iconName), label)
                .OnClick([selectedAgent, index] { selectedAgent = index; })
                .With(huxerui::Tooltip(label));
        if (selectedAgent.Get() == index) {
            button = std::move(button).With(
                huxerui::Background(islands.raised),
                huxerui::CornerRadius(islands.nested_radius));
        }
        agentButtons.push_back(std::move(button));
        agentPages.push_back(
            AgentSessionsPanel(id, selectedSession, selectedAgent, index)
                .Key("session-agent:" + id));
    }

    return PageScaffold(
        "会话管理",
        huxerui::Row(std::move(agentButtons))
            .With(huxerui::Spacing(8.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Pager(std::move(agentPages), selectedAgent)
            .ScrollAxis(huxerui::Axis::Horizontal)
            .DragEnabled(false)
            .OnChanged([selectedAgent](std::size_t index) {
                selectedAgent = index;
            })
            .With(huxerui::Grow(1.0F)));
}

[[huxerui::composable]] huxerui::View SessionDetailPage(
    huxerui::State<sessions::SessionInfo> selectedSession) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto messages = huxerui::UseState(
        std::make_shared<const std::vector<sessions::SessionMessage>>());
    auto loading = huxerui::UseState(false);
    auto loadingOlder = huxerui::UseState(false);
    auto loadError = huxerui::UseState(std::string{});
    auto requestGeneration = huxerui::UseState<std::uint64_t>(0);
    auto beforeOffset = huxerui::UseState<std::uintmax_t>(0);
    auto hasMore = huxerui::UseState(false);
    auto scroll = huxerui::UseScrollController();

    const sessions::SessionInfo session = selectedSession.Get();
    const std::string targetKey = session.path.generic_string();
    const std::string tool = session.tool;
    const std::filesystem::path path = session.path;

    auto load = [tasks, messages, loading, loadingOlder, loadError,
                 requestGeneration, beforeOffset, hasMore, scroll](
                    std::string targetTool, std::filesystem::path targetPath) {
        const std::uint64_t request = requestGeneration.Get() + 1;
        requestGeneration = request;
        loading = true;
        loadingOlder = false;
        loadError = std::string{};
        tasks.Launch([messages, loading, loadError, requestGeneration,
                      beforeOffset, hasMore, scroll,
                      targetTool = std::move(targetTool),
                      targetPath = std::move(targetPath), request]()
                         -> huxerui::Task<void> {
            try {
                auto result = co_await huxerui::RunWorker(
                    [](std::string selectedTool, std::filesystem::path file) {
                        return sessions::readSessionPage(selectedTool, file, 0, 50);
                    },
                    targetTool, targetPath);
                if (requestGeneration.Get() != request) co_return;
                messages = std::make_shared<
                    const std::vector<sessions::SessionMessage>>(
                    std::move(result.messages));
                beforeOffset = result.nextBeforeOffset;
                hasMore = result.hasMore;
                loading = false;
                co_await huxerui::Delay(std::chrono::duration<double>{0});
                const auto snapshot = messages.Get();
                if (!snapshot->empty()) {
                    static_cast<void>(scroll.ScrollToItem(
                        snapshot->size() - 1, huxerui::ScrollAlignment::End));
                }
            } catch (const std::exception& e) {
                if (requestGeneration.Get() != request) co_return;
                loadError = e.what();
                loading = false;
            }
        });
    };
    huxerui::Lifecycle(
        [load, tasks, messages, loading, loadingOlder, loadError,
         requestGeneration, beforeOffset, hasMore, scroll, tool, path] {
            if (!path.empty()) load(tool, path);
            const std::uint64_t lifecycleRequest = requestGeneration.Get();
            tasks.Launch([messages, loading, loadingOlder, loadError,
                          requestGeneration, beforeOffset, hasMore, scroll,
                          tool, path, lifecycleRequest]() -> huxerui::Task<void> {
                while (true) {
                    co_await huxerui::Delay(std::chrono::duration<double>{0.15});
                    if (requestGeneration.Get() != lifecycleRequest) co_return;
                    if (path.empty() || loading.Get() || loadingOlder.Get() ||
                        !hasMore.Get() || !scroll.IsConnected() ||
                        scroll.Offset() > 240.0F) {
                        continue;
                    }
                    const float oldOffset = scroll.Offset();
                    loadingOlder = true;
                    try {
                        auto page = co_await huxerui::RunWorker(
                            [](std::string selectedTool,
                               std::filesystem::path file,
                               std::uintmax_t offset) {
                                return sessions::readSessionPage(
                                    selectedTool, file, offset, 50);
                            },
                            tool, path, beforeOffset.Get());
                        if (requestGeneration.Get() != lifecycleRequest) co_return;
                        const std::size_t added = page.messages.size();
                        auto combined = std::move(page.messages);
                        const auto current = messages.Get();
                        combined.insert(combined.end(), current->begin(), current->end());
                        messages = std::make_shared<
                            const std::vector<sessions::SessionMessage>>(
                            std::move(combined));
                        beforeOffset = page.nextBeforeOffset;
                        hasMore = page.hasMore;
                        loadingOlder = false;
                        co_await huxerui::Delay(std::chrono::duration<double>{0});
                        if (added > 0 && scroll.ScrollToItem(
                                             added, huxerui::ScrollAlignment::Start)) {
                            static_cast<void>(scroll.ScrollBy(oldOffset));
                        }
                    } catch (const std::exception& e) {
                        if (requestGeneration.Get() != lifecycleRequest) co_return;
                        loadError = e.what();
                        loadingOlder = false;
                    }
                }
            });
            return [requestGeneration] { ++requestGeneration; };
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

    const auto messageItems = messages.Get();
    const auto buildMessageRow = [messageItems](std::size_t index) {
        const auto& message = messageItems->at(index);
        return SessionMessageRow(message, index);
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
    } else if (messageItems->empty()) {
        content = huxerui::Column {
            huxerui::Text("未找到可显示的文本消息。").Style(
                huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                   theme.colors.on_surface_variant}),
        }.With(huxerui::Grow(1.0F),
               huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    } else {
        content = huxerui::VirtualList(messageItems->size(), buildMessageRow)
                      .EstimatedItemExtent(140.0F)
                      .CacheExtent(480.0F)
                      .Controller(scroll)
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
