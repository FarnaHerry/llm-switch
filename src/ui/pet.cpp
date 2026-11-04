// pet.cpp — AI 桌宠实现：莲花球视觉（NodeExtension 逐帧重绘）、活动状态轮询、
// 指针交互与右键菜单。莲花球使用主题纯色底与反色矢量莲花，
// 深浅主题均为黑白，缩放随相位起伏，
// 空闲 ~6s 一个呼吸周期。状态由本地路由和用户连接的 Agent hooks 汇总：
//   路由在途请求或原生 Agent 工作中/等待输入时呼吸更深更快；最近一次请求失败
//   6s 内叠加外环脉冲（failure）；一波活动批次（含成功请求）静默 ≥8s 结束
//   后右上角挂 ✓ 完成提醒（done，静态绘制，点击/右键/新批次消解，30 秒自动消退）；
//   底部角标显示今日请求数，顶部浮标显示活跃 Agent（≤2 个）。
// 动画走 NodeExtension 的保留绘制（30fps 上限，reduced-motion 下静止——✓ 与
// 浮标仍显示），统计轮询走 TaskScope + Delay（每秒一次，窗口退役时随作用域
// 取消）。右键菜单由独立窗口承载，共用主应用主题与共享主题状态。
#include <huxerui/huxerui.h>

#include <chrono>
#include <cmath>
#include <ctime>
#include <format>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <utility>
#include <vector>

#include "app.h"
#include "app_resources.h"
#include "pet.h"
#include "theme_colors.h"
#include "ui.h"

import llmswitch.models;
import llmswitch.router;
import llmswitch.store;
import llmswitch.agents;
import llmswitch.agents.bridge;

namespace llmswitch::ui {
namespace {

using huxerui::Color;
using huxerui::Point;
using huxerui::Rect;

// ---- Orb 视觉 ---------------------------------------------------------------

struct Orb {
    class Extension;
    bool active = false;
    bool waiting = false;
    bool failure = false;
    bool done = false;  // 完成提醒挂起（✓ 标记，静态绘制）
    bool animate = true;
    // 点击互动（PetController::Play）：epoch 变化触发重播，kind 选动画形态。
    int playEpoch = 0;
    int playKind = 0;
    // 使用矢量标志按主题着色，避免栅格素材自身的冷白色残留。
    huxerui::VectorAsset lotus{};
    Color background{};
    Color foreground{};
    bool operator==(const Orb&) const = default;
};

// 互动动画时长（秒）：点击后播放一次，结束后静止回呼吸。
constexpr float kReactDuration = 0.9F;

// 普通纯色球体；球外透明像素在 Windows 上保留原生点击穿透。
void PaintOrb(huxerui::PaintContext& paint, huxerui::Size size, float phase,
              const Orb& orb, float react_elapsed) {
    const Point center{size.width * 0.5F, size.height * 0.5F};
    const float radius = std::min(size.width, size.height) * 0.37F;
    if (radius <= 0.0F) return;
    const Rect bounds{center.x - radius, center.y - radius, radius * 2.0F, radius * 2.0F};
    const huxerui::Path circle = huxerui::Path::RoundedRect(bounds, huxerui::CornerRadii{radius});
    // 互动反应进度（0→1）；epoch 为 0 表示从未点击过。
    const float react = std::clamp(react_elapsed / kReactDuration, 0.0F, 1.0F);
    const bool reacting = orb.playEpoch > 0 && react < 1.0F;
    paint.FillPath(circle, orb.background);
    paint.PushPathClip(circle);

    // 品牌莲花居中呼吸（按主题反色绘制矢量资源）：缩放随相位
    // 起伏，活动时呼吸更深更快——OnFrame 的 phase 步长控制呼吸周期。
    // 点击互动叠加一次性位移：弹跳（kind 0）上跳落定，摇摆（kind 1）
    // 阻尼左右晃。
    float lotus_dx = 0.0F;
    float lotus_dy = 0.0F;
    if (reacting) {
        if (orb.playKind == 0) {
            const float hop = std::sin(std::min(react / 0.75F, 1.0F) *
                                       std::numbers::pi_v<float>);
            lotus_dy = -hop * radius * 0.15F;
        } else if (orb.playKind == 1) {
            lotus_dx = std::sin(react * 4.0F * std::numbers::pi_v<float>) *
                       (1.0F - react) * radius * 0.08F;
        }
    }
    if (orb.lotus.HasValue()) {
        const float breathe = 1.0F + (orb.active ? 0.060F : 0.035F) * std::sin(phase);
        const float lotus_radius = radius * 0.58F * breathe;
        const Rect lotus_bounds{center.x - lotus_radius + lotus_dx,
                                center.y - lotus_radius + lotus_dy,
                                lotus_radius * 2.0F, lotus_radius * 2.0F};
        paint.DrawImage(orb.lotus, lotus_bounds, orb.foreground);
    }

    paint.PopClip();

    // 闪亮（kind 2）：三粒反色小点从球顶升起并缩小（球体裁剪之外）。
    if (reacting && orb.playKind == 2) {
        for (int i = 0; i < 3; ++i) {
            const float delay = 0.12F * i;
            const float p = std::clamp((react - delay) / (1.0F - delay), 0.0F, 1.0F);
            if (p <= 0.0F || p >= 1.0F) continue;
            paint.DrawCircle(
                {center.x + (i - 1) * radius * 0.5F,
                 center.y - radius * (0.9F + 0.55F * p)},
                radius * (0.07F + 0.03F * (1.0F - p)),
                orb.foreground);
        }
    }

    paint.StrokePath(circle, orb.foreground, huxerui::StrokeStyle{.width = 0.8F});

    // failure：黑白外环随相位改变宽度，仍停留 6s。
    if (orb.failure) {
        const float width = 2.2F + 0.8F * std::sin(phase * 2.4F);
        paint.DrawArc(center, radius * 1.06F, 0.0F, 6.28318F,
                      orb.foreground,
                      huxerui::StrokeStyle{.width = width, .cap = huxerui::StrokeCap::Round});
    }
    // 等待确认使用粗弧，工作中弧线宽度随呼吸变化。
    if (orb.waiting) {
        paint.DrawArc(center, radius * 0.94F, 3.80F, 0.86F,
                      orb.foreground,
                      huxerui::StrokeStyle{.width = 1.8F, .cap = huxerui::StrokeCap::Round});
    } else if (orb.active) {
        const float width = 1.4F + 0.4F * std::sin(phase);
        paint.DrawArc(center, radius * 0.94F, 3.80F, 0.86F,
                      orb.foreground,
                      huxerui::StrokeStyle{.width = width, .cap = huxerui::StrokeCap::Round});
    } else {
        paint.DrawArc(center, radius * 0.94F, 3.80F, 0.86F,
                      orb.foreground,
                      huxerui::StrokeStyle{.width = 0.8F, .cap = huxerui::StrokeCap::Round});
    }

    // 完成提醒：右上角 ✓ 徽标（静态绘制，不随相位动——reduced-motion 下
    // 同样显示），点击桌宠或右键消解。反色底 + 主题底色对勾。
    if (orb.done) {
        const Point badgeCenter{center.x + radius * 0.72F, center.y - radius * 0.72F};
        const float badgeRadius = radius * 0.30F;
        paint.DrawCircle(badgeCenter, badgeRadius, orb.foreground);
        const huxerui::StrokeStyle check{.width = badgeRadius * 0.28F,
                                .cap = huxerui::StrokeCap::Round,
                                .join = huxerui::StrokeJoin::Round};
        const float k = badgeRadius * 0.45F;
        paint.DrawLine({badgeCenter.x - k, badgeCenter.y},
                       {badgeCenter.x - k * 0.25F, badgeCenter.y + k * 0.7F},
                       orb.background, check);
        paint.DrawLine({badgeCenter.x - k * 0.25F, badgeCenter.y + k * 0.7F},
                       {badgeCenter.x + k, badgeCenter.y - k * 0.55F},
                       orb.background, check);
    }
}

class Orb::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode& node, const Orb& orb) { Update(node, orb); }

