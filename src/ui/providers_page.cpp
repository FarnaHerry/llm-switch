// providers_page.cpp — 供应商列表页：各 agent 工具组共用同一组件（参数化
// currentTool State，注册表见 models::toolRegistry()）。工具图标栏在供应商
// 岛屿头部行左侧（ToolBar，选中项实心变体 + raised 底块高亮，换工具写
// currentTool，外层 .Key(tool) 重建整页；头部行右侧是新增按钮，无标题
// 文字）。每个供应商一张卡片三段式：左信息列（名称 / 实际访问 URL / 备注 /
// 「使用中」徽章（group.current 或 detectCurrent 命中），
// Grow 吃满剩余宽度）｜ 中间状态列（连通检测延迟 + 用量文本/刷新图标，
// 垂直居中落在内容与操作组之间，两者皆无时塌缩为零宽）｜ 右侧操作图标组
// （切换 swap / 联通检测 activity / 编辑 edit / 用量查询配置 gauge /
// 复制 copy / 删除 trash，自绘 SVG + Tooltip，删除走内置确认框）。联通检测经
// net::pingLatencyMs（RunWorker 派到 worker 线程），连通后卡片显示
// 「延迟 N ms」，失败显示「不可达：…」（error 色）。有官方厂商的工具
// （claude-code / claude / codex，models::officialVendorName）列表第一位固定
// 一张「官方」常驻卡：切换 = store.restoreOfficial 还原厂商原生状态，
// active = 组 current 与 detectCurrent 均为空。头部只有一个加号 IconButton
// 进入新增页；编辑/新增都是整页表单（ProviderFormPage，字段太多弹窗太挤），
// 新增页顶部内嵌预设模板区（点选预填）；用量查询配置是独立整页
// UsageFormPage（formTarget = "usage:" + id 进入）。
// 表单按 ToolSpec 适配：完整 URL switch 与上游格式 Select 控制 URL 后缀，codex
// 显示 config.toml 原文、needsModel（opencode/pi）模型必填、hasApiFormat 显示
// API 协议分段选择；hasModelMappings（claude-code /
// claude）额外显示三档模型映射行（Haiku/Sonnet/Opus）。模型字段旁「获取模型」
// 按当前实际 URL/apiKey/apiFormat 经 llmswitch.net 拉取模型列表（阻塞网络调用
// 经 huxerui::RunWorker 派到 worker 线程，结果回 UI 线程写 State）；拉取成功
// 后模型行在按钮前出现 Select 下拉，点选回填该行的模型字段（不弹窗）。
// 用量查询：usageUrl 非空的卡片显示用量文本 + 手动刷新按钮；页面可见期间
// 按 config 的 usageEnabled/usageRefreshMinutes 轮询全部配置了 usageUrl 的
// 供应商，缓存为页面级 State（只在 UI 线程写）。
//
// 数据流：所有 store 读写都在 UI 线程（store 无内部锁，UI 线程独占是契约；
// live 文件读写为微秒级本地 IO，不经任务线程）。写操作后 revision+1，
// 驱动本页重读、托盘菜单重建。detectCurrent 在页面首组合跑一次。
// 列表多模式：formTarget State（"" = 列表；"new" = 新增；"usage:"+id = 用量
// 配置页；否则 = 编辑的 provider id）驱动末尾单 return 多选一，所有
// UseState 都在分支之前。
#include <huxerui/huxerui.h>

#include <array>
#include <chrono>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "ui.h"
#include "app_resources.h"

import llmswitch.config;
import llmswitch.models;
import llmswitch.net;
import llmswitch.store;

