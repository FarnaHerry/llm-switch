// pet.cpp — AI 桌宠实现：莲花球视觉（NodeExtension 逐帧重绘）、活动状态轮询、
// 指针交互与右键菜单。视觉为 llm-switch 冷调石板族的固定深色玻璃球，品牌莲花
// （lotus_bloom，白色 alpha-mask）居中慢旋：空闲 ~11s/圈，路由请求完成后 3s
// 内进入 thinking（提速到 ~3.3s/圈 + 提亮 + 高亮弧线），最近一次请求失败 6s
// 内叠加误差红脉冲外环；底部角标显示今日请求数。
// 动画走 NodeExtension 的保留绘制（30fps 上限，reduced-motion 下静止），统计
// 轮询走 TaskScope + Delay（每秒一次，窗口退役时随作用域取消）。右键菜单经
// PetThemeDefinition 套主应用同款主题（深浅随 Poll 跟随）。
#include <huxerui/huxerui.h>

#include <chrono>
#include <cmath>
#include <format>
#include <memory>
#include <numbers>
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

namespace llmswitch::ui {
namespace {

using huxerui::Color;
using huxerui::Point;
using huxerui::Rect;

// ---- Orb 视觉 ---------------------------------------------------------------

struct Orb {
    class Extension;
    bool thinking = false;
    bool failure = false;
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
    const float intensity = orb.thinking ? 1.0F : 0.6F;

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