    void Update(huxerui::ViewNode&, const Orb& orb) {
        if (orb_ != orb) {
            // 新一次点击互动：从 0 重播（epoch 只增，别的状态变化不打断）。
            if (orb.playEpoch != orb_.playEpoch) react_elapsed_ = 0.0F;
            orb_ = orb;
            InvalidatePaint();
        }
    }

    NodeExtension::FrameResult OnFrame(huxerui::ViewNode&, const huxerui::FrameInfo& frame) override {
        if (frame.reduced_motion || !orb_.animate) return {};
        const float delta = static_cast<float>(std::clamp(frame.delta_time, 0.0, 0.1));
        bool invalidated = false;
        if (delta > 0.0F) {
            constexpr float wave_cycle = 20.0F * std::numbers::pi_v<float>;
            // 呼吸周期：空闲 ~6s，活动中 ~2.5s。
            phase_ = std::fmod(phase_ + delta * (orb_.active ? 2.5F : 1.0F), wave_cycle);
            invalidated = true;
        }
        // 互动动画推进；播完停在时长处（react == 1 → 静止）。
        if (react_elapsed_ < kReactDuration) {
            react_elapsed_ = std::min(react_elapsed_ + delta, kReactDuration);
            invalidated = true;
        }
        if (invalidated) InvalidatePaint();
        return {.wake_after = 1.0 / 30.0};
    }

    void PaintAboveContent(const huxerui::ViewNode& node, huxerui::PaintContext& paint) const override {
        PaintOrb(paint, node.LayoutSize(), phase_, orb_, react_elapsed_);
    }

private:
    Orb orb_;
    float phase_ = 0.0F;
    float react_elapsed_ = kReactDuration;  // 未点击过 = 已播完（静止）。
};

// 角标药丸：不透明主题纯色底与反色文字。
huxerui::View TodayBadge(std::int64_t today,
                         const huxerui::ThemeSpec& theme) {
    return huxerui::Text(std::format("{} 次", today))
        .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
                                  theme.colors.on_surface})
        .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 3.0F)),
              huxerui::Background(GlassFill(theme)),
              huxerui::CornerRadius(9.0F))
        .With(huxerui::Align(huxerui::HorizontalAlignment::Center,
                             huxerui::VerticalAlignment::End),
              huxerui::Padding(huxerui::EdgeInsets{.bottom = 6.0F}));
}

// 活跃 Agent 浮标：顶缘居中的一枚纯色药丸，内嵌最多 2 个品牌 Agent
// 图标（品牌图标按深浅主题选黑/白变体）。白龙「双会话」同款语义。
huxerui::View ActiveAgentBadge(const std::vector<std::string>& tools,
                               const huxerui::ThemeSpec& theme) {
    const bool dark = theme.brightness == huxerui::ThemeBrightness::Dark;
    std::vector<huxerui::View> icons;
    for (const std::string& tool : tools) {
        icons.push_back(huxerui::Image(ToolIcon(tool, dark))
                            .With(huxerui::Frame{.width = 14.0F, .height = 14.0F}));
    }
    std::string label;
    for (std::size_t i = 0; i < tools.size(); ++i) {
        if (i > 0) label += "、";
        label += ToolName(tools[i]);
    }
    return huxerui::Row{std::move(icons)}
        .With(huxerui::Spacing(5.0F),
              huxerui::Padding(huxerui::EdgeInsets::Symmetric(7.0F, 3.0F)),
              huxerui::Background(GlassFill(theme)),
              huxerui::CornerRadius(9.0F),
              huxerui::Semantics{.label = std::format("活跃 Agent：{}", label)})
        .With(huxerui::Align(huxerui::HorizontalAlignment::Center,
                             huxerui::VerticalAlignment::Start),
              huxerui::Padding(huxerui::EdgeInsets{.top = 6.0F}));
}

huxerui::View UnknownActivityBadge(const huxerui::ThemeSpec& theme) {
    return huxerui::Row{huxerui::Text("?")
                            .Style(huxerui::TextStyle{
                                huxerui::Font::System(font_size::kCaption)
                                    .WithWeight(huxerui::FontWeight::Bold),
                                theme.colors.on_secondary_container})}
        .With(huxerui::Frame{.width = 20.0F, .height = 20.0F},
              huxerui::Background(theme.colors.secondary_container),
              huxerui::CornerRadius(10.0F),
              huxerui::Border(theme.colors.outline, 1.0F),
              huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
              huxerui::Semantics{.label = "活动状态未知：尚未观察到本地路由请求"})
        .With(huxerui::Align(huxerui::HorizontalAlignment::End,
                             huxerui::VerticalAlignment::Start),
              huxerui::Padding(
                  huxerui::EdgeInsets{.top = 6.0F, .right = 6.0F}));
}

} // namespace