namespace llmswitch::ui {
namespace {

// 表单字段集合：新增/编辑共用一组 State 句柄（State 是可拷贝句柄，
// 归打开表单页的组合作用域所有）。upstreamFormat 为上游 URL 格式下标
// （0=Anthropic，1=OpenAI）；fullUrl 打开时 URL 原样使用，不追加默认后缀。
// apiFormat 为 opencode / pi 的 API 适配器下标（0=OpenAI 兼容（默认，
// 存空串）/ 1=anthropic / 2=openai-responses）。
// haiku/sonnet/opus 为三档模型映射（仅 hasModelMappings 工具展示）；
// selModel/selHaiku/selSonnet/selOpus 为各行模型下拉的选中下标。
// 用量查询三字段不在此——已拆到独立的 UsageFormPage（卡片 gauge 按钮进入）。
struct FormStates {
    huxerui::State<huxerui::TextEditingValue> name;
    huxerui::State<huxerui::TextEditingValue> baseUrl;
    huxerui::State<huxerui::TextEditingValue> apiKey;
    huxerui::State<huxerui::TextEditingValue> model;
    huxerui::State<huxerui::TextEditingValue> website;
    huxerui::State<huxerui::TextEditingValue> notes;
    huxerui::State<huxerui::TextEditingValue> toml;  // 仅 codex 组展示
    huxerui::State<int> upstreamFormat;
    huxerui::State<bool> fullUrl;
    huxerui::State<int> apiFormat;                   // 仅 opencode / pi 展示
    huxerui::State<huxerui::TextEditingValue> haiku;   // 仅 claude 系展示
    huxerui::State<huxerui::TextEditingValue> sonnet;
    huxerui::State<huxerui::TextEditingValue> opus;
    huxerui::State<std::size_t> selModel;
    huxerui::State<std::size_t> selHaiku;
    huxerui::State<std::size_t> selSonnet;
    huxerui::State<std::size_t> selOpus;
};

// 表单状态初始化（ProviderFormPage 用）：hcg 要求 composable 返回 View（不能
// 抽返回 FormStates 的 composable 辅助），宏在调用点展开。各字段以入参
// provider 为初值——UseState 初值只在首次组合生效，配合表单页的
// .Key("form:" + target) 换编辑目标即整体重建状态。
#define LLMSWITCH_FORM_STATES_INIT(p)                                       \
    {huxerui::UseState(huxerui::TextEditingValue{(p).name}),                \
     huxerui::UseState(huxerui::TextEditingValue{(p).baseUrl}),             \
     huxerui::UseState(huxerui::TextEditingValue{(p).apiKey}),              \
     huxerui::UseState(huxerui::TextEditingValue{(p).model}),               \
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
     huxerui::UseState(std::size_t{0}),                                     \
     huxerui::UseState(std::size_t{0}),                                     \
     huxerui::UseState(std::size_t{0}),                                     \
     huxerui::UseState(std::size_t{0})}

void FillForm(const FormStates& fs, const models::Provider& p) {
    fs.name = huxerui::TextEditingValue{p.name};
    fs.baseUrl = huxerui::TextEditingValue{p.baseUrl};
    fs.apiKey = huxerui::TextEditingValue{p.apiKey};
    fs.model = huxerui::TextEditingValue{p.model};
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
    fs.selModel = std::size_t{0};
    fs.selHaiku = std::size_t{0};
    fs.selSonnet = std::size_t{0};
    fs.selOpus = std::size_t{0};
}

// 用量缓存：providerId → 展示文本（含「查询失败：…」错误文本），页面级 State，
// 只在 UI 线程写（RunWorker 协程恢复点恒为 UI 线程）。
using UsageCache = huxerui::State<std::map<std::string, std::string>>;

// 拉单个供应商的用量并格式化成展示文本（worker 线程跑阻塞 fetchUsage，
// 恢复点在 UI 线程；本函数不写 State）。
huxerui::Task<std::string> FetchUsageText(models::Provider p) {
    try {
        const std::string value = co_await huxerui::RunWorker(
            [](std::string u, std::string k, std::string path) {
                return net::fetchUsage(u, k, path);
            },
            p.usageUrl, p.apiKey, p.usagePath);
        co_return p.usageLabel.empty() ? value
                                       : std::format("{} {}", value, p.usageLabel);
    } catch (const std::exception& e) {
        co_return std::format("查询失败：{}", e.what());
    }
}

void WriteUsageCache(UsageCache cache, const std::string& id, std::string text) {
    auto m = cache.Get();
    m[id] = std::move(text);
    cache = std::move(m);
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

// 工具没有 apiFormat 字段时按工具推断默认协议：claude-code / claude 走
// anthropic 协议（{base}/v1/models + x-api-key），codex 走 OpenAI 兼容。
std::string DefaultApiFormat(std::string_view tool) {
    if (tool == "claude-code" || tool == "claude") return "anthropic";
    return "";
}

// 模型下拉：拉取成功后出现在模型行内（TextField 与「获取模型」按钮之间），
// 点选回填该行的模型字段（受控值仍以 TextField 的 TextEditingValue 为权威，
// 下拉只是快捷填值入口）。items 会整体替换，factory 结果带稳定 Key。
huxerui::View ModelSelect(huxerui::State<std::vector<std::string>> fetched,
                          huxerui::State<std::size_t> sel,
                          huxerui::State<huxerui::TextEditingValue> target) {
    return huxerui::Select(fetched, sel,
                           [](const std::string& id) {
                               return huxerui::Text(id).Key(id);
                           })
        .Label("选择")
        .OnChanged([fetched, sel, target](std::size_t index) {
            const auto& items = fetched.Get();
            if (index < items.size()) {
                target = huxerui::TextEditingValue{items[index]};
            }
            sel = index;
        })
        .With(huxerui::Frame{.width = 150.0F});
}

// 新增/编辑供应商页（整页表单，不再是弹窗——字段太多弹窗太挤）。
// 表单状态以 initial 为初值（UseState 初值只在首次组合生效；调用方用
// .Key("form:" + target) 保证换编辑目标整体重建）。isNew 时顶部内嵌预设
// 模板区（点选 FillForm 预填）。校验：名称必填；非 codex 组 baseUrl 必填；
// codex 组 apiKey 必填；needsModel 组模型必填。保存成功 toast 后返回列表
// （写 formTarget 会卸载点击节点：经 tasks.Launch + Delay(0) 推迟）。
[[huxerui::composable]] huxerui::View ProviderFormPage(
    std::string tool, models::Provider initial, bool isNew,
    huxerui::State<int> revision, huxerui::State<std::string> formTarget) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
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
    // 「获取模型」拉取状态：fetching 驱动按钮加载态；fetchedModels 缓存本次
    // 表单会话内最后一次拉取结果；非空时模型行出现下拉选择。
    auto fetching = huxerui::UseState(false);
    auto fetchedModels = huxerui::UseState<std::vector<std::string>>({});
    // API Key 明文开关：Secure(bool) 切换掩码，眼睛按钮用 SDK 内置的可交互
    // TrailingIcon（icon + 语义标签 → OnTrailingIconClick 事件），且只在悬停
    // 输入框任意位置（或已明文）时才显示（ViewEvents::Hover 只在 Enter/Leave
    // 写 State，Move 不重组合）。
    auto showKey = huxerui::UseState(false);
    auto keyHover = huxerui::UseState(false);

    // 返回列表（写 formTarget 会卸载点击路径上的节点：推迟出指针事件路径）。
    auto goBack = [tasks, formTarget] {
        tasks.Launch([formTarget]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            formTarget = "";
        });
    };

