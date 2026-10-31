// provider_switch.cpp — 供应商切换入口（直连 / 路由 / 聚合共用）。
//
// 三种模式与 cc-switch 的分类对齐：直连时 Agent 的 live 配置直接写供应
// 商地址；路由时写本地路由 /<tool>/，真实凭据留在应用里；聚合项同样走
// 本地路由，由 router 按 model 分发。route / aggregate 进入前必须先保证
// 路由已启动、该工具的路由代理已启用并持久化，否则下次启动或 Router 页
// 里仍是关闭状态。direct 回退供应商记录在 ProviderGroup::direct，切换
// 路由目标或聚合项不会丢失它。
#include "ui.h"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>

import llmswitch.models;
import llmswitch.store;

namespace llmswitch::ui {
namespace {

const models::Provider* FindProvider(const models::ProviderGroup& group,
                                     std::string_view id) {
    const auto it = std::ranges::find_if(
        group.providers, [id](const models::Provider& provider) {
            return provider.id == id;
        });
    return it == group.providers.end() ? nullptr : &*it;
}

bool IsUsableDirect(const models::ProviderGroup& group,
                    std::string_view id) {
    if (id.empty()) return false;
    const models::Provider* provider = FindProvider(group, id);
    return provider != nullptr && !models::isAggregateProvider(*provider);
}

std::string PickDirect(const models::ProviderGroup& group) {
    if (IsUsableDirect(group, group.direct)) return group.direct;
    for (const auto& provider : group.providers) {
        if (!models::isAggregateProvider(provider)) return provider.id;
    }
    return {};
}

// 进入 route / aggregate 前的共同前置：路由服务运行、该工具代理启用并
// 持久化。任何一步失败都抛出，让调用方在写 live 前中止。
void EnsureRouteReady(std::string_view tool) {
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

} // namespace

void SwitchProvider(std::string_view tool, const std::string& id) {
    const models::ProviderGroup group =
        providerStore().group(std::string(tool));
    const models::Provider* target = FindProvider(group, id);
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
        EnsureRouteReady(tool);
        providerStore().switchTo(tool, id, /*viaRouter=*/true);
        return;
    }

    // 普通供应商 = 回到直连。先写回真实地址，再撤销该工具的路由代理；
    // 反序会在写失败时留下指向本机端口的 live 配置。
    const bool wasRouteMode =
        group.mode == "route" || group.mode == "aggregate";
    providerStore().switchTo(tool, id, /*viaRouter=*/false);
    const auto* spec = models::findTool(tool);
    if (spec != nullptr && spec->additiveProviders && !wasRouteMode) {
        // dsh / zcode 的切换只改 live 条目；它们没有直连/路由/聚合页签，
        // 不应顺手关掉用户在路由页手动开启的代理。
        return;
    }
    if (routerInstance().toolEnabled(tool)) {
        routerInstance().setToolEnabled(tool, false);
    }
    const auto& config = providerStore().config();
    if (std::ranges::find(config.routerTools, tool) !=
        config.routerTools.end()) {
        providerStore().setRouterToolEnabled(tool, false);
    }
}

void StartRoute(std::string_view tool, const std::string& targetId) {
    const models::ProviderGroup group =
        providerStore().group(std::string(tool));
    const models::Provider* target = FindProvider(group, targetId);
    if (target == nullptr) {
        throw std::runtime_error(std::format("供应商不存在：{}", targetId));
    }
    if (models::isAggregateProvider(*target)) {
        throw std::runtime_error("聚合供应商没有独立路由目标，请使用聚合模式");
    }
    // switchTo 的 route 分支会在没有 direct 回退项时把本次目标补成回退项，
    // 因此首次从官方状态进入路由也不会留下空回退。
    EnsureRouteReady(tool);
    providerStore().switchTo(tool, targetId, /*viaRouter=*/true);
}

void SwitchRouteTarget(std::string_view tool, const std::string& targetId) {
    const models::ProviderGroup group =
        providerStore().group(std::string(tool));
    const models::Provider* target = FindProvider(group, targetId);
    if (target == nullptr) {
        throw std::runtime_error(std::format("供应商不存在：{}", targetId));
    }
    if (models::isAggregateProvider(*target)) {
        throw std::runtime_error("聚合供应商不能作为单供路由目标");
    }
    if (group.mode != "route") {
        StartRoute(tool, targetId);
        return;
    }
    EnsureRouteReady(tool);
    providerStore().switchTo(tool, targetId, /*viaRouter=*/true);
}

void SwitchOfficial(std::string_view tool) {
    providerStore().restoreOfficial(tool);
    if (routerInstance().toolEnabled(tool)) {
        routerInstance().setToolEnabled(tool, false);
    }
    const auto& config = providerStore().config();
    if (std::ranges::find(config.routerTools, tool) !=
        config.routerTools.end()) {
        providerStore().setRouterToolEnabled(tool, false);
    }
}

void StopRoute(std::string_view tool) {
    const models::ProviderGroup group =
        providerStore().group(std::string(tool));
    const std::string direct = PickDirect(group);
    if (direct.empty()) {
        // 没有普通供应商可回退：撤掉路由覆盖，交还厂商原生登录状态。
        providerStore().restoreOfficial(tool);
    } else {
        providerStore().switchTo(tool, direct, /*viaRouter=*/false);
    }
    if (routerInstance().toolEnabled(tool)) {
        routerInstance().setToolEnabled(tool, false);
    }
    const auto& config = providerStore().config();
    if (std::ranges::find(config.routerTools, tool) !=
        config.routerTools.end()) {
        providerStore().setRouterToolEnabled(tool, false);
    }
}

} // namespace llmswitch::ui
