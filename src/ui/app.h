// app.h — llm-switch 应用根声明（定义在 app.cpp，HuxerUI composable 函数）。
#pragma once

#include <huxerui/huxerui.h>

namespace llmswitch::ui {

// 应用根：当前为占位（一行文字），后续迭代替换为标题栏 + 侧边导航 + 页面切换。
// 由 src/app.cpp 注册到 Application。
huxerui::View AppRoot();

} // namespace llmswitch::ui