    std::vector<huxerui::View> fields;
    // 预设模板区（仅新增）：点选预填表单；清掉可能已拉取的旧模型列表。
    if (isNew) {
        const auto presets = models::builtinPresets(tool);
        if (!presets.empty()) {
            fields.push_back(huxerui::Text("预设模板（点选预填，API Key 需自行填写）")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.on_surface_variant}));
            std::vector<huxerui::View> chips;
            for (const auto& preset : presets) {
                chips.push_back(
                    huxerui::Button(preset.name)
                        .OnClick([fs, fetchedModels, preset] {
                            fetchedModels = std::vector<std::string>{};
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
    fields.push_back(huxerui::Switch("完整 URL", fs.fullUrl.Get())
        .OnChanged([fs](bool checked) { fs.fullUrl = checked; }));
    fields.push_back(huxerui::TextField(fs.baseUrl.Get())
        .Label(isCodex ? "URL（可选，codex 以 config.toml 为准）" : "URL")
        .Placeholder("https://...")
        .Variant(huxerui::TextFieldVariant::Outlined)
        .OnChanged([fs](const huxerui::TextEditingValue& v) { fs.baseUrl = v; }));
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
        // needsModel（opencode / pi）：切换生效依赖默认模型，必填；codex 的
        // 模型选填，切换时行级写入 config.toml 顶层 model 键。
        const char* modelLabel = needsModel ? "模型（必填）"
            : tool == "claude-code"    ? "主模型（可选，写入 ANTHROPIC_MODEL）"
            : tool == "codex"
                ? "模型（可选，切换时写入 config.toml 顶层 model）"
                                       : "主模型（可选）";
        // 「获取模型」：用表单当前的实际 URL/apiKey/apiFormat（实际 URL 会按
        // 完整 URL 开关与上游格式计算；无 apiFormat 字段的工具按
        // DefaultApiFormat 推断）调 llmswitch.net::fetchModels。
        // fetchModels 是阻塞网络调用，经 huxerui::RunWorker 派到 worker 线程；
        // 协程恢复点恒为 UI 线程，State 写回安全（State 只在 UI 线程写）。
        // 成功后下拉出现在 TextField 与按钮之间，点选直接回填该行。
        const bool canFetch = !fetching.Get() && !fs.baseUrl.Get().text.empty() &&
                              !fs.apiKey.Get().text.empty();
        fields.push_back(huxerui::Row {
            huxerui::TextField(fs.model.Get())
                .Label(modelLabel)
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([fs](const huxerui::TextEditingValue& v) { fs.model = v; })
                .With(huxerui::Grow(1.0F)),
            fetchedModels.Get().empty()
                ? huxerui::View{huxerui::Row{}}
                : ModelSelect(fetchedModels, fs.selModel, fs.model),
            huxerui::Button(fetching.Get() ? "获取中…" : "获取模型")
                .OnClick([=] {
                    const std::string u = models::effectiveBaseUrl(
                        fs.baseUrl.Get().text,
                        UpstreamFormatFromIndex(fs.upstreamFormat.Get()),
                        fs.fullUrl.Get());
                    const std::string k = fs.apiKey.Get().text;
                    const std::string f = hasApiFormat
                                              ? ApiFormatFromIndex(fs.apiFormat.Get())
                                              : DefaultApiFormat(tool);
                    fetching = true;
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        try {
                            auto models = co_await huxerui::RunWorker(
                                [](const std::string& u, const std::string& k,
                                   const std::string& f) {
                                    return net::fetchModels(u, k, f);
                                },
                                u, k, f);
                            fetching = false;
                            fetchedModels = models;
                            if (models.empty()) {
                                toast.Show("模型列表为空");
                            }
                        } catch (const std::exception& e) {
                            fetching = false;
                            toast.Show(e.what());
                        }
                    });
                })
                .With(huxerui::Enabled(canFetch),
                      huxerui::Tooltip(canFetch
                                           ? "按当前 Base URL + API Key 拉取模型列表"
                                           : "请先填写 Base URL 与 API Key")),
        }.With(huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
    }
    if (hasMappings) {
        // 三档模型映射（claude-code 写 ANTHROPIC_DEFAULT_*_MODEL env；
        // claude desktop 写 inferenceModels 映射条目），均选填，共享同一份
        // fetchedModels 下拉。
        fields.push_back(huxerui::Text("模型映射（可选）")
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                theme.colors.on_surface_variant}));
        const std::array<std::pair<const char*,
                                   huxerui::State<huxerui::TextEditingValue>>,
                         3>
            mappingFields{{
                {"Haiku 映射", fs.haiku},
                {"Sonnet 映射", fs.sonnet},
                {"Opus 映射", fs.opus},
            }};
        const std::array<huxerui::State<std::size_t>, 3> mappingSels{
            fs.selHaiku, fs.selSonnet, fs.selOpus};
        for (std::size_t i = 0; i < mappingFields.size(); ++i) {
            const auto& [label, field] = mappingFields[i];
            fields.push_back(huxerui::Row {
                huxerui::TextField(field.Get())
                    .Label(label)
                    .Variant(huxerui::TextFieldVariant::Outlined)
                    .OnChanged([field](const huxerui::TextEditingValue& v) {
                        field = v;
                    })
                    .With(huxerui::Grow(1.0F)),
                fetchedModels.Get().empty()
                    ? huxerui::View{huxerui::Row{}}
                    : ModelSelect(fetchedModels, mappingSels[i], field),
            }.With(huxerui::Spacing(8.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
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
    // 用量查询三字段不在这里——卡片 gauge 按钮进 UsageFormPage 独立配置。
    fields.push_back(huxerui::TextField(fs.website.Get())
        .Label("官网（可选）")
        .Variant(huxerui::TextFieldVariant::Outlined)
        .OnChanged([fs](const huxerui::TextEditingValue& v) { fs.website = v; }));
    fields.push_back(huxerui::TextField(fs.notes.Get())
        .Label("备注（可选）")
        .Variant(huxerui::TextFieldVariant::Outlined)
        .OnChanged([fs](const huxerui::TextEditingValue& v) { fs.notes = v; }));
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
                        if (needsModel && model.empty()) {
                            toast.Show("模型不能为空");
                            return;
                        }
                        models::Provider p;
                        p.id = editingId;
                        p.name = name;
                        p.baseUrl = baseUrl;
                        p.apiKey = apiKey;
                        p.model = model;
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
                        // 用量查询配置归 UsageFormPage 管，编辑保留原值。
                        p.usageUrl = initial.usageUrl;
                        p.usagePath = initial.usagePath;
                        p.usageLabel = initial.usageLabel;
                        try {
                            if (editingId.empty()) {
                                providerStore().addProvider(tool, std::move(p));
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

// 用量查询配置页（整页，从卡片的 gauge 图标进入，formTarget = "usage:" + id）。
// 字段：用量 URL / 取值路径 / 单位标签；「自动填充」按供应商 baseUrl 匹配
// models::suggestUsageQuery 的内置端点模板。usageUrl 留空 = 不查询（清空已
// 有配置也用这招）。保存 = updateProvider 只改三个用量字段。
[[huxerui::composable]] huxerui::View UsageFormPage(
    std::string tool, models::Provider initial, huxerui::State<int> revision,
    huxerui::State<std::string> formTarget) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto usageUrl = huxerui::UseState(huxerui::TextEditingValue{initial.usageUrl});
    auto usagePath =
        huxerui::UseState(huxerui::TextEditingValue{initial.usagePath});
    auto usageLabel =
        huxerui::UseState(huxerui::TextEditingValue{initial.usageLabel});

    // 返回列表（写 formTarget 会卸载点击路径上的节点：推迟出指针事件路径）。
    auto goBack = [tasks, formTarget] {
        tasks.Launch([formTarget]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            formTarget = "";
        });
    };

    std::vector<huxerui::View> fields;
    fields.push_back(huxerui::Row {
        huxerui::TextField(usageUrl.Get())
            .Label("用量 URL")
            .Placeholder("https://...（留空 = 不查询）")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([usageUrl](const huxerui::TextEditingValue& v) {
                usageUrl = v;
            })
            .With(huxerui::Grow(1.0F)),
        huxerui::Button("自动填充")
            .OnClick([usageUrl, usagePath, toast, initial] {
                const auto suggested =
                    models::suggestUsageQuery(initial.baseUrl);
                if (!suggested) {
                    toast.Show("该供应商暂无内置用量端点模板，请手动填写");
                    return;
                }
                usageUrl = huxerui::TextEditingValue{suggested->first};
                usagePath = huxerui::TextEditingValue{suggested->second};
            })
            .With(huxerui::Tooltip("按 Base URL 匹配内置用量端点模板")),
    }.With(huxerui::Spacing(8.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
    fields.push_back(huxerui::TextField(usagePath.Get())
        .Label("取值路径")
        .Placeholder("balance_infos.0.total_balance")
        .Variant(huxerui::TextFieldVariant::Outlined)
        .OnChanged([usagePath](const huxerui::TextEditingValue& v) {
            usagePath = v;
        }));
    fields.push_back(huxerui::TextField(usageLabel.Get())
        .Label("单位标签")
        .Placeholder("如 CNY")
        .Variant(huxerui::TextFieldVariant::Outlined)
        .OnChanged([usageLabel](const huxerui::TextEditingValue& v) {
            usageLabel = v;
        }));
    fields.push_back(huxerui::Text(
        "轮询开关与间隔在「设置」页统一配置；查询带供应商 API Key 做 Bearer 鉴权。")
        .Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kCaption),
            theme.colors.on_surface_variant}));

    return PageScaffold(
        "用量查询 — " + initial.name,
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
                huxerui::Button("保存")
                    .OnClick([=] {
                        const std::string url = usageUrl.Get().text;
                        const std::string path = usagePath.Get().text;
                        if (!url.empty() && path.empty()) {
                            toast.Show("取值路径不能为空（或清空用量 URL 停用查询）");
                            return;
                        }
                        models::Provider p = initial;
                        p.usageUrl = url;
                        p.usagePath = path;
                        p.usageLabel = usageLabel.Get().text;
                        try {
                            providerStore().updateProvider(tool, p);
                        } catch (const std::exception& e) {
                            toast.Show(e.what());
                            return;
                        }
                        revision = revision.Get() + 1;
                        toast.Show("用量查询配置已保存");
                        goBack();
                    }),
            }.With(huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween)),
        }.With(huxerui::Spacing(12.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

// 工具图标栏（供应商岛屿头部行左侧）：遍历注册表渲染 agent 图标（ToolIcon
// 在 common.cpp，与会话页过滤组共用），单套图标由主题 tint 自适应，
// 选中项用 raised 底块高亮。写 currentTool 会让外层
// .Key(tool) 重建整个岛屿子树（含被点击的图标）：经 tasks.Launch +
// Delay(0) 推迟出指针事件路径。
[[huxerui::composable]] huxerui::View ToolBar(
    huxerui::State<std::string> currentTool) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    auto tasks = huxerui::UseTaskScope();
    const std::string tool = currentTool.Get();
    std::vector<huxerui::View> buttons;
    for (const auto& spec : models::toolRegistry()) {
        const std::string id(spec.id);
        const huxerui::ImageResource icon = ToolIcon(spec.iconName);
        const std::string displayName(spec.displayName);
        const bool selected = tool == id;
        huxerui::View button =
            huxerui::IconButton(icon, displayName)
                .OnClick([tasks, currentTool, id] {
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        co_await huxerui::Delay(std::chrono::duration<double>{0});
                        currentTool = id;
                    });
                })
                .With(huxerui::Tooltip(displayName));
        // 选中态只由 raised 承载底块表达，图标几何与主题 tint 保持一致。
        if (selected) {
            button = std::move(button).With(
                huxerui::Background(islands.raised),
                huxerui::CornerRadius(islands.nested_radius));
        }
        buttons.push_back(std::move(button));
    }
    return huxerui::Row(std::move(buttons))
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

// 官方常驻卡：有官方厂商的工具（models::officialVendorName 非空）固定在// 供应商列表第一位；切换 = store.restoreOfficial 还原厂商原生状态（与
// 普通卡同样的切换语义，无确认框）。active = 组 current 与 detectCurrent
// 均为空（即当前生效的就是厂商原生状态）。
[[huxerui::composable]] huxerui::View OfficialCard(std::string tool, bool active,
                                                   huxerui::ToastHandle toast,
                                                   huxerui::State<int> revision) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    return Card(huxerui::Row {
        huxerui::Column {
            huxerui::Row {
                huxerui::Text(std::string(models::officialVendorName(tool)))
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kBody)
                            .WithWeight(huxerui::FontWeight::SemiBold),
                        theme.colors.on_surface}),
                active
                    ? huxerui::View{
                          huxerui::Text("使用中").Style(huxerui::TextStyle{
                              huxerui::Font::System(font_size::kCaption),
                              theme.colors.on_primary})}
                          .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(
                                    8.0F, 2.0F)),
                                huxerui::Background(theme.colors.primary),
                                huxerui::CornerRadius(islands.nested_radius))
                    : huxerui::View{huxerui::Row{}},
            }.With(huxerui::Spacing(6.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
            huxerui::Text("厂商原生状态（撤掉第三方配置覆盖，回到官方登录）")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.on_surface_variant}),
        }.With(huxerui::Spacing(6.0F),
               huxerui::Grow(1.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start)),
        // 操作组右对齐：图标 + Tooltip（active 时禁用）。
        huxerui::Row {
            huxerui::IconButton(app::images::swap, "切换")
                .OnClick([toast, tool, revision] {
                    try {
                        providerStore().restoreOfficial(tool);
                        toast.Show("已切换到官方原生状态");
                    } catch (const std::exception& e) {
                        toast.Show(e.what());
                    }
                    revision = revision.Get() + 1;
                })
                .With(huxerui::Enabled(!active),
                      huxerui::Tooltip(active ? "当前使用"
                                              : "切换到官方原生状态")),
        }.With(huxerui::Spacing(4.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
    }.With(huxerui::Spacing(8.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)))
        .Key("official");
}

[[huxerui::composable]] huxerui::View ProviderCard(
    std::string tool, const models::Provider& provider, bool active, bool isCurrent,
    huxerui::TaskScope tasks, huxerui::ToastHandle toast,
    huxerui::State<int> revision, UsageCache usageCache,
    huxerui::State<std::string> formTarget) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    auto dialog = huxerui::UseDialog();
    const std::string id = provider.id;
    const std::string name = provider.name;
    const std::string accessUrl = models::effectiveBaseUrl(provider);

    // 连通检测状态（卡片级）：latency 空 = 未检测；检测中禁用按钮。
    // State 经 .Key(id) 随卡片保活，revision 重读不丢。
    auto checking = huxerui::UseState(false);
    auto latency = huxerui::UseState<std::string>({});

    // 用量展示：usageUrl 非空才显示；缓存未命中显示占位，错误文本用 error 色。
    std::string usageText;
    bool usageError = false;
    if (!provider.usageUrl.empty()) {
        const auto& cache = usageCache.Get();
        if (const auto it = cache.find(id); it != cache.end()) {
            usageText = it->second;
            usageError = it->second.starts_with("查询失败");
        } else {
            usageText = "用量待查询";
        }
    }

    auto bump = [revision] { revision = revision.Get() + 1; };

    // 编辑：进入整页表单（写 formTarget 会卸载点击路径上的节点：推迟）。
    auto showEdit = [tasks, formTarget, id] {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            formTarget = id;
        });
    };

