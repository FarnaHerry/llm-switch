// pet.h — AI 桌宠（透明工具窗口小圆球）。
//
// 框架 501e8ca 起支持运行时多窗口与透明 Utility 窗口（examples/desktop_pet
// 是同一套能力的官方演示）；llm-switch 在其上做一个 AI 语义的桌宠：
//   * 视觉：石墨玻璃球 + 青蓝波纹（主应用深色主题同族色，固定深色玻璃——
//     桌面饰品在深浅桌面下都可读，不随应用主题切换）；
//   * 状态：每秒轮询本地路由统计——最近有请求完成时波纹提亮加速（thinking），
//     最近一次请求失败时外圈红脉冲（failure），底部角标显示今日请求数；
//   * 交互：点击打开主窗口，拖拽移动，右键菜单（路由开关 / 隐藏 / 退出）。
// 控制器由 InstallApplication 注册为应用级服务，设置页与托盘共用同一实例。
#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include <huxerui/huxerui.h>

namespace llmswitch::ui {

// 桌宠窗口的 WindowOptions::key；AppRoot 按 key 把组合分发到 PetContent。
inline constexpr std::string_view kPetWindowKey = "pet";

// 桌宠窗口参数：透明、无边框 Utility，创建时请求置顶与被动显示
// （Wayland 拒绝时由 Show 回退到合成器默认策略）。
inline huxerui::WindowOptions PetOptions(bool topmost) {
    return {
        .title = "llm-switch 桌宠",
        .key = std::string(kPetWindowKey),
        .initial_size = {200.0F, 200.0F},
        .presentation = huxerui::WindowPresentation::Utility,
        .transparent_surface = true,
        .show_without_activation = true,
        .topmost = topmost,
    };
}

// 桌宠控制器：持有桌宠窗口句柄与 AI 活动状态。线程约定同框架示例——
// 句柄管理与状态读写都发生在 UI 线程（组合 / 事件 / Task 续体），不加锁。
struct PetController {
    std::optional<huxerui::WindowHandle> pet;
    std::optional<huxerui::Point> saved_position;
    std::uint64_t generation = 0;
    huxerui::State<bool> topmost{true};
    huxerui::State<bool> animate{true};
    // 轻量级模式（对齐 Clash-Flux 的轻量级托盘）：隐藏时退役窗口、释放
    // UI 树与渲染资源（应用状态/托盘/进程继续运行），唤出时按进程内保留的
    // 位置与偏好重建，波纹相位从头开始。
    huxerui::State<bool> lightweight{false};
    // AI 活动状态（Poll 写、组合读；State 变更驱动桌宠重组）：
    huxerui::State<bool> thinking{false};      // 最近有路由请求完成
    huxerui::State<bool> failure{false};       // 最近一次路由请求失败
    huxerui::State<std::int64_t> todayRequests{0};  // 今日路由请求数（角标）
    // 窗口存活性代次：Show/Retire 自增，托盘菜单以它为依赖跟随重建勾选态
    // （轻量级模式下隐藏即退役，托盘勾选必须随之消失）。
    huxerui::State<std::uint64_t> epoch{0};

    // 每秒由 PetContent 的 Delay 循环调用：读本地路由统计并刷新状态。
    void Poll();

    void Show(const huxerui::ApplicationHandle& application);
    void Retire();
    void Hide(bool trayAvailable);
    void SetTopmost(bool value);
    // 轻量级开关：对已经藏起的桌宠启用时立即退役（释放资源）；托盘不可用
    // 时无唤出入口，拒绝开启。
    void SetLightweight(bool value, bool trayAvailable);
    void RecoverPosition();
};

// 桌宠窗口内容组合与窗口句柄绑定（生命周期挂载 / 卸载）。
[[huxerui::composable]] huxerui::View PetContent();

inline void UsePetAttachment(const std::shared_ptr<PetController>& controller) {
    const auto window = huxerui::UseWindow();
    huxerui::Lifecycle([controller, window] {
        const auto current = ++controller->generation;
        controller->pet = window;
        // 创建时请求置顶失败（Wayland）则记住合成器策略，不再反复请求。
        if (controller->topmost.Get() &&
            window.SetTopmost(true) != huxerui::WindowRequestResult::Submitted) {
            controller->topmost = false;
        }
        if (!controller->saved_position) controller->RecoverPosition();
        return [controller, current] {
            // 退役先于新窗口挂载时（Retire++generation 已使代次失配），
            // 旧附件的清理不得清掉新窗口的句柄。
            if (controller->generation == current) controller->pet.reset();
        };
    });
}

} // namespace llmswitch::ui