[[huxerui::composable]]
huxerui::View PetLotusBody(
    const std::shared_ptr<PetController>& controller,
    const huxerui::ThemeSpec& theme) {
    const auto window = huxerui::UseWindow();
    const huxerui::VectorAsset lotus =
        huxerui::UseVectorImage(app::images::lotus_bloom);
    const std::vector<std::string> activeTools = controller->activeTools.Get();
    const std::int64_t today = controller->todayRequests.Get();
    const PetPlay play = controller->play.Get();
    const bool visible =
        window.LifecycleState() != huxerui::WindowLifecycleState::Background;
    std::vector<huxerui::View> children;
    if (!activeTools.empty()) {
        children.push_back(ActiveAgentBadge(activeTools, theme));
    }
    if (today > 0) children.push_back(TodayBadge(today, theme));
    return huxerui::Stack{children}.With(
        huxerui::Indication{},
        Orb{.active = controller->streaming.Get() ||
                      controller->agentWorking.Get(),
            .waiting = controller->waitingForInput.Get(),
            .failure = controller->failure.Get(),
            .done = controller->donePendingAck.Get(),
            .animate = visible && controller->animate.Get(),
            .playEpoch = play.epoch,
            .playKind = play.kind,
            .lotus = lotus,
            .background = theme.colors.surface,
            .foreground = theme.colors.on_surface},
        huxerui::Grow(1.0F));
}

[[huxerui::composable]]
huxerui::View PetSkinFallback(
    const std::shared_ptr<PetController>& controller, std::string status,
    const huxerui::ThemeSpec& theme) {
    return huxerui::Stack {
        PetLotusBody(controller, theme),
        huxerui::Text(status)
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                theme.colors.on_surface})
            .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 4.0F)),
                  huxerui::Background(GlassFill(theme)),
                  huxerui::Border{GlassBorder(theme)},
                  huxerui::CornerRadius(9.0F),
                  huxerui::Align(huxerui::HorizontalAlignment::Center,
                                 huxerui::VerticalAlignment::End),
                  huxerui::Padding(huxerui::EdgeInsets{.bottom = 8.0F})),
    }.With(huxerui::Grow(1.0F));
}

// ---- 主题（右键菜单与主应用同款）--------------------------------------------

// 桌宠与信息卡复用主应用完整控件主题，右键菜单由独立窗口承载。
huxerui::ThemeDefinition PetThemeDefinition(bool dark) {
    return AppThemeDefinition(dark ? AppDarkThemeSpec() : AppLightThemeSpec());
}

// ---- PetController ----------------------------------------------------------

// 本地当天 0 点的毫秒时间戳（与路由统计 todayRequests 同一划分口径）。
// 不用 std::chrono::zoned_time：Apple libc++ 未启用 tzdb（CI 实证），
// 便携实现与 router.cpp 的 todayStartMillis 相同。
std::int64_t localMidnightMillis() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    ::localtime_s(&tm, &t);  // MSVC 安全版（参数对调）
#else
    ::localtime_r(&t, &tm);
#endif
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    return static_cast<std::int64_t>(std::mktime(&tm)) * 1000;
}

std::string FormatPetTime(std::int64_t timestampMillis) {
    const std::time_t timestamp = static_cast<std::time_t>(timestampMillis / 1000);
    std::tm tm{};
#ifdef _WIN32
    ::localtime_s(&tm, &timestamp);
#else
    ::localtime_r(&timestamp, &tm);
#endif
    return std::format("{:02}:{:02}", tm.tm_hour, tm.tm_min);
}

