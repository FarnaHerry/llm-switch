// provider_card.cpp — 供应商列表卡片与卡片操作.
#include <huxerui/huxerui.h>

#include <chrono>
#include <format>
#include <string>
#include <utility>

#include "app_resources.h"
#include "providers_internal.h"

import llmswitch.models;
import llmswitch.store;

namespace llmswitch::ui {

using provider_detail::FetchLatency;
using provider_detail::FetchUsageText;
using provider_detail::WriteUsageCache;

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
    const auto http = huxerui::UseService<huxerui::HttpClient>();
    const std::string id = provider.id;
    const std::string name = provider.name;
    const std::string accessUrl = models::effectiveBaseUrl(provider);

    // 连通检测状态（卡片级）：latency 空 = 未检测；检测中禁用按钮。
    // State 经 .Key(id) 随卡片保活，revision 重读不丢。
    auto checking = huxerui::UseState(false);
    auto latency = huxerui::UseState<std::string>({});

    // 用量展示：启用开关打开且 usageUrl 非空才显示；缓存未命中显示占位。
    std::string usageText;
    bool usageError = false;
    const bool usageConfigured = provider.usageEnabled && !provider.usageUrl.empty();
    if (usageConfigured) {
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
        // （启用开关打开且 usageUrl 非空才显示，带手动刷新图标）。两者皆无时
        // 整列塌缩为零宽。
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
            !usageConfigured
                ? huxerui::View{huxerui::Row{}}
                : huxerui::View{huxerui::Row {
                      huxerui::Text(usageText)
                          .Style(huxerui::TextStyle{
                              huxerui::Font::System(font_size::kCaption),
                              usageError ? theme.colors.error
                                         : theme.colors.on_surface_variant}),
                      huxerui::IconButton(app::images::refresh, "刷新")
                          .OnClick([tasks, usageCache, provider, http] {
                              // HuxerUI HTTP 异步请求完成后回 UI 线程写缓存 State。
                              tasks.Launch([usageCache, provider,
                                            http]() -> huxerui::Task<void> {
                                  WriteUsageCache(usageCache, provider.id,
                                                  co_await FetchUsageText(http, provider));
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
            // 联通检测：HuxerUI HttpClient 等待响应头，最长 10s，完成后回 UI
            // 线程写卡片 State。URL 空（codex 可留空）时禁用。
            huxerui::IconButton(app::images::activity, "联通检测")
                .OnClick([tasks, checking, latency, http,
                          url = accessUrl] {
                    checking = true;
                    latency = std::string{};
                    tasks.Launch([checking, latency,
                                  http, url]() -> huxerui::Task<void> {
                        try {
                            const double ms = co_await FetchLatency(http, url);
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

} // namespace llmswitch::ui
