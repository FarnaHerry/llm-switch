// pet.cpp — AI 桌宠实现：莲花球视觉（NodeExtension 逐帧重绘）、活动状态轮询、
// 指针交互与右键菜单。视觉为 llm-switch 冷调石板族的固定深色玻璃球，品牌莲花
// （lotus_bloom_raster，冷白 alpha-mask）居中呼吸：辉光与缩放随相位起伏，
// 空闲 ~6s 一个呼吸周期。状态由本地路由和用户连接的 Agent hooks 汇总：
//   路由在途请求或原生 Agent 工作中/等待输入时呼吸更深更快；最近一次请求失败
//   6s 内叠加误差红脉冲（failure）；一波活动批次（含成功请求）静默 ≥8s 结束
//   后右上角挂 ✓ 完成提醒（done，静态绘制，点击/右键/新批次消解，30 秒自动消退）；
//   底部角标显示今日请求数，顶部浮标显示活跃 Agent（≤2 个）。
// 动画走 NodeExtension 的保留绘制（30fps 上限，reduced-motion 下静止——✓ 与
// 浮标仍显示），统计轮询走 TaskScope + Delay（每秒一次，窗口退役时随作用域
// 取消）。右键菜单经 PetThemeDefinition 套主应用同款主题（深浅随 Poll 跟随）。
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

import llmswitch.config;
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
    // 品牌莲花（组合期经 UseImage 解析；白色 alpha-mask，衬在深色玻璃球上
    // 无需 tint）。ImageAsset 值语义共享编码存储，比较为内含标识比较。
    huxerui::ImageAsset lotus{};
    bool operator==(const Orb&) const = default;
};