void PetController::Poll() {
    auto& router = routerInstance();
    const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    // 路由 streaming 是真状态（在途计数），不再用「最近完成」猜测窗；failure
    // 脉冲窗 6s；批次静默 8s 判定结束（结束且含成功请求 → 完成提醒）；
    // 活跃 Agent 看 60s 窗口。
    constexpr std::int64_t kFailureWindowMs = 6000;
    constexpr std::int64_t kBatchQuietMs = 8000;
    constexpr std::int64_t kActiveToolWindowMs = 60000;
    constexpr std::int64_t kDoneReminderDurationMs = 30000;
    const std::vector<router::RequestLog> logs = router.recentLogs(24);
    PetNativeActivitySnapshot native;
    if (readNativeActivity) native = readNativeActivity(now);

    bool nativeObserved = false;
    bool nativeUncertain = false;
    bool next_agent_working = false;
    bool next_waiting = false;
    bool nativeFailure = false;
    bool newNativeCompletion = false;
    for (const auto& source : native.sources) {
        if (!source.enabled) {
            lastNativeOutcomeMillis.erase(source.tool);
            continue;
        }
        nativeUncertain = nativeUncertain || source.unknown;
        nativeObserved = nativeObserved ||
            !source.unknown;
        next_agent_working = next_agent_working ||
            source.state == PetNativeState::working ||
            source.state == PetNativeState::waiting;
        next_waiting = next_waiting ||
            source.state == PetNativeState::waiting;
        nativeFailure = nativeFailure || source.state == PetNativeState::failed;
    }
    for (const auto& outcome : native.recentOutcomes) {
        std::int64_t& lastSeen = lastNativeOutcomeMillis[outcome.tool];
        if (outcome.occurredAtMillis <= lastSeen) continue;
        lastSeen = outcome.occurredAtMillis;
        newNativeCompletion = newNativeCompletion ||
            outcome.state == PetNativeState::completed;
    }

    // 首个 Poll 只对齐水位：启动时的历史日志不算新活动（否则一开桌宠就
    // 凭旧日志挂出完成提醒）。
    if (firstPoll) {
        firstPoll = false;
        if (!logs.empty()) lastSeenLogTs = logs.front().tsMillis;
    }

    // 新完成请求（水位推进）与成败归并。
    bool newActivity = false;
    bool newSuccess = false;
    for (const auto& log : logs) {
        if (log.tsMillis <= lastSeenLogTs) continue;
        newActivity = true;
        lastSeenLogTs = log.tsMillis;
        if (log.status >= 200 && log.status < 400) newSuccess = true;
    }

    const bool routerAvailable = router.running();
    const bool next_streaming = routerAvailable && router.inflight() > 0;
    if (!routerAvailable) {
        localRouterObservedThisSession = false;
    } else if (next_streaming || newActivity) {
        localRouterObservedThisSession = true;
    }
    const bool routerObserved = routerAvailable && localRouterObservedThisSession;
    const bool nextUnknown = (!routerObserved && !nativeObserved) || nativeUncertain;
    if (activityUnknown.Get() != nextUnknown) activityUnknown = nextUnknown;

    // 活动批次状态机：在途或新完成都算「有流量」；静默超时判定批次结束，
    // 含成功请求则挂完成提醒（点击 / 右键 / 新批次消解）。
    if (next_streaming || newActivity || next_agent_working) {
        if (!batchActive) {
            batchActive = true;
            batchAnySuccess = false;
            if (donePendingAck.Get()) AckDone();
        }
        batchAnySuccess = batchAnySuccess || newSuccess;
        lastActivityMs = now;
    } else if (batchActive && now - lastActivityMs >= kBatchQuietMs) {
        batchActive = false;
        if (batchAnySuccess) {
            donePendingAck = true;
            doneExpiresAtMillis = now + kDoneReminderDurationMs;
        }
    }

    // failure：最新一条日志在脉冲窗内且失败（传输失败或 ≥400）。
    bool next_failure = false;
    if (!logs.empty()) {
        const auto& newest = logs.front();
        if (now - newest.tsMillis <= kFailureWindowMs &&
            (newest.status == 0 || newest.status >= 400)) {
            next_failure = true;
        }
    }

    if (streaming.Get() != next_streaming) streaming = next_streaming;
    if (agentWorking.Get() != next_agent_working) agentWorking = next_agent_working;
    if (waitingForInput.Get() != next_waiting) waitingForInput = next_waiting;
    next_failure = next_failure || nativeFailure;
    if (failure.Get() != next_failure) failure = next_failure;
    if (newNativeCompletion) {
        donePendingAck = true;
        doneExpiresAtMillis = now + kDoneReminderDurationMs;
    }
    if (donePendingAck.Get() && doneExpiresAtMillis > 0 &&
        now >= doneExpiresAtMillis) {
        AckDone();
    }

    // 活跃 Agent：60s 窗口内有请求的工具，按最近时间序去重，最多 2 个
    //（白龙「最多两个会话、更多轮换」的同款语义）。
    std::vector<std::string> next_tools;
    for (const auto& tool : native.activeTools) {
        if (next_tools.size() == 2) break;
        if (std::find(next_tools.begin(), next_tools.end(), tool) ==
            next_tools.end()) {
            next_tools.push_back(tool);
        }
    }
    for (const auto& log : logs) {  // 新的在前：先见即更近
        if (now - log.tsMillis > kActiveToolWindowMs) break;
        if (std::find(next_tools.begin(), next_tools.end(), log.tool) ==
            next_tools.end()) {
            next_tools.push_back(log.tool);
        }
        if (next_tools.size() == 2) break;
    }
    if (activeTools.Get() != next_tools) activeTools = next_tools;

    const std::int64_t next_today = router.snapshot().todayRequests;
    if (todayRequests.Get() != next_today) todayRequests = next_today;

    // 信息卡快照：按注册表序聚合今日（本地 0 点起）每 Agent 的请求与
    // tokens（-1 未提取不计）；环内最多 1000 条，超出的极早期请求不回看。
    {
        const std::int64_t sinceMs = localMidnightMillis();
        PetCardSnapshot next_snapshot;
        next_snapshot.todayRequests = next_today;
        next_snapshot.rows.reserve(models::toolRegistry().size());
        for (const auto& spec : models::toolRegistry()) {
            if (!models::toolEnabled(providerStore().config(), spec.id)) continue;
            next_snapshot.rows.push_back(PetAgentRow{.tool = std::string(spec.id)});
        }
        for (auto& row : next_snapshot.rows) {
            const auto source = std::find_if(native.sources.begin(), native.sources.end(),
                [&row](const PetNativeSourceSnapshot& item) {
                    return item.tool == row.tool;
                });
            if (source == native.sources.end() ||
                !source->enabled) {
                continue;
            }
            switch (source->state) {
                case PetNativeState::working: row.activityLabel = "原生来源：工作中"; break;
                case PetNativeState::waiting: row.activityLabel = "原生来源：等待处理"; break;
                case PetNativeState::completed: row.activityLabel = "原生来源：刚完成"; break;
                case PetNativeState::failed: row.activityLabel = "原生来源：刚失败"; break;
                case PetNativeState::cancelled: row.activityLabel = "原生来源：已中断"; break;
                case PetNativeState::idle: row.activityLabel = "原生来源：已观察为空闲"; break;
                case PetNativeState::unknown:
                    row.activityLabel = "原生来源：状态未知";
                    break;
            }
        }
        for (const auto& log : logs) {
            if (log.tsMillis < sinceMs) continue;
            const auto it = std::find_if(
                next_snapshot.rows.begin(), next_snapshot.rows.end(),
                [&log](const PetAgentRow& row) { return row.tool == log.tool; });
            if (it == next_snapshot.rows.end()) continue;
            ++it->requests;
            if (log.promptTokens >= 0) it->promptTokens += log.promptTokens;
            if (log.completionTokens >= 0) it->completionTokens += log.completionTokens;
            if (log.status == 0 || log.status >= 400) ++it->errors;
        }

        // 最近结果：路由记录提供供应商与可用 token；原生 hook 只保留终态、Agent
        // 和时间。相同 Agent、相同结果且时间相差不超过 3 秒时合并为一条，避免
        // 同一任务同时经过 hooks 与 LocalRouter 时重复显示。
        for (const auto& log : logs) {
            PetRecentResult result{
                .tool = log.tool,
                .providerName = log.providerName.empty() ? log.providerId
                                                         : log.providerName,
                .sourceLabel = "本地路由",
                .outcome = log.status == 0 || log.status >= 400
                    ? PetRecentOutcome::failed : PetRecentOutcome::completed,
                .timestampMillis = log.tsMillis,
                .statusCode = log.status,
                .promptTokens = log.promptTokens,
                .completionTokens = log.completionTokens,
            };
            next_snapshot.recentResults.push_back(std::move(result));
        }
        for (const auto& outcome : native.recentOutcomes) {
            PetRecentOutcome resultOutcome = PetRecentOutcome::completed;
            if (outcome.state == PetNativeState::failed) {
                resultOutcome = PetRecentOutcome::failed;
            } else if (outcome.state == PetNativeState::cancelled) {
                resultOutcome = PetRecentOutcome::cancelled;
            } else if (outcome.state != PetNativeState::completed) {
                continue;
            }
            PetRecentResult result{
                .tool = outcome.tool,
                .sourceLabel = "原生 hook",
                .outcome = resultOutcome,
                .timestampMillis = outcome.occurredAtMillis,
            };
            const bool alreadyListed = std::any_of(
                next_snapshot.recentResults.begin(), next_snapshot.recentResults.end(),
                [&result](const PetRecentResult& routed) {
                    if (routed.tool != result.tool || routed.outcome != result.outcome) {
                        return false;
                    }
                    const std::int64_t delta = routed.timestampMillis >= result.timestampMillis
                        ? routed.timestampMillis - result.timestampMillis
                        : result.timestampMillis - routed.timestampMillis;
                    return delta <= 3000;
                });
            if (!alreadyListed) next_snapshot.recentResults.push_back(std::move(result));
        }
        std::sort(next_snapshot.recentResults.begin(), next_snapshot.recentResults.end(),
                  [](const PetRecentResult& left, const PetRecentResult& right) {
                      return left.timestampMillis > right.timestampMillis;
                  });
        if (next_snapshot.recentResults.size() > 5) {
            next_snapshot.recentResults.resize(5);
        }
        if (cardSnapshot.Get() != next_snapshot) cardSnapshot = next_snapshot;
    }
}

