// provider_form_openai.cpp — opencode / pi 供应商表单的 agent 专属区块：
// API 协议选择（OpenAI 兼容 / Anthropic / OpenAI Responses），写进
// opencode 的 npm 段 / pi 的 api 字段（见 store）。两家共用同一策略
// 常量（模型必填、默认 URL / API Key 标签）。
#include <huxerui/huxerui.h>

#include <string>
#include <vector>

#include "providers_internal.h"

namespace llmswitch::ui {

const AgentFormPolicy& OpenAiCliFormPolicy() {
    static const AgentFormPolicy policy{
        .urlLabel = "URL",
        .keyLabel = "API Key",
        .urlRequired = true,
        .keyRequired = false,
        .primaryModelLabel = "模型（必填）",
        .showMappings = false,
        .showApiFormat = true,
        .showToml = false,
    };
    return policy;
}

[[huxerui::composable]] [[huxerui::composable]] huxerui::View ApiFormatFields(const FormStates& fs) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Column {
        huxerui::Text("API 协议")
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                theme.colors.on_surface_variant}),
        huxerui::SegmentedButton(
            {"OpenAI 兼容", "Anthropic", "OpenAI Responses"},
            static_cast<std::size_t>(fs.apiFormat.Get()))
            .OnChanged([fs](std::size_t index) {
                fs.apiFormat = static_cast<int>(index);
            }),
    }.With(huxerui::Spacing(8.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace llmswitch::ui
