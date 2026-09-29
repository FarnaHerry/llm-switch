// provider_form_dsh.cpp — dsh（DeepSeek Harness）供应商表单的 agent 专属区块：
// 推理档位（reasoningEfforts）。dsh 的 llm-pi-ai 适配器把「模型支持思考」当
// per-model 能力：手工声明的路由不在模型条目里写 reasoningEfforts，模型就被
// 当成不支持推理，模型菜单里连「推理等级」都不出现。这里选择要声明的档位，
// 切换时写进 llmswitch-* 条目的首个模型条目（见 store 的 dsh 写入端）。
#include <huxerui/huxerui.h>

#include <vector>

#include "providers_internal.h"

import llmswitch.models;

namespace llmswitch::ui {

const AgentFormPolicy& DshFormPolicy() {
    static const AgentFormPolicy policy{
        .urlLabel = "URL",
        .keyLabel = "API Key",
        .urlRequired = true,
        .keyRequired = false,
        .primaryModelLabel = "模型（必填）",
        .showMappings = false,
        .showApiFormat = true,
        .showToml = false,
        .showReasoningEfforts = true,
    };
    return policy;
}

[[huxerui::composable]] huxerui::View DshReasoningFields(const FormStates& fs) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    std::vector<huxerui::View> chips;
    const auto& levels = models::reasoningLevels();
    for (std::size_t i = 0; i < levels.size(); ++i) {
        const int bit = 1 << static_cast<int>(i);
        const bool selected = (fs.reasoningMask.Get() & bit) != 0;
        chips.push_back(
            huxerui::Chip(models::reasoningLevelLabel(levels[i]), selected)
                .OnChanged([fs, bit](bool on) {
                    fs.reasoningMask =
                        on ? (fs.reasoningMask.Get() | bit)
                           : (fs.reasoningMask.Get() & ~bit);
                }));
    }
    return huxerui::Column {
        huxerui::Text("推理档位（写入 dsh 的 reasoningEfforts；不选 = 不声明，"
                      "dsh 视为不支持思考）")
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                theme.colors.on_surface_variant}),
        // 档位较多，用 Flow 自动换行避免单行 Row 横向溢出。
        huxerui::Flow(std::move(chips)).With(huxerui::Spacing(8.0F)),
    }.With(huxerui::Spacing(8.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace llmswitch::ui