PetSkinDescriptor PetController::Descriptor() const {
    const PetSkin kind = skin.Get();
    PetSkinDescriptor descriptor{.kind = kind};
    if (kind == PetSkin::shimeji) {
        descriptor.modelId = shimejiPack.Get();
        descriptor.size = {shimejiWidth, shimejiHeight};
    } else if (kind == PetSkin::live2d) {
        descriptor.modelId = live2dModel.Get();
        descriptor.size = {live2dWidth, live2dHeight};
        descriptor.available = PetLive2DAvailable();
        if (!descriptor.available) descriptor.status = "此构建未启用 Live2D";
    }
    return descriptor;
}

void PetController::Show(const huxerui::ApplicationHandle& application) {
    if (!pet) {
        auto options = PetOptions(topmost.Get(), compact.Get(), Descriptor());
        options.initial_position = saved_position;
        try {
            pet = application.OpenWindow(
                [] { return PetContent(); }, options);
        } catch (const std::logic_error&) {
            options.show_without_activation = false;
            options.topmost = false;
            pet = application.OpenWindow(
                [] { return PetContent(); }, options);
            topmost = false;
        }
        if (!pet->Capabilities().topmost) topmost = false;
    }
    if (!pet->Capabilities().transparent_surface) {
        Close();
        throw std::runtime_error("当前平台不支持透明桌宠窗口");
    }
    pet->Show();
    epoch = epoch.Get() + 1;
    if (refreshTray) refreshTray();
}

void PetController::CloseCard() {
    ++cardGeneration;
    auto closing_card = std::exchange(card, std::nullopt);
    if (closing_card) closing_card->Close();
}

void PetController::Close() {
    if (pet) saved_position = pet->Position();
    // 先失配代次并摘下句柄，旧附件的清理不会清掉新开的替代窗口；关闭
    // 回调也能据此区分控制器主动 Close 与用户的关闭请求。
    ++generation;
    CloseContextMenu();
    CloseCard();
    auto closing_pet = std::exchange(pet, std::nullopt);
    if (closing_pet) closing_pet->Close();
    epoch = epoch.Get() + 1;
    if (refreshTray) refreshTray();
}

void PetController::Hide(bool trayAvailable) {
    if (!trayAvailable) return;
    CloseContextMenu();
    if (lightweight.Get()) {
        Close();
    } else {
        if (card) card->Hide();
        if (pet) pet->Hide();
    }
}

void PetController::SetLightweight(bool value, bool trayAvailable) {
    if (!trayAvailable) return;
    lightweight = value;
    if (value && pet &&
        pet->LifecycleState() == huxerui::WindowLifecycleState::Background) {
        Hide(true);
    }
}

void PetController::SetTopmost(bool value) {
    if (!pet || pet->SetTopmost(value) == huxerui::WindowRequestResult::Submitted) {
        topmost = value;
    }
}

void PetController::ToggleCard(const huxerui::ApplicationHandle& application) {
    if (card) {
        CloseCard();
        return;
    }
    huxerui::WindowOptions options{
        .title = "llm-switch 桌宠信息卡",
        .initial_size = {1080.0F, 720.0F},
        .minimum_size = huxerui::Size{800.0F, 600.0F},
        // 与主应用同款自定义标题栏（24pt，莲花 + 应用名），不出现平台
        // 原生标题栏（见 AGENTS.md UI 视觉硬约束）。
        .chrome_mode = huxerui::WindowChromeMode::Custom,
        .title_bar_height = 24.0F,
    };
    // 锚定在桌宠旁：优先右侧，越过工作区右沿则放左侧，纵向夹进工作区。
    if (pet) {
        constexpr float kCardWidth = 1080.0F;
        constexpr float kCardHeight = 720.0F;
        constexpr float kCardGap = 16.0F;
        const float petWidth = PetOptions(topmost.Get(), compact.Get(), Descriptor())
                                   .initial_size.width;
        const std::optional<huxerui::Point> anchor = pet->Position();
        if (anchor && pet->WorkArea()) {
            const huxerui::Rect work = *pet->WorkArea();
            float x = anchor->x + petWidth + kCardGap;
            if (x + kCardWidth > work.x + work.width) {
                x = anchor->x - kCardWidth - kCardGap;
            }
            x = std::clamp(x, work.x, work.x + std::max(0.0F, work.width - kCardWidth));
            const float y = std::clamp(anchor->y, work.y,
                                       work.y + std::max(0.0F, work.height - kCardHeight));
            options.initial_position = huxerui::Point{x, y};
        }
    }
    card = application.OpenWindow(
        [] { return PetCardContent(); }, options, {.owner = pet});
    card->Activate();
}

void PetController::Rebuild(const huxerui::ApplicationHandle& application) {
    if (!pet) return;
    const bool wasVisible =
        pet->LifecycleState() != huxerui::WindowLifecycleState::Background;
    Close();
    if (wasVisible || !saved_position) Show(application);
}

void PetController::SetCompact(bool value,
                               const huxerui::ApplicationHandle& application) {
    if (compact.Get() == value) return;
    compact = value;
    Rebuild(application);
}

void PetController::SetSkin(PetSkin value,
                            const huxerui::ApplicationHandle& application) {
    if (skin.Get() == value) return;
    skin = value;
    Rebuild(application);
}

void PetController::SetShimejiPack(
    std::string value, const huxerui::ApplicationHandle& application) {
    if (shimejiPack.Get() == value) return;
    shimejiPack = std::move(value);
    Rebuild(application);
}

void PetController::SetLive2DModel(
    std::string value, const huxerui::ApplicationHandle& application) {
    if (live2dModel.Get() == value) return;
    live2dModel = std::move(value);
    Rebuild(application);
}

