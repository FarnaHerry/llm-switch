// provider_switch.cpp — 供应商切换入口（普通 / 聚合共用）。
//
// 聚合供应商不直连上游：Agent 的 live 配置会指向本地路由的 /<tool>/。
// 因此切换前必须先保证路由已启动、该工具的路由代理已启用，并把这两项写入
// 配置，否则下次启动或 Router 页里仍是关闭状态，聚合供应商会不可用。
#include "ui.h"

#include <format>
#include <stdexcept>
#include <string>
#include <string_view>

import llmswitch.models;
import llmswitch.store;

namespace llmswitch::ui {

void SwitchProvider(std::string_view tool, const std::string& id) {
    const models::ProviderGroup group = providerStore().group(std::string(tool));
    const models::Provider* target = nullptr;
    for (const auto& provider : group.providers) {
        if (provider.id == id) {
            target = &provider;
            break;
        }
    }
    if (target == nullptr) {
        throw std::runtime_error(std::format("供应商不存在：{}", id));
    }
    if (models::isAggregateProvider(*target)) {
        if (tool == "kimi-code" && target->model.empty()) {
            // switchTo 会从成员回退默认模型；这里只拦截所有成员都没有模型
            // 的情况，避免先启动路由再写失败。
            bool memberModel = false;
            for (const auto& upstream : target->upstreams) {
                if (upstream.providerId.empty()) {
                    if (!upstream.model.empty()) memberModel = true;
                } else {
                    for (const auto& provider : group.providers) {
                        if (provider.id == upstream.providerId &&
                            (!upstream.model.empty() ||
                             !provider.model.empty() ||
                             !provider.models.empty())) {
                            memberModel = true;
                            break;
                        }
                    }
                }
                if (memberModel) break;
            }
            if (!memberModel) {
                throw std::runtime_error(
                    "Kimi Code 聚合需要默认模型：请先给至少一个成员设置模型");
            }
        }
        auto& localRouter = routerInstance();
        if (!localRouter.running()) {
            localRouter.start(providerStore().config().routerPort);
        }
        if (!localRouter.toolEnabled(tool)) {
            localRouter.setToolEnabled(tool, true);
            providerStore().setRouterToolEnabled(tool, true);
        }
        if (!providerStore().config().routerEnabled) {
            providerStore().setRouterEnabled(true);
        }
    }
    providerStore().switchTo(tool, id);
}

} // namespace llmswitch::ui
