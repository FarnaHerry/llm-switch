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

std::string FormatMessageTimestamp(std::string_view timestamp) {
    if (timestamp.empty()) return "时间未知";
    std::string result(timestamp);
    if (result.size() > 10 && result[10] == 'T') result[10] = ' ';
    return result;
}

std::string SingleLineMessagePreview(std::string_view text) {
    constexpr std::size_t kMaxCodePoints = 12;
    std::string normalized;
    normalized.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\r' || text[index] == '\n') {
            normalized.push_back(' ');
            if (text[index] == '\r' && index + 1 < text.size() &&
                text[index + 1] == '\n') {
                ++index;
            }
        } else {
            normalized.push_back(text[index]);
        }
    }
    if (normalized.empty()) return "—";

    std::size_t codePoints = 0;
    std::size_t end = 0;
    while (end < normalized.size() && codePoints < kMaxCodePoints) {
        const auto byte = static_cast<unsigned char>(normalized[end]);
        const std::size_t width = byte < 0x80U
                                      ? 1U
                                      : (byte & 0xE0U) == 0xC0U
                                            ? 2U
                                            : (byte & 0xF0U) == 0xE0U ? 3U : 4U;
        end = std::min(end + width, normalized.size());
        ++codePoints;
    }
    if (end == normalized.size()) return normalized;

    // 预览文本限制为固定字符数，确保导航栏每项只占一行；按 UTF-8 边界截断。
    if (codePoints == kMaxCodePoints) {
        std::size_t shorterEnd = 0;
        std::size_t count = 0;
        while (shorterEnd < normalized.size() && count + 1 < kMaxCodePoints) {
            const auto byte = static_cast<unsigned char>(normalized[shorterEnd]);
            const std::size_t width = byte < 0x80U
                                          ? 1U
                                          : (byte & 0xE0U) == 0xC0U
                                                ? 2U
                                                : (byte & 0xF0U) == 0xE0U ? 3U : 4U;
            shorterEnd = std::min(shorterEnd + width, normalized.size());
            ++count;
        }
        return normalized.substr(0, shorterEnd) + "…";
    }
    return normalized;
}

struct UserMessageNavigationItem {
    std::size_t messageIndex = 0;
    std::size_t ordinal = 0;
};

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

    auto open = [onOpen, session] {
        // onOpen 只是写 selectedSession State：State 失效排的是稍后重组，
        // 不会同步卸载点击路径上的节点，直接写即可（同 provider 表单的修复）。
        onOpen(session);
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
                // 弹窗会卸载点击路径上的节点：经事件队列推迟，不走帧调度。
                tasks.Launch([showDeleteConfirm]() -> huxerui::Task<void> {
                    showDeleteConfirm();
                    co_return;
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
    const sessions::SessionMessage& message,
    std::shared_ptr<huxerui::Clipboard> clipboard,
    huxerui::ToastHandle toast) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    auto hovered = huxerui::UseState(false);
    const bool isUser = message.role == "user";
    const std::string role = isUser ? "用户" : "助手";
    const std::string copiedText = message.text;
    const std::string timestamp = FormatMessageTimestamp(message.timestamp);

    const auto onHover = [hovered](const huxerui::HoverEvent& event) {
        if (event.type == huxerui::HoverEventType::Leave) {
            hovered = false;
        } else if (event.type == huxerui::HoverEventType::Enter) {
            hovered = true;
        }
    };
    const auto copy = huxerui::IconButton(app::images::copy, "复制消息")
                          .OnClick([clipboard, toast, copiedText] {
                              if (!clipboard->IsAvailable() ||
                                  !clipboard->WriteText(copiedText)) {
                                  toast.Show("剪贴板不可用，复制失败");
                                  return;
                              }
                              toast.Show("已复制当前消息");
                          })
                          .With(huxerui::Opacity(hovered.Get()),
                                huxerui::Tooltip("复制消息"));

    const std::string messageKey = std::format(
        "message:{}:{}", message.sourceOffset, message.role);
    const huxerui::View messageCard =
        huxerui::Column {
            huxerui::Row {
                huxerui::Text(role)
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kChip)
                            .WithWeight(huxerui::FontWeight::SemiBold),
                        theme.colors.on_surface_variant}),
                huxerui::Spacer(),
                huxerui::Text(timestamp)
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kCaption),
                        theme.colors.on_surface_variant}),
                copy,
            }.With(huxerui::Spacing(4.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
            huxerui::Text(message.text)
                .Align(huxerui::TextAlign::Leading)
                .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                          theme.colors.on_surface}),
        }
            .With(huxerui::Spacing(6.0F),
                  huxerui::Padding(
                      huxerui::EdgeInsets::Symmetric(10.0F, 8.0F)),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                  huxerui::Background(theme.colors.surface_container),
                  huxerui::CornerRadius(islands.nested_radius))
            .On<huxerui::ViewEvents::Hover>(onHover)
            .Key(messageKey);
    huxerui::View messageSlot =
        huxerui::Column {messageCard}.With(
            huxerui::Grow(4.0F),
            huxerui::CrossAlign(isUser ? huxerui::CrossAxisAlignment::End
                                       : huxerui::CrossAxisAlignment::Start));
    huxerui::View row =
        isUser ? huxerui::View{huxerui::Row{huxerui::Spacer(), messageSlot}}
               : huxerui::View{huxerui::Row{messageSlot, huxerui::Spacer()}};
    return std::move(row).Key("row:" + messageKey);
}