    // 用量查询配置：进入独立配置页（同样推迟）。
    auto showUsage = [tasks, formTarget, id] {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            formTarget = "usage:" + id;
        });
    };

    // 删除：内置确认框（主题化 DialogStyle 见 app.cpp MinimalThemed）。
    auto showDeleteConfirm = [dialog, toast, tool, id, name, bump] {
        dialog.Show(
            "删除供应商",
            std::format("确定删除「{}」？此操作不可撤销。", name),
            "删除", "取消",
            [toast, tool, id, name, bump] {
                try {
                    providerStore().removeProvider(tool, id);
                    toast.Show(std::format("已删除 {}", name));
                } catch (const std::exception& e) {
                    toast.Show(e.what());
                }
                bump();
            },
            {});
    };

    // 三段式：左信息列（Grow 吃满剩余宽度）｜ 中间状态列（延迟 + 用量，
    // 垂直居中落在内容与操作组之间）｜ 右侧操作图标组（自绘图标 + Tooltip）。
    return Card(huxerui::Row {
        huxerui::Column {
            huxerui::Row {
                huxerui::Text(name).Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kBody)
                        .WithWeight(huxerui::FontWeight::SemiBold),
                    theme.colors.on_surface}),
                active
                    ? huxerui::View{
                          huxerui::Text("使用中").Style(huxerui::TextStyle{
                              huxerui::Font::System(font_size::kCaption),
                              theme.colors.on_primary})}
                          .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(
                                    8.0F, 2.0F)),
                                huxerui::Background(theme.colors.primary),
                                huxerui::CornerRadius(islands.nested_radius))
                    : huxerui::View{huxerui::Row{}},
            }.With(huxerui::Spacing(6.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
            accessUrl.empty()
                ? huxerui::View{huxerui::Row{}}
                : huxerui::View{huxerui::Text(accessUrl)
                                    .Style(huxerui::TextStyle{
                                        huxerui::Font::Monospace(font_size::kChip),
                                        theme.colors.on_surface_variant})},
            provider.notes.empty()
                ? huxerui::View{huxerui::Row{}}
                : huxerui::View{huxerui::Text(provider.notes)
                                    .Style(huxerui::TextStyle{
                                        huxerui::Font::System(font_size::kCaption),
                                        theme.colors.on_surface_variant})},
        }.With(huxerui::Spacing(6.0F),
               huxerui::Grow(1.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start)),
        // 中间状态列：连通检测结果（未检测不显示；失败 error 色）+ 用量
        // （usageUrl 非空才显示，带手动刷新图标）。两者皆无时整列塌缩为零宽。
        huxerui::Column {
            (!checking.Get() && latency.Get().empty())
                ? huxerui::View{huxerui::Row{}}
                : huxerui::View{huxerui::Text(
                      checking.Get() ? "连通检测中…" : latency.Get())
                      .Style(huxerui::TextStyle{
                          huxerui::Font::System(font_size::kCaption),
                          latency.Get().starts_with("不可达")
                              ? theme.colors.error
                              : theme.colors.on_surface_variant})},
            provider.usageUrl.empty()
                ? huxerui::View{huxerui::Row{}}
                : huxerui::View{huxerui::Row {
                      huxerui::Text(usageText)
                          .Style(huxerui::TextStyle{
                              huxerui::Font::System(font_size::kCaption),
                              usageError ? theme.colors.error
                                         : theme.colors.on_surface_variant}),
                      huxerui::IconButton(app::images::refresh, "刷新")
                          .OnClick([tasks, usageCache, provider] {
                              // fetchUsage 阻塞最长 10s：RunWorker 跑，恢复点回
                              // UI 线程后写缓存 State。
                              tasks.Launch([usageCache,
                                            provider]() -> huxerui::Task<void> {
                                  WriteUsageCache(usageCache, provider.id,
                                                  co_await FetchUsageText(provider));
                              });
                          })
                          .With(huxerui::Tooltip("重新查询用量")),
                  }.With(huxerui::Spacing(4.0F),
                         huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))},
        }.With(huxerui::Spacing(4.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start)),
        huxerui::Row {
            huxerui::IconButton(app::images::swap, "切换")
                .OnClick([toast, tool, id, name, bump] {
                    try {
                        providerStore().switchTo(tool, id);
                        toast.Show(std::format("已切换到 {}", name));
                    } catch (const std::exception& e) {
                        toast.Show(e.what());
                    }
                    bump();
                })
                .With(huxerui::Enabled(!isCurrent),
                      huxerui::Tooltip(isCurrent ? "当前使用" : "切换到此供应商")),
            // 联通检测：pingLatencyMs 阻塞最长 10s，RunWorker 派到 worker
            // 线程，恢复点回 UI 线程写卡片 State。URL 空（codex 可留空）
            // 时禁用。
            huxerui::IconButton(app::images::activity, "联通检测")
                .OnClick([tasks, checking, latency,
                          url = accessUrl] {
                    checking = true;
                    latency = std::string{};
                    tasks.Launch([checking, latency,
                                  url]() -> huxerui::Task<void> {
                        try {
                            const double ms = co_await huxerui::RunWorker(
                                [](const std::string& u) {
                                    return net::pingLatencyMs(u);
                                },
                                url);
                            checking = false;
                            latency = std::format("延迟 {:.0f} ms", ms);
                        } catch (const std::exception& e) {
                            checking = false;
                            latency = std::format("不可达：{}", e.what());
                        }
                    });
                })
                .With(huxerui::Enabled(!checking.Get() &&
                                       !accessUrl.empty()),
                      huxerui::Tooltip(accessUrl.empty()
                                           ? "该供应商未设置 URL"
                                           : "检测实际访问 URL 的连通性与延迟")),
            huxerui::IconButton(app::images::edit, "编辑")
                .OnClick([showEdit] { showEdit(); })
                .With(huxerui::Tooltip("编辑")),
            huxerui::IconButton(app::images::gauge, "用量查询配置")
                .OnClick([showUsage] { showUsage(); })
                .With(huxerui::Tooltip("用量查询配置")),
            huxerui::IconButton(app::images::copy, "复制")
                .OnClick([toast, tool, id, bump] {
                    try {
                        const auto copy = providerStore().duplicateProvider(tool, id);
                        toast.Show(std::format("已复制为 {}", copy.name));
                    } catch (const std::exception& e) {
                        toast.Show(e.what());
                    }
                    bump();
                })
                .With(huxerui::Tooltip("复制")),
            huxerui::IconButton(app::images::trash, "删除")
                .OnClick([tasks, showDeleteConfirm] {
                    // 弹窗会卸载点击路径上的节点：推迟出指针事件路径。
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        co_await huxerui::Delay(std::chrono::duration<double>{0});
                        showDeleteConfirm();
                    });
                })
                .With(huxerui::Tooltip("删除")),
        }.With(huxerui::Spacing(4.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
    }.With(huxerui::Spacing(8.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)))
        .Key(id);
}

} // namespace

