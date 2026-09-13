// provider_form_zcode.cpp — ZCode 供应商表单的 agent 专属区块：条目启用
// 开关（对应 ZCode config.json 的 enabled 字段）、模型清单（zcode 无主模型
// 概念，清单即唯一输入）与每模型参数面板（输入/输出模态、上下文窗口、
// 最大输出、思维链——直接改 modelsMeta 原值，收编自 ZCode 的元数据按原值
// 回放）。策略常量与校验/装配/保存后置也在这里。
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
        .primaryModelLabel = {},  // 无主模型行：模型清单是唯一输入
        .showMappings = false,
        .showApiFormat = false,
        .showToml = false,
    };
    return policy;
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
    // 对应 ZCode 条目的 enabled 字段（ZCode 页面里每个供应商都有
    // 启用/停用开关）；保存时随条目写回，不影响切换的互斥语义。
    huxerui::View toggle =
        huxerui::Switch("在 ZCode 中启用此供应商", zcodeEnabled.Get())
            .OnChanged([zcodeEnabled](bool checked) { zcodeEnabled = checked; });

    AlternateModelExtras extras;
    extras.listTitle = "模型清单（必填，保存即写入 ZCode）";
    extras.onRemove = [expandedMeta](std::size_t) { expandedMeta = -1; };
    // 行尾「模型参数」按钮：展开该行的参数面板（再点收起）。
    extras.rowTrailing = [expandedMeta](std::size_t index,
                                        const std::string& id)
        -> huxerui::View {
        return huxerui::IconButton(app::images::edit, "模型参数")
            .OnClick([expandedMeta, index, id] {
                if (expandedMeta.Get() == static_cast<int>(index)) {
                    expandedMeta = -1;
                    return;
                }
                expandedMeta = static_cast<int>(index);
            })
            .With(huxerui::Tooltip("编辑模型参数"));
    };
    // 每模型参数编辑面板：直接改 modelsMeta 原值（收编自 ZCode
    // 的 reasoning/limit/modalities/zcode 元数据），未提供控件的
    // 字段保存时按原值回放。输入/输出模态复选框与 ZCode 页面
    // 一一对应（数组值 text/image/video/pdf）。仅展开行返回面板。
    extras.panel = [theme, modelMeta, expandedMeta, contextInput, outputInput](
                       std::size_t index, const std::string& id)
        -> huxerui::View {
        if (expandedMeta.Get() != static_cast<int>(index)) {
            return huxerui::View{huxerui::Row{}};
        }
        const auto& meta = modelMeta.Get();
        return huxerui::Column {
            huxerui::Row {
                huxerui::Text("输入类型")
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kBody),
                        theme.colors.on_surface_variant}),
                huxerui::Checkbox("文本", ModelHasModality(meta, id, "input", "text"))
                    .OnChanged([modelMeta, id](bool on) {
                        modelMeta =
                            WithModelModality(modelMeta.Get(), id, "input", "text", on);
                    }),
                huxerui::Checkbox("图片", ModelHasModality(meta, id, "input", "image"))
                    .OnChanged([modelMeta, id](bool on) {
                        modelMeta =
                            WithModelModality(modelMeta.Get(), id, "input", "image", on);
                    }),
                huxerui::Checkbox("视频", ModelHasModality(meta, id, "input", "video"))
                    .OnChanged([modelMeta, id](bool on) {
                        modelMeta =
                            WithModelModality(modelMeta.Get(), id, "input", "video", on);
                    }),
                huxerui::Checkbox("PDF", ModelHasModality(meta, id, "input", "pdf"))
                    .OnChanged([modelMeta, id](bool on) {
                        modelMeta =
                            WithModelModality(modelMeta.Get(), id, "input", "pdf", on);
                    }),
            }.With(huxerui::Spacing(10.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
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
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
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
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
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
               huxerui::Padding(huxerui::EdgeInsets::Symmetric(16.0F, 0.0F)));
    };

    return huxerui::Column {
        toggle,
        ModelFetchButton(fs, fetching, fetchedModels, tasks, http, toast),
        AlternateModelList(fs, modelList, addModel, fetchedModels, toast,
                           extras),
    }.With(huxerui::Spacing(12.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

bool ZcodeModelListValid(huxerui::StateList<std::string> modelList,
                         huxerui::ToastHandle toast) {
    if (modelList.Empty()) {
        toast.Show("模型清单不能为空");
        return false;
    }
    return true;
}

void AssembleZcodeProvider(models::Provider& p,
                           huxerui::StateList<std::string> modelList,
                           huxerui::State<nlohmann::json> modelMeta) {
    // ZCode 没有主模型概念：主模型仅为本应用展示用默认值，取清单首项。
    p.model = p.models.empty() ? "" : p.models.front();
    p.modelsMeta = modelMeta.Get();
}

void AfterSaveZcode(const std::string& savedId, bool enabled) {
    providerStore().setZcodeEntryEnabled(savedId, enabled);
}

} // namespace llmswitch::ui
