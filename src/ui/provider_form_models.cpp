// provider_form_models.cpp — 供应商表单的模型辅助：模型选择器
//（ModelSelect，锚定 Popup 的搜索 + 列表）与每模型参数（modelsMeta）
// 读写（reasoning / modalities / limit 的取值与改写）。声明在
// providers_internal.h，供 provider_form.cpp 的表单体使用。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "app_resources.h"
#include "providers_internal.h"

import llmswitch.models;
import nlohmann.json;

namespace llmswitch::ui {

std::vector<std::string> FilterModelIds(
    const huxerui::StateList<std::string>& models, std::string_view query) {
    std::vector<std::string> filtered;
    filtered.reserve(models.Size());
    for (const auto& model : models) {
        if (query.empty() || model.find(query) != std::string::npos) {
            filtered.push_back(model);
        }
    }
    return filtered;
}

// 模型清单去重（保持顺序）：上游 /models 偶有重复 id，下拉弹层的
// VirtualList 以模型 id 为 key，重复 key 会直接 abort，进下拉前统一去重。
std::vector<std::string> DedupeModels(std::vector<std::string> values) {
    std::vector<std::string> unique;
    unique.reserve(values.size());
    std::unordered_set<std::string> seen;
    for (auto& value : values) {
        if (seen.insert(value).second) {
            unique.push_back(std::move(value));
        }
    }
    return unique;
}

void ReplaceModelList(const huxerui::StateList<std::string>& destination,
                      std::vector<std::string> values) {
    destination.Clear();
    for (auto& value : DedupeModels(std::move(values))) {
        destination.PushBack(std::move(value));
    }
}

// 复制 id 对应的模型参数（无则给空对象），交给 mutate 修改后写回副本。
nlohmann::json WithModelMeta(
    nlohmann::json meta, const std::string& id,
    const std::function<void(nlohmann::json&)>& mutate) {
    nlohmann::json m = meta.contains(id) && meta[id].is_object()
                           ? meta[id]
                           : nlohmann::json::object();
    mutate(m);
    meta[id] = std::move(m);
    return meta;
}

// modalities 数组里是否含某模态（input: text/image/video/pdf，output: text）。
bool ModelHasModality(const nlohmann::json& meta, const std::string& id,
                      const char* kind, const char* value) {
    if (!meta.contains(id) || !meta[id].is_object()) return false;
    const auto& m = meta[id];
    if (!m.contains("modalities") || !m["modalities"].is_object()) return false;
    const auto& arr = m["modalities"][kind];
    return arr.is_array() && std::find(arr.begin(), arr.end(),
                                       nlohmann::json(value)) != arr.end();
}

// 勾选/取消一个模态复选框（数组不存在时创建）。
nlohmann::json WithModelModality(nlohmann::json meta, const std::string& id,
                                 const char* kind, const char* value, bool on) {
    return WithModelMeta(std::move(meta), id,
                         [kind, value, on](nlohmann::json& m) {
                             if (!m.contains("modalities") ||
                                 !m["modalities"].is_object()) {
                                 m["modalities"] = nlohmann::json::object();
                             }
                             auto& arr = m["modalities"][kind];
                             if (!arr.is_array()) {
                                 arr = nlohmann::json::array();
                             }
                             auto it = std::find(arr.begin(), arr.end(),
                                                 nlohmann::json(value));
                             if (on && it == arr.end()) {
                                 arr.push_back(value);
                             } else if (!on && it != arr.end()) {
                                 arr.erase(it);
                             }
                         });
}

// 把十进制数字写进 limit.<field>（context / output）；空串或非法输入保留
// 原值不动，避免打字中间态污染元数据。
void ApplyModelLimit(huxerui::State<nlohmann::json> meta, const std::string& id,
                     const char* field, const std::string& text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return;
    const auto last = text.find_last_not_of(" \t");
    const std::string digits = text.substr(first, last - first + 1);
    if (digits.empty() || digits.find_first_not_of("0123456789") !=
                              std::string::npos) {
        return;
    }
    long long value = 0;
    try {
        value = std::stoll(digits);
    } catch (const std::exception&) {
        return;
    }
    meta = WithModelMeta(meta.Get(), id, [field, value](nlohmann::json& m) {
        if (!m.contains("limit") || !m["limit"].is_object()) {
            m["limit"] = nlohmann::json::object();
        }
        m["limit"][field] = value;
    });
}

// 读 limit.<field> 的十进制展示文本；缺失或异常返回空串。
std::string ModelLimitText(const nlohmann::json& meta, const std::string& id,
                           const char* field) {
    if (!meta.contains(id) || !meta[id].is_object()) return {};
    const auto& m = meta[id];
    if (!m.contains("limit") || !m["limit"].is_object()) return {};
    const auto& v = m["limit"][field];
    if (!v.is_number()) return {};
    try {
        return std::to_string(v.get<long long>());
    } catch (const std::exception&) {
        return {};
    }
}

// 是否开启了思维链（reasoning.enabled）。
bool ModelHasReasoning(const nlohmann::json& meta, const std::string& id) {
    if (!meta.contains(id) || !meta[id].is_object()) return false;
    const auto& m = meta[id];
    return m.contains("reasoning") && m["reasoning"].is_object() &&
           m["reasoning"].value("enabled", false);
}

// 模型选择器：关闭时只有一个搜索图标；点击后由锚定 Popup 展开搜索框和模型
// 列表，避免每一行都占用一个宽大的下拉输入框。点选回填目标模型字段；映射行
// 还会同步回填菜单显示名，方便先选模型、再手动修改显示名称。onPicked 非空时
// 点选后额外回调（清单添加行用它直接把所选模型加进清单）。
[[huxerui::composable]] huxerui::View ModelSelect(
    huxerui::StateList<std::string> fetched,
    huxerui::State<huxerui::TextEditingValue> search,
    huxerui::State<huxerui::TextEditingValue> target,
    huxerui::State<huxerui::TextEditingValue> displayTarget,
    std::function<void(const std::string&)> onPicked) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    const auto popup = huxerui::UsePopup();