[[huxerui::composable]] huxerui::View ProvidersPage(
    huxerui::State<std::string> currentTool, huxerui::State<int> revision) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    // 当前工具 id：AgentPage 持有的 State，岛屿内部顶部的工具图标栏写它
    // 换组；外层以 .Key(tool) 组合本页，换工具即整体重建。
    const std::string tool = currentTool.Get();
    // 首组合时探测 live 文件命中（本地文件读，UI 线程直接跑）。
    auto detected = huxerui::UseState<std::string>({});
    huxerui::Lifecycle(
        [tool, detected] {
            detected = providerStore().detectCurrent(tool);
            return [] {};
        },
        0);
    // 列表/表单多模式："" = 列表；"new" = 新增；"usage:" + id = 用量查询
    // 配置页；否则 = 编辑的 provider id。子页以 .Key 组合，换目标即重建状态。
    auto formTarget = huxerui::UseState<std::string>({});

    // 用量缓存（页面级）+ 自动轮询：页面可见期间运行（TaskScope 随页面卸载
    // 取消）。每个周期在 UI 线程重读 config：usageEnabled 且
    // usageRefreshMinutes>0 时立即拉一轮所有配置了 usageUrl 的供应商（全部分组，
    // 不只当前工具）再睡一个间隔；关闭/仅手动时按 30s 轻量再检查（设置页改动
    // 至多 30s 生效，避免睡死在一个长间隔里）。State 只在 UI 线程写。
    auto usageCache = huxerui::UseState<std::map<std::string, std::string>>({});
    huxerui::Lifecycle(
        [tasks, usageCache] {
            tasks.Launch([usageCache]() -> huxerui::Task<void> {
                while (true) {
                    const auto& config = providerStore().config();
                    if (!config.usageEnabled || config.usageRefreshMinutes <= 0) {
                        co_await huxerui::Delay(std::chrono::duration<double>{30});
                        continue;
                    }
                    std::vector<models::Provider> targets;
                    for (const auto& [toolId, grp] : config.groups) {
                        for (const auto& p : grp.providers) {
                            if (!p.usageUrl.empty()) targets.push_back(p);
                        }
                    }
                    // 顺序拉取（每次最长 10s），每个完成即回写缓存。
                    for (const auto& p : targets) {
                        WriteUsageCache(usageCache, p.id,
                                        co_await FetchUsageText(p));
                    }
                    co_await huxerui::Delay(std::chrono::duration<double>{
                        config.usageRefreshMinutes * 60.0});
                }
            });
            return [] {};
        },
        0);

    // 订阅全局变更计数：托盘切换 / 设置页导入后本页重读。
    (void)revision.Get();

    // 卡片列表：官方常驻卡（有官方厂商的工具）排第一，其后是供应商卡；
    // 「使用中」= 组内 current 或 detectCurrent 命中。
    const auto& g = providerStore().group(tool);
    const bool hasOfficial = !models::officialVendorName(tool).empty();
    const std::size_t providerCount = g.providers.size();
    const std::string currentProvider = g.current;
    const std::string detectedProvider = detected.Get();
    huxerui::View providerCards = huxerui::Row{};
    if (hasOfficial) {
        providerCards = OfficialCard(
            tool, currentProvider.empty() && detectedProvider.empty(), toast,
            revision);
    }
    if (providerCount > 0) {
        const huxerui::View providerList =
            huxerui::VirtualList(
                g.providers,
                [tool, currentProvider, detectedProvider, tasks, toast, revision,
                 usageCache, formTarget](const models::Provider& provider) {
                    const bool isCurrent = currentProvider == provider.id;
                    const bool active =
                        isCurrent || detectedProvider == provider.id;
                    return ProviderCard(tool, provider, active, isCurrent, tasks,
                                        toast, revision, usageCache, formTarget);
                })
                .EstimatedItemExtent(150.0F)
                .CacheExtent(480.0F)
                .With(huxerui::Spacing(10.0F), huxerui::Grow(1.0F));
        providerCards = hasOfficial
                            ? huxerui::Column {
                                  providerCards,
                                  providerList,
                              }
                                  .With(huxerui::Spacing(10.0F),
                                        huxerui::CrossAlign(
                                            huxerui::CrossAxisAlignment::Stretch))
                            : providerList;
    }
    const bool hasCards = hasOfficial || providerCount > 0;

    // 列表模式用自定义一级岛（不走 PageScaffold）：标题文字已删（选中图标
    // 自带高亮可辨），头部一行 = 工具图标栏（左）+ 新增按钮（右）。
    const IslandTheme islands = ResolveIslandTheme(theme);
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    std::vector<huxerui::View> listItems;
    listItems.push_back(huxerui::Row {
        ToolBar(currentTool),
        huxerui::Spacer(),
        huxerui::IconButton(app::images::add, "新增供应商")
            .OnClick([tasks, formTarget] {
                // 写 formTarget 会卸载点击节点：推迟出指针事件路径。
                tasks.Launch([=]() -> huxerui::Task<void> {
                    co_await huxerui::Delay(std::chrono::duration<double>{0});
                    formTarget = "new";
                });
            })
            .With(huxerui::Tooltip("新增供应商")),
    }.With(huxerui::Spacing(theme.spacing.small),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));

    // Claude Desktop 平台提示条（仅 macOS / Windows 可用；图标照常显示，
    // 增删改可用但本平台无法切换生效）。
    if (tool == "claude" && cfg::claudeDesktopDir().empty()) {
        listItems.push_back(
            huxerui::Text("Claude Desktop 仅支持 macOS / "
                          "Windows，当前平台切换不可用")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.on_surface_variant})
                .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 6.0F)),
                      huxerui::Background(islands.raised),
                      huxerui::CornerRadius(islands.nested_radius)));
    }
    listItems.push_back(
        !hasCards
            ? huxerui::View{
                  huxerui::Column {
                      huxerui::Text("还没有供应商。点击右上角 + 新增。")
                          .Style(huxerui::TextStyle{
                              huxerui::Font::System(font_size::kBody),
                              theme.colors.on_surface_variant}),
                  }.With(huxerui::Padding(32.0F),
                         huxerui::Grow(1.0F),
                         huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                         huxerui::CrossAlign(
                             huxerui::CrossAxisAlignment::Center))}
            : providerCards);

    huxerui::View root = huxerui::Column(std::move(listItems))
        .With(huxerui::Padding(compact ? theme.spacing.medium
                                       : theme.spacing.large),
              huxerui::Spacing(theme.spacing.medium),
              huxerui::Background(islands.base),
              huxerui::CornerRadius(islands.island_radius),
              huxerui::Border(islands.outline_soft, 0.75F),
              huxerui::ClipChildren(),
              huxerui::Grow(1.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    // 表单/用量配置模式：initial = 目标拷贝（新增为空 Provider）。单 return
    // 前的所有 UseState 都在分支之前，State 恒定性不受模式切换影响。
    const std::string target = formTarget.Get();
    if (target.starts_with("usage:")) {
        const std::string id = target.substr(6);
        models::Provider initial;
        for (const auto& p : g.providers) {
            if (p.id == id) initial = p;
        }
        root = UsageFormPage(tool, initial, revision, formTarget)
                   .Key("usage:" + id);
    } else if (!target.empty()) {
        const bool isNew = target == "new";
        models::Provider initial;
        if (!isNew) {
            for (const auto& p : g.providers) {
                if (p.id == target) initial = p;
            }
        }
        root = ProviderFormPage(tool, initial, isNew, revision, formTarget)
                   .Key("form:" + target);
    }
    return root;
}

} // namespace llmswitch::ui
