// provider_form_aggregate.cpp — 聚合供应商编辑页。
//
// 聚合供应商不再手填 URL/Key：直接从当前 Agent 的现有供应商里勾选成员，
// 凭据只保留在原供应商条目里。选中后 Agent 的 live 配置指向本地路由，由
// router 按请求 model 优先匹配成员，并在 429/5xx 时按列表顺序继续尝试。
// 成员必须与目标 Agent 使用同一协议；聚合项不能嵌套另一个聚合项。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <exception>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "providers_internal.h"

import llmswitch.models;
import llmswitch.store;

namespace llmswitch::ui {
namespace {

struct MemberInfo {
    std::string id;
    std::string name;
    std::string model;
    std::string url;
};

std::vector<MemberInfo> CandidateMembers(std::string_view tool,
                                         std::string_view selfId) {
    std::vector<MemberInfo> members;
    try {
        const auto& group = providerStore().group(std::string(tool));
        for (const auto& provider : group.providers) {
            if (provider.id == selfId ||
                models::isAggregateProvider(provider)) {
                continue;
            }
            members.push_back(MemberInfo{
                .id = provider.id,
                .name = provider.name,
                .model = provider.model,
                .url = models::effectiveBaseUrl(provider),
            });
        }
    } catch (...) {
    }
    return members;
}

std::vector<std::string> InitialSelection(
    const models::Provider& initial,
    const std::vector<MemberInfo>& candidates) {
    std::vector<std::string> selected;
    for (const auto& member : initial.upstreams) {
        std::string id = member.providerId;
        if (id.empty()) {
            // 旧版直填成员：按访问 URL 尝试迁移到现有供应商；匹配不到就
            // 不再保留，用户需要重新勾选。
            for (const auto& candidate : candidates) {
                if (!member.baseUrl.empty() &&
                    candidate.url == member.baseUrl) {
                    id = candidate.id;
                    break;
                }
            }
        }
        if (id.empty()) continue;
        const bool exists = std::ranges::any_of(
            candidates, [&id](const MemberInfo& candidate) {
                return candidate.id == id;
            });
        if (exists &&
            std::ranges::find(selected, id) == selected.end()) {
            selected.push_back(std::move(id));
        }
    }
    return selected;
}

std::vector<std::string> AvailableModels(
    const std::vector<MemberInfo>& candidates,
    const std::vector<std::string>& selected) {
    std::vector<std::string> models;
    for (const auto& candidate : candidates) {
        if (candidate.model.empty() ||
            std::ranges::find(selected, candidate.id) == selected.end() ||
            std::ranges::find(models, candidate.model) != models.end()) {
            continue;
        }
        models.push_back(candidate.model);
    }
    return models;
}

} // namespace

[[huxerui::composable]] huxerui::View AggregateFormPage(
    std::string tool, models::Provider initial, bool isNew,
    huxerui::State<int> revision, huxerui::State<std::string> formTarget) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    auto toast = huxerui::UseToast();

    auto name = huxerui::UseState(
        huxerui::TextEditingValue{initial.name});
    auto notes = huxerui::UseState(
        huxerui::TextEditingValue{initial.notes});
    auto chosenModel = huxerui::UseState(initial.model);
    const std::vector<MemberInfo> candidates =
        CandidateMembers(tool, initial.id);
    auto selected = huxerui::UseState(InitialSelection(initial, candidates));

    const std::vector<std::string> selectedIds = selected.Get();
    const std::vector<std::string> models =
        AvailableModels(candidates, selectedIds);
    std::string effectiveModel = chosenModel.Get();
    if (std::ranges::find(models, effectiveModel) == models.end()) {
        effectiveModel = models.empty() ? std::string{} : models.front();
    }
    const std::size_t modelIndex =
        effectiveModel.empty()
            ? 0
            : static_cast<std::size_t>(std::distance(
                  models.begin(), std::ranges::find(models, effectiveModel)));

    const bool hasLegacyEntries = std::ranges::any_of(
        initial.upstreams, [](const models::ProviderUpstream& member) {
            return member.providerId.empty();
        });

