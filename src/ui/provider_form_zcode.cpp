// provider_form_zcode.cpp — ZCode 供应商表单的 agent 专属区块：条目启用
// 开关（对应 ZCode config.json 的 enabled 字段）、模型列表（zcode 无主模型
// 概念，列表即唯一输入：每行可编辑，行尾下拉搜索与「模型参数」按钮，添加
// 走弹窗）与每模型参数面板（输入/输出模态、上下文窗口、最大输出、思维链
// ——直接改 modelsMeta 原值，收编自 ZCode 的元数据按原值回放）。策略常量
// 与校验/装配/保存后置也在这里。
#include <huxerui/huxerui.h>

#include <format>
#include <functional>
#include <string>
#include <vector>

#include "app_resources.h"
#include "providers_internal.h"

import llmswitch.models;
import llmswitch.store;
import nlohmann.json;

namespace llmswitch::ui {

const AgentFormPolicy& ZcodeFormPolicy() {
    static const AgentFormPolicy policy{
        .urlLabel = "URL",
        .keyLabel = "API Key",
        .urlRequired = true,
        .keyRequired = false,
        .primaryModelLabel = {},  // 无主模型行：模型列表是唯一输入
        .showMappings = false,
        .showApiFormat = false,
        .showToml = false,
    };
    return policy;
}

// 添加模型弹窗：输入模型 ID 或从拉取结果下拉点选，去重后入列表。
[[huxerui::composable]] huxerui::View ZcodeAddModelContent(
    const FormStates& fs, huxerui::State<huxerui::TextEditingValue> newModel,
    huxerui::StateList<std::string> modelList,
    huxerui::StateList<std::string> fetchedModels,
    huxerui::DialogContext ctx, huxerui::ToastHandle toast) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const auto add = [modelList, newModel, ctx, toast] {
        const std::string raw = newModel.Get().text;
        const auto first = raw.find_first_not_of(" \t");
        if (first == std::string::npos) {
            toast.Show("模型 ID 不能为空");
            return;
        }
        const auto last = raw.find_last_not_of(" \t");
        const std::string id = raw.substr(first, last - first + 1);
        for (std::size_t i = 0; i < modelList.Size(); ++i) {
            if (modelList.At(i) == id) {
                toast.Show("该模型已在列表中");
                return;
            }
        }
        modelList.PushBack(id);
        ctx.Dismiss();
    };
    return DialogCard(huxerui::ScrollView(huxerui::Column {
        huxerui::Text("添加模型", huxerui::TextRole::Title),
        huxerui::TextField(newModel.Get())
            .Label("模型 ID")
            .Placeholder("例如 glm-5.3")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([newModel](const huxerui::TextEditingValue& v) {
                newModel = v;
            }),
        fetchedModels.Empty()
            ? huxerui::View{huxerui::Row{}}
            : ModelSelect(fetchedModels, fs.modelSearch, newModel),
        huxerui::Row {
            huxerui::Button("取消").OnClick([ctx] { ctx.Dismiss(); }),
            huxerui::Button("添加").OnClick([add] { add(); }),
        }.With(huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween)),
    }.With(huxerui::Spacing(12.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))))
        .With(huxerui::Frame{.width = 480.0F});
}

