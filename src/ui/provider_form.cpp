// provider_form.cpp — 供应商新增/编辑与用量配置表单.
#include <huxerui/huxerui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <functional>
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

namespace {


// 表单字段集合：新增/编辑共用一组 State 句柄（State 是可拷贝句柄，
// 归打开表单页的组合作用域所有）。upstreamFormat 为上游 URL 格式下标
// （0=Anthropic，1=OpenAI）；fullUrl 打开时 URL 原样使用，不追加默认后缀。
// apiFormat 为 opencode / pi 的 API 适配器下标（0=OpenAI 兼容（默认，
// 存空串）/ 1=anthropic / 2=openai-responses）。
// model 及 haiku/sonnet/opus 为模型字段；后三档映射（仅
// hasModelMappings 工具展示）每档包含菜单显示名、实际请求模型和
// supports1m 声明。各模型下拉只保留搜索值：选中后清空控件显示，实际模型
// 始终由对应的 TextField 状态保存。
// 用量查询配置不在此——已拆到独立的 UsageFormPage（卡片 gauge 按钮进入）。
struct FormStates {
    huxerui::State<huxerui::TextEditingValue> name;
    huxerui::State<huxerui::TextEditingValue> baseUrl;
    huxerui::State<huxerui::TextEditingValue> modelFetchUrl;
    huxerui::State<huxerui::TextEditingValue> apiKey;
    huxerui::State<huxerui::TextEditingValue> model;
    huxerui::State<bool> modelSupports1m;
    huxerui::State<huxerui::TextEditingValue> website;
    huxerui::State<huxerui::TextEditingValue> notes;
    huxerui::State<huxerui::TextEditingValue> toml;  // 仅 codex 组展示
    huxerui::State<int> upstreamFormat;
    huxerui::State<bool> fullUrl;
    huxerui::State<int> apiFormat;                   // 仅 opencode / pi 展示
    huxerui::State<huxerui::TextEditingValue> haiku;   // 仅 claude 系展示
    huxerui::State<huxerui::TextEditingValue> sonnet;
    huxerui::State<huxerui::TextEditingValue> opus;
    huxerui::State<huxerui::TextEditingValue> haikuDisplayName;
    huxerui::State<huxerui::TextEditingValue> sonnetDisplayName;
    huxerui::State<huxerui::TextEditingValue> opusDisplayName;
    huxerui::State<bool> haikuSupports1m;
    huxerui::State<bool> sonnetSupports1m;
    huxerui::State<bool> opusSupports1m;
    huxerui::State<huxerui::TextEditingValue> modelSearch;
    huxerui::State<huxerui::TextEditingValue> haikuSearch;
    huxerui::State<huxerui::TextEditingValue> sonnetSearch;
    huxerui::State<huxerui::TextEditingValue> opusSearch;
};

// 表单状态初始化（ProviderFormPage 用）：hcg 要求 composable 返回 View（不能
// 抽返回 FormStates 的 composable 辅助），宏在调用点展开。各字段以入参
// provider 为初值——UseState 初值只在首次组合生效，配合表单页的
// .Key("form:" + target) 换编辑目标即整体重建状态。
#define LLMSWITCH_FORM_STATES_INIT(p)                                       \
    {huxerui::UseState(huxerui::TextEditingValue{(p).name}),                \
     huxerui::UseState(huxerui::TextEditingValue{(p).baseUrl}),             \
     huxerui::UseState(huxerui::TextEditingValue{(p).modelFetchUrl}),        \
     huxerui::UseState(huxerui::TextEditingValue{(p).apiKey}),              \
     huxerui::UseState(huxerui::TextEditingValue{(p).model}),               \
     huxerui::UseState((p).modelSupports1m),                                \
     huxerui::UseState(huxerui::TextEditingValue{(p).website}),             \
     huxerui::UseState(huxerui::TextEditingValue{(p).notes}),               \
     huxerui::UseState(huxerui::TextEditingValue{(p).codexConfigToml}),     \
     huxerui::UseState((p).upstreamFormat == "anthropic" ? 0 : 1),          \
     huxerui::UseState((p).fullUrl),                                         \
     huxerui::UseState((p).apiFormat == "anthropic"         ? 1             \
                      : (p).apiFormat == "openai-responses" ? 2             \
                                                            : 0),           \
     huxerui::UseState(huxerui::TextEditingValue{(p).haikuModel}),          \
     huxerui::UseState(huxerui::TextEditingValue{(p).sonnetModel}),         \
     huxerui::UseState(huxerui::TextEditingValue{(p).opusModel}),           \
     huxerui::UseState(huxerui::TextEditingValue{(p).haikuDisplayName}),    \
     huxerui::UseState(huxerui::TextEditingValue{(p).sonnetDisplayName}),   \
     huxerui::UseState(huxerui::TextEditingValue{(p).opusDisplayName}),     \
     huxerui::UseState((p).haikuSupports1m),                                \
     huxerui::UseState((p).sonnetSupports1m),                               \
     huxerui::UseState((p).opusSupports1m),                                 \
     huxerui::UseState(huxerui::TextEditingValue{}),                         \
     huxerui::UseState(huxerui::TextEditingValue{}),                         \
     huxerui::UseState(huxerui::TextEditingValue{}),                         \
     huxerui::UseState(huxerui::TextEditingValue{})}

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

} // namespace