[[huxerui::composable]] huxerui::View SessionMessageNavigation(
    std::shared_ptr<const std::vector<sessions::SessionMessage>> snapshot,
    huxerui::State<huxerui::TextEditingValue> search,
    huxerui::ScrollController scroll) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const std::string query = search.Get().text;
    std::vector<UserMessageNavigationItem> items;
    std::size_t ordinal = 0;
    for (std::size_t index = 0; index < snapshot->size(); ++index) {
        const auto& message = snapshot->at(index);
        if (message.role != "user") continue;
        ++ordinal;
        if (!query.empty() && message.text.find(query) == std::string::npos) {
            continue;
        }
        items.push_back(UserMessageNavigationItem{index, ordinal});
    }

    const huxerui::Color navigationNumberColor = theme.colors.on_surface_variant;
    const huxerui::Color navigationTextColor = theme.colors.on_surface;
    const auto buildItem = [snapshot, items, scroll, navigationNumberColor,
                            navigationTextColor](std::size_t index) {
        const auto item = items.at(index);
        const auto& message = snapshot->at(item.messageIndex);
        return huxerui::Row {
            huxerui::Text(std::to_string(item.ordinal))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption)
                        .WithWeight(huxerui::FontWeight::SemiBold),
                    navigationNumberColor})
                .With(huxerui::Frame{.width = 24.0F}),
            huxerui::Text(SingleLineMessagePreview(message.text))
                .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
                                          navigationTextColor})
                .With(huxerui::Grow(1.0F)),
        }
            .With(huxerui::Spacing(6.0F),
                  huxerui::Padding(
                      huxerui::EdgeInsets::Symmetric(6.0F, 4.0F)),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                  huxerui::PointerCursor(huxerui::PointerCursorKind::Hand),
                  huxerui::Tooltip(message.text))
            .OnClick([scroll, messageIndex = item.messageIndex] {
                // ScrollToItem 先把视口瞬时跳到目标（变高列表此刻仍是估算落点），
                // 随后挂一个 pending 请求逐帧收敛：每帧只测量视口 ± CacheExtent
                // 的窗口，跳得越远要啃的估算误差越多，表现为可见的慢速爬行。
                // 紧跟一次 ScrollTo 即可终止它——SDK 的 ScrollTo 会先
                // CancelPending()（scroll.cpp），视口就停在瞬时落点上。
                static_cast<void>(scroll.ScrollToItem(
                    messageIndex, huxerui::ScrollAlignment::Start));
                static_cast<void>(scroll.ScrollTo(scroll.Offset()));
            })
            .Key(std::format("session-nav:{}", item.messageIndex));
    };

    huxerui::View list = items.empty()
                             ? huxerui::View{
                                   huxerui::Text(query.empty()
                                                     ? "暂无用户输入"
                                                     : "没有匹配的用户输入")
                                       .Style(huxerui::TextStyle{
                                           huxerui::Font::System(font_size::kCaption),
                                           theme.colors.on_surface_variant})}
                             : huxerui::View{
                                   huxerui::VirtualList(items.size(), buildItem)
                                       .ItemExtent(32.0F)
                                       .CacheExtent(64.0F)
                                       .With(huxerui::Grow(1.0F))};
    return Card(huxerui::Column {
        huxerui::Text("用户消息导航", huxerui::TextRole::Label),
        huxerui::TextField(search.Get())
            .Label("搜索用户输入")
            .Placeholder("输入关键词自动筛选")
            .LeadingIcon(app::images::search)
            .Variant(huxerui::TextFieldVariant::Outlined)
            .LineLimits(huxerui::TextFieldLineLimits::SingleLine())
            .OnChanged([search](const huxerui::TextEditingValue& value) {
                search = value;
            }),
        list,
    }.With(huxerui::Spacing(8.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
        // 导航固定窄栏；这里**不能**再给 Grow——它是 Row 的主轴弹性，会与
        // 内容区各分一份弹量变成左右对半。纵向铺满由父级 CrossAlign(Stretch)
        // 保证，无需本节点参与主轴分配。
        .With(huxerui::Frame{.width = 200.0F},
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
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
        return QuietCard(huxerui::Column {
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
                          // 行每次测量都重新组合并排版，缓存范围只留小缓冲。
                          .CacheExtent(120.0F)
                          .With(huxerui::Grow(1.0F));
    }

    return std::move(listContent).With(
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
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
    const auto clipboard = huxerui::UseApplication().Clipboard();
    auto toast = huxerui::UseToast();
    // 完整消息与右侧导航索引在 worker 中一次读取，UI 线程以不可变快照
    // 一次替换生效；VirtualList 仍只组合可见行，避免长会话逐项写入触发重组。
    auto messages = huxerui::UseState(
        std::make_shared<const std::vector<sessions::SessionMessage>>());
    auto loading = huxerui::UseState(false);
    auto loadError = huxerui::UseState(std::string{});
    auto requestGeneration = huxerui::UseState<std::uint64_t>(0);
    auto scroll = huxerui::UseScrollController();
    auto navigationSearch =
        huxerui::UseState(huxerui::TextEditingValue{});

    const sessions::SessionInfo session = selectedSession.Get();
    const std::string targetKey = session.path.generic_string();
    const std::string tool = session.tool;
    const std::filesystem::path path = session.path;

    auto load = [tasks, messages, loading, loadError, requestGeneration,
                 scroll](
                    std::string targetTool, std::filesystem::path targetPath) {
        const std::uint64_t request = requestGeneration.Get() + 1;
        requestGeneration = request;
        loading = true;
        loadError = std::string{};
        tasks.Launch([messages, loading, loadError, requestGeneration, scroll,
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
                messages =
                    std::make_shared<const std::vector<sessions::SessionMessage>>(
                        std::move(result));
                loading = false;
                co_await huxerui::Delay(std::chrono::duration<double>{0});
                if (!messages.Get()->empty()) {
                    // 详情页停在会话第一条消息。第一条的内容偏移恒为 0，
                    // 不需要经 ScrollToItem 的估算/收敛路径：直接 ScrollTo(0)
                    // 瞬时精确到位，并顺带取消可能存在的 pending 收敛请求。
                    static_cast<void>(scroll.ScrollTo(0.0F));
                }
            } catch (const std::exception& e) {
                if (requestGeneration.Get() != request) co_return;
                loadError = e.what();
                loading = false;
            }
        });
    };
    huxerui::Lifecycle(
        [load, requestGeneration, tool, path] {
            if (!path.empty()) load(tool, path);
            return [requestGeneration] { ++requestGeneration; };
        },
        targetKey);
    auto goBack = [selectedSession] { selectedSession = sessions::SessionInfo{}; };
    auto retry = [load, tool, path] {
        if (!path.empty()) load(tool, path);
    };

    // 本轮组合绑定当前快照：快照不可变，行构建期间数据恒定；新快照写入
    // 触发一次重组后整体切换。
    const auto snapshot = messages.Get();
    const auto buildMessageRow = [snapshot, clipboard, toast](std::size_t index) {
        return SessionMessageRow((*snapshot)[index], clipboard, toast);
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
    } else if (snapshot->empty()) {
        content = huxerui::Column {
            huxerui::Text("未找到可显示的文本消息。").Style(
                huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                   theme.colors.on_surface_variant}),
        }.With(huxerui::Grow(1.0F),
               huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    } else {
        // 行高估算与缓存范围按聊天消息的实际形态调过；缓存范围刻意很小——
        // 可视区外的行每次测量同样会重新组合并重新经 Pango 排版，预 realize
        // 越多每帧开销越大。
        content = huxerui::VirtualList(snapshot->size(), buildMessageRow)
                      .EstimatedItemExtent(240.0F)
                      .CacheExtent(64.0F)
                      .Controller(scroll)
                      .With(huxerui::Spacing(6.0F), huxerui::Grow(1.0F));
    }

    const huxerui::View navigation =
        SessionMessageNavigation(snapshot, navigationSearch, scroll);

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
            huxerui::Row {
                content,
                navigation,
            }.With(huxerui::Spacing(12.0F),
                   huxerui::Grow(1.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
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
