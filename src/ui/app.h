// app.h — llm-switch 应用根声明（定义在 app.cpp，HuxerUI composable 函数）。
#pragma once

#include <huxerui/huxerui.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "ui.h"

namespace llmswitch::ui {

struct PetController;

// 统一的控制中心窗口配置；既用于 Application 默认值，也用于按需重建。
huxerui::WindowOptions ControlCenterWindowOptions();

// 主窗口内容工厂；桌宠和信息卡由各自的内容工厂创建独立窗口。
// 由 src/app.cpp 注册到 Application，也供控制中心按需重建。
huxerui::View AppRoot();

// 按需创建并激活控制中心，同时让应用级入口跟踪当前挂载的窗口。
class MainWindowController final {
public:
    MainWindowController(huxerui::ApplicationHandle application,
                         huxerui::WindowOptions options)
        : application_(std::move(application)), options_(std::move(options)) {}

    std::uint64_t Bind(huxerui::WindowHandle window,
                       huxerui::ToastHandle toast);
    void Unbind(std::uint64_t generation) noexcept;
    void Activate();
    void Hide() const;
    void ShowMessage(std::string message, bool openIfNeeded = false);
    [[nodiscard]] std::optional<std::string> TakePendingMessage();
    [[nodiscard]] bool IsOpen() const noexcept { return window_.has_value(); }

private:
    huxerui::ApplicationHandle application_;
    huxerui::WindowOptions options_;
    std::optional<huxerui::WindowHandle> window_;
    std::optional<huxerui::ToastHandle> toast_;
    std::optional<std::string> pendingMessage_;
    std::uint64_t generation_ = 0;
};

// 托盘是应用级呈现，不能随着控制中心窗口退役而消失。状态页挂载时只把
// revision/startupMode 绑定进来，用于菜单操作后通知现存页面更新。
class TrayPresentationController final
    : public std::enable_shared_from_this<TrayPresentationController> {
public:
    TrayPresentationController(
        huxerui::ApplicationHandle application, huxerui::SystemTrayHandle tray,
        std::shared_ptr<MainWindowController> mainWindow,
        std::shared_ptr<PetController> pet)
        : application_(std::move(application)), tray_(std::move(tray)),
          mainWindow_(std::move(mainWindow)), pet_(std::move(pet)) {}

    std::uint64_t BindUiState(huxerui::State<int> revision,
                              huxerui::State<std::size_t> startupMode);
    void UnbindUiState(std::uint64_t generation) noexcept;
    void NotifyConfigurationChanged(
        std::optional<std::size_t> startupMode = std::nullopt);
    void Refresh();

private:
    huxerui::ApplicationHandle application_;
    huxerui::SystemTrayHandle tray_;
    std::shared_ptr<MainWindowController> mainWindow_;
    std::shared_ptr<PetController> pet_;
    std::optional<huxerui::State<int>> revision_;
    std::optional<huxerui::State<std::size_t>> startupMode_;
    std::uint64_t uiStateGeneration_ = 0;
};

// 主窗口页面导航目标：AppContent 持有 navPage State（组合期局部），而
// 桌宠信息卡等窗口外入口需要「跳到某页面并激活主窗口」。与
// ApplicationHook 里 Provide 一次，主窗口 Lifecycle 里绑定/解绑。
class MainNavTarget final {
public:
    void Bind(huxerui::State<std::size_t> page,
              huxerui::State<std::size_t> workspacePage,
              huxerui::State<std::size_t> selectedAgentTool,
              huxerui::State<int> revision,
              huxerui::State<int> themeMode,
              huxerui::State<std::size_t> startupMode,
              huxerui::State<bool> trayEnabled) {
        page_ = std::move(page);
        workspacePage_ = std::move(workspacePage);
        selectedAgentTool_ = std::move(selectedAgentTool);
        revision_ = std::move(revision);
        themeMode_ = std::move(themeMode);
        startupMode_ = std::move(startupMode);
        trayEnabled_ = std::move(trayEnabled);
        if (pendingAgentTool_) {
            *selectedAgentTool_ = *pendingAgentTool_;
            pendingAgentTool_.reset();
        }
        if (pendingWorkspacePage_) {
            *workspacePage_ = *pendingWorkspacePage_;
            pendingWorkspacePage_.reset();
        }
        if (pendingPage_) {
            *page_ = *pendingPage_;
            pendingPage_.reset();
        }
    }
    void Unbind() noexcept {
        page_ = {};
        workspacePage_ = {};
        selectedAgentTool_ = {};
        revision_ = {};
        themeMode_ = {};
        startupMode_ = {};
        trayEnabled_ = {};
    }
    std::optional<huxerui::State<int>> RevisionState() const { return revision_; }
    std::optional<huxerui::State<int>> ThemeModeState() const { return themeMode_; }
    std::optional<huxerui::State<std::size_t>> StartupModeState() const {
        return startupMode_;
    }
    std::optional<huxerui::State<bool>> TrayEnabledState() const {
        return trayEnabled_;
    }
    // 写入页面下标（pages::kXxx）；主窗口未打开时暂存到首次组合。
    void RequestPage(std::size_t index) const {
        if (page_.has_value()) *page_ = index;
        else pendingPage_ = index;
    }
    // 信息卡按 Agent 跳转；控制中心未打开时，目标 Agent 与页面一起暂存。
    void RequestAgentPage(std::size_t pageIndex, std::size_t toolIndex) const {
        if (selectedAgentTool_.has_value()) *selectedAgentTool_ = toolIndex;
        else pendingAgentTool_ = toolIndex;
        RequestPage(pageIndex);
    }
    // 桌宠工作台内页；控制中心未打开时暂存页面与 Agent 选择。
    void RequestWorkspacePage(std::size_t index) const {
        if (workspacePage_.has_value()) *workspacePage_ = index;
        else pendingWorkspacePage_ = index;
        RequestPage(pages::kWorkspace);
    }
    void RequestWorkspaceAgentPage(std::size_t toolIndex) const {
        if (selectedAgentTool_.has_value()) *selectedAgentTool_ = toolIndex;
        else pendingAgentTool_ = toolIndex;
        RequestWorkspacePage(workspace_pages::kAgents);
    }

private:
    std::optional<huxerui::State<std::size_t>> page_;
    std::optional<huxerui::State<std::size_t>> workspacePage_;
    std::optional<huxerui::State<std::size_t>> selectedAgentTool_;
    std::optional<huxerui::State<int>> revision_;
    std::optional<huxerui::State<int>> themeMode_;
    std::optional<huxerui::State<std::size_t>> startupMode_;
    std::optional<huxerui::State<bool>> trayEnabled_;
    mutable std::optional<std::size_t> pendingPage_;
    mutable std::optional<std::size_t> pendingWorkspacePage_;
    mutable std::optional<std::size_t> pendingAgentTool_;
};

// 在控制中心或桌宠首次挂载时同步登录启动项并自启 LocalRouter。
void EnsureRuntimeServices(huxerui::ToastHandle toast);

// ApplicationHook：每个 Runtime 调用一次，注册应用级托盘激活处理器。
// 由 src/app.cpp 的 AppOptions::application_hooks 装配。
void InstallApplication(huxerui::ApplicationContext& context);

} // namespace llmswitch::ui