    // 品牌莲花居中慢旋（白色 alpha-mask 资源，衬深色玻璃球；thinking 时
    // 加速——OnFrame 的 phase 步长控制转速）。变换为中心旋转：先平移回
    // 原点旋转再放回，避免公转。
    if (orb.lotus.HasValue()) {
        const float lotus_radius = radius * 0.58F;
        const Rect lotus_bounds{center.x - lotus_radius, center.y - lotus_radius,
                                lotus_radius * 2.0F, lotus_radius * 2.0F};
        const float s = std::sin(phase);
        const float c = std::cos(phase);
        huxerui::Transform2D spin;
        spin.m11 = c;
        spin.m12 = s;
        spin.m21 = -s;
        spin.m22 = c;
        spin.translate_x = center.x - (c * center.x - s * center.y);
        spin.translate_y = center.y - (s * center.x + c * center.y);
        paint.PushTransform(spin);
        paint.DrawImage(orb.lotus, lotus_bounds);
        paint.PopTransform();
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

    // failure：误差红外环，随相位呼吸（红脉冲在莲花转速恢复后仍停留 6s）。
    if (orb.failure) {
        const float pulse = 0.38F + 0.28F * std::sin(phase * 2.4F);
        paint.DrawArc(center, radius * 1.06F, 0.0F, 6.28318F,
                      Color::Rgb(240, 115, 108, pulse),
                      huxerui::StrokeStyle{.width = 2.2F, .cap = huxerui::StrokeCap::Round});
    }
    // thinking：旋转弧线；空闲退化为固定装饰弧。
    if (orb.thinking) {
        paint.DrawArc(center, radius * 0.94F, phase * 1.9F, 1.1F,
                      Color::Rgb(224, 242, 254, 0.45F),
                      huxerui::StrokeStyle{.width = 1.4F, .cap = huxerui::StrokeCap::Round});
    } else {
        paint.DrawArc(center, radius * 0.94F, 3.80F, 0.86F,
                      Color::Rgb(224, 242, 254, 0.28F),
                      huxerui::StrokeStyle{.width = 0.8F, .cap = huxerui::StrokeCap::Round});
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
            // 转速：空闲 ~11s/圈，thinking ~3.3s/圈。
            phase_ = std::fmod(phase_ + delta * (orb_.thinking ? 1.9F : 0.55F), wave_cycle);
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

} // namespace

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

void PetController::Poll() {
    auto& router = routerInstance();
    const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    // thinking 用短窗（请求完成后波纹提亮 3s）；failure 长窗（红脉冲停留
    // 6s，比波纹更晚消失，让用户注意到失败）。
    constexpr std::int64_t kThinkingWindowMs = 3000;
    constexpr std::int64_t kFailureWindowMs = 6000;
    bool next_thinking = false;
    bool next_failure = false;
    if (router.running()) {
        const std::vector<router::RequestLog> logs = router.recentLogs(1);
        if (!logs.empty()) {
            const std::int64_t age = now - logs.front().tsMillis;
            if (age <= kThinkingWindowMs) next_thinking = true;
            if (age <= kFailureWindowMs &&
                (logs.front().status == 0 || logs.front().status >= 400)) {
                next_failure = true;
            }
        }
    }
    if (thinking.Get() != next_thinking) thinking = next_thinking;
    if (failure.Get() != next_failure) failure = next_failure;
    const std::int64_t next_today = router.snapshot().todayRequests;
    if (todayRequests.Get() != next_today) todayRequests = next_today;
    // 主题跟随（与主应用同一规则：显式深浅优先，否则系统偏好）。
    const std::string& mode = providerStore().config().themeMode;
    const bool next_dark =
        mode == "dark" || (mode != "light" && cfg::systemPrefersDark());
    if (darkTheme.Get() != next_dark) darkTheme = next_dark;
}

void PetController::Show(const huxerui::ApplicationHandle& application) {
    if (!pet) {
        auto options = PetOptions(topmost.Get());
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
    pet->Show();
    epoch = epoch.Get() + 1;
}

void PetController::Retire() {
    if (pet) saved_position = pet->Position();
    // 退役是排队操作：先失配代次，旧附件的清理就不会清掉新开的替代窗口。
    ++generation;
    if (pet) pet->Retire();
    pet.reset();
    epoch = epoch.Get() + 1;
}

void PetController::Hide(bool trayAvailable) {
    if (!trayAvailable) return;
    if (lightweight.Get()) {
        Retire();
    } else if (pet) {
        pet->Hide();
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
    const auto activation = huxerui::UseService<TrayActivationTarget>();
    const auto menu = huxerui::UseMenu();
    const auto toast = huxerui::UseToast();
    const auto tasks = huxerui::UseTaskScope();
    UsePetAttachment(controller);

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

    const bool trayAvailable = application.SystemTray().IsAvailable();
    // 托盘不可用时关闭请求不能收起桌宠（没有再打开的入口），保持显示。
    huxerui::Lifecycle([window, trayAvailable] {
        if (!trayAvailable) window.Show();
    }, trayAvailable);
    window.OnCloseRequest(
        [controller, trayAvailable] {
            // 关闭桌宠 = 收起（应用与托盘继续运行）；无托盘时退役，设置页
            // 与托盘菜单可随时重建。
            if (trayAvailable) {
                controller->Hide(true);
            } else {
                controller->Retire();
            }
            return true;
        },
        trayAvailable);

    const bool visible = window.LifecycleState() != huxerui::WindowLifecycleState::Background;
    const std::int64_t today = controller->todayRequests.Get();
    // 品牌莲花在组合期解析成光栅资源，经 Orb 修饰器带进保留绘制。
    const huxerui::ImageAsset lotus = huxerui::UseImage(app::images::lotus_bloom);

    // Orb 是保留型节点修饰器（NodeExtension，挂在 Stack 节点上整窗作画），
    // 角标是 Stack 的普通子节点（底缘居中悬浮在球体下沿）。
    std::vector<huxerui::View> children;
    if (today > 0) children.push_back(TodayBadge(today));
    huxerui::View content = huxerui::Stack{std::move(children)}.With(
        huxerui::Indication{},
        Orb{.thinking = controller->thinking.Get(),
            .failure = controller->failure.Get(),
            .animate = visible && controller->animate.Get(),
            .lotus = lotus},
        huxerui::DragGesture{},
        huxerui::Semantics{
            .role = huxerui::SemanticRole::Button,
            .label = "llm-switch AI 桌宠。点击打开主窗口；拖拽移动；右键打开菜单。"})
        .OnClick([activation] { activation->Activate(); })
        .On<huxerui::DragEvents::Started>(
            [window](const huxerui::DragEvent&) { static_cast<void>(window.BeginMove()); })
        .On<huxerui::ViewEvents::ContextMenuRequested>(
            [controller, activation, application, menu, toast,
             trayAvailable](huxerui::Point position) {
                const int routerPort = providerStore().config().routerPort;
                menu.ShowAt(position, {
                    huxerui::MenuItem(
                        std::format("本地路由（127.0.0.1:{}）", routerPort),
                        [toast] {
                            try {
                                if (routerInstance().running()) {
                                    routerInstance().stop();
                                    toast.Show("本地路由已停止");
                                } else {
                                    routerInstance().start(
                                        providerStore().config().routerPort);
                                    toast.Show(std::format(
                                        "本地路由已启动（127.0.0.1:{}）",
                                        providerStore().config().routerPort));
                                }
                            } catch (const std::exception& e) {
                                toast.Show(e.what());
                            }
                        })
                            .Checked(routerInstance().running()),
                    huxerui::MenuItem("打开主窗口",
                                      [activation] { activation->Activate(); }),
                    huxerui::MenuSection{},
                    huxerui::MenuItem(
                        "隐藏桌宠",
                        [controller, trayAvailable] {
                            controller->Hide(trayAvailable);
                        })
                            .Enabled(trayAvailable),
                    huxerui::MenuSection{},
                    huxerui::MenuItem("退出 llm-switch",
                                      [application] { application.Quit(); }),
                });
            });
    // 右键菜单与桌宠窗口主题套应用同款配色（深浅随 darkTheme 跟随）。
    return huxerui::Theme(PetThemeDefinition(controller->darkTheme.Get()),
                          std::move(content));
}

} // namespace llmswitch::ui