void PetController::RecoverPosition() {
    if (!pet) {
        saved_position.reset();
        return;
    }
    if (const auto work = pet->WorkArea()) {
        const huxerui::Point position{work->x + std::max(0.0F, work->width - 220.0F),
                                      work->y + std::max(0.0F, work->height - 220.0F)};
        if (pet->SetPosition(position) == huxerui::WindowRequestResult::Submitted) {
            saved_position = position;
        }
    }
}

// ---- PetContent -------------------------------------------------------------

[[huxerui::composable]]
huxerui::View PetContent() {
    const auto application = huxerui::UseApplication();
    const auto window = huxerui::UseWindow();
    const auto controller = huxerui::UseService<PetController>();
    const auto nav = huxerui::UseService<MainNavTarget>();
    const auto activation = huxerui::UseService<MainWindowController>();
    const auto trayPresentation =
        huxerui::UseService<TrayPresentationController>();
    const auto tasks = huxerui::UseTaskScope();
    const auto toast = huxerui::UseToast();
    UsePetAttachment(controller);

    huxerui::Lifecycle([toast] {
        EnsureRuntimeServices(toast);
        return [] {};
    }, 0);

    // 统计轮询：桌宠窗口生命周期内每秒一次；窗口退役时挂起的 Delay 随
    // TaskScope 取消，循环自然终止（Delay 续体回到 UI 线程）。
    huxerui::Lifecycle([controller, tasks] {
        tasks.Launch([controller]() -> huxerui::Task<void> {
            for (;;) {
                co_await huxerui::Delay(std::chrono::milliseconds{1000});
                controller->Poll();
            }
        });
    });

    const bool trayAvailable = application.SystemTray().IsAvailable() &&
                               controller->trayEnabled.Get();
    // 隐藏/退役当前正在派发点击的窗口是 use-after-free：指针 Up 派发还没
    // 返回，窗口先被销毁，PointerInteraction 的会话表跟着失效（coredump
    // 实证：HandlePointerUp 内 hashtable erase 段错误）。所有会动到本窗口
    // 存活态的操作（关闭请求、菜单里的隐藏/胶囊切换）一律经应用级
    // TaskScope 推迟到派发返回之后执行。
    const huxerui::TaskScope appTasks = huxerui::UseApplicationTaskScope();
    // 托盘不可用时关闭请求不能收起桌宠（没有再打开的入口），保持显示。
    huxerui::Lifecycle([window, trayAvailable] {
        if (!trayAvailable) window.Show();
    }, trayAvailable);
    // 托盘可用性只影响托盘展示与隐藏选项；控制中心由用户从桌宠菜单或信息卡打开。
    huxerui::Lifecycle(
        [trayPresentation] {
            trayPresentation->Refresh();
            return [] {};
        },
        trayAvailable);
    window.OnCloseRequest(
        [controller, appTasks, trayAvailable] {
            // Close() 先摘下控制器句柄；此时放行框架关闭，避免再次进入本回调。
            if (!controller->pet) return false;
            // 用户关闭桌宠 = 收起（应用与托盘继续运行）；无托盘时关闭，设置页
            // 与托盘菜单可随时重建。
            appTasks.Post([controller, trayAvailable] {
                if (trayAvailable) {
                    controller->Hide(true);
                } else {
                    controller->Close();
                }
            });
            return true;
        },
        trayAvailable);

    const std::int64_t today = controller->todayRequests.Get();
    const bool donePending = controller->donePendingAck.Get();
    const bool activityUnknown = controller->activityUnknown.Get();
    const bool compact = controller->compact.Get();
    const int themeMode = controller->themeMode.Get();
    const bool systemDark =
        huxerui::UseSystemThemeBrightness() == huxerui::ThemeBrightness::Dark;
    const bool dark = themeMode == 1 || (themeMode == 0 && systemDark);
    const huxerui::ThemeSpec petSpec =
        dark ? AppDarkThemeSpec() : AppLightThemeSpec();

    // 角色皮肤优先于胶囊；所有形态共用拖拽、点击分流与右键语义。
    // TODO(pet-theme): 明暗皮肤素材接入后，在这里结合
    // controller->followDarkMode、themeMode 与系统亮度选择 body；当前所有
    // 皮肤只有一套资源，followDarkMode 仅保存偏好，不影响渲染。
    huxerui::View body;
    const PetSkin skin = controller->skin.Get();
    if (skin == PetSkin::live2d) {
        body = PetLive2DBody(controller, petSpec);
    } else if (skin == PetSkin::shimeji) {
        body = PetShimejiBody(controller, petSpec);
    } else if (compact) {
        // 黑白主题用短文字区分状态，不再依赖彩色圆点。
        std::string status = activityUnknown ? "未知" : "空闲";
        if (controller->waitingForInput.Get()) {
            status = "等待";
        } else if (controller->streaming.Get() || controller->agentWorking.Get()) {
            status = "工作";
        } else if (controller->failure.Get()) {
            status = "失败";
        } else if (donePending) {
            status = "完成";
        }
        body = huxerui::Row {
            huxerui::Text(status)
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    petSpec.colors.on_surface}),
            huxerui::Text(std::format("{} 次", today))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    petSpec.colors.on_surface}),
        }.With(huxerui::Spacing(7.0F),
               huxerui::Padding(huxerui::EdgeInsets::Symmetric(14.0F, 4.0F)),
               huxerui::Background(GlassFill(petSpec)),
               huxerui::CornerRadius(20.0F),
               huxerui::Border{GlassBorder(petSpec)},
               huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
               huxerui::Grow(1.0F));
    } else {
        body = PetLotusBody(controller, petSpec);
    }
    if (activityUnknown) {
        body = huxerui::Stack{body, UnknownActivityBadge(petSpec)}
                   .With(huxerui::Grow(1.0F));
    }

    huxerui::View content = std::move(body).With(
        huxerui::DragGesture{},
        huxerui::Semantics{
            .role = huxerui::SemanticRole::Button,
            .label = donePending
                         ? "llm-switch AI 桌宠。有任务完成：点击消解提醒。"
                         : activityUnknown
                               ? "llm-switch AI 桌宠。活动状态未知或有已启用来源尚未观察到事件。"
                               : "llm-switch AI 桌宠。点击与它互动，拖拽移动，右键打开菜单。"})
        .OnClick([controller, donePending] {
            // 完成提醒挂起时点击 = 消解标记（白龙同款）；否则与桌宠互动
            // （一次性小动画），不再打开任何窗口——信息卡经右键菜单或托盘打开。
            if (donePending) {
                controller->AckDone();
            } else {
                controller->Play();
            }
        })
        .On<huxerui::DragEvents::Started>(
            [controller, window](const huxerui::DragEvent&) {
                controller->dragging = true;
                static_cast<void>(window.BeginMove());
            })
        .On<huxerui::DragEvents::Ended>([controller](const huxerui::DragEvent&) {
            controller->dragging = false;
            controller->dropEpoch = controller->dropEpoch.Get() + 1;
        })
        .On<huxerui::DragEvents::Canceled>([controller](const huxerui::DragEvent&) {
            controller->dragging = false;
        });

    return huxerui::Theme(PetThemeDefinition(dark),
        std::move(content).On<huxerui::ViewEvents::ContextMenuRequested>(
            [controller, application, appTasks, toast](huxerui::Point position) {
                appTasks.Post([controller, application, position, toast] {
                    try {
                        controller->ShowContextMenu(application, position);
                    } catch (const std::exception& error) {
                        toast.Show(error.what());
                    }
                });
            }));
}