// 石墨玻璃球 + 青蓝波纹。zero-alpha 像素在 Windows 上原生点击穿透
// （分层表面按预乘 alpha 命中测试），半透明球体区域保持可交互。
void PaintOrb(huxerui::PaintContext& paint, huxerui::Size size, float phase,
              const Orb& orb) {
    const Point center{size.width * 0.5F, size.height * 0.5F};
    const float radius = std::min(size.width, size.height) * 0.37F;
    if (radius <= 0.0F) return;
    const Rect bounds{center.x - radius, center.y - radius, radius * 2.0F, radius * 2.0F};
    const huxerui::Path circle = huxerui::Path::RoundedRect(bounds, huxerui::CornerRadii{radius});
    // 辉光随呼吸起伏；路由流式请求或原生 hook 工作时基准更亮。
    const float intensity =
        (orb.active ? 1.0F : 0.6F) * (1.0F + 0.12F * std::sin(phase));

    // 球外环境辉光：透明窗口上形成一圈淡青晕。
    paint.DrawRect({center.x - radius * 1.24F, center.y - radius * 1.24F,
                    radius * 2.48F, radius * 2.48F},
                   huxerui::RadialGradient{.stops = {
                       {0.0F, Color::Rgb(56, 189, 248, 0.14F * intensity)},
                       {0.70F, Color::Rgb(56, 189, 248, 0.07F * intensity)},
                       {1.0F, Color::Transparent()},
                   }});
    // 球体：石墨玻璃（深色主题表面同族）。
    paint.FillPath(circle, huxerui::RadialGradient{
        .center = {0.30F, 0.16F},
        .radius = {0.95F, 0.95F},
        .stops = {
            {0.0F, Color::Rgb(45, 53, 61, 0.98F)},
            {0.48F, Color::Rgb(26, 30, 34, 0.98F)},
            {1.0F, Color::Rgb(13, 15, 18, 0.96F)},
        },
    }, bounds);

    paint.PushPathClip(circle);
    // 内部青蓝氤氲。
    paint.DrawRect(bounds, huxerui::RadialGradient{
        .center = {0.48F, 0.57F},
        .radius = {0.65F, 0.43F},
        .stops = {
            {0.0F, Color::Rgb(56, 189, 248, 0.30F * intensity)},
            {0.65F, Color::Rgb(14, 165, 233, 0.10F * intensity)},
            {1.0F, Color::Transparent()},
        },
    });

    // 品牌莲花居中呼吸（白色 alpha-mask 栅格，衬深色玻璃球）：缩放随相位
    // 起伏，活动时呼吸更深更快——OnFrame 的 phase 步长控制呼吸周期。
    if (orb.lotus.HasValue()) {
        const float breathe = 1.0F + (orb.active ? 0.060F : 0.035F) * std::sin(phase);
        const float lotus_radius = radius * 0.58F * breathe;
        const Rect lotus_bounds{center.x - lotus_radius, center.y - lotus_radius,
                                lotus_radius * 2.0F, lotus_radius * 2.0F};
        paint.DrawImage(orb.lotus, lotus_bounds);
    }

    // 顶部冷白高光。
    paint.DrawRect(bounds, huxerui::RadialGradient{
        .center = {0.32F, 0.06F}, .radius = {0.68F, 0.48F},
        .stops = {{0.0F, Color::Rgb(232, 234, 237, 0.16F)}, {1.0F, Color::Transparent()}},
    });
    paint.PopClip();

    // 玻璃边缘描边。
    paint.StrokePath(circle, huxerui::LinearGradient{
        .start = {0.15F, 0.0F}, .end = {0.85F, 1.0F},
        .stops = {
            {0.0F, Color::Rgb(224, 242, 254, 0.50F)},
            {0.35F, Color::Rgb(56, 189, 248, 0.22F)},
            {0.72F, Color::Rgb(45, 51, 58, 0.16F)},
            {1.0F, Color::Rgb(125, 211, 252, 0.32F)},
        },
    }, bounds, huxerui::StrokeStyle{.width = 0.8F});

    // failure：误差红外环，随相位脉冲（红脉冲在莲花呼吸恢复后仍停留 6s）。
    if (orb.failure) {
        const float pulse = 0.38F + 0.28F * std::sin(phase * 2.4F);
        paint.DrawArc(center, radius * 1.06F, 0.0F, 6.28318F,
                      Color::Rgb(240, 115, 108, pulse),
                      huxerui::StrokeStyle{.width = 2.2F, .cap = huxerui::StrokeCap::Round});
    }
    // 活动时装饰弧随呼吸提亮；等待确认用暖色弧标记。
    if (orb.waiting) {
        paint.DrawArc(center, radius * 0.94F, 3.80F, 0.86F,
                      Color::Rgb(251, 191, 36, 0.74F),
                      huxerui::StrokeStyle{.width = 1.8F, .cap = huxerui::StrokeCap::Round});
    } else if (orb.active) {
        const float breathe = 0.34F + 0.22F * (0.5F + 0.5F * std::sin(phase));
        paint.DrawArc(center, radius * 0.94F, 3.80F, 0.86F,
                      Color::Rgb(224, 242, 254, breathe),
                      huxerui::StrokeStyle{.width = 1.4F, .cap = huxerui::StrokeCap::Round});
    } else {
        paint.DrawArc(center, radius * 0.94F, 3.80F, 0.86F,
                      Color::Rgb(224, 242, 254, 0.28F),
                      huxerui::StrokeStyle{.width = 0.8F, .cap = huxerui::StrokeCap::Round});
    }

    // 完成提醒：右上角 ✓ 徽标（静态绘制，不随相位动——reduced-motion 下
    // 同样显示），点击桌宠或右键消解。冷白底 + 石墨对勾，与球体形成正负形。
    if (orb.done) {
        const Point badgeCenter{center.x + radius * 0.72F, center.y - radius * 0.72F};
        const float badgeRadius = radius * 0.30F;
        paint.DrawCircle(badgeCenter, badgeRadius, Color::Rgb(224, 242, 254, 0.96F));
        const huxerui::StrokeStyle check{.width = badgeRadius * 0.28F,
                                .cap = huxerui::StrokeCap::Round,
                                .join = huxerui::StrokeJoin::Round};
        const float k = badgeRadius * 0.45F;
        paint.DrawLine({badgeCenter.x - k, badgeCenter.y},
                       {badgeCenter.x - k * 0.25F, badgeCenter.y + k * 0.7F},
                       Color::Rgb(26, 30, 34), check);
        paint.DrawLine({badgeCenter.x - k * 0.25F, badgeCenter.y + k * 0.7F},
                       {badgeCenter.x + k, badgeCenter.y - k * 0.55F},
                       Color::Rgb(26, 30, 34), check);
    }
}

