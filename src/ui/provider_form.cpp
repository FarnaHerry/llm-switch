// provider_form.cpp — 供应商新增/编辑页（表单调度器）。
//
// 页面只负责：组合表单状态、按 AgentPolicyFor(tool) 取该 agent 的策略、
// 依序拼装公共区块（provider_form_common.cpp）与 agent 专属区块
// （provider_form_zcode.cpp / provider_form_claude.cpp /
// provider_form_codex.cpp / provider_form_openai.cpp）、统一校验与装配、
// 保存与返回。公共代码不出现 tool == 分支；agent 差异全部收敛在各自的
// 策略常量与专属区块文件里。
#include <huxerui/huxerui.h>

#include <chrono>
#include <string>
#include <vector>

#include "providers_internal.h"

import llmswitch.config;
import llmswitch.models;
import llmswitch.store;
import nlohmann.json;

namespace llmswitch::ui {

// 新增/编辑供应商页（整页表单，不再是弹窗——字段太多弹窗太挤）。
// 表单状态以 initial 为初值（UseState 初值只在首次组合生效；调用方用
// .Key("form:" + target) 保证换编辑目标整体重建）。isNew 时顶部内嵌预设
// 模板区（点选 FillForm 预填）。通用校验：名称必填；URL / API Key 按各
// agent 策略；有主模型行的 agent 按 needsModel 校验模型必填；zcode 校验
// 模型清单非空。保存成功 toast 后返回列表。
// 点击回调只写 State：框架在后续帧重组并卸载本页，不必排入异步任务队列。
[[huxerui::composable]] huxerui::View ProviderFormPage(
    std::string tool, models::Provider initial, bool isNew,
    huxerui::State<int> revision, huxerui::State<std::string> formTarget) {
    const auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    const auto http = huxerui::UseService<huxerui::HttpClient>();
    const auto* spec = models::findTool(tool);
    const AgentFormPolicy& policy = AgentPolicyFor(tool);
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
    // 才是供应商的模型清单（仅 zcode 有清单 UI：唯一模型输入；其他工具
    // 无清单 UI，仅承装已保存清单，保存时原样回存不丢数据），
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

    // State 赋值只请求下一帧；直接完成导航，避免等待低优先级 UI 任务队列。
    auto goBack = [formTarget] { formTarget = ""; };

    // 公共区块：预设（仅新增）+ 通用字段。
    huxerui::View fields =
        huxerui::Column {
            PresetsSection(tool, fs, fetchedModels),
            CommonFields(policy, fs, showKey, keyHover, showModelFetchOptions),
        }.With(huxerui::Spacing(12.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    // agent 专属区块：zcode 一整块（含模型清单）；其余 agent 只有
    // 主模型行 + 策略开关的区块，没有备选模型清单。
    if (tool == "zcode") {
        fields = huxerui::Column {
            std::move(fields),
            ZcodeFields(fs, zcodeEnabled, fetching, fetchedModels, modelList,
                        addModel, modelMeta, expandedMeta, contextInput,
                        outputInput, tasks, http, toast),
        }.With(huxerui::Spacing(12.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    } else {
        fields = huxerui::Column {
            std::move(fields),
            policy.primaryModelLabel.empty()
                ? huxerui::View{huxerui::Row{}}
                : PrimaryModelRow(policy, fs, fetchedModels),
            ModelFetchButton(fs, fetching, fetchedModels, tasks, http, toast),
            policy.showMappings ? MappingFields(fs, fetchedModels)
                                : huxerui::View{huxerui::Row{}},
            policy.showApiFormat ? ApiFormatFields(fs)
                                 : huxerui::View{huxerui::Row{}},
            policy.showToml ? TomlField(fs) : huxerui::View{huxerui::Row{}},
        }.With(huxerui::Spacing(12.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
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
            huxerui::ScrollView(std::move(fields)).With(huxerui::Grow(1.0F)),
            huxerui::Row {
                huxerui::Button("取消").OnClick([goBack] { goBack(); }),
                huxerui::Button(isNew ? "添加" : "保存")
                    .OnClick([=] {
                        const std::string name = fs.name.Get().text;
                        const std::string baseUrl = fs.baseUrl.Get().text;
                        const std::string model = fs.model.Get().text;
                        if (name.empty()) {
                            toast.Show("名称不能为空");
                            return;
                        }
                        if (policy.urlRequired && baseUrl.empty()) {
                            toast.Show("URL 不能为空");
                            return;
                        }
                        if (fs.fullUrl.Get() && baseUrl.empty()) {
                            toast.Show("完整 URL 不能为空");
                            return;
                        }
                        if (policy.keyRequired && fs.apiKey.Get().text.empty()) {
                            toast.Show("API Key 不能为空");
                            return;
                        }
                        if (spec != nullptr && spec->needsModel &&
                            !policy.primaryModelLabel.empty() && model.empty()) {
                            toast.Show("模型不能为空");
                            return;
                        }
                        if (tool == "zcode" &&
                            !ZcodeModelListValid(modelList, toast)) {
                            return;
                        }
                        models::Provider p;
                        p.id = editingId;
                        p.name = name;
                        p.baseUrl = baseUrl;
                        p.modelFetchUrl = fs.modelFetchUrl.Get().text;
                        p.apiKey = fs.apiKey.Get().text;
                        p.model = model;
                        // 模型清单 = 表单当前清单（初值来自已保存清单，手动
                        // 增删都体现在这里）。
                        for (std::size_t i = 0; i < modelList.Size(); ++i) {
                            p.models.push_back(modelList.At(i));
                        }
                        if (tool == "zcode") {
                            AssembleZcodeProvider(p, modelList, modelMeta);
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
                                AfterSaveZcode(savedId, zcodeEnabled.Get());
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
