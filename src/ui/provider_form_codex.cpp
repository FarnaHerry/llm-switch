// provider_form_codex.cpp — Codex 供应商表单的 agent 专属区块：
// config.toml 原文（可选；切换时整体替换，model 非空时行级重写顶层
// model 键）。策略常量（URL 可选以 config.toml 为准、API Key 必写
// auth.json）也在这里。
#include <huxerui/huxerui.h>

#include <string>
#include <vector>

#include "providers_internal.h"

namespace llmswitch::ui {

const AgentFormPolicy& CodexFormPolicy() {
    static const AgentFormPolicy policy{
        .urlLabel = "URL（可选，codex 以 config.toml 为准）",
        .keyLabel = "API Key（写入 auth.json）",
        .urlRequired = false,
        .keyRequired = true,
        .primaryModelLabel = "模型（可选，切换时写入 config.toml 顶层 model）",
        .showMappings = false,
        .showApiFormat = false,
        .showToml = true,
    };
    return policy;
}

[[huxerui::composable]] [[huxerui::composable]] huxerui::View TomlField(const FormStates& fs) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Column {
        huxerui::Text("config.toml 原文（可选；切换时整体替换）")
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                theme.colors.on_surface_variant}),
        huxerui::TextField(fs.toml.Get())
            .Variant(huxerui::TextFieldVariant::Outlined)
            .LineLimits(huxerui::TextFieldLineLimits::MultiLine(6, 12))
            .OnChanged([fs](const huxerui::TextEditingValue& v) { fs.toml = v; }),
    }.With(huxerui::Spacing(8.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace llmswitch::ui