class Orb::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode& node, const Orb& orb) { Update(node, orb); }

    void Update(huxerui::ViewNode&, const Orb& orb) {
        if (orb_ != orb) {
            orb_ = orb;
            InvalidatePaint();
        }
    }

    NodeExtension::FrameResult OnFrame(huxerui::ViewNode&, const huxerui::FrameInfo& frame) override {
        if (frame.reduced_motion || !orb_.animate) return {};
        const float delta = static_cast<float>(std::clamp(frame.delta_time, 0.0, 0.1));
        if (delta > 0.0F) {
            constexpr float wave_cycle = 20.0F * std::numbers::pi_v<float>;
            // 呼吸周期：空闲 ~6s，活动中 ~2.5s。
            phase_ = std::fmod(phase_ + delta * (orb_.active ? 2.5F : 1.0F), wave_cycle);
            InvalidatePaint();
        }
        return {.wake_after = 1.0 / 30.0};
    }

    void PaintAboveContent(const huxerui::ViewNode& node, huxerui::PaintContext& paint) const override {
        PaintOrb(paint, node.LayoutSize(), phase_, orb_);
    }

private:
    Orb orb_;
    float phase_ = 0.0F;
};

// 角标玻璃药丸：固定深色玻璃（不随应用主题），冷白文字。
huxerui::View TodayBadge(std::int64_t today) {
    return huxerui::Text(std::format("{} 次", today))
        .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
                                  Color::Rgb(224, 242, 254)})
        .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 3.0F)),
              huxerui::Background(Color::Rgb(16, 18, 20, 0.72F)),
              huxerui::CornerRadius(9.0F))
        .With(huxerui::Align(huxerui::HorizontalAlignment::Center,
                             huxerui::VerticalAlignment::End),
              huxerui::Padding(huxerui::EdgeInsets{.bottom = 6.0F}));
}

// 活跃 Agent 浮标：顶缘居中的一枚玻璃药丸，内嵌最多 2 个 Agent 图标
//（ToolIcon 的白色 alpha-mask 经 Tint 映冷白）。白龙「双会话」同款语义。
huxerui::View ActiveAgentBadge(const std::vector<std::string>& tools) {
    std::vector<huxerui::View> icons;
    for (const std::string& tool : tools) {
        icons.push_back(huxerui::Image(ToolIcon(tool))
                            .Tint(Color::Rgb(224, 242, 254))
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
              huxerui::Background(Color::Rgb(16, 18, 20, 0.72F)),
              huxerui::CornerRadius(9.0F),
              huxerui::Semantics{.label = std::format("活跃 Agent：{}", label)})
        .With(huxerui::Align(huxerui::HorizontalAlignment::Center,
                             huxerui::VerticalAlignment::Start),
              huxerui::Padding(huxerui::EdgeInsets{.top = 6.0F}));
}

huxerui::View UnknownActivityBadge() {
    return huxerui::Row{huxerui::Text("?")
                            .Style(huxerui::TextStyle{
                                huxerui::Font::System(font_size::kCaption)
                                    .WithWeight(huxerui::FontWeight::Bold),
                                Color::Rgb(224, 242, 254)})}
        .With(huxerui::Frame{.width = 20.0F, .height = 20.0F},
              huxerui::Background(Color::Rgb(71, 85, 105, 0.92F)),
              huxerui::CornerRadius(10.0F),
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
    const std::shared_ptr<PetController>& controller) {
    const auto window = huxerui::UseWindow();
    const huxerui::ImageAsset lotus =
        huxerui::UseImage(app::images::lotus_bloom_raster);
    const std::vector<std::string> activeTools = controller->activeTools.Get();
    const std::int64_t today = controller->todayRequests.Get();
    const bool visible =
        window.LifecycleState() != huxerui::WindowLifecycleState::Background;
    std::vector<huxerui::View> children;
    if (!activeTools.empty()) children.push_back(ActiveAgentBadge(activeTools));
    if (today > 0) children.push_back(TodayBadge(today));
    return huxerui::Stack{children}.With(
        huxerui::Indication{},
        Orb{.active = controller->streaming.Get() ||
                      controller->agentWorking.Get(),
            .waiting = controller->waitingForInput.Get(),
            .failure = controller->failure.Get(),
            .done = controller->donePendingAck.Get(),
            .animate = visible && controller->animate.Get(),
            .lotus = lotus},
        huxerui::Grow(1.0F));
}

[[huxerui::composable]]
huxerui::View PetLive2DFallback(
    const std::shared_ptr<PetController>& controller, std::string status) {
    return huxerui::Stack {
        PetLotusBody(controller),
        huxerui::Text(status)
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                Color::Rgb(224, 242, 254)})
            .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 4.0F)),
                  huxerui::Background(Color::Rgb(16, 18, 20, 0.86F)),
                  huxerui::CornerRadius(9.0F),
                  huxerui::Align(huxerui::HorizontalAlignment::Center,
                                 huxerui::VerticalAlignment::End),
                  huxerui::Padding(huxerui::EdgeInsets{.bottom = 8.0F})),
    }.With(huxerui::Grow(1.0F));
}

