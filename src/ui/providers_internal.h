// providers_internal.h — 供应商页跨编译单元的私有组件声明.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ui.h"

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
                               huxerui::State<std::string> formTarget);

huxerui::View UsageFormPage(std::string tool, models::Provider initial,
                            huxerui::State<int> revision,
                            huxerui::State<std::string> formTarget);

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