    return huxerui::IconButton(app::images::search, "选择模型")
        .OnClick([popup, fetched, search, target, displayTarget, islands,
                  onPicked = std::move(onPicked)] {
            popup.Show(
                [fetched, search, target, displayTarget, islands,
                 onPicked](huxerui::PopupContext context) {
                    const auto suggestions =
                        FilterModelIds(fetched, search.Get().text);
                    huxerui::View modelList = huxerui::Text("没有匹配的模型");
                    if (!suggestions.empty()) {
                        modelList = huxerui::VirtualList(
                                        suggestions,
                                        [context, search, target,
                                         displayTarget, onPicked](const std::string& model) {
                                            return huxerui::Button(model)
                                                .Key(model)
                                                .OnClick([context, model, search,
                                                          target, displayTarget,
                                                          onPicked] {
                                                    context.Dismiss();
                                                    const huxerui::TextEditingValue value{
                                                        model};
                                                    target = value;
                                                    if (displayTarget.IsValid()) {
                                                        displayTarget = value;
                                                    }
                                                    search = huxerui::TextEditingValue{};
                                                    if (onPicked) {
                                                        onPicked(model);
                                                    }
                                                });
                                        })
                                        .EstimatedItemExtent(40.0F)
                                        .CacheExtent(120.0F)
                                        .With(huxerui::Spacing(4.0F));
                    }
                    return huxerui::Column {
                        huxerui::TextField(search.Get())
                            .Label("搜索模型")
                            .Placeholder("输入模型名称")
                            .LeadingIcon(app::images::search)
                            .Variant(huxerui::TextFieldVariant::Outlined)
                            .OnChanged([search](
                                           const huxerui::TextEditingValue& value) {
                                search = value;
                            }),
                        huxerui::View{modelList}
                            .With(huxerui::Frame{.height = 240.0F}),
                    }.With(huxerui::Spacing(8.0F),
                           huxerui::Padding(islands.island_padding),
                           huxerui::Background(islands.overlay),
                           huxerui::Border(islands.outline_soft, 1.0F),
                           huxerui::CornerRadius(islands.nested_radius),
                           huxerui::Frame{.width = 320.0F},
                           huxerui::CrossAlign(
                               huxerui::CrossAxisAlignment::Stretch));
                },
                huxerui::PopupOptions{
                    .placement = {huxerui::AnchorSide::Below,
                                  huxerui::AnchorAlignment::End}});
        })
        .With(popup.Anchor(), huxerui::Tooltip("选择模型"));
}

} // namespace llmswitch::ui