// ---- 主题（右键菜单与主应用同款）--------------------------------------------

// 桌宠窗口唯一的主题化 UI 是右键菜单：套与主应用 AppThemed 同源的 spec +
// 同一款菜单覆盖（同表面、8px 圆角、同阴影同悬浮态），弹出的菜单与主窗口
// 右键菜单观感一致。深浅由 PetController::darkTheme（Poll 每秒跟随）驱动。
huxerui::ThemeDefinition PetThemeDefinition(bool dark) {
    const huxerui::ThemeSpec spec = dark ? AppDarkThemeSpec() : AppLightThemeSpec();
    huxerui::ThemeDefinition definition = huxerui::MaterialThemeDefinition(spec);
    const auto withAlpha = [](Color color, float alpha) {
        color.alpha = alpha;
        return color;
    };
    huxerui::MenuStyle menus = huxerui::MenuStyle::Default();
    menus.background = spec.colors.surface_container;
    menus.foreground = spec.colors.on_surface;
    menus.icon_tint = spec.colors.on_surface_variant;
    menus.separator_color = spec.colors.outline;
    menus.shadow = huxerui::Shadow{Color::Rgb(0, 0, 0, 0.24F), {}, 8.0F, 0.0F};
    menus.corner_radii = huxerui::CornerRadii{spec.shapes.small};
    menus.item_indication = huxerui::Indication{
        .hover = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_surface, 0.08F)},
        .press = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_surface, 0.12F)},
    };
    definition.Set(menus);
    return definition;
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

    // 主题跟随（与主应用同一规则：显式深浅优先，否则系统偏好）。
    const std::string& mode = providerStore().config().themeMode;
    const bool next_dark =
        mode == "dark" || (mode != "light" && cfg::systemPrefersDark());
    if (darkTheme.Get() != next_dark) darkTheme = next_dark;
}

void PetController::Show(const huxerui::ApplicationHandle& application) {
    if (!pet) {
        auto options = PetOptions(topmost.Get(), compact.Get(),
                                  live2d.Get() && PetLive2DAvailable());
        options.initial_position = saved_position;
        try {
            pet = application.OpenWindow(options);
        } catch (const std::logic_error&) {
            // Wayland 拒绝被动显示与 keep-above：退回合成器默认策略重开。
            options.show_without_activation = false;
            options.topmost = false;
            pet = application.OpenWindow(options);
            topmost = false;
        }
    }
    if (!pet->Capabilities().transparent_surface) {
        Retire();
        throw std::runtime_error("当前平台不支持透明桌宠窗口");
    }
    pet->Show();
    epoch = epoch.Get() + 1;
    if (refreshTray) refreshTray();
}

void PetController::Retire() {
    if (pet) saved_position = pet->Position();
    // 退役是排队操作：先失配代次，旧附件的清理就不会清掉新开的替代窗口。
    ++generation;
    // 信息卡 owner 是桌宠，桌宠退役时框架会连带退役它；这里同步失配卡片
    // 代次并清句柄，ToggleCard 的「开着的卡」判定立即恢复为关。
    ++cardGeneration;
    if (card) card->Retire();
    card.reset();
    if (pet) pet->Retire();
    pet.reset();
    epoch = epoch.Get() + 1;
    if (refreshTray) refreshTray();
}