// ---- 信息卡 -------------------------------------------------------------------

namespace {

// 一行 Agent 今日统计：图标 + 名称 + 当前供应商 + 「N 次 · 输入/输出 tok」。
// 颜色由调用方按当前主题传入（本函数是普通函数，不能 UseTheme）。
huxerui::View CardAgentRow(const PetAgentRow& row, const std::string& providerName,
                           bool hasActivity, bool activityUnknown,
                           Color onSurface, Color onSurfaceVariant) {
    std::string meta = hasActivity
        ? std::format("{} 次 · ↑{} ↓{} tok", row.requests,
                      row.promptTokens, row.completionTokens)
        : activityUnknown ? std::string("活动状态未知")
                          : std::string("今日无路由请求");
    if (!row.activityLabel.empty()) {
        if (hasActivity) meta += " · ";
        else meta.clear();
        meta += row.activityLabel;
    }
    return huxerui::Row {
        huxerui::Image(ToolIcon(row.tool, true))
            .With(huxerui::Frame{.width = 16.0F, .height = 16.0F}),
        huxerui::Column {
            huxerui::Text(ToolName(row.tool))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kBody), onSurface}),
            huxerui::Text(std::format("{} · {}", providerName, meta))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kChip), onSurfaceVariant}),
        }.With(huxerui::Spacing(2.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start),
               huxerui::Grow(1.0F)),
    }.With(huxerui::Spacing(10.0F),
           huxerui::Padding(huxerui::EdgeInsets::Symmetric(4.0F, 8.0F)),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

huxerui::View CardRecentResultRow(const PetRecentResult& result, Color onSurface,
                                  Color onSurfaceVariant, Color error,
                                  huxerui::State<std::size_t> workspacePage,
                                  huxerui::State<std::size_t> selectedTool) {
    std::string outcome;
    Color outcomeColor = onSurface;
    switch (result.outcome) {
        case PetRecentOutcome::completed:
            outcome = "已完成";
            break;
        case PetRecentOutcome::failed:
            outcome = result.sourceLabel == "本地路由"
                ? (result.statusCode == 0 ? "连接失败"
                                          : std::format("HTTP {} 失败", result.statusCode))
                : "刚失败";
            outcomeColor = error;
            break;
        case PetRecentOutcome::cancelled:
            outcome = "已中断";
            break;
    }

    const std::string provider = result.providerName.empty()
        ? "供应商未知" : result.providerName;
    std::string meta = std::format("{} · {} · {}", FormatPetTime(result.timestampMillis),
                                   result.sourceLabel, provider);
    if (result.promptTokens >= 0 || result.completionTokens >= 0) {
        const std::string prompt = result.promptTokens >= 0
            ? std::to_string(result.promptTokens) : "—";
        const std::string completion = result.completionTokens >= 0
            ? std::to_string(result.completionTokens) : "—";
        meta += std::format(" · ↑{} ↓{} tok", prompt, completion);
    }

    const std::size_t toolIndex = ToolRegistryIndex(result.tool);
    return huxerui::Row {
        huxerui::Image(ToolIcon(result.tool, true))
            .With(huxerui::Frame{.width = 16.0F, .height = 16.0F}),
        huxerui::Column {
            huxerui::Text(std::format("{} · {}", ToolName(result.tool), outcome))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kBody), outcomeColor}),
            huxerui::Text(meta)
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kChip), onSurfaceVariant}),
        }.With(huxerui::Spacing(2.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start),
               huxerui::Grow(1.0F)),
    }.With(huxerui::Spacing(10.0F),
           huxerui::Padding(huxerui::EdgeInsets::Symmetric(4.0F, 8.0F)),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
           huxerui::PointerCursor(huxerui::PointerCursorKind::Hand),
           huxerui::Tooltip("打开对应活动页面"))
        .OnClick([workspacePage, selectedTool, toolIndex,
                  isRouter = result.sourceLabel == "本地路由"] {
            if (isRouter) {
                workspacePage = workspace_pages::kRouter;
            } else {
                selectedTool = toolIndex;
                workspacePage = workspace_pages::kAgents;
            }
        });
}

} // namespace

