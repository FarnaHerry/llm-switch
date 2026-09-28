// subscriptions_page.cpp — 内置订阅供应商目录。条目按可配置的 Agent 分组，
// 点「添加」跳到对应新增表单并用该模板预填。
#include <huxerui/huxerui.h>

#include <string>
#include <utility>
#include <vector>

#include "ui.h"

import llmswitch.models;

namespace llmswitch::ui {

[[huxerui::composable]] huxerui::View SubscriptionsPage(
    huxerui::State<std::size_t> navPage,
    huxerui::State<std::size_t> selectedTool,
    huxerui::State<std::string> addProviderRequest,
    huxerui::State<std::string> pendingSubscriptionPresetName) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    std::vector<huxerui::View> sections;
    const auto& registry = models::toolRegistry();

    for (std::size_t toolIndex = 0; toolIndex < registry.size(); ++toolIndex) {
        const auto& tool = registry[toolIndex];
        const auto presets = models::builtinPresets(tool.id);
        if (presets.subscription.empty()) continue;

        std::vector<huxerui::View> entries;
        entries.reserve(presets.subscription.size());
        for (const auto& preset : presets.subscription) {
            const std::string toolId(tool.id);
            const std::string presetName(preset.name);
            entries.push_back(
                QuietCard(huxerui::Row {
                    huxerui::Image(PresetIcon(preset.name))
                        .Tint(theme.colors.on_surface)
                        .With(huxerui::Frame{.width = 24.0F, .height = 24.0F}),
                    huxerui::Column {
                        huxerui::Text(preset.name),
                        huxerui::Text(preset.website)
                            .Style(huxerui::TextStyle{
                                huxerui::Font::System(font_size::kCaption),
                                theme.colors.on_surface_variant}),
                    }.With(huxerui::Grow(1.0F),
                           huxerui::CrossAlign(
                               huxerui::CrossAxisAlignment::Start)),
                    huxerui::Button("添加").OnClick(
                        [navPage, selectedTool, addProviderRequest,
                         pendingSubscriptionPresetName, toolId, presetName,
                         toolIndex] {
                            selectedTool = toolIndex;
                            pendingSubscriptionPresetName = presetName;
                            addProviderRequest = toolId;
                            navPage = pages::kAgents;
                        }),
                }.With(huxerui::Spacing(theme.spacing.medium),
                       huxerui::CrossAlign(
                           huxerui::CrossAxisAlignment::Center)))
                    .Key("subscription:" + toolId + ":" + presetName));
        }

        const std::string toolId(tool.id);
        sections.push_back(
            PageSection(
                huxerui::Text(std::string(tool.displayName),
                              huxerui::TextRole::Title),
                huxerui::Column(std::move(entries))
                    .With(huxerui::Spacing(theme.spacing.extra_small),
                          huxerui::CrossAlign(
                              huxerui::CrossAxisAlignment::Stretch)))
                .Key("subscription-group:" + toolId));
        sections.push_back(SectionDivider());
    }
    if (!sections.empty()) sections.pop_back();

    return PageScaffold(
        "订阅供应商",
        huxerui::Row{},
        huxerui::ScrollView(
            huxerui::Column {
                huxerui::Text("选择一个内置订阅模板，自动跳转到对应 Agent 并预填新增表单。")
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kBody),
                        theme.colors.on_surface_variant}),
                huxerui::Column(std::move(sections))
                    .With(huxerui::Spacing(theme.spacing.medium),
                          huxerui::CrossAlign(
                              huxerui::CrossAxisAlignment::Stretch)),
            }.With(huxerui::Spacing(theme.spacing.large),
                   huxerui::CrossAlign(
                       huxerui::CrossAxisAlignment::Stretch))));
}

} // namespace llmswitch::ui