void PetController::Hide(bool trayAvailable) {
    if (!trayAvailable) return;
    if (lightweight.Get()) {
        Retire();
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
        ++cardGeneration;
        card->Retire();
        card.reset();
        return;
    }
    huxerui::WindowOptions options{
        .title = "llm-switch 信息卡",
        .key = std::string(kPetCardWindowKey),
        .initial_size = {330.0F, 440.0F},
        .minimum_size = huxerui::Size{280.0F, 320.0F},
        // 与主应用同款自定义标题栏（24pt，莲花 + 应用名），不出现平台
        // 原生标题栏（见 AGENTS.md UI 视觉硬约束）。
        .chrome_mode = huxerui::WindowChromeMode::Custom,
        .title_bar_height = 24.0F,
    };
    // 锚定在桌宠旁：优先右侧，越过工作区右沿则放左侧，纵向夹进工作区。
    if (pet) {
        constexpr float kCardWidth = 330.0F;
        constexpr float kCardGap = 16.0F;
        const float petWidth = live2d.Get() && PetLive2DAvailable()
                                   ? 300.0F
                                   : (compact.Get() ? 150.0F : 200.0F);
        const std::optional<huxerui::Point> anchor = pet->Position();
        if (anchor && pet->WorkArea()) {
            const huxerui::Rect work = *pet->WorkArea();
            float x = anchor->x + petWidth + kCardGap;
            if (x + kCardWidth > work.x + work.width) {
                x = anchor->x - kCardWidth - kCardGap;
            }
            x = std::clamp(x, work.x, work.x + std::max(0.0F, work.width - kCardWidth));
            const float y = std::clamp(anchor->y, work.y,
                                       work.y + std::max(0.0F, work.height - 200.0F));
            options.initial_position = huxerui::Point{x, y};
        }
    }
    card = application.OpenWindow(options, {.owner = pet});
    card->Activate();
}

void PetController::SetCompact(bool value,
                               const huxerui::ApplicationHandle& application) {
    if (compact.Get() == value) return;
    compact = value;
    if (!pet) return;
    // 同一窗口不能改尺寸约定：退役后按新尺寸重建，位置保留；信息卡随
    // owner 退役，用户需要时再点开（不试图把旧卡搬到新锚点旁）。
    const bool wasVisible =
        pet->LifecycleState() != huxerui::WindowLifecycleState::Background;
    Retire();
    if (wasVisible || !saved_position) Show(application);
}

