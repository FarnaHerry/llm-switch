// provider_form_aggregate.cpp — 聚合供应商编辑页。
//
// 一个聚合供应商 = 名称 + 默认模型 + 多条完整 API（URL / Key / 模型）。
// 选中后 Agent 的 live 配置指向本地路由，由 router 按请求 model 优先匹配
// 成员并在 429/5xx 时继续尝试下一条。成员必须与目标 Agent 使用同一协议；
// 这里只收完整 URL，不再猜测 /v1 或 /anthropic 后缀。
#include <huxerui/huxerui.h>

#include <cstdint>
#include <exception>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "providers_internal.h"
#include "app_resources.h"

import llmswitch.models;
import llmswitch.store;

namespace llmswitch::ui {
namespace {

struct UpstreamDraft {
    std::uint64_t key = 0;
    std::string name;
    std::string baseUrl;
    std::string apiKey;
    std::string model;
    bool operator==(const UpstreamDraft&) const = default;
};

std::vector<UpstreamDraft> DraftsFromProvider(const models::Provider& provider) {
    std::vector<UpstreamDraft> drafts;
    drafts.reserve(provider.upstreams.size());
    for (std::size_t i = 0; i < provider.upstreams.size(); ++i) {
        const auto& upstream = provider.upstreams[i];
        drafts.push_back(UpstreamDraft{
            .key = i,
            .name = upstream.name,
            .baseUrl = upstream.baseUrl,
            .apiKey = upstream.apiKey,
            .model = upstream.model,
        });
    }
    return drafts;
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
    auto model = huxerui::UseState(
        huxerui::TextEditingValue{initial.model});
    std::vector<UpstreamDraft> initialDrafts = DraftsFromProvider(initial);
    bool seededEmptyRow = false;
    if (isNew && initialDrafts.empty()) {
        initialDrafts.push_back(UpstreamDraft{});
        seededEmptyRow = true;
    }
    auto drafts = huxerui::UseState(std::move(initialDrafts));
    auto nextKey = huxerui::UseState<std::uint64_t>(
        static_cast<std::uint64_t>(initial.upstreams.size() +
                                   (seededEmptyRow ? 1 : 0)));
    auto showKeys = huxerui::UseState(false);

    const std::vector<UpstreamDraft> current = drafts.Get();
    std::vector<huxerui::View> cards;
    cards.reserve(current.size());
    for (std::size_t i = 0; i < current.size(); ++i) {
        const UpstreamDraft draft = current[i];
        huxerui::View card = huxerui::Column {
            huxerui::Row {
                huxerui::Text(std::format("API {}", i + 1))
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kChip)
                            .WithWeight(huxerui::FontWeight::SemiBold),
                        theme.colors.on_surface}),
                huxerui::Spacer(),
                huxerui::IconButton(app::images::trash, "删除这条 API")
                    .OnClick([drafts, i] {
                        auto list = drafts.Get();
                        if (i < list.size()) list.erase(list.begin() + i);
                        drafts = std::move(list);
                    })
                    .With(huxerui::Tooltip("删除这条 API")),
            }.With(huxerui::Spacing(8.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
            huxerui::Row {
                huxerui::TextField(huxerui::TextEditingValue{draft.name})
                    .Label("名称（可选）")
                    .Placeholder("例如 主力 / 备用")
                    .Variant(huxerui::TextFieldVariant::Outlined)
                    .OnChanged([drafts, i](const huxerui::TextEditingValue& v) {
                        auto list = drafts.Get();
                        if (i < list.size()) list[i].name = v.text;
                        drafts = std::move(list);
                    })
                    .With(huxerui::Grow(1.0F)),
                huxerui::TextField(huxerui::TextEditingValue{draft.model})
                    .Label("模型（用于按 model 匹配，可选）")
                    .Placeholder("例如 gpt-5.6")
                    .Variant(huxerui::TextFieldVariant::Outlined)
                    .OnChanged([drafts, i](const huxerui::TextEditingValue& v) {
                        auto list = drafts.Get();
                        if (i < list.size()) list[i].model = v.text;
                        drafts = std::move(list);
                    })
                    .With(huxerui::Grow(1.0F)),
            }.With(huxerui::Spacing(8.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
            huxerui::TextField(huxerui::TextEditingValue{draft.baseUrl})
                .Label("Base URL（完整地址，含 /v1 等路径）")
                .Placeholder("https://api.example.com/v1")
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([drafts, i](const huxerui::TextEditingValue& v) {
                    auto list = drafts.Get();
                    if (i < list.size()) list[i].baseUrl = v.text;
                    drafts = std::move(list);
                }),
            huxerui::TextField(huxerui::TextEditingValue{draft.apiKey})
                .Label("API Key")
                .Variant(huxerui::TextFieldVariant::Outlined)
                .Secure(!showKeys.Get())
                .OnChanged([drafts, i](const huxerui::TextEditingValue& v) {
                    auto list = drafts.Get();
                    if (i < list.size()) list[i].apiKey = v.text;
                    drafts = std::move(list);
                }),
        }.With(huxerui::Spacing(8.0F),
               huxerui::Background(islands.raised),
               huxerui::CornerRadius(islands.nested_radius),
               huxerui::Padding(islands.island_padding),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
        cards.push_back(std::move(card).Key(
            "aggregate-api:" + std::to_string(draft.key)));
    }

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
                    "聚合供应商把多条 API 组织成一个可选条目。切换后 Agent "
                    "的请求先发到本地路由，再按请求里的 model 优先匹配成员；"
                    "成员返回 429/5xx 或连接失败时继续尝试下一条。成员 URL "
                    "必须是完整地址，且协议与目标 Agent 一致。")
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kCaption),
                        theme.colors.on_surface_variant}),
                huxerui::TextField(name.Get())
                    .Label("名称")
                    .Variant(huxerui::TextFieldVariant::Outlined)
                    .OnChanged([name](const huxerui::TextEditingValue& v) {
                        name = v;
                    }),
                huxerui::TextField(model.Get())
                    .Label("默认模型（可选；成员都留空时必填）")
                    .Placeholder("填给 Agent 的默认 model")
                    .Variant(huxerui::TextFieldVariant::Outlined)
                    .OnChanged([model](const huxerui::TextEditingValue& v) {
                        model = v;
                    }),
                huxerui::TextField(notes.Get())
                    .Label("备注（可选）")
                    .Variant(huxerui::TextFieldVariant::Outlined)
                    .OnChanged([notes](const huxerui::TextEditingValue& v) {
                        notes = v;
                    }),
                protocolHint,
                huxerui::Switch("显示 API Key", showKeys.Get())
                    .OnChanged([showKeys](bool on) { showKeys = on; }),
                huxerui::Column(std::move(cards))
                    .With(huxerui::Spacing(10.0F),
                          huxerui::CrossAlign(
                              huxerui::CrossAxisAlignment::Stretch)),
                huxerui::Row {
                    huxerui::Button("添加 API").OnClick(
                        [drafts, nextKey] {
                            auto list = drafts.Get();
                            list.push_back(UpstreamDraft{
                                .key = nextKey.Get(), .name = {}, .baseUrl = {},
                                .apiKey = {}, .model = {}});
                            nextKey = nextKey.Get() + 1;
                            drafts = std::move(list);
                        }),
                },
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
                    std::vector<models::ProviderUpstream> upstreams;
                    for (const auto& draft : drafts.Get()) {
                        if (draft.baseUrl.empty()) continue;
                        upstreams.push_back(models::ProviderUpstream{
                            .name = draft.name,
                            .baseUrl = draft.baseUrl,
                            .apiKey = draft.apiKey,
                            .model = draft.model,
                        });
                    }
                    if (upstreams.empty()) {
                        toast.Show("至少填写一条 API Base URL");
                        return;
                    }
                    std::string defaultModel = model.Get().text;
                    if (defaultModel.empty()) {
                        for (const auto& upstream : upstreams) {
                            if (!upstream.model.empty()) {
                                defaultModel = upstream.model;
                                break;
                            }
                        }
                    }
                    if (defaultModel.empty()) {
                        toast.Show("请填写默认模型，或给至少一条 API 填写模型");
                        return;
                    }
                    models::Provider provider = initial;
                    provider.name = formName;
                    provider.notes = notes.Get().text;
                    provider.model = defaultModel;
                    provider.baseUrl.clear();
                    provider.modelFetchUrl.clear();
                    provider.apiKey = "llmswitch-aggregate";
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
                        // 当前聚合项改了默认模型/成员时，live 配置里的 model
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