[[huxerui::composable]] huxerui::View PetInfoSummary(
    huxerui::State<std::size_t> workspacePage,
    huxerui::State<std::size_t> selectedTool) {
    const auto controller = huxerui::UseService<PetController>();
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const PetCardSnapshot snapshot = controller->cardSnapshot.Get();
    const bool streaming = controller->streaming.Get() ||
                           controller->agentWorking.Get();
    const bool waiting = controller->waitingForInput.Get();
    const bool activityUnknown = controller->activityUnknown.Get();
    const Color onSurface = theme.colors.on_surface;
    const Color onSurfaceVariant = theme.colors.on_surface_variant;

    std::vector<huxerui::View> rows;
    rows.reserve(snapshot.rows.size());
    for (const auto& row : snapshot.rows) {
        std::string providerName = "未设置";
        try {
            const auto& group = providerStore().group(row.tool);
            for (const auto& provider : group.providers) {
                if (provider.id == group.current) providerName = provider.name;
            }
            if (group.current.empty()) {
                const std::string_view official =
                    models::officialVendorName(row.tool);
                if (!official.empty()) providerName = std::string(official);
            }
        } catch (const std::exception&) {
            continue;
        }
        const std::size_t toolIndex = ToolRegistryIndex(row.tool);
        rows.push_back(
            CardAgentRow(row, providerName, row.requests > 0, activityUnknown,
                         onSurface, onSurfaceVariant)
                .With(huxerui::PointerCursor(huxerui::PointerCursorKind::Hand),
                      huxerui::Tooltip("在此信息卡中打开对应 Agent"))
                .OnClick([workspacePage, selectedTool, toolIndex] {
                    selectedTool = toolIndex;
                    workspacePage = workspace_pages::kAgents;
                }));
    }

    std::vector<huxerui::View> recentRows;
    recentRows.reserve(snapshot.recentResults.size());
    for (const auto& result : snapshot.recentResults) {
        recentRows.push_back(CardRecentResultRow(
            result, onSurface, onSurfaceVariant, theme.colors.error,
            workspacePage, selectedTool));
    }

    huxerui::View recentResultsContent = huxerui::Text(
        "当前没有可展示的完成或失败事件。")
        .Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kChip), onSurfaceVariant});
    if (!recentRows.empty()) {
        recentResultsContent = huxerui::Column{std::move(recentRows)}
            .With(huxerui::Spacing(4.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }

    std::vector<huxerui::View> agentRows;
    agentRows.reserve(rows.size());
    for (auto& row : rows) agentRows.push_back(std::move(row));
    huxerui::View agentContent = agentRows.empty()
        ? huxerui::View{huxerui::Text("暂无 Agent 活动数据。")
                            .Style(huxerui::TextStyle{
                                huxerui::Font::System(font_size::kChip),
                                onSurfaceVariant})}
        : huxerui::View{huxerui::Column{std::move(agentRows)}
                            .With(huxerui::Spacing(4.0F),
                                  huxerui::CrossAlign(
                                      huxerui::CrossAxisAlignment::Stretch))};

    const char* status = waiting ? "等待处理"
        : streaming ? "进行中"
        : activityUnknown ? "未知" : "已观察为空闲";
    return huxerui::Column {
        Card(huxerui::Row {
            huxerui::Column {
                huxerui::Text("今日总览", huxerui::TextRole::Label)
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kCaption),
                        onSurfaceVariant}),
                huxerui::Text(status, huxerui::TextRole::Title)
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kTitle)
                            .WithWeight(huxerui::FontWeight::Bold),
                        onSurface}),
                huxerui::Text(activityUnknown
                                  ? "路由或已启用的 Agent 来源尚未提供足够活动事件。"
                                  : "Agent 活动汇总自本地路由与已连接 hooks。")
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kCaption),
                        onSurfaceVariant}),
            }.With(huxerui::Spacing(4.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start),
                   huxerui::Grow(1.0F)),
            huxerui::Column {
                huxerui::Text(std::to_string(snapshot.todayRequests))
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kTitle)
                            .WithWeight(huxerui::FontWeight::Bold),
                        onSurface}),
                huxerui::Text("本地路由请求")
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kCaption),
                        onSurfaceVariant}),
            }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::End)),
        }.With(huxerui::Spacing(theme.spacing.large),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))),
        PageSection(
            huxerui::Text("Agent 信息卡", huxerui::TextRole::Label),
            std::move(agentContent)),
        PageSection(huxerui::Text("最近完成与失败", huxerui::TextRole::Label),
                    std::move(recentResultsContent)),
    }.With(huxerui::Spacing(theme.spacing.medium),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]]
huxerui::View PetCardContent() {
    const auto controller = huxerui::UseService<PetController>();
    const auto window = huxerui::UseWindow();
    // 主窗口打开时与其共享 revision / 主题 / 启动入口 / 托盘状态（卡内写入
    // 即时同步控制中心与托盘菜单）；主窗口未打开时回落到卡内自有状态。
    const auto nav = huxerui::UseService<MainNavTarget>();
    const huxerui::TaskScope appTasks = huxerui::UseApplicationTaskScope();
    UseCardAttachment(controller);

    // 自定义标题栏的关闭按钮是窗口内视图：点击在指针派发中同步关闭本窗口
    // 会在派发返回后访问已释放的 PointerInteraction（use-after-free，与
    // 桌宠窗口同款坑）。用户请求推迟执行；控制器 CloseCard() 已摘下句柄时放行。
    window.OnCloseRequest([controller, appTasks] {
        if (!controller->card) return false;
        appTasks.Post([controller] { controller->CloseCard(); });
        return true;
    });

    auto navPage = huxerui::UseState<std::size_t>(
        std::size_t{pages::kWorkspace});
    auto workspacePage = huxerui::UseState<std::size_t>(
        std::size_t{workspace_pages::kHome});
    auto selectedTool = huxerui::UseState<std::size_t>(0);
    auto localRevision = huxerui::UseState(0);
    const std::string startupModeValue = providerStore().config().startupMode;
    auto localStartupMode = huxerui::UseState<std::size_t>(
        startupModeValue == "control-center" ? 1U : 0U);
    auto localTrayEnabled = huxerui::UseState(providerStore().config().trayEnabled);
    auto revision = nav->RevisionState().value_or(localRevision);
    auto themeMode = controller->themeMode;
    auto startupMode = nav->StartupModeState().value_or(localStartupMode);
    auto trayEnabled = nav->TrayEnabledState().value_or(localTrayEnabled);

    const bool systemDark =
        huxerui::UseSystemThemeBrightness() == huxerui::ThemeBrightness::Dark;
    const bool dark =
        themeMode.Get() == 1 || (themeMode.Get() == 0 && systemDark);
    const huxerui::ThemeSpec spec = dark
        ? AppDarkThemeSpec() : AppLightThemeSpec();
    huxerui::View content = huxerui::Column {
        huxerui::WindowTitleBar {
            huxerui::Image(app::images::lotus_bloom)
                .Tint(spec.colors.on_surface)
                .With(huxerui::Frame{.width = 16.0F, .height = 16.0F}),
            huxerui::Text("llm-switch")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kChip)
                        .WithWeight(huxerui::FontWeight::Bold),
                    spec.colors.on_surface}),
            huxerui::Spacer(),
        }.With(huxerui::Padding(
            huxerui::EdgeInsets::Symmetric(spec.spacing.large, 0.0F))),
        PetWorkspacePage(navPage, workspacePage, revision, selectedTool,
                         themeMode, startupMode, trayEnabled)
            .With(huxerui::Grow(1.0F)),
    }.With(huxerui::Grow(1.0F),
           huxerui::Background(spec.colors.background),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
        .Key("pet-card-surface");
    return huxerui::Theme(PetThemeDefinition(dark), std::move(content));
}

} // namespace llmswitch::ui