void PetController::SetLive2D(bool value,
                              const huxerui::ApplicationHandle& application) {
    if (live2d.Get() == value) return;
    live2d = value;
    if (!pet) return;
    // 与胶囊切换同一套重建语义（live2d 优先于 compact 决定窗口尺寸）。
    const bool wasVisible =
        pet->LifecycleState() != huxerui::WindowLifecycleState::Background;
    Retire();
    if (wasVisible || !saved_position) Show(application);
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
            // 关闭桌宠 = 收起（应用与托盘继续运行）；无托盘时退役，设置页
            // 与托盘菜单可随时重建。
            appTasks.Post([controller, trayAvailable] {
                if (trayAvailable) {
                    controller->Hide(true);
                } else {
                    controller->Retire();
                }
            });
            return true;
        },
        trayAvailable);

    const std::int64_t today = controller->todayRequests.Get();
    const bool donePending = controller->donePendingAck.Get();
    const bool activityUnknown = controller->activityUnknown.Get();
    const bool compact = controller->compact.Get();

    // 主体形态（互斥，live2d 优先）：Live2D 皮肤 / 胶囊 / 莲花球。三种形态
    // 共用同一组交互（拖拽 / 点击分流 / 右键 / 语义）。
    huxerui::View body;
    if (controller->live2d.Get() && PetLive2DAvailable()) {
        body = PetLive2DBody(controller);
    } else if (compact) {
        // 状态点：等待输入暖黄 / 工作青蓝 / failure 误差红 / done 冷白。
        Color dot = Color::Rgb(154, 161, 170, 0.9F);
        if (controller->waitingForInput.Get()) {
            dot = Color::Rgb(251, 191, 36);
        } else if (controller->streaming.Get() || controller->agentWorking.Get()) {
            dot = Color::Rgb(56, 189, 248);
        } else if (controller->failure.Get()) {
            dot = Color::Rgb(240, 115, 108);
        } else if (donePending) {
            dot = Color::Rgb(224, 242, 254, 0.95F);
        }
        body = huxerui::Row {
            huxerui::Row{}.With(huxerui::Frame{.width = 10.0F, .height = 10.0F},
                                huxerui::Background(dot),
                                huxerui::CornerRadius(5.0F)),
            huxerui::Text(std::format("{} 次", today))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    Color::Rgb(224, 242, 254)}),
        }.With(huxerui::Spacing(7.0F),
               huxerui::Padding(huxerui::EdgeInsets::Symmetric(14.0F, 4.0F)),
               huxerui::Background(Color::Rgb(16, 18, 20, 0.86F)),
               huxerui::CornerRadius(20.0F),
               huxerui::Border{Color::Rgb(224, 242, 254, 0.14F)},
               huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
               huxerui::Grow(1.0F));
    } else {
        body = PetLotusBody(controller);
    }
    if (activityUnknown) {
        body = huxerui::Stack{body, UnknownActivityBadge()}
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
                               : "llm-switch AI 桌宠。状态汇总本地路由与已连接的 Agent hook；点击展开信息卡，拖拽移动。"})
        .OnClick([controller, application, donePending] {
            // 完成提醒挂起时点击 = 消解标记（白龙同款）；否则切换信息卡。
            if (donePending) {
                controller->AckDone();
            } else {
                controller->ToggleCard(application);
            }
        })
        .On<huxerui::DragEvents::Started>(
            [window](const huxerui::DragEvent&) { static_cast<void>(window.BeginMove()); });

    // MenuHandle 必须在 Theme 的子作用域内获取：Theme 包在本函数的返回值
    // 上，函数体顶层 UseMenu() 捕获的是默认环境（右键菜单落回框架默认浅色
    // 样式）——与 app.cpp 的 DialogHandle 同款坑（AGENTS.md UI 视觉硬约束）。
    // Scope 的工厂在组合期执行，Use* 合法（app.cpp windowBehavior 同款）。
    return huxerui::Theme(
        PetThemeDefinition(controller->darkTheme.Get()),
        huxerui::Scope([controller, activation, application, appTasks, trayAvailable,
                        content = std::move(content)]() mutable -> huxerui::View {
            const auto menu = huxerui::UseMenu();
            return std::move(content).On<huxerui::ViewEvents::ContextMenuRequested>(
                [controller, activation, application, menu, appTasks,
                 trayAvailable](huxerui::Point position) {
                    // 框架的 Menu 是**窗口内浮层**（LayerAnchorMode::
                    // FixedWindowPoint），会被窗口边界裁剪，宽度也受视口
                    // （= 本窗口 200pt）夹取：胶囊（44pt 高）装不下任何菜单，
                    // 右键直接展开信息卡（真窗口，有空间）；球形态只放 5 项
                    // 无分隔短标签菜单（高度 ≈188pt、自然宽度 ≤180pt 最小值
                    // 都装得下）。「退出 llm-switch」这类长标签会被截断，路由
                    // 开关在托盘菜单/设置页，完成提醒点击即消解，都不进这里。
                    if (controller->compact.Get()) {
                        controller->ToggleCard(application);
                        return;
                    }
                    menu.ShowAt(position, {
                        huxerui::MenuItem(
                            "信息卡",
                            [controller, application] {
                                controller->ToggleCard(application);
                            })
                            .Checked(controller->card.has_value()),
                        huxerui::MenuItem(
                            "胶囊模式",
                            [controller, application, appTasks] {
                                const bool next = !controller->compact.Get();
                                // 切换会退役本窗口（点击正在派发中），推迟执行。
                                appTasks.Post([controller, application, next] {
                                    try {
                                        providerStore().setPetCompact(next);
                                        controller->SetCompact(next, application);
                                    } catch (const std::exception&) {
                                    }
                                });
                            })
                            .Checked(controller->compact.Get()),
                        huxerui::MenuItem("打开主窗口",
                                          [activation] { activation->Activate(); }),
                        huxerui::MenuItem(
                            "隐藏桌宠",
                            [controller, appTasks, trayAvailable] {
                                appTasks.Post([controller, trayAvailable] {
                                    controller->Hide(trayAvailable);
                                });
                            })
                            .Enabled(trayAvailable),
                        huxerui::MenuItem("退出",
                                          [application] { application.Quit(); }),
                    });
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
        huxerui::Image(ToolIcon(row.tool))
            .Tint(hasActivity || row.activityLabel.find("工作中") != std::string::npos ||
                      row.activityLabel.find("等待处理") != std::string::npos
                      ? onSurface : onSurfaceVariant)
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
                                  std::shared_ptr<MainNavTarget> nav,
                                  std::shared_ptr<MainWindowController> activation) {
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
        huxerui::Image(ToolIcon(result.tool))
            .Tint(result.outcome == PetRecentOutcome::failed ? error : onSurfaceVariant)
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
        .OnClick([nav, activation, toolIndex, isRouter = result.sourceLabel == "本地路由"] {
            if (isRouter) nav->RequestPage(pages::kRouter);
            else nav->RequestWorkspaceAgentPage(toolIndex);
            activation->Activate();
        });
}

} // namespace

[[huxerui::composable]]
huxerui::View PetCardContent() {
    const auto controller = huxerui::UseService<PetController>();
    const auto activation = huxerui::UseService<MainWindowController>();
    const auto nav = huxerui::UseService<MainNavTarget>();
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    const auto window = huxerui::UseWindow();
    const huxerui::TaskScope appTasks = huxerui::UseApplicationTaskScope();
    UseCardAttachment(controller);

    // 自定义标题栏的关闭按钮是窗口内视图：点击在指针派发中同步退役本窗口
    // 会在派发返回后访问已释放的 PointerInteraction（use-after-free，与
    // 桌宠窗口同款坑）。拦截关闭请求并推迟退役。
    window.OnCloseRequest([window, appTasks] {
        appTasks.Post([window] { window.Retire(); });
        return true;
    });

    const bool dark = controller->darkTheme.Get();
    const huxerui::ThemeSpec spec = dark ? AppDarkThemeSpec() : AppLightThemeSpec();
    const PetCardSnapshot snapshot = controller->cardSnapshot.Get();
    const bool streaming = controller->streaming.Get() || controller->agentWorking.Get();
    const bool waiting = controller->waitingForInput.Get();
    const bool activityUnknown = controller->activityUnknown.Get();
    const Color onSurface = spec.colors.on_surface;
    const Color onSurfaceVariant = spec.colors.on_surface_variant;

    std::vector<huxerui::View> rows;
    for (const auto& row : snapshot.rows) {
        std::string providerName = "未设置";
        try {
            const auto& group = providerStore().group(row.tool);
            for (const auto& p : group.providers) {
                if (p.id == group.current) providerName = p.name;
            }
            if (group.current.empty()) {
                const std::string_view official =
                    models::officialVendorName(row.tool);
                if (!official.empty()) providerName = std::string(official);
            }
        } catch (const std::exception&) {
            continue;  // 工具不在注册表（不应发生）：跳过该行
        }
        const bool hasActivity = row.requests > 0;
        const std::size_t toolIndex = ToolRegistryIndex(row.tool);
        rows.push_back(CardAgentRow(row, providerName, hasActivity,
                                    activityUnknown, onSurface, onSurfaceVariant)
                           .OnClick([nav, activation, toolIndex] {
                               nav->RequestWorkspaceAgentPage(toolIndex);
                               activation->Activate();
                           }));
    }
    std::vector<huxerui::View> recentRows;
    recentRows.reserve(snapshot.recentResults.size());
    for (const auto& result : snapshot.recentResults) {
        recentRows.push_back(CardRecentResultRow(
            result, onSurface, onSurfaceVariant, spec.colors.error, nav, activation));
    }
    huxerui::View recentResultsContent = huxerui::Text(
        "当前没有可展示的完成或失败事件。")
        .Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kChip), onSurfaceVariant});
    if (!snapshot.recentResults.empty()) {
        recentResultsContent = huxerui::Column{std::move(recentRows)}
            .With(huxerui::Spacing(4.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }

    // 自定义标题栏与主应用同款：莲花 + 应用名 + 拖拽区/关闭按钮（框架按
    // 平台解析的 24pt 高度渲染 caption 控件）；左右边距与正文同为 24，
    // 应用名与页面标题共享同一条边线（AGENTS.md UI 视觉硬约束）。
    huxerui::View content = huxerui::Column {
        huxerui::WindowTitleBar {
            huxerui::Image(app::images::lotus_bloom)
                .Tint(onSurface)
                .With(huxerui::Frame{.width = 16.0F, .height = 16.0F}),
            huxerui::Text("llm-switch")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kChip)
                        .WithWeight(huxerui::FontWeight::Bold),
                    onSurface}),
            huxerui::Spacer(),
        }.With(huxerui::Padding(
            huxerui::EdgeInsets::Symmetric(spec.spacing.large, 0.0F))),
        huxerui::ScrollView {
            huxerui::Column {
                huxerui::Row {
                    huxerui::Text("今日总览")
                        .Style(huxerui::TextStyle{
                            huxerui::Font::System(font_size::kTitle)
                                .WithWeight(huxerui::FontWeight::Bold),
                            onSurface}),
                    huxerui::Spacer(),
                    huxerui::Text(waiting ? "等待处理"
                                      : streaming ? "进行中"
                                      : activityUnknown ? "未知" : "已观察为空闲")
                        .Style(huxerui::TextStyle{
                            huxerui::Font::System(font_size::kChip), onSurfaceVariant}),
                }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
                huxerui::Text(activityUnknown
                                  ? "路由或已启用的 Agent 来源尚未提供足够活动事件。"
                                  : "Agent 活动合并路由与已连接 hooks；请求数和 tokens 仅统计本地路由。")
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kChip), onSurfaceVariant}),
                huxerui::Text(std::format("本地路由今日转发 {} 次请求",
                                          snapshot.todayRequests))
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kChip), onSurfaceVariant}),
                huxerui::Column{std::move(rows)}
                    .With(huxerui::Spacing(4.0F),
                          huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
                huxerui::Text("最近结果")
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kBody)
                            .WithWeight(huxerui::FontWeight::Bold),
                        onSurface}),
                std::move(recentResultsContent),
                huxerui::Column {
                    huxerui::Row {
                        huxerui::Button("工作台")
                            .With(huxerui::Frame{.min_width = 60.0F})
                            .OnClick([nav, activation] {
                                nav->RequestPage(pages::kWorkspace);
                                activation->Activate();
                            }),
                        huxerui::Button("Agent 管理")
                            .With(huxerui::Frame{.min_width = 60.0F})
                            .OnClick([nav, activation] {
                                nav->RequestWorkspaceAgentPage(0);
                                activation->Activate();
                            }),
                        huxerui::Button("使用统计")
                            .With(huxerui::Frame{.min_width = 60.0F})
                            .OnClick([nav, activation] {
                                nav->RequestWorkspacePage(
                                    workspace_pages::kStats);
                                activation->Activate();
                            }),
                    }.With(huxerui::Spacing(8.0F)),
                    huxerui::Row {
                        huxerui::Button("会话历史")
                            .With(huxerui::Frame{.min_width = 60.0F})
                            .OnClick([nav, activation] {
                                nav->RequestWorkspacePage(
                                    workspace_pages::kSessions);
                                activation->Activate();
                            }),
                        huxerui::Button("桌宠设置")
                            .With(huxerui::Frame{.min_width = 60.0F})
                            .OnClick([nav, activation] {
                                nav->RequestPage(pages::kSettings);
                                activation->Activate();
                            }),
                        huxerui::Spacer(),
                    }.With(huxerui::Spacing(8.0F)),
                    huxerui::Row {
                        // 胶囊/球互切：胶囊右键没有菜单空间，这个按钮是胶囊态
                        // 回到球形态的主入口（切换会退役桌宠与这张卡——包括
                        // 正被点击的本窗口，推迟执行）。
                        huxerui::Button(controller->compact.Get() ? "球形态"
                                                                  : "胶囊模式")
                            .With(huxerui::Frame{.min_width = 60.0F})
                            .OnClick([controller, application, appTasks] {
                                const bool next = !controller->compact.Get();
                                appTasks.Post([controller, application, next] {
                                    try {
                                        providerStore().setPetCompact(next);
                                        controller->SetCompact(next, application);
                                    } catch (const std::exception&) {
                                    }
                                });
                            }),
                        // Live2D 皮肤互切（构建可用时才显示；切换同样退役本卡）。
                        PetLive2DAvailable()
                            ? huxerui::Button(controller->live2d.Get() ? "莲花球"
                                                                       : "Live2D")
                                  .With(huxerui::Frame{.min_width = 60.0F})
                                  .OnClick([controller, application, appTasks] {
                                      const bool next = !controller->live2d.Get();
                                      appTasks.Post([controller, application, next] {
                                          try {
                                              providerStore().setPetLive2D(next);
                                              controller->SetLive2D(next, application);
                                          } catch (const std::exception&) {
                                          }
                                      });
                                  })
                            : huxerui::View{},
                        huxerui::Spacer(),
                    }.With(huxerui::Spacing(8.0F)),
                }.With(huxerui::Spacing(8.0F)),
            }.With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(24.0F, 16.0F)),
                   huxerui::Spacing(14.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        }.With(huxerui::Grow(1.0F)),
    }.With(huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    return huxerui::Theme(PetThemeDefinition(dark), std::move(content));
}

} // namespace llmswitch::ui
