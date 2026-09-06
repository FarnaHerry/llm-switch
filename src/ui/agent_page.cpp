// agent_page.cpp — Agent 管理页（薄宿主）：持有当前工具 State（currentTool，
// 默认注册表第一个 claude-code），其余全部交给 ProvidersPage——工具图标栏
// 在供应商岛屿内部顶部（ToolBar），写 currentTool 换组，本层以 .Key(tool)
// 组合 ProvidersPage，换工具即整体重建（页面状态/表单状态随之重置）。
#include <huxerui/huxerui.h>

#include <string>

#include "ui.h"

import llmswitch.models;

namespace llmswitch::ui {

[[huxerui::composable]] huxerui::View AgentPage(huxerui::State<int> revision) {
    auto currentTool = huxerui::UseState<std::string>(
        std::string(models::toolRegistry().front().id));
    return ProvidersPage(currentTool, revision)
        .Key(currentTool.Get())
        .With(huxerui::Grow(1.0F));
}

} // namespace llmswitch::ui
