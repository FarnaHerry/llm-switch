// providers_page.cpp — 供应商列表页：各 agent 工具组共用同一组件（参数化
// tool，注册表见 models::toolRegistry()）。每个供应商一张卡片：名称 / baseUrl /
// 掩码 apiKey / 备注 /「使用中」徽章（group.current 或 detectCurrent 命中），
// 操作：切换 / 编辑 / 复制 / 删除（删除走内置确认框）。顶部：新增供应商 +
// 预设模板入口。表单按 ToolSpec 适配：codex 显示 config.toml 原文、
// needsModel（opencode/pi）模型必填、hasApiFormat 显示 API 协议分段选择；
// 模型字段旁「获取模型」按当前 baseUrl/apiKey/apiFormat 经 llmswitch.net
// 拉取模型列表（阻塞网络调用经 huxerui::RunWorker 派到 worker 线程，
// 结果回 UI 线程写 State），成功弹选择列表回填 model 字段。
// 用量查询：表单三字段（URL/取值路径/单位标签，「自动填充」按 baseUrl 套
// models::suggestUsageQuery 模板）；usageUrl 非空的卡片显示用量文本 + 手动
// 刷新按钮；页面可见期间按 config 的 usageEnabled/usageRefreshMinutes 轮询
// 全部配置了 usageUrl 的供应商，缓存为页面级 State（只在 UI 线程写）。
//
// 数据流：所有 store 读写都在 UI 线程（store 无内部锁，UI 线程独占是契约；
// live 文件读写为微秒级本地 IO，不经任务线程）。写操作后 revision+1，
// 驱动本页重读、托盘菜单重建。detectCurrent 在页面首组合跑一次。
#include <huxerui/huxerui.h>

#include <chrono>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "ui.h"

import llmswitch.models;
import llmswitch.net;
import llmswitch.store;

