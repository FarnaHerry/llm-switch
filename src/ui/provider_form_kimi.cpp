// provider_form_kimi.cpp — Kimi Code 供应商表单的策略与校验。
//
// Kimi Code（CLI 与 Desktop 共用 ~/.kimi-code/config.toml）的供应商由
// [providers.<key>] 与 [models.<alias>] 两条并存的表组成，模型别名上的
// max_context_size 是 Kimi 的必填字段；这里只做表单侧的必填校验与策略，
// TOML 的增量读写全部在 store_kimi.cpp。
#include <huxerui/huxerui.h>

#include "providers_internal.h"

import llmswitch.models;

namespace llmswitch::ui {

const AgentFormPolicy& KimiFormPolicy() {
    static const AgentFormPolicy policy{
        .urlLabel = "Base URL",
        .keyLabel = "API Key",
        .urlRequired = true,
        .keyRequired = false,
        .primaryModelLabel = "模型（必填）",
        .showMappings = false,
        .showApiFormat = true,
        .showToml = false,
        .showDshModelFields = true,
        .contextRequired = true,
        .customReasoning = true,
    };
    return policy;
}

bool KimiContextValid(const FormStates& fs, huxerui::ToastHandle toast) {
    if (fs.contextWindow.Get().text.empty()) {
        toast.Show("Kimi Code 的模型必须填写上下文窗口，例如 262144 或 256K");
        return false;
    }
    if (DshCapacity(fs.contextWindow) <= 0) {
        toast.Show("上下文窗口必须是正数，例如 262144、256K 或 1M");
        return false;
    }
    return true;
}

} // namespace llmswitch::ui