// 新增/编辑供应商页（整页表单，不再是弹窗——字段太多弹窗太挤）。
// 表单状态以 initial 为初值（UseState 初值只在首次组合生效；调用方用
// .Key("form:" + target) 保证换编辑目标整体重建）。isNew 时顶部内嵌预设
// 模板区（点选 FillForm 预填）。校验：名称必填；非 codex 组 baseUrl 必填；
// codex 组 apiKey 必填；needsModel 组模型必填。保存成功 toast 后返回列表
// （写 formTarget 会卸载点击节点：经 closeTasks.Launch + Delay(0) 推迟；
// closeTasks 是父级页面的 scope——本页 scope 会随 formTarget 写入一起
// 卸载，不能承载关闭任务本身）。
[[huxerui::composable]] huxerui::View ProviderFormPage(
    std::string tool, models::Provider initial, bool isNew,
    huxerui::State<int> revision, huxerui::State<std::string> formTarget,
    huxerui::TaskScope closeTasks) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    const auto http = huxerui::UseService<huxerui::HttpClient>();
    const auto* spec = models::findTool(tool);
    const bool isCodex = tool == "codex";
    const bool needsModel = spec != nullptr && spec->needsModel;
    const bool hasApiFormat = spec != nullptr && spec->hasApiFormat;
    const bool hasMappings = spec != nullptr && spec->hasModelMappings;
    // 新增表单默认使用 agent 原生协议并关闭完整 URL；编辑则严格沿用
    // 已保存的模式（旧配置由 models::providerFromJson 标记为完整 URL）。
    models::Provider formInitial = initial;
    if (isNew) {
        formInitial.upstreamFormat = DefaultUpstreamFormat(tool);
        formInitial.fullUrl = false;
    }
    const FormStates fs LLMSWITCH_FORM_STATES_INIT(formInitial);
    const std::string editingId = isNew ? "" : initial.id;
    // 「获取模型」拉取状态：fetching 驱动按钮加载态。fetchedModels 只是
    // fetch 结果的选择源（供各选择弹层挑选），不直接变成清单；modelList
    // 才是供应商的模型清单（zcode：唯一模型输入；其他工具：备选缓存），
    // 初值 = 已保存清单，由逐条添加/移除维护。两个清单都必须走
    // UseStateList 初值重载（和 UseState 一样只在首次组合生效）：在组合体
    // 里逐项 PushBack 会在每次重组合时重复追加，弹出列表的重复 key 直接
    // abort。
    auto fetching = huxerui::UseState(false);
    auto fetchedModels = huxerui::UseStateList<std::string>();
    auto modelList = huxerui::UseStateList(DedupeModels(formInitial.models));
    // 每模型参数原值（zcode：reasoning/识图等；编辑面板直接改这份）。
    auto modelMeta = huxerui::UseState(formInitial.modelsMeta);
    // 展开的模型参数行下标（-1 = 全部收起）。
    auto expandedMeta = huxerui::UseState(-1);
    // 展开行的 limit 文本输入（context / output），展开时从元数据装载。
    auto contextInput = huxerui::UseState(huxerui::TextEditingValue{});
    auto outputInput = huxerui::UseState(huxerui::TextEditingValue{});
    // ZCode 条目的启用开关：状态以 live 条目的 enabled 为初值，保存时写回。
    auto zcodeEnabled = huxerui::UseState(
        tool == "zcode" && providerStore().zcodeEntryEnabled(initial.id));
    // 清单「添加」行的输入框。
    auto addModel = huxerui::UseState(huxerui::TextEditingValue{});
    auto showModelFetchOptions =
        huxerui::UseState(!formInitial.modelFetchUrl.empty());
    // API Key 明文开关：Secure(bool) 切换掩码，眼睛按钮用 SDK 内置的可交互
    // TrailingIcon（icon + 语义标签 → OnTrailingIconClick 事件），且只在悬停
    // 输入框任意位置（或已明文）时才显示（ViewEvents::Hover 只在 Enter/Leave
    // 写 State，Move 不重组合）。
    auto showKey = huxerui::UseState(false);
    auto keyHover = huxerui::UseState(false);

    // 返回列表（写 formTarget 会卸载本页与点击路径上的节点：推迟出指针
    // 事件路径；任务挂父级 closeTasks——本页 scope 随写入一起销毁）。
    auto goBack = [closeTasks, formTarget] {
        closeTasks.Launch([formTarget]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            formTarget = "";
        });
    };

    std::vector<huxerui::View> fields;
    // 预设模板区（仅新增）：点选预填表单；清掉可能已拉取的旧模型列表。
    // 订阅站与按量 API 分两行展示——Row 不换行，分组避免窄窗口横向溢出。
    if (isNew) {
        const auto groups = models::builtinPresets(tool);
        if (!groups.subscription.empty()) {
            fields.push_back(
                huxerui::Text("订阅供应商（点选预填，订阅密钥作为 API Key 填写）")
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kCaption),
                        theme.colors.on_surface_variant}));
            std::vector<huxerui::View> chips;
            for (const auto& preset : groups.subscription) {
                chips.push_back(
                    huxerui::Button(preset.name)
                        .OnClick([fs, fetchedModels, preset] {
                            fetchedModels.Clear();
                            FillForm(fs, preset);
                        }));
            }
            fields.push_back(huxerui::Row(std::move(chips))
                                 .With(huxerui::Spacing(8.0F)));
        }
        if (!groups.metered.empty()) {
            fields.push_back(
                huxerui::Text("按量 API（点选预填，API Key 需自行填写）")
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kCaption),
                        theme.colors.on_surface_variant}));
            std::vector<huxerui::View> chips;
            for (const auto& preset : groups.metered) {
                chips.push_back(
                    huxerui::Button(preset.name)
                        .OnClick([fs, fetchedModels, preset] {
                            fetchedModels.Clear();
                            FillForm(fs, preset);
                        }));
            }
            fields.push_back(huxerui::Row(std::move(chips))
                                 .With(huxerui::Spacing(8.0F)));
        }
    }
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
        .Label(isCodex ? "URL（可选，codex 以 config.toml 为准）" : "URL")
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
    {
        // API Key 行：Secure(!showKey) 掩码 + 内置交互 TrailingIcon 眼睛切换
        // 明文/掩码（受控值仍是同一 TextEditingValue，切换不丢内容）。眼睛
        // 仅悬停（或已明文）时挂载——非悬停态字段是干净的一行。
        const char* keyLabel =
            isCodex ? "API Key（写入 auth.json）" : "API Key";
        auto keyField =
            huxerui::TextField(fs.apiKey.Get())
                .Label(keyLabel)
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
    }
    {
        // 模型输入：ZCode 条目只有模型清单、没有主模型概念（选模型是
        // ZCode 运行时行为），zcode 页删除主模型行，模型清单即唯一输入；
        // 其余工具保留主模型行（claude-code 另有 1M 勾选）。
        const bool zcodeModels = tool == "zcode";
        const char* modelLabel = needsModel ? "模型（必填）"
            : tool == "claude-code"    ? "主模型（可选，写入 ANTHROPIC_MODEL）"
            : tool == "codex"
                ? "模型（可选，切换时写入 config.toml 顶层 model）"
                                       : "主模型（可选）";
        // 「获取模型」：默认使用表单当前的实际 URL/apiKey/上游格式（实际 URL
        // 会按完整 URL 开关与上游格式计算）；高级选项可覆盖模型列表完整地址。
        // HuxerUI HttpClient 使用平台原生异步网络，协程恢复点恒为 UI 线程，
        // State 写回安全（State 只在 UI 线程写）。结果只进选择源 fetchedModels，
        // 由用户逐条挑选加入清单，不整包覆盖。
        const bool canFetch = !fetching.Get() && !fs.baseUrl.Get().text.empty() &&
                              !fs.apiKey.Get().text.empty();
        auto fetchButton = huxerui::Button(
                               fetching.Get() ? "获取中…" : "获取模型列表")
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
        if (!zcodeModels) {
            fields.push_back(huxerui::Column {
                huxerui::Row {
                    huxerui::TextField(fs.model.Get())
                        .Label(modelLabel)
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
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
        }
        if (zcodeModels) {
            // 对应 ZCode 条目的 enabled 字段（ZCode 页面里每个供应商都有
            // 启用/停用开关）；保存时随条目写回，不影响切换的互斥语义。
            fields.push_back(
                huxerui::Switch("在 ZCode 中启用此供应商", zcodeEnabled.Get())
                    .OnChanged([zcodeEnabled](bool checked) {
                        zcodeEnabled = checked;
                    }));
        }
        fields.push_back(huxerui::Row{std::move(fetchButton)}
                             .With(huxerui::CrossAlign(
                                 huxerui::CrossAxisAlignment::Center)));
        // 清单添加：输入框手动填，或从选择源弹层点选直接加入一条；重复
        // 提示，不清空已建清单。主模型字段为空时首个加入的模型顺便回填
        // （仅非 zcode 工具展示主模型）。
        const auto addToList = [modelList, addModel, fs, toast](
                                   std::string id) {
            const auto first = id.find_first_not_of(" \t");
            if (first == std::string::npos) {
                return;
            }
            const auto last = id.find_last_not_of(" \t");
            id = id.substr(first, last - first + 1);
            for (std::size_t i = 0; i < modelList.Size(); ++i) {
                if (modelList.At(i) == id) {
                    toast.Show("该模型已在清单中");
                    return;
                }
            }
            modelList.PushBack(id);
            if (fs.model.Get().text.empty()) {
                fs.model = huxerui::TextEditingValue{id};
            }
            addModel = huxerui::TextEditingValue{};
        };
        fields.push_back(huxerui::Text(zcodeModels
                                           ? "模型清单（必填，保存即写入 ZCode）"
                                           : "备选模型（可增删）")
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                theme.colors.on_surface_variant}));
        for (std::size_t i = 0; i < modelList.Size(); ++i) {
            const std::string id = modelList.At(i);
            fields.push_back(
                huxerui::Row {
                    huxerui::Text(id)
                        .Style(huxerui::TextStyle{
                            huxerui::Font::System(font_size::kBody),
                            theme.colors.on_surface})
                        .With(huxerui::Grow(1.0F)),
                    zcodeModels
                        ? huxerui::View{huxerui::IconButton(
                                            app::images::edit, "模型参数")
                                            .OnClick([expandedMeta, modelMeta,
                                                      contextInput, outputInput,
                                                      i, id] {
                                                if (expandedMeta.Get() ==
                                                    static_cast<int>(i)) {
                                                    expandedMeta = -1;
                                                    return;
                                                }
                                                expandedMeta =
                                                    static_cast<int>(i);
                                                const auto& m = modelMeta.Get();
                                                contextInput =
                                                    huxerui::TextEditingValue{
                                                        ModelLimitText(
                                                            m, id, "context")};
                                                outputInput =
                                                    huxerui::TextEditingValue{
                                                        ModelLimitText(
                                                            m, id, "output")};
                                            })}
                        : huxerui::View{huxerui::Row{}},
                    huxerui::IconButton(app::images::trash, "移除")
                        .OnClick([modelList, expandedMeta, i] {
                            modelList.Erase(i);
                            expandedMeta = -1;
                        }),
                }.With(huxerui::Spacing(8.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
            if (zcodeModels && expandedMeta.Get() == static_cast<int>(i)) {
                // 每模型参数编辑面板：直接改 modelsMeta 原值（收编自 ZCode
                // 的 reasoning/limit/modalities/zcode 元数据），未提供控件的
                // 字段保存时按原值回放。输入/输出模态复选框与 ZCode 页面
                // 一一对应（数组值 text/image/video/pdf）。
                const auto& meta = modelMeta.Get();
                fields.push_back(
                    huxerui::Column {
                        huxerui::Row {
                            huxerui::Text("输入类型")
                                .Style(huxerui::TextStyle{
                                    huxerui::Font::System(font_size::kBody),
                                    theme.colors.on_surface_variant}),
                            huxerui::Checkbox("文本",
                                              ModelHasModality(meta, id, "input", "text"))
                                .OnChanged([modelMeta, id](bool on) {
                                    modelMeta = WithModelModality(
                                        modelMeta.Get(), id, "input", "text",
                                        on);
                                }),
                            huxerui::Checkbox("图片",
                                              ModelHasModality(meta, id, "input", "image"))
                                .OnChanged([modelMeta, id](bool on) {
                                    modelMeta = WithModelModality(
                                        modelMeta.Get(), id, "input", "image",
                                        on);
                                }),
                            huxerui::Checkbox("视频",
                                              ModelHasModality(meta, id, "input", "video"))
                                .OnChanged([modelMeta, id](bool on) {
                                    modelMeta = WithModelModality(
                                        modelMeta.Get(), id, "input", "video",
                                        on);
                                }),
                            huxerui::Checkbox("PDF",
                                              ModelHasModality(meta, id, "input", "pdf"))
                                .OnChanged([modelMeta, id](bool on) {
                                    modelMeta = WithModelModality(
                                        modelMeta.Get(), id, "input", "pdf", on);
                                }),
                        }.With(huxerui::Spacing(10.0F),
                               huxerui::CrossAlign(
                                   huxerui::CrossAxisAlignment::Center)),
                        huxerui::Row {
                            huxerui::Text("输出类型")
                                .Style(huxerui::TextStyle{
                                    huxerui::Font::System(font_size::kBody),
                                    theme.colors.on_surface_variant}),
                            huxerui::Checkbox("文本",
                                              ModelHasModality(meta, id, "output", "text"))
                                .OnChanged([modelMeta, id](bool on) {
                                    modelMeta = WithModelModality(
                                        modelMeta.Get(), id, "output", "text",
                                        on);
                                }),
                        }.With(huxerui::Spacing(10.0F),
                               huxerui::CrossAlign(
                                   huxerui::CrossAxisAlignment::Center)),
                        huxerui::Row {
                            huxerui::TextField(contextInput.Get())
                                .Label("上下文窗口（tokens）")
                                .Placeholder("例如 1000000")
                                .Variant(huxerui::TextFieldVariant::Outlined)
                                .OnChanged([modelMeta, id,
                                            contextInput](
                                               const huxerui::TextEditingValue& v) {
                                    contextInput = v;
                                    ApplyModelLimit(modelMeta, id, "context",
                                                    v.text);
                                })
                                .With(huxerui::Grow(1.0F)),
                            huxerui::TextField(outputInput.Get())
                                .Label("最大输出（tokens）")
                                .Placeholder("例如 128000")
                                .Variant(huxerui::TextFieldVariant::Outlined)
                                .OnChanged([modelMeta, id,
                                            outputInput](
                                               const huxerui::TextEditingValue& v) {
                                    outputInput = v;
                                    ApplyModelLimit(modelMeta, id, "output",
                                                    v.text);
                                })
                                .With(huxerui::Grow(1.0F)),
                        }.With(huxerui::Spacing(10.0F),
                               huxerui::CrossAlign(
                                   huxerui::CrossAxisAlignment::Center)),
                        huxerui::Switch("思维链（reasoning）",
                                        ModelHasReasoning(modelMeta.Get(), id))
                            .OnChanged([modelMeta, id](bool on) {
                                modelMeta = WithModelMeta(
                                    modelMeta.Get(), id,
                                    [on](nlohmann::json& m) {
                                        if (!m.contains("reasoning") ||
                                            !m["reasoning"].is_object()) {
                                            m["reasoning"] =
                                                nlohmann::json::object();
                                        }
                                        m["reasoning"]["enabled"] = on;
                                    });
                            }),
                        huxerui::Text("思考档位等其余参数按 ZCode 原值保留，可在 ZCode 内修改")
                            .Style(huxerui::TextStyle{
                                huxerui::Font::System(font_size::kCaption),
                                theme.colors.on_surface_variant}),
                    }.With(huxerui::Spacing(6.0F),
                           huxerui::Padding(
                               huxerui::EdgeInsets::Symmetric(16.0F, 0.0F))));
            }
        }
        fields.push_back(huxerui::Row {
            huxerui::TextField(addModel.Get())
                .Label("添加模型")
                .Placeholder("输入模型 ID")
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([addModel](const huxerui::TextEditingValue& v) {
                    addModel = v;
                })
                .With(huxerui::Grow(1.0F)),
            fetchedModels.Empty()
                ? huxerui::View{huxerui::Row{}}
                : ModelSelect(fetchedModels, fs.modelSearch, addModel, {},
                              addToList),
            huxerui::Button("添加")
                .OnClick([addToList, addModel] {
                    addToList(addModel.Get().text);
                }),
        }.With(huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
    }
    if (hasMappings) {
        // 三档模型映射（claude-code 写 ANTHROPIC_DEFAULT_*_MODEL env；
        // claude desktop 写 inferenceModels 映射条目），均选填，共享同一份
        // fetchedModels 下拉。实际请求模型仍是现有三档 *Model 字段；显示名
        // 与 supports1m 仅用于 Claude Desktop 菜单元数据。
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
                       huxerui::CrossAlign(
                           huxerui::CrossAxisAlignment::Center)),
            }.With(huxerui::Spacing(6.0F),
                   huxerui::CrossAlign(
                       huxerui::CrossAxisAlignment::Stretch)));
        }
    }
    if (hasApiFormat) {
        // API 协议：写进 opencode 的 npm 段 / pi 的 api 字段（见 store）。
        fields.push_back(huxerui::Text("API 协议")
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                theme.colors.on_surface_variant}));
        fields.push_back(
            huxerui::SegmentedButton(
                {"OpenAI 兼容", "Anthropic", "OpenAI Responses"},
                static_cast<std::size_t>(fs.apiFormat.Get()))
                .OnChanged([fs](std::size_t index) {
                    fs.apiFormat = static_cast<int>(index);
                }));
    }
    // 用量查询配置不在这里——卡片 gauge 按钮进 UsageFormPage 独立配置。
    if (isCodex) {
        fields.push_back(
            huxerui::Text("config.toml 原文（可选；切换时整体替换）")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.on_surface_variant}));
        fields.push_back(huxerui::TextField(fs.toml.Get())
            .Variant(huxerui::TextFieldVariant::Outlined)
            .LineLimits(huxerui::TextFieldLineLimits::MultiLine(6, 12))
            .OnChanged([fs](const huxerui::TextEditingValue& v) { fs.toml = v; }));
    }

    const std::string title =
        isNew ? "新增供应商 — " + std::string(ToolName(tool))
              : "编辑供应商 — " + initial.name;
    return PageScaffold(
        title,
        huxerui::Row {
            huxerui::Button("返回").OnClick([goBack] { goBack(); }),
        },
        huxerui::Column {
            huxerui::ScrollView(
                huxerui::Column(std::move(fields))
                    .With(huxerui::Spacing(12.0F),
                          huxerui::CrossAlign(
                              huxerui::CrossAxisAlignment::Stretch)))
                .With(huxerui::Grow(1.0F)),
            huxerui::Row {
                huxerui::Button("取消").OnClick([goBack] { goBack(); }),
                huxerui::Button(isNew ? "添加" : "保存")
                    .OnClick([=] {
                        const std::string name = fs.name.Get().text;
                        const std::string baseUrl = fs.baseUrl.Get().text;
                        const std::string apiKey = fs.apiKey.Get().text;
                        const std::string model = fs.model.Get().text;
                        if (name.empty()) {
                            toast.Show("名称不能为空");
                            return;
                        }
                        if (!isCodex && baseUrl.empty()) {
                            toast.Show("URL 不能为空");
                            return;
                        }
                        if (fs.fullUrl.Get() && baseUrl.empty()) {
                            toast.Show("完整 URL 不能为空");
                            return;
                        }
                        if (isCodex && apiKey.empty()) {
                            toast.Show("API Key 不能为空");
                            return;
                        }
                        if (needsModel && tool != "zcode" && model.empty()) {
                            toast.Show("模型不能为空");
                            return;
                        }
                        if (tool == "zcode" && modelList.Empty()) {
                            toast.Show("模型清单不能为空");
                            return;
                        }
                        models::Provider p;
                        p.id = editingId;
                        p.name = name;
                        p.baseUrl = baseUrl;
                        p.modelFetchUrl = fs.modelFetchUrl.Get().text;
                        p.apiKey = apiKey;
                        p.model = model;
                        // 模型清单 = 表单当前清单（初值来自已保存清单，手动
                        // 增删都体现在这里）。ZCode 没有主模型概念：主模型
                        // 仅为本应用展示用默认值，取清单首项。
                        for (std::size_t i = 0; i < modelList.Size(); ++i) {
                            p.models.push_back(modelList.At(i));
                        }
                        if (tool == "zcode") {
                            p.model = p.models.empty() ? "" : p.models.front();
                            p.modelsMeta = modelMeta.Get();
                        }
                        p.modelSupports1m = fs.modelSupports1m.Get();
                        p.upstreamFormat =
                            UpstreamFormatFromIndex(fs.upstreamFormat.Get());
                        p.fullUrl = fs.fullUrl.Get();
                        p.website = fs.website.Get().text;
                        p.notes = fs.notes.Get().text;
                        p.codexConfigToml = fs.toml.Get().text;
                        p.apiFormat = ApiFormatFromIndex(fs.apiFormat.Get());
                        p.haikuModel = fs.haiku.Get().text;
                        p.sonnetModel = fs.sonnet.Get().text;
                        p.opusModel = fs.opus.Get().text;
                        p.haikuDisplayName = fs.haikuDisplayName.Get().text;
                        p.sonnetDisplayName = fs.sonnetDisplayName.Get().text;
                        p.opusDisplayName = fs.opusDisplayName.Get().text;
                        p.haikuSupports1m = fs.haikuSupports1m.Get();
                        p.sonnetSupports1m = fs.sonnetSupports1m.Get();
                        p.opusSupports1m = fs.opusSupports1m.Get();
                        // 用量查询配置归 UsageFormPage 管，编辑保留原值。
                        p.usageEnabled = initial.usageEnabled;
                        p.usageRefreshMinutes = initial.usageRefreshMinutes;
                        p.usageUrl = initial.usageUrl;
                        p.usagePath = initial.usagePath;
                        p.usageLabel = initial.usageLabel;
                        std::string savedId = editingId;
                        try {
                            if (editingId.empty()) {
                                savedId =
                                    providerStore().addProvider(tool,
                                                                std::move(p));
                            } else {
                                // 编辑保留原创建时间。
                                for (const auto& cur :
                                     providerStore().group(tool).providers) {
                                    if (cur.id == editingId) {
                                        p.createdAt = cur.createdAt;
                                        break;
                                    }
                                }
                                providerStore().updateProvider(tool, p);
                            }
                            if (tool == "zcode") {
                                providerStore().setZcodeEntryEnabled(
                                    savedId, zcodeEnabled.Get());
                            }
                        } catch (const std::exception& e) {
                            toast.Show(e.what());
                            return;
                        }
                        revision = revision.Get() + 1;
                        toast.Show(editingId.empty() ? "已添加" : "已保存");
                        goBack();
                    }),
            }.With(huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween)),
        }.With(huxerui::Spacing(12.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

} // namespace llmswitch::ui
