// providers_internal.h — 供应商页跨编译单元的私有组件声明.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui.h"

import nlohmann.json;

namespace llmswitch::ui::provider_detail {

huxerui::Task<std::vector<std::string>> FetchModelIdsWithFallback(
    std::shared_ptr<huxerui::HttpClient> http, std::vector<std::string> urls,
    std::string apiKey, std::string upstreamFormat);

huxerui::Task<std::string> FetchUsageText(
    std::shared_ptr<huxerui::HttpClient> http, models::Provider provider);

huxerui::Task<double> FetchLatency(std::shared_ptr<huxerui::HttpClient> http,
                                   std::string url);

void WriteUsageCache(UsageCache cache, const std::string& id,
                     std::string text);

} // namespace llmswitch::ui::provider_detail

namespace llmswitch::ui {

huxerui::View ProviderFormPage(std::string tool, models::Provider initial,
                               bool isNew, huxerui::State<int> revision,
                               huxerui::State<std::string> formTarget,
                               huxerui::TaskScope closeTasks);

huxerui::View UsageFormPage(std::string tool, models::Provider initial,
                            huxerui::State<int> revision,
                            huxerui::State<std::string> formTarget,
                            huxerui::TaskScope closeTasks);

// ---- 模型清单 / 每模型参数辅助（provider_form_models.cpp）----
// modelsMeta 是 zcode 条目 models map 的原值回放（reasoning/modalities/
// limit），读写都走这组函数，改写一律「复制 → mutate → 写回」。
std::vector<std::string> FilterModelIds(
    const huxerui::StateList<std::string>& models, std::string_view query);
// 模型清单去重（保持顺序）：下拉弹层 VirtualList 以模型 id 为 key，重复
// key 会直接 abort，进下拉前统一去重。
std::vector<std::string> DedupeModels(std::vector<std::string> values);
void ReplaceModelList(const huxerui::StateList<std::string>& destination,
                      std::vector<std::string> values);
nlohmann::json WithModelMeta(
    nlohmann::json meta, const std::string& id,
    const std::function<void(nlohmann::json&)>& mutate);
bool ModelHasModality(const nlohmann::json& meta, const std::string& id,
                      const char* kind, const char* value);
nlohmann::json WithModelModality(nlohmann::json meta, const std::string& id,
                                 const char* kind, const char* value, bool on);
void ApplyModelLimit(huxerui::State<nlohmann::json> meta, const std::string& id,
                     const char* field, const std::string& text);
std::string ModelLimitText(const nlohmann::json& meta, const std::string& id,
                           const char* field);
bool ModelHasReasoning(const nlohmann::json& meta, const std::string& id);

// 模型选择器（锚定 Popup 的搜索 + 列表）；displayTarget 有效时点选同步
// 回填显示名，onPicked 非空时点选后额外回调（清单添加行直接入清单）。
huxerui::View ModelSelect(
    huxerui::StateList<std::string> fetched,
    huxerui::State<huxerui::TextEditingValue> search,
    huxerui::State<huxerui::TextEditingValue> target,
    huxerui::State<huxerui::TextEditingValue> displayTarget = {},
    std::function<void(const std::string&)> onPicked = {});

// ---- 用量刷新间隔（provider_usage_form.cpp 定义，供应商表单页共用）----
extern const std::vector<huxerui::StringVariant> kUsageIntervals;
extern const std::vector<int> kUsageMinutes;
int UsageIntervalIndex(int minutes);

huxerui::View OfficialCard(std::string tool, bool active,
                           huxerui::ToastHandle toast,
                           huxerui::State<int> revision);

huxerui::View ProviderCard(std::string tool, const models::Provider& provider,
                           bool active, bool isCurrent,
                           huxerui::TaskScope tasks,
                           huxerui::ToastHandle toast,
                           huxerui::State<int> revision,
                           UsageCache usageCache,
                           huxerui::State<std::string> formTarget);

} // namespace llmswitch::ui
