// app.h — llm-switch 应用根声明（定义在 app.cpp，HuxerUI composable 函数）。
#pragma once

#include <huxerui/huxerui.h>

#include <cstddef>
#include <optional>

namespace llmswitch::ui {

// 应用根：当前为占位（一行文字），后续迭代替换为标题栏 + 侧边导航 + 页面切换。
// 由 src/app.cpp 注册到 Application。
huxerui::View AppRoot();

// 托盘激活目标：SystemTrayHandle::OnActivate 是 Runtime 生命周期的一次性注册
// （重复注册抛 std::logic_error），必须在 ApplicationHook 里装；但激活要落到
// 具体窗口，而注册时还没有组合、拿不到 WindowHandle。这个应用级服务补上中间
// 一层——组合期绑定当前窗口、卸载时解绑，托盘回调只读它。
// 注册、绑定、回调都在应用线程，无需加锁。
class TrayActivationTarget final {
public:
    void Bind(huxerui::WindowHandle window) { window_ = std::move(window); }
    void Unbind() noexcept { window_.reset(); }
    void Activate() const;

private:
    std::optional<huxerui::WindowHandle> window_;
};

// 主窗口页面导航目标：AppContent 持有 navPage State（组合期局部），而
// 桌宠信息卡等窗口外入口需要「跳到某页面并激活主窗口」。与
// TrayActivationTarget 同一模式：ApplicationHook 里 Provide 一次，主窗口
// Lifecycle 里绑定/解绑，外部只读调用。注册、绑定、调用都在应用线程。
class MainNavTarget final {
public:
    void Bind(huxerui::State<std::size_t> page) { page_ = std::move(page); }
    void Unbind() noexcept { page_ = {}; }
    // 写入页面下标（pages::kXxx）；未绑定（主窗口未组合）时为 no-op。
    void RequestPage(std::size_t index) const {
        if (page_.has_value()) *page_ = index;
    }

private:
    std::optional<huxerui::State<std::size_t>> page_;
};

// ApplicationHook：每个 Runtime 调用一次，注册应用级托盘激活处理器。
// 由 src/app.cpp 的 AppOptions::application_hooks 装配。
void InstallApplication(huxerui::ApplicationContext& context);

} // namespace llmswitch::ui
