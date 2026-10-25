// app.cpp — llm-switch 应用声明：HuxerUI Application 单例。
// 平台入口 main() 在 platform/<platform>/main.cpp（HuxerUI CLI 生成格式）；
// UI 内容在 src/ui/*.cpp（composable 普通源，经 huxerui_add_app 的 codegen 处理）。
#include <huxerui/huxerui.h>

#include "ui/app.h"

const huxerui::Application application{
    llmswitch::ui::AppRoot,
    huxerui::AppOptions{
        .window = llmswitch::ui::ControlCenterWindowOptions(),
        // 桌宠优先启动不创建管理窗口；ApplicationHook 根据启动模式按需打开。
        .create_initial_window = false,
        // 应用级安装：托盘激活处理器等 Runtime 生命周期的一次性注册走这里
        // （见 llmswitch::ui::InstallApplication）。
        .application_hooks = {llmswitch::ui::InstallApplication},
    },
};