[[huxerui::composable]] huxerui::View ZcodeFields(
    const FormStates& fs, huxerui::State<bool> zcodeEnabled,
    huxerui::State<bool> fetching, huxerui::StateList<std::string> fetchedModels,
    huxerui::StateList<std::string> modelList,
    huxerui::State<huxerui::TextEditingValue> addModel,
    huxerui::State<nlohmann::json> modelMeta, huxerui::State<int> expandedMeta,
    huxerui::State<huxerui::TextEditingValue> contextInput,
    huxerui::State<huxerui::TextEditingValue> outputInput,
    huxerui::TaskScope tasks, std::shared_ptr<huxerui::HttpClient> http,
    huxerui::ToastHandle toast) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto dialog = huxerui::UseDialog();
    // 对应 ZCode 条目的 enabled 字段（ZCode 页面里每个供应商都有
    // 启用/停用开关）；保存时随条目写回，不影响切换的互斥语义。
    huxerui::View toggle =
        huxerui::Switch("在 ZCode 中启用此供应商", zcodeEnabled.Get())
            .OnChanged([zcodeEnabled](bool checked) { zcodeEnabled = checked; });

    // 添加模型：弹窗输入/点选（写 formTarget 类的弹窗会卸载点击路径上的
    // 节点：推迟出指针事件路径）。打开时清空上次的输入。
    auto showAddDialog = [fs, tasks, dialog, addModel, modelList,
                          fetchedModels, toast] {
        addModel = huxerui::TextEditingValue{};
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            dialog.Show(
                [=](huxerui::DialogContext ctx) -> huxerui::View {
                    return ZcodeAddModelContent(fs, addModel, modelList,
                                                fetchedModels, ctx, toast);
                },
                huxerui::DialogOptions{});
        });
    };

    std::vector<huxerui::View> items;
    items.push_back(
        huxerui::Text("模型列表")
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                theme.colors.on_surface_variant}));
    for (std::size_t i = 0; i < modelList.Size(); ++i) {
        const std::string id = modelList.At(i);
        // 每行：可编辑输入框（就地改 ID）+ 下拉搜索（点选替换该行）+
        // 「模型参数」按钮 + 移除。
        std::vector<huxerui::View> row;
        row.push_back(
            huxerui::TextField(huxerui::TextEditingValue{id})
                .Placeholder("模型 ID")
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([modelList, i](const huxerui::TextEditingValue& v) {
                    modelList.Set(i, v.text);
                })
                .With(huxerui::Grow(1.0F)));
        row.push_back(
            fetchedModels.Empty()
                ? huxerui::View{huxerui::Row{}}
                : ModelSelect(fetchedModels, fs.modelSearch, addModel, {},
                              [modelList, i](const std::string& picked) {
                                  modelList.Set(i, picked);
                              }));
        const bool expanded = expandedMeta.Get() == static_cast<int>(i);
        row.push_back(
            huxerui::IconButton(app::images::edit, "模型参数")
                .OnClick([expandedMeta, i] {
                    if (expandedMeta.Get() == static_cast<int>(i)) {
                        expandedMeta = -1;
                        return;
                    }
                    expandedMeta = static_cast<int>(i);
                })
                .With(huxerui::Tooltip("编辑模型参数")));
        row.push_back(
            huxerui::IconButton(app::images::trash, "移除")
                .OnClick([modelList, expandedMeta, i] {
                    modelList.Erase(i);
                    expandedMeta = -1;
                }));
        items.push_back(
            huxerui::Row(std::move(row))
                .With(huxerui::Spacing(8.0F),
                      huxerui::CrossAlign(
                          huxerui::CrossAxisAlignment::Center)));
        // 每模型参数编辑面板：直接改 modelsMeta 原值（收编自 ZCode
        // 的 reasoning/limit/modalities/zcode 元数据），未提供控件的
        // 字段保存时按原值回放。输入/输出模态复选框与 ZCode 页面
        // 一一对应（数组值 text/image/video/pdf）。仅展开行显示。
        if (!expanded) continue;
        const auto& meta = modelMeta.Get();
        items.push_back(
            huxerui::Column {
                huxerui::Row {
                    huxerui::Text("输入类型")
                        .Style(huxerui::TextStyle{
                            huxerui::Font::System(font_size::kBody),
                            theme.colors.on_surface_variant}),
                    huxerui::Checkbox("文本", ModelHasModality(meta, id, "input", "text"))
                        .OnChanged([modelMeta, id](bool on) {
                            modelMeta = WithModelModality(
                                modelMeta.Get(), id, "input", "text", on);
                        }),
                    huxerui::Checkbox("图片", ModelHasModality(meta, id, "input", "image"))
                        .OnChanged([modelMeta, id](bool on) {
                            modelMeta = WithModelModality(
                                modelMeta.Get(), id, "input", "image", on);
                        }),
                    huxerui::Checkbox("视频", ModelHasModality(meta, id, "input", "video"))
                        .OnChanged([modelMeta, id](bool on) {
                            modelMeta = WithModelModality(
                                modelMeta.Get(), id, "input", "video", on);
                        }),
                    huxerui::Checkbox("PDF", ModelHasModality(meta, id, "input", "pdf"))
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
                    huxerui::Checkbox("文本", ModelHasModality(meta, id, "output", "text"))
                        .OnChanged([modelMeta, id](bool on) {
                            modelMeta = WithModelModality(
                                modelMeta.Get(), id, "output", "text", on);
                        }),
                }.With(huxerui::Spacing(10.0F),
                       huxerui::CrossAlign(
                           huxerui::CrossAxisAlignment::Center)),
                huxerui::Row {
                    huxerui::TextField(contextInput.Get())
                        .Label("上下文窗口（tokens）")
                        .Placeholder("例如 1000000")
                        .Variant(huxerui::TextFieldVariant::Outlined)
                        .OnChanged([modelMeta, id, contextInput](
                                       const huxerui::TextEditingValue& v) {
                            contextInput = v;
                            ApplyModelLimit(modelMeta, id, "context", v.text);
                        })
                        .With(huxerui::Grow(1.0F)),
                    huxerui::TextField(outputInput.Get())
                        .Label("最大输出（tokens）")
                        .Placeholder("例如 128000")
                        .Variant(huxerui::TextFieldVariant::Outlined)
                        .OnChanged([modelMeta, id, outputInput](
                                       const huxerui::TextEditingValue& v) {
                            outputInput = v;
                            ApplyModelLimit(modelMeta, id, "output", v.text);
                        })
                        .With(huxerui::Grow(1.0F)),
                }.With(huxerui::Spacing(10.0F),
                       huxerui::CrossAlign(
                           huxerui::CrossAxisAlignment::Center)),
                huxerui::Switch("思维链（reasoning）",
                                ModelHasReasoning(modelMeta.Get(), id))
                    .OnChanged([modelMeta, id](bool on) {
                        modelMeta = WithModelMeta(
                            modelMeta.Get(), id, [on](nlohmann::json& m) {
                                if (!m.contains("reasoning") ||
                                    !m["reasoning"].is_object()) {
                                    m["reasoning"] = nlohmann::json::object();
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
    items.push_back(
        huxerui::Row {huxerui::Button("添加").OnClick([showAddDialog] {
            showAddDialog();
        })}.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));

    return huxerui::Column {
        toggle,
        ModelFetchButton(fs, fetching, fetchedModels, tasks, http, toast),
        huxerui::Column(std::move(items))
            .With(huxerui::Spacing(8.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
    }.With(huxerui::Spacing(12.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

bool ZcodeModelListValid(huxerui::StateList<std::string> modelList,
                         huxerui::ToastHandle toast) {
    // 空白行视同不存在：可编辑输入框允许打字中间态，保存前统一裁剪。
    for (std::size_t i = 0; i < modelList.Size(); ++i) {
        if (modelList.At(i).find_first_not_of(" \t") != std::string::npos) {
            return true;
        }
    }
    toast.Show("模型列表不能为空");
    return false;
}

void AssembleZcodeProvider(models::Provider& p,
                           huxerui::StateList<std::string> modelList,
                           huxerui::State<nlohmann::json> modelMeta) {
    // 就地编辑可能留下空白行：装配时裁剪空 ID 去重（保持顺序）。
    p.models.clear();
    for (std::size_t i = 0; i < modelList.Size(); ++i) {
        const std::string& raw = modelList.At(i);
        const auto first = raw.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        const auto last = raw.find_last_not_of(" \t");
        const std::string id = raw.substr(first, last - first + 1);
        bool dup = false;
        for (const auto& cur : p.models) dup = dup || cur == id;
        if (!dup) p.models.push_back(id);
    }
    // ZCode 没有主模型概念：主模型仅为本应用展示用默认值，取列表首项。
    p.model = p.models.empty() ? "" : p.models.front();
    p.modelsMeta = modelMeta.Get();
}

void AfterSaveZcode(const std::string& savedId, bool enabled) {
    providerStore().setZcodeEntryEnabled(savedId, enabled);
}

} // namespace llmswitch::ui
