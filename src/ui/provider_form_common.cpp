// provider_form_common.cpp — 供应商表单的公共区块：表单状态读写、
// 预设模板区、通用字段（名称/官网/备注/URL/上游格式/API Key）、模型拉取
// 按钮、主模型行与备选模型清单。所有区块对 agent 中立——标签与必填规则
// 来自 AgentFormPolicy（各 agent 的策略常量在 provider_form_<agent>.cpp），
// 这里不出现任何 tool == 分支。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "app_resources.h"
#include "providers_internal.h"

import llmswitch.config;
import llmswitch.models;
import llmswitch.net;
import llmswitch.store;
import nlohmann.json;

namespace llmswitch::ui {

using provider_detail::FetchModelIdsWithFallback;

void FillForm(const FormStates& fs, const models::Provider& p) {
    fs.name = huxerui::TextEditingValue{p.name};
    fs.baseUrl = huxerui::TextEditingValue{p.baseUrl};
    fs.modelFetchUrl = huxerui::TextEditingValue{p.modelFetchUrl};
    fs.apiKey = huxerui::TextEditingValue{p.apiKey};
    fs.model = huxerui::TextEditingValue{p.model};
    fs.modelSupports1m = p.modelSupports1m;
    fs.website = huxerui::TextEditingValue{p.website};
    fs.notes = huxerui::TextEditingValue{p.notes};
    fs.toml = huxerui::TextEditingValue{p.codexConfigToml};
    fs.upstreamFormat = p.upstreamFormat == "anthropic" ? 0 : 1;
    fs.fullUrl = p.fullUrl;
    fs.apiFormat = p.apiFormat == "anthropic"      ? 1
                   : p.apiFormat == "openai-responses" ? 2
                                                       : 0;
    fs.haiku = huxerui::TextEditingValue{p.haikuModel};
    fs.sonnet = huxerui::TextEditingValue{p.sonnetModel};
    fs.opus = huxerui::TextEditingValue{p.opusModel};
    fs.haikuDisplayName = huxerui::TextEditingValue{p.haikuDisplayName};
    fs.sonnetDisplayName = huxerui::TextEditingValue{p.sonnetDisplayName};
    fs.opusDisplayName = huxerui::TextEditingValue{p.opusDisplayName};
    fs.haikuSupports1m = p.haikuSupports1m;
    fs.sonnetSupports1m = p.sonnetSupports1m;
    fs.opusSupports1m = p.opusSupports1m;
    fs.modelSearch = huxerui::TextEditingValue{};
    fs.haikuSearch = huxerui::TextEditingValue{};
    fs.sonnetSearch = huxerui::TextEditingValue{};
    fs.opusSearch = huxerui::TextEditingValue{};
    fs.presetCarry = p;
}

// apiFormat 下标 → Provider.apiFormat 存储值（0 = 默认 OpenAI 兼容，存空串）。
std::string ApiFormatFromIndex(int index) {
    if (index == 1) return "anthropic";
    if (index == 2) return "openai-responses";
    return "";
}

std::string UpstreamFormatFromIndex(int index) {
    return index == 0 ? "anthropic" : "openai";
}

// 新供应商的默认上游格式跟 agent 原生协议保持一致；用户仍可在表单中切换。
std::string DefaultUpstreamFormat(std::string_view tool) {
    if (tool == "claude-code" || tool == "claude") return "anthropic";
    return "openai";
}

const AgentFormPolicy& AgentPolicyFor(std::string_view tool) {
    if (tool == "claude-code") return ClaudeCodeFormPolicy();
    if (tool == "claude") return ClaudeDesktopFormPolicy();
    if (tool == "codex") return CodexFormPolicy();
    if (tool == "opencode" || tool == "pi" || tool == "dsh") {
        return OpenAiCliFormPolicy();
    }
    if (tool == "zcode") return ZcodeFormPolicy();
    // 未单列策略的 agent（gemini / qwen 等）：标签按注册表 flags 推导。
    static std::map<std::string, AgentFormPolicy> generic;
    auto it = generic.find(std::string(tool));
    if (it == generic.end()) {
        const auto* spec = models::findTool(tool);
        AgentFormPolicy p{.urlLabel = "URL",
                          .keyLabel = "API Key",
                          .urlRequired = true,
                          .keyRequired = false,
                          .primaryModelLabel = spec != nullptr && spec->needsModel
                                                   ? "模型（必填）"
                                                   : "主模型（可选）",
                          .showMappings = false,
                          .showApiFormat = false,
                          .showToml = false};
        it = generic.emplace(std::string(tool), std::move(p)).first;
    }
    return it->second;
}

[[huxerui::composable]] huxerui::View PresetsSection(
    std::string_view tool, const FormStates& fs,
    huxerui::StateList<std::string> fetchedModels) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    std::vector<huxerui::View> items;
    const auto groups = models::builtinPresets(tool);
    if (!groups.subscription.empty()) {
        items.push_back(
            huxerui::Text("订阅供应商（点选预填，订阅密钥作为 API Key 填写）")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.on_surface_variant}));
        std::vector<huxerui::View> chips;
        for (const auto& preset : groups.subscription) {
            chips.push_back(huxerui::Button(preset.name)
                                .OnClick([fs, fetchedModels, preset] {
                                    fetchedModels.Clear();
                                    FillForm(fs, preset);
                                }));
        }
        // 预设较多时用 Flow 自动换行，避免单行 Row 横向溢出。
        items.push_back(
            huxerui::Flow(std::move(chips)).With(huxerui::Spacing(8.0F)));
    }
    if (!groups.metered.empty()) {
        items.push_back(
            huxerui::Text("按量 API（点选预填，API Key 需自行填写）")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.on_surface_variant}));
        std::vector<huxerui::View> chips;
        for (const auto& preset : groups.metered) {
            chips.push_back(huxerui::Button(preset.name)
                                .OnClick([fs, fetchedModels, preset] {
                                    fetchedModels.Clear();
                                    FillForm(fs, preset);
                                }));
        }
        items.push_back(
            huxerui::Flow(std::move(chips)).With(huxerui::Spacing(8.0F)));
    }
    if (items.empty()) return huxerui::View{huxerui::Row{}};
    return huxerui::Column(std::move(items))
        .With(huxerui::Spacing(12.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View CommonFields(
    const AgentFormPolicy& policy, const FormStates& fs,
    huxerui::State<bool> showKey, huxerui::State<bool> keyHover,
    huxerui::State<bool> showModelFetchOptions) {
    std::vector<huxerui::View> fields;
    fields.push_back(huxerui::TextField(fs.name.Get())
        .Label("名称")
        .Variant(huxerui::TextFieldVariant::Outlined)
        .OnChanged([fs](const huxerui::TextEditingValue& v) { fs.name = v; }));
    fields.push_back(huxerui::TextField(fs.website.Get())
        .Label("官网（可选）")
        .Variant(huxerui::TextFieldVariant::Outlined)
        .OnChanged([fs](const huxerui::TextEditingValue& v) { fs.website = v; }));
    fields.push_back(huxerui::TextField(fs.notes.Get())
        .Label("备注（可选）")
        .Variant(huxerui::TextFieldVariant::Outlined)
        .OnChanged([fs](const huxerui::TextEditingValue& v) { fs.notes = v; }));
    fields.push_back(huxerui::Switch("完整 URL", fs.fullUrl.Get())
        .OnChanged([fs](bool checked) { fs.fullUrl = checked; }));
    fields.push_back(huxerui::TextField(fs.baseUrl.Get())
        .Label(policy.urlLabel)
        .Placeholder("https://...")
        .Variant(huxerui::TextFieldVariant::Outlined)
        .OnChanged([fs](const huxerui::TextEditingValue& v) { fs.baseUrl = v; }));
    fields.push_back(huxerui::Switch("高级选项", showModelFetchOptions.Get())
        .OnChanged([showModelFetchOptions](bool checked) {
            showModelFetchOptions = checked;
        }));
    if (showModelFetchOptions.Get()) {
        fields.push_back(huxerui::TextField(fs.modelFetchUrl.Get())
            .Label("模型获取 URL（可选，留空跟随 URL）")
            .Placeholder("完整地址，例如 https://.../v1/models")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([fs](const huxerui::TextEditingValue& v) {
                fs.modelFetchUrl = v;
            }));
    }
    {
        const std::array<std::string, 2> upstreamFormats{
            "Anthropic（默认后缀：/anthropic）", "OpenAI（默认后缀：/v1）"};
        fields.push_back(
            huxerui::Select(upstreamFormats,
                            static_cast<std::size_t>(fs.upstreamFormat.Get()),
                            [](const std::string& option) {
                                return huxerui::Text(option).Key(option);
                            })
                .Label("上游格式")
                .OnChanged([fs](std::size_t index) {
                    fs.upstreamFormat = static_cast<int>(index);
                })
                .With(huxerui::Enabled(!fs.fullUrl.Get())));
    }
    // API Key 行：Secure(!showKey) 掩码 + 内置交互 TrailingIcon 眼睛切换
    // 明文/掩码（受控值仍是同一 TextEditingValue，切换不丢内容）。眼睛
    // 仅悬停（或已明文）时挂载——非悬停态字段是干净的一行。
    auto keyField =
        huxerui::TextField(fs.apiKey.Get())
            .Label(policy.keyLabel)
            .Variant(huxerui::TextFieldVariant::Outlined)
            .Secure(!showKey.Get())
            .On<huxerui::ViewEvents::Hover>(
                [keyHover](const huxerui::HoverEvent& e) {
                    if (e.type == huxerui::HoverEventType::Enter) {
                        keyHover = true;
                    } else if (e.type == huxerui::HoverEventType::Leave) {
                        keyHover = false;
                    }
                })
            .OnChanged([fs](const huxerui::TextEditingValue& v) {
                fs.apiKey = v;
            });
    huxerui::View keyView = (keyHover.Get() || showKey.Get())
        ? huxerui::View{std::move(keyField)
                            .TrailingIcon(showKey.Get() ? app::images::eye_off
                                                        : app::images::eye,
                                          showKey.Get() ? "隐藏密钥" : "显示密钥")
                            .OnTrailingIconClick(
                                [showKey] { showKey = !showKey.Get(); })}
        : huxerui::View{std::move(keyField)};
    fields.push_back(std::move(keyView));
    return huxerui::Column(std::move(fields))
        .With(huxerui::Spacing(12.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View ModelFetchButton(
    const FormStates& fs, huxerui::State<bool> fetching,
    huxerui::StateList<std::string> fetchedModels, huxerui::TaskScope tasks,
    std::shared_ptr<huxerui::HttpClient> http, huxerui::ToastHandle toast) {
    // 「获取模型」：默认使用表单当前的实际 URL/apiKey/上游格式（实际 URL
    // 会按完整 URL 开关与上游格式计算）；高级选项可覆盖模型列表完整地址。
    // HuxerUI HttpClient 使用平台原生异步网络，协程恢复点恒为 UI 线程，
    // State 写回安全（State 只在 UI 线程写）。结果只进选择源 fetchedModels，
    // 由用户逐条挑选加入清单，不整包覆盖。
    const bool canFetch = !fetching.Get() && !fs.baseUrl.Get().text.empty() &&
                          !fs.apiKey.Get().text.empty();
    huxerui::View fetchButton =
        huxerui::Button(fetching.Get() ? "获取中…" : "获取模型列表")
            .OnClick([=] {
                const std::string fetchBase = models::effectiveBaseUrl(
                    fs.baseUrl.Get().text,
                    UpstreamFormatFromIndex(fs.upstreamFormat.Get()),
                    fs.fullUrl.Get());
                const std::string customFetchUrl = fs.modelFetchUrl.Get().text;
                const std::string f =
                    UpstreamFormatFromIndex(fs.upstreamFormat.Get());
                const std::vector<std::string> urls = customFetchUrl.empty()
                    ? net::modelListUrlCandidates(fetchBase, f)
                    : std::vector<std::string>{customFetchUrl};
                const std::string k = fs.apiKey.Get().text;
                // 模型列表端点属于上游 URL 协议，不能复用 opencode/pi 的
                // apiFormat，也不能按 claude-code 的工具类型猜测。
                fetching = true;
                tasks.Launch([=]() -> huxerui::Task<void> {
                    try {
                        auto models = co_await FetchModelIdsWithFallback(
                            http, urls, k, f);
                        fetching = false;
                        const std::size_t modelCount = models.size();
                        ReplaceModelList(fetchedModels, std::move(models));
                        toast.Show(std::format("已获取 {} 个模型", modelCount));
                    } catch (const std::exception& e) {
                        fetching = false;
                        toast.Show(e.what());
                    }
                });
            })
            .With(huxerui::Enabled(canFetch),
                  huxerui::Tooltip(canFetch
                                       ? "按模型获取 URL + API Key 拉取模型列表"
                                       : "请先填写 Base URL 与 API Key"));
    return huxerui::Row{std::move(fetchButton)}
        .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

[[huxerui::composable]] huxerui::View PrimaryModelRow(
    const AgentFormPolicy& policy, const FormStates& fs,
    huxerui::StateList<std::string> fetchedModels) {
    return huxerui::Column {
        huxerui::Row {
            huxerui::TextField(fs.model.Get())
                .Label(policy.primaryModelLabel)
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([fs](const huxerui::TextEditingValue& v) {
                    fs.model = v;
                })
                .With(huxerui::Grow(1.0F)),
            fetchedModels.Empty()
                ? huxerui::View{huxerui::Row{}}
                : ModelSelect(fetchedModels, fs.modelSearch, fs.model),
            huxerui::Checkbox("1M", fs.modelSupports1m.Get())
                .OnChanged([fs](bool checked) {
                    fs.modelSupports1m = checked;
                }),
        }.With(huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
    }.With(huxerui::Spacing(8.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace llmswitch::ui
