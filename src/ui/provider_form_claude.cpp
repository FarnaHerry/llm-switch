// provider_form_claude.cpp — Claude 系（claude-code / claude desktop）供应商
// 表单的 agent 专属区块：三档模型映射（Haiku/Sonnet/Opus，claude-code 写
// ANTHROPIC_DEFAULT_*_MODEL env，claude desktop 写 inferenceModels 映射条目，
// 均选填，共享同一份 fetchedModels 下拉）。策略常量也在这里。
#include <huxerui/huxerui.h>

#include <array>
#include <string>
#include <vector>

#include "providers_internal.h"

import llmswitch.models;

namespace llmswitch::ui {

const AgentFormPolicy& ClaudeCodeFormPolicy() {
    static const AgentFormPolicy policy{
        .urlLabel = "URL",
        .keyLabel = "API Key",
        .urlRequired = true,
        .keyRequired = false,
        .primaryModelLabel = "主模型（可选，写入 ANTHROPIC_MODEL）",
        .showMappings = true,
        .showApiFormat = false,
        .showToml = false,
    };
    return policy;
}

const AgentFormPolicy& ClaudeDesktopFormPolicy() {
    static const AgentFormPolicy policy{
        .urlLabel = "URL",
        .keyLabel = "API Key",
        .urlRequired = true,
        .keyRequired = false,
        .primaryModelLabel = "主模型（可选）",
        .showMappings = true,
        .showApiFormat = false,
        .showToml = false,
    };
    return policy;
}

[[huxerui::composable]] [[huxerui::composable]] huxerui::View MappingFields(
    const FormStates& fs, huxerui::StateList<std::string> fetchedModels) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    // 实际请求模型仍是现有三档 *Model 字段；显示名与 supports1m 仅用于
    // Claude Desktop 菜单元数据。
    std::vector<huxerui::View> fields;
    fields.push_back(huxerui::Text("模型映射（可选）")
        .Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kCaption),
            theme.colors.on_surface_variant}));
    struct MappingField {
        const char* role;
        huxerui::State<huxerui::TextEditingValue> displayName;
        huxerui::State<huxerui::TextEditingValue> model;
        huxerui::State<bool> supports1m;
    };
    const std::array<MappingField, 3> mappingFields{{
        {"Haiku", fs.haikuDisplayName, fs.haiku, fs.haikuSupports1m},
        {"Sonnet", fs.sonnetDisplayName, fs.sonnet, fs.sonnetSupports1m},
        {"Opus", fs.opusDisplayName, fs.opus, fs.opusSupports1m},
    }};
    const std::array<huxerui::State<huxerui::TextEditingValue>, 3>
        mappingSearches{fs.haikuSearch, fs.sonnetSearch, fs.opusSearch};
    for (std::size_t i = 0; i < mappingFields.size(); ++i) {
        const auto& mapping = mappingFields[i];
        fields.push_back(huxerui::Column {
            huxerui::Text(mapping.role)
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kBody),
                    theme.colors.on_surface}),
            huxerui::Row {
                huxerui::TextField(mapping.displayName.Get())
                    .Label("菜单显示名称")
                    .Placeholder(std::string(mapping.role) + " 菜单名称")
                    .Variant(huxerui::TextFieldVariant::Outlined)
                    .OnChanged([field = mapping.displayName](
                                   const huxerui::TextEditingValue& v) {
                        field = v;
                    })
                    .With(huxerui::Grow(1.0F)),
                huxerui::TextField(mapping.model.Get())
                    .Label("实际请求模型")
                    .Placeholder("发送给上游的模型 ID")
                    .Variant(huxerui::TextFieldVariant::Outlined)
                    .OnChanged([field = mapping.model](
                                   const huxerui::TextEditingValue& v) {
                        field = v;
                    })
                    .With(huxerui::Grow(1.0F)),
                fetchedModels.Empty()
                    ? huxerui::View{huxerui::Row{}}
                    : ModelSelect(fetchedModels, mappingSearches[i], mapping.model,
                                  mapping.displayName),
                huxerui::Checkbox("1M", mapping.supports1m.Get())
                    .OnChanged([supports1m = mapping.supports1m](bool checked) {
                        supports1m = checked;
                    }),
            }.With(huxerui::Spacing(8.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        }.With(huxerui::Spacing(6.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
    }
    return huxerui::Column(std::move(fields))
        .With(huxerui::Spacing(12.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace llmswitch::ui