    std::vector<huxerui::View> memberRows;
    memberRows.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        const bool checked =
            std::ranges::find(selectedIds, candidate.id) != selectedIds.end();
        huxerui::View row = huxerui::Row {
            huxerui::Checkbox(checked).OnChanged(
                [selected, id = candidate.id](bool on) {
                    auto list = selected.Get();
                    const auto found =
                        std::ranges::find(list, id);
                    if (on && found == list.end()) {
                        list.push_back(id);
                    } else if (!on && found != list.end()) {
                        list.erase(found);
                    }
                    selected = std::move(list);
                }),
            huxerui::Column {
                huxerui::Text(candidate.name).Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kBody)
                        .WithWeight(huxerui::FontWeight::SemiBold),
                    theme.colors.on_surface}),
                huxerui::Text(
                    candidate.model.empty()
                        ? "未设置默认模型"
                        : std::format("模型 {}", candidate.model))
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kChip),
                        theme.colors.on_surface_variant}),
            }.With(huxerui::Spacing(2.0F), huxerui::Grow(1.0F)),
            huxerui::Text(candidate.url.empty() ? "未设置 URL" : candidate.url)
                .Style(huxerui::TextStyle{
                    huxerui::Font::Monospace(font_size::kChip),
                    theme.colors.on_surface_variant}),
        }.With(huxerui::Spacing(10.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
               huxerui::Padding(
                   huxerui::EdgeInsets::Symmetric(12.0F, 8.0F)),
               huxerui::Background(checked ? theme.colors.primary_container
                                           : theme.colors.surface_container_high),
               huxerui::CornerRadius(theme.shapes.extra_small),
               huxerui::Border{checked ? theme.colors.primary
                                       : theme.colors.outline,
                               1.0F});
        memberRows.push_back(std::move(row).Key("aggregate-member:" +
                                                candidate.id));
    }

    huxerui::View memberList =
        memberRows.empty()
            ? huxerui::View{
                  huxerui::Text(
                      "当前 Agent 还没有可用的普通供应商；请先在供应商页新增，"
                      "再回来做聚合。")
                      .Style(huxerui::TextStyle{
                          huxerui::Font::System(font_size::kCaption),
                          theme.colors.on_surface_variant})}
            : huxerui::View{huxerui::Column(std::move(memberRows))
                                .With(huxerui::Spacing(8.0F),
                                      huxerui::CrossAlign(
                                          huxerui::CrossAxisAlignment::Stretch))};

    huxerui::View modelPicker =
        models.empty()
            ? huxerui::View{
                  huxerui::Text(
                      "选中的供应商都未设置模型；切换后 Agent 会沿用现有模型。")
                      .Style(huxerui::TextStyle{
                          huxerui::Font::System(font_size::kCaption),
                          theme.colors.on_surface_variant})}
            : huxerui::View{
                  huxerui::Select(
                      models, modelIndex,
                      [](const std::string& option) {
                          return huxerui::Text(option).Key(option);
                      })
                      .Label("默认模型")
                      .OnChanged([chosenModel, models](std::size_t index) {
                          if (index < models.size()) {
                              chosenModel = models[index];
                          }
                      })};

    const huxerui::View protocolHint =
        tool == "codex"
            ? huxerui::View{
                  huxerui::Text(
                      "Codex 聚合固定用 chat wire_api，成员需支持 "
                      "/chat/completions（Responses 成员请改用普通供应商）。")
                      .Style(huxerui::TextStyle{
                          huxerui::Font::System(font_size::kChip),
                          theme.colors.on_surface_variant})}
            : huxerui::View{huxerui::Row{}};

    huxerui::View body = huxerui::Column {
        huxerui::ScrollView(
            huxerui::Column {
                huxerui::Text(
                    "聚合供应商把当前 Agent 的多个现有供应商组织成一个可选"
                    "条目。切换后请求先发到本地路由，按请求里的 model 优先"
                    "匹配成员；成员返回 429/5xx 或连接失败时，按下方列表顺序"
                    "继续尝试下一条。成员协议必须与目标 Agent 一致。")
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kCaption),
                        theme.colors.on_surface_variant}),
                huxerui::TextField(name.Get())
                    .Label("名称")
                    .Variant(huxerui::TextFieldVariant::Outlined)
                    .OnChanged([name](const huxerui::TextEditingValue& v) {
                        name = v;
                    }),
                huxerui::TextField(notes.Get())
                    .Label("备注（可选）")
                    .Variant(huxerui::TextFieldVariant::Outlined)
                    .OnChanged([notes](const huxerui::TextEditingValue& v) {
                        notes = v;
                    }),
                protocolHint,
                huxerui::Text("选择现有供应商")
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kBody)
                            .WithWeight(huxerui::FontWeight::SemiBold),
                        theme.colors.on_surface}),
                huxerui::Text(
                    "按列表顺序作为兜底顺序；凭据仍只保存在原供应商条目里。")
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kCaption),
                        theme.colors.on_surface_variant}),
                hasLegacyEntries
                    ? huxerui::View{
                          huxerui::Text(
                              "这个聚合项含旧版直填 API；保存后会改为下方"
                              "勾选的现有供应商。")
                              .Style(huxerui::TextStyle{
                                  huxerui::Font::System(font_size::kChip),
                                  theme.colors.error})}
                    : huxerui::View{huxerui::Row{}},
                std::move(memberList),
                std::move(modelPicker),
            }.With(huxerui::Spacing(10.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
                .With(huxerui::Grow(1.0F)),
        huxerui::Row {
            huxerui::Button("取消").OnClick([formTarget] { formTarget = ""; }),
            huxerui::Button(isNew ? "添加" : "保存").OnClick(
                [=] {
                    const std::string formName = name.Get().text;
                    if (formName.empty()) {
                        toast.Show("名称不能为空");
                        return;
                    }
                    const std::vector<std::string> currentSelection =
                        selected.Get();
                    std::vector<models::ProviderUpstream> upstreams;
                    for (const auto& candidate : candidates) {
                        if (std::ranges::find(currentSelection,
                                              candidate.id) ==
                            currentSelection.end()) {
                            continue;
                        }
                        upstreams.push_back(models::ProviderUpstream{
                            .name = candidate.name,
                            .providerId = candidate.id,
                            .model = candidate.model,
                        });
                    }
                    if (upstreams.empty()) {
                        toast.Show("至少选择一个现有供应商");
                        return;
                    }
                    const std::vector<std::string> modelChoices =
                        AvailableModels(candidates, currentSelection);
                    std::string defaultModel = chosenModel.Get();
                    if (std::ranges::find(modelChoices, defaultModel) ==
                        modelChoices.end()) {
                        defaultModel = modelChoices.empty()
                                           ? std::string{}
                                           : modelChoices.front();
                    }
                    if (tool == "kimi-code" && defaultModel.empty()) {
                        toast.Show(
                            "Kimi Code 聚合需要默认模型：请先给至少一个成员"
                            "设置模型");
                        return;
                    }
                    models::Provider provider = initial;
                    provider.name = formName;
                    provider.notes = notes.Get().text;
                    provider.model = std::move(defaultModel);
                    provider.baseUrl.clear();
                    provider.modelFetchUrl.clear();
                    provider.apiKey =
                        std::string(models::kAggregateApiKey);
                    provider.fullUrl = true;
                    provider.upstreamFormat = DefaultUpstreamFormat(tool);
                    provider.apiFormat.clear();
                    provider.upstreams = std::move(upstreams);
                    const bool wasCurrent =
                        !isNew &&
                        providerStore().group(tool).current == provider.id;
                    try {
                        if (isNew) {
                            providerStore().addProvider(tool,
                                                        std::move(provider));
                        } else {
                            providerStore().updateProvider(tool, provider);
                        }
                        // 当前聚合项改了成员/默认模型时，live 配置里的 model
                        // 也要跟着走；路由本身读 store，不用重启。
                        if (wasCurrent) SwitchProvider(tool, provider.id);
                    } catch (const std::exception& e) {
                        toast.Show(e.what());
                        return;
                    }
                    revision = revision.Get() + 1;
                    toast.Show(isNew ? "已添加聚合供应商" : "已保存聚合供应商");
                    formTarget = "";
                }),
        }.With(huxerui::Spacing(8.0F),
               huxerui::MainAlign(huxerui::MainAxisAlignment::End),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
    }.With(huxerui::Spacing(12.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    const std::string title =
        isNew ? "新增聚合供应商 — " + std::string(ToolName(tool))
              : "编辑聚合供应商 — " + initial.name;
    return PageScaffold(
        title,
        huxerui::Row {
            huxerui::Button("返回").OnClick([formTarget] { formTarget = ""; }),
        },
        std::move(body));
}

} // namespace llmswitch::ui