namespace llmswitch::ui {
namespace {

// 表单字段集合：新增/编辑共用一组 State 句柄（State 是可拷贝句柄，
// 归打开弹窗的组合作用域所有）。apiFormat 为选项下标（0=OpenAI 兼容（默认，
// 存空串）/ 1=anthropic / 2=openai-responses），仅 hasApiFormat 工具展示。
// usageUrl/usagePath/usageLabel 为用量查询三字段（usageUrl 空 = 不查询）。
struct FormStates {
    huxerui::State<huxerui::TextEditingValue> name;
    huxerui::State<huxerui::TextEditingValue> baseUrl;
    huxerui::State<huxerui::TextEditingValue> apiKey;
    huxerui::State<huxerui::TextEditingValue> model;
    huxerui::State<huxerui::TextEditingValue> website;
    huxerui::State<huxerui::TextEditingValue> notes;
    huxerui::State<huxerui::TextEditingValue> toml;  // 仅 codex 组展示
    huxerui::State<int> apiFormat;                   // 仅 opencode / pi 展示
    huxerui::State<huxerui::TextEditingValue> usageUrl;
    huxerui::State<huxerui::TextEditingValue> usagePath;
    huxerui::State<huxerui::TextEditingValue> usageLabel;
};

void FillForm(const FormStates& fs, const models::Provider& p) {
    fs.name = huxerui::TextEditingValue{p.name};
    fs.baseUrl = huxerui::TextEditingValue{p.baseUrl};
    fs.apiKey = huxerui::TextEditingValue{p.apiKey};
    fs.model = huxerui::TextEditingValue{p.model};
    fs.website = huxerui::TextEditingValue{p.website};
    fs.notes = huxerui::TextEditingValue{p.notes};
    fs.toml = huxerui::TextEditingValue{p.codexConfigToml};
    fs.apiFormat = p.apiFormat == "anthropic"      ? 1
                   : p.apiFormat == "openai-responses" ? 2
                                                       : 0;
    fs.usageUrl = huxerui::TextEditingValue{p.usageUrl};
    fs.usagePath = huxerui::TextEditingValue{p.usagePath};
    fs.usageLabel = huxerui::TextEditingValue{p.usageLabel};
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

// 工具没有 apiFormat 字段时按工具推断默认协议：claude-code / claude 走
// anthropic 协议（{base}/v1/models + x-api-key），codex 走 OpenAI 兼容。
std::string DefaultApiFormat(std::string_view tool) {
    if (tool == "claude-code" || tool == "claude") return "anthropic";
    return "";
}

// 模型选择弹窗内容（composable）：拉取成功的模型列表，点选回填 model 字段。
[[huxerui::composable]] huxerui::View ModelPickerContent(
    std::vector<std::string> models,
    huxerui::State<huxerui::TextEditingValue> modelState,
    huxerui::DialogContext ctx) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    std::vector<huxerui::View> items;
    for (const auto& id : models) {
        items.push_back(
            huxerui::Text(id)
                .Style(huxerui::TextStyle{
                    huxerui::Font::Monospace(font_size::kBody),
                    theme.colors.on_surface})
                .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 6.0F)),
                      huxerui::CornerRadius(8.0F))
                .OnClick([ctx, modelState, id] {
                    modelState = huxerui::TextEditingValue{id};
                    ctx.Dismiss();
                }));
    }
    return DialogCard(huxerui::Column {
        huxerui::Text("选择模型", huxerui::TextRole::Title),
        huxerui::ScrollView(
            huxerui::Column(std::move(items))
                .With(huxerui::Spacing(2.0F),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
            .With(huxerui::Frame{.height = 320.0F}),
        huxerui::Row {
            huxerui::Spacer(),
            huxerui::Button("取消").OnClick([ctx] { ctx.Dismiss(); }),
        },
    }.With(huxerui::Spacing(12.0F),
           huxerui::Frame{.width = 380.0F},
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

// 打开模型选择弹窗（普通函数：dialog.Show 的工厂转发到 composable 内容）。
void ShowModelPicker(huxerui::DialogHandle dialog, std::vector<std::string> models,
                     huxerui::State<huxerui::TextEditingValue> modelState) {
    dialog.Show(
        [models = std::move(models), modelState](
            huxerui::DialogContext ctx) -> huxerui::View {
            return ModelPickerContent(models, modelState, ctx);
        },
        huxerui::DialogOptions{});
}

// 新增/编辑供应商弹窗内容（composable：UseTheme 等组合函数只能在
// composable 体内调用，dialog 工厂只是转发到这里）。editingId 为空 = 新增
// （store 生成 id/createdAt）。校验：名称必填；非 codex 组 baseUrl 必填；
// codex 组 apiKey 必填；needsModel 组模型必填。成功才关弹窗（失败 toast
// 提示，表单保留）。
[[huxerui::composable]] huxerui::View ProviderFormContent(
    std::string title, std::string tool, FormStates fs, std::string editingId,
    huxerui::DialogContext ctx, huxerui::ToastHandle toast,
    huxerui::State<int> revision) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto dialog = huxerui::UseDialog();
    const auto* spec = models::findTool(tool);
    const bool isCodex = tool == "codex";
    const bool needsModel = spec != nullptr && spec->needsModel;
    const bool hasApiFormat = spec != nullptr && spec->hasApiFormat;
    // 「获取模型」拉取状态：fetching 驱动按钮加载态；fetchedModels 缓存本次
    // 表单会话内最后一次拉取结果（重新打开表单不保留，每次编辑重新拉）。
    auto fetching = huxerui::UseState(false);
    auto fetchedModels = huxerui::UseState<std::vector<std::string>>({});
    std::vector<huxerui::View> fields;
    fields.push_back(huxerui::Text(title, huxerui::TextRole::Title));
    fields.push_back(huxerui::TextField(fs.name.Get())
        .Label("名称")
        .Variant(huxerui::TextFieldVariant::Outlined)
        .OnChanged([fs](const huxerui::TextEditingValue& v) { fs.name = v; }));
    fields.push_back(huxerui::TextField(fs.baseUrl.Get())
        .Label(isCodex ? "Base URL（可选，codex 以 config.toml 为准）"
                       : "Base URL")
        .Placeholder("https://...")
        .Variant(huxerui::TextFieldVariant::Outlined)
        .OnChanged([fs](const huxerui::TextEditingValue& v) { fs.baseUrl = v; }));
    fields.push_back(huxerui::TextField(fs.apiKey.Get())
        .Label(isCodex ? "API Key（写入 auth.json）" : "API Key")
        .Variant(huxerui::TextFieldVariant::Outlined)
        .Secure()
        .OnChanged([fs](const huxerui::TextEditingValue& v) { fs.apiKey = v; }));
    if (!isCodex) {
        // needsModel（opencode / pi）：切换生效依赖默认模型，必填。
        const char* modelLabel = needsModel ? "模型（必填）"
            : tool == "claude-code"    ? "模型（可选，写入 ANTHROPIC_MODEL）"
                                       : "模型（可选）";
        // 「获取模型」：用表单当前的 baseUrl/apiKey/apiFormat（无 apiFormat
        // 字段的工具按 DefaultApiFormat 推断）调 llmswitch.net::fetchModels。
        // fetchModels 是阻塞网络调用，经 huxerui::RunWorker 派到 worker 线程；
        // 协程恢复点恒为 UI 线程，State 写回安全（State 只在 UI 线程写）。
        const bool canFetch = !fetching.Get() && !fs.baseUrl.Get().text.empty() &&
                              !fs.apiKey.Get().text.empty();
        fields.push_back(huxerui::Row {
            huxerui::TextField(fs.model.Get())
                .Label(modelLabel)
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([fs](const huxerui::TextEditingValue& v) { fs.model = v; })
                .With(huxerui::Grow(1.0F)),
            huxerui::Button(fetching.Get() ? "获取中…" : "获取模型")
                .OnClick([=] {
                    const std::string u = fs.baseUrl.Get().text;
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
                                co_return;
                            }
                            ShowModelPicker(dialog, std::move(models), fs.model);
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
    // 用量查询（可选；usageUrl 空 = 不查询）。「自动填充」按当前 baseUrl 匹配
    // models::suggestUsageQuery 的内置端点模板。
    fields.push_back(huxerui::Text("用量查询（可选）")
        .Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kCaption),
            theme.colors.on_surface_variant}));
    fields.push_back(huxerui::Row {
        huxerui::TextField(fs.usageUrl.Get())
            .Label("用量 URL")
            .Placeholder("https://...（留空 = 不查询）")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([fs](const huxerui::TextEditingValue& v) { fs.usageUrl = v; })
            .With(huxerui::Grow(1.0F)),
        huxerui::Button("自动填充")
            .OnClick([fs, toast] {
                const auto suggested =
                    models::suggestUsageQuery(fs.baseUrl.Get().text);
                if (!suggested) {
                    toast.Show("该供应商暂无内置用量端点模板，请手动填写");
                    return;
                }
                fs.usageUrl = huxerui::TextEditingValue{suggested->first};
                fs.usagePath = huxerui::TextEditingValue{suggested->second};
            })
            .With(huxerui::Tooltip("按 Base URL 匹配内置用量端点模板")),
    }.With(huxerui::Spacing(8.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
    fields.push_back(huxerui::TextField(fs.usagePath.Get())
        .Label("取值路径")
        .Placeholder("balance_infos.0.total_balance")
        .Variant(huxerui::TextFieldVariant::Outlined)
        .OnChanged([fs](const huxerui::TextEditingValue& v) { fs.usagePath = v; }));
    fields.push_back(huxerui::TextField(fs.usageLabel.Get())
        .Label("单位标签")
        .Placeholder("如 CNY")
        .Variant(huxerui::TextFieldVariant::Outlined)
        .OnChanged([fs](const huxerui::TextEditingValue& v) { fs.usageLabel = v; }));
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
    fields.push_back(huxerui::Row {
        huxerui::Button("取消").OnClick([ctx] { ctx.Dismiss(); }),
        huxerui::Button(editingId.empty() ? "添加" : "保存")
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
                    toast.Show("Base URL 不能为空");
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
                p.website = fs.website.Get().text;
                p.notes = fs.notes.Get().text;
                p.codexConfigToml = fs.toml.Get().text;
                p.apiFormat = ApiFormatFromIndex(fs.apiFormat.Get());
                p.usageUrl = fs.usageUrl.Get().text;
                p.usagePath = fs.usagePath.Get().text;
                p.usageLabel = fs.usageLabel.Get().text;
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
                ctx.Dismiss();
                revision = revision.Get() + 1;
                toast.Show(editingId.empty() ? "已添加" : "已保存");
            }),
    }.With(huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween)));
    return DialogCard(huxerui::ScrollView(huxerui::Column(std::move(fields))
        .With(huxerui::Spacing(12.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
        .With(huxerui::Frame{.width = 420.0F}));
}

// 打开新增/编辑弹窗（普通函数：dialog.Show 的工厂转发到 composable 内容）。
void ShowProviderForm(huxerui::DialogHandle dialog, huxerui::ToastHandle toast,
                      const std::string& tool, const std::string& title,
                      const FormStates& fs, const std::string& editingId,
                      huxerui::State<int> revision) {
    dialog.Show(
        [=](huxerui::DialogContext ctx) -> huxerui::View {
            return ProviderFormContent(title, tool, fs, editingId, ctx, toast,
                                       revision);
        },
        huxerui::DialogOptions{});
}

[[huxerui::composable]] huxerui::View ProviderCard(
    std::string tool, const models::Provider& provider, bool active, bool isCurrent,
    huxerui::TaskScope tasks, huxerui::ToastHandle toast,
    huxerui::State<int> revision, UsageCache usageCache) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    auto dialog = huxerui::UseDialog();
    // 编辑表单状态归卡片作用域（弹窗内容随卡片存活）。
    const FormStates fs{huxerui::UseState(huxerui::TextEditingValue{""}),
                        huxerui::UseState(huxerui::TextEditingValue{""}),
                        huxerui::UseState(huxerui::TextEditingValue{""}),
                        huxerui::UseState(huxerui::TextEditingValue{""}),
                        huxerui::UseState(huxerui::TextEditingValue{""}),
                        huxerui::UseState(huxerui::TextEditingValue{""}),
                        huxerui::UseState(huxerui::TextEditingValue{""}),
                        huxerui::UseState(0),
                        huxerui::UseState(huxerui::TextEditingValue{""}),
                        huxerui::UseState(huxerui::TextEditingValue{""}),
                        huxerui::UseState(huxerui::TextEditingValue{""})};
    const std::string id = provider.id;
    const std::string name = provider.name;

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

    // 编辑：预填表单后开弹窗（弹窗会卸载点击路径上的节点：推迟出指针事件路径）。
    auto showEdit = [dialog, tasks, toast, tool, fs, provider, id, revision] {
        FillForm(fs, provider);
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            ShowProviderForm(dialog, toast, tool, "编辑供应商 — " + provider.name, fs,
                             id, revision);
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

    return Card(huxerui::Column {
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
            huxerui::Spacer(),
        }.With(huxerui::Spacing(6.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        provider.baseUrl.empty()
            ? huxerui::View{huxerui::Row{}}
            : huxerui::View{huxerui::Text(provider.baseUrl)
                                .Style(huxerui::TextStyle{
                                    huxerui::Font::Monospace(font_size::kChip),
                                    theme.colors.on_surface_variant})},
        huxerui::Text("密钥 " + MaskedApiKey(provider.apiKey))
            .Style(huxerui::TextStyle{huxerui::Font::Monospace(font_size::kChip),
                                      theme.colors.on_surface_variant}),
        provider.usageUrl.empty()
            ? huxerui::View{huxerui::Row{}}
            : huxerui::View{huxerui::Row {
                  huxerui::Text(usageText)
                      .Style(huxerui::TextStyle{
                          huxerui::Font::System(font_size::kCaption),
                          usageError ? theme.colors.error
                                     : theme.colors.on_surface_variant}),
                  huxerui::Button("刷新")
                      .OnClick([tasks, usageCache, provider] {
                          // fetchUsage 阻塞最长 10s：RunWorker 跑，恢复点回 UI
                          // 线程后写缓存 State。
                          tasks.Launch([usageCache,
                                        provider]() -> huxerui::Task<void> {
                              WriteUsageCache(usageCache, provider.id,
                                              co_await FetchUsageText(provider));
                          });
                      })
                      .With(huxerui::Tooltip("重新查询用量")),
              }.With(huxerui::Spacing(8.0F),
                     huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))},
        provider.notes.empty()
            ? huxerui::View{huxerui::Row{}}
            : huxerui::View{huxerui::Text(provider.notes)
                                .Style(huxerui::TextStyle{
                                    huxerui::Font::System(font_size::kCaption),
                                    theme.colors.on_surface_variant})},
        huxerui::Row {
            huxerui::Button(isCurrent ? "当前使用" : "切换")
                .OnClick([toast, tool, id, name, bump] {
                    try {
                        providerStore().switchTo(tool, id);
                        toast.Show(std::format("已切换到 {}", name));
                    } catch (const std::exception& e) {
                        toast.Show(e.what());
                    }
                    bump();
                })
                .With(huxerui::Enabled(!isCurrent)),
            huxerui::Button("编辑").OnClick([showEdit] { showEdit(); }),
            huxerui::Button("复制").OnClick([toast, tool, id, bump] {
                try {
                    const auto copy = providerStore().duplicateProvider(tool, id);
                    toast.Show(std::format("已复制为 {}", copy.name));
                } catch (const std::exception& e) {
                    toast.Show(e.what());
                }
                bump();
            }),
            huxerui::Button("删除").OnClick([tasks, showDeleteConfirm] {
                // 弹窗会卸载点击路径上的节点：推迟出指针事件路径。
                tasks.Launch([=]() -> huxerui::Task<void> {
                    co_await huxerui::Delay(std::chrono::duration<double>{0});
                    showDeleteConfirm();
                });
            }),
        }.With(huxerui::Spacing(8.0F)),
    }.With(huxerui::Spacing(6.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
        .Key(id);
}

} // namespace

[[huxerui::composable]] huxerui::View ProvidersPage(std::string tool,
                                                    huxerui::State<int> revision) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto dialog = huxerui::UseDialog();
    // 首组合时探测 live 文件命中（本地文件读，UI 线程直接跑）。
    auto detected = huxerui::UseState<std::string>({});
    huxerui::Lifecycle(
        [tool, detected] {
            detected = providerStore().detectCurrent(tool);
            return [] {};
        },
        0);
    // 新增表单状态归页面作用域。
    const FormStates fs{huxerui::UseState(huxerui::TextEditingValue{""}),
                        huxerui::UseState(huxerui::TextEditingValue{""}),
                        huxerui::UseState(huxerui::TextEditingValue{""}),
                        huxerui::UseState(huxerui::TextEditingValue{""}),
                        huxerui::UseState(huxerui::TextEditingValue{""}),
                        huxerui::UseState(huxerui::TextEditingValue{""}),
                        huxerui::UseState(huxerui::TextEditingValue{""}),
                        huxerui::UseState(0),
                        huxerui::UseState(huxerui::TextEditingValue{""}),
                        huxerui::UseState(huxerui::TextEditingValue{""}),
                        huxerui::UseState(huxerui::TextEditingValue{""})};

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

    // 新增弹窗：prefill 非空时用预设模板预填（打开前清空上一轮输入）。
    auto showCreateDialog = [=](const models::Provider* prefill) {
        FillForm(fs, prefill ? *prefill : models::Provider{});
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            ShowProviderForm(dialog, toast, tool,
                             "新增供应商 — " + std::string(ToolName(tool)), fs, "",
                             revision);
        });
    };

    // 预设模板弹窗：内置预设列表，点选后预填新增表单。
    auto showPresetsDialog = [=] {
        const auto presets = models::builtinPresets(tool);
        dialog.Show(
            [=](huxerui::DialogContext ctx) -> huxerui::View {
                const huxerui::ThemeSpec& theme = huxerui::UseTheme();
                std::vector<huxerui::View> items;
                for (const auto& preset : presets) {
                    items.push_back(
                        huxerui::Column {
                            huxerui::Text(preset.name).Style(huxerui::TextStyle{
                                huxerui::Font::System(font_size::kBody)
                                    .WithWeight(huxerui::FontWeight::SemiBold),
                                theme.colors.on_surface}),
                            huxerui::Text(preset.baseUrl)
                                .Style(huxerui::TextStyle{
                                    huxerui::Font::Monospace(font_size::kChip),
                                    theme.colors.on_surface_variant}),
                        }.With(huxerui::Spacing(2.0F),
                               huxerui::Padding(huxerui::EdgeInsets::Symmetric(
                                   10.0F, 8.0F)),
                               huxerui::CornerRadius(8.0F),
                               huxerui::CrossAlign(
                                   huxerui::CrossAxisAlignment::Stretch))
                            .OnClick([ctx, showCreateDialog, preset] {
                                ctx.Dismiss();
                                showCreateDialog(&preset);
                            }));
                }
                return DialogCard(huxerui::Column {
                    huxerui::Text("预设模板", huxerui::TextRole::Title),
                    huxerui::Text("选择预填新增表单；API Key 需自行填写。")
                        .Style(huxerui::TextStyle{
                            huxerui::Font::System(font_size::kCaption),
                            theme.colors.on_surface_variant}),
                    huxerui::Column(std::move(items))
                        .With(huxerui::Spacing(4.0F),
                              huxerui::CrossAlign(
                                  huxerui::CrossAxisAlignment::Stretch)),
                    huxerui::Row {
                        huxerui::Spacer(),
                        huxerui::Button("取消").OnClick([ctx] { ctx.Dismiss(); }),
                    },
                }.With(huxerui::Spacing(12.0F),
                       huxerui::Frame{.width = 380.0F},
                       huxerui::CrossAlign(
                           huxerui::CrossAxisAlignment::Stretch)));
            },
            huxerui::DialogOptions{});
    };

    // 卡片列表：「使用中」= 组内 current 或 detectCurrent 命中。
    const auto& g = providerStore().group(tool);
    std::vector<huxerui::View> cards;
    for (const auto& p : g.providers) {
        const bool isCurrent = g.current == p.id;
        const bool active = isCurrent || detected.Get() == p.id;
        cards.push_back(
            ProviderCard(tool, p, active, isCurrent, tasks, toast, revision,
                         usageCache));
    }

    const std::string title = std::string(ToolName(tool)) + " 供应商";
    return PageScaffold(
        title,
        huxerui::Row {
            huxerui::Button("预设模板").OnClick([tasks, showPresetsDialog] {
                tasks.Launch([=]() -> huxerui::Task<void> {
                    co_await huxerui::Delay(std::chrono::duration<double>{0});
                    showPresetsDialog();
                });
            }),
            huxerui::Button("新增供应商").OnClick([showCreateDialog] {
                showCreateDialog(nullptr);
            }),
        }.With(huxerui::Spacing(8.0F)),
        cards.empty()
            ? huxerui::View{
                  huxerui::Column {
                      huxerui::Text("还没有供应商。点击右上角「新增供应商」或"
                                    "「预设模板」开始。")
                          .Style(huxerui::TextStyle{
                              huxerui::Font::System(font_size::kBody),
                              theme.colors.on_surface_variant}),
                  }.With(huxerui::Padding(32.0F),
                         huxerui::Grow(1.0F),
                         huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                         huxerui::CrossAlign(
                             huxerui::CrossAxisAlignment::Center))}
            : huxerui::View{huxerui::ScrollView(
                                huxerui::Column(std::move(cards))
                                    .With(huxerui::Spacing(10.0F),
                                          huxerui::CrossAlign(
                                              huxerui::CrossAxisAlignment::Stretch)))
                                .With(huxerui::Grow(1.0F))});
}

} // namespace llmswitch::ui
