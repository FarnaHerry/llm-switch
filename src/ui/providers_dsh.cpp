// providers_dsh.cpp — dsh（DeepSeek Harness）供应商页：左右两列对照.
//
// dsh 的 live 配置（~/.dsh/settings.yaml 的 llm-pi-ai.providers）是「多条手写
// 路由并存」的 map，而本应用的 config.json 组是另一份留存——两者会被对方改
// 动（dsh 自己、其它工具、手改），本来就可能不同步。所以这一页不再是单列卡片
// 列表，而是：
//   左列 = DSH 实际配置（settings.yaml 里现存的每条手写路由，含未纳管项）
//   右列 = 本应用留存（config.json 组内供应商）
// 左右各自的行显示同步状态（使用中 / 已纳管 / 未纳管 / 已同步 / 有差异 /
// 未写入），并可逐条动作：
//   左列：收编（live → 本地，条目被接管成 llmswitch-<id>，默认路由跟着改指）、
//         从 dsh 删除（只删这一条，别的条目原样保留）；
//   右列：设为 dsh 默认（只改 agent-default-model）、写入/更新 dsh（增量
//         upsert 本应用全部条目）、编辑 / 用量 / 联通 / 复制 / 删除。
// 顶部另有「全部写入 dsh」「全部收编」两个批量动作。
// 写入语义在 store（syncDshProviders / adoptDshProvider / removeDshProvider）：
// 本应用条目恒为 llmswitch-<id>，别家条目一律不动；每次同步重建本应用条目
// 集合，孤儿 llmswitch-* 随之清理。
#include <huxerui/huxerui.h>

#include <format>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "ui.h"
#include "providers_internal.h"
#include "app_resources.h"

import llmswitch.models;
import llmswitch.store;

namespace llmswitch::ui {

namespace {

using provider_detail::FetchLatency;
using provider_detail::FetchUsageText;
using provider_detail::WriteUsageCache;

// dsh 内置官方路由的键：它是 dsh 出厂自带的，不属于「可收编的第三方条目」，
// 「全部收编」不能把它变成供应商卡（官方状态由列表首位的官方卡表达）。
constexpr std::string_view kDshOfficialKey = "deepseek-official";

// 小徽章：品牌/语义底 + 小字。视觉与官方卡、供应商卡的「使用中」一致。
huxerui::View DshBadge(const std::string& text, huxerui::Color background,
                       huxerui::Color foreground, float radius) {
    return huxerui::Text(text)
        .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
                                  foreground})
        .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(8.0F, 2.0F)),
              huxerui::Background(background),
              huxerui::CornerRadius(radius));
}

// 行内第三段的窄状态文本（延迟 / 用量）：两列布局本来就只有半个页面宽，
// 这里再给一个硬宽度上限，否则一条失败文本会把操作图标顶出行外。
huxerui::View DshStatusText(const std::string& text, huxerui::Color color) {
    if (text.empty()) return huxerui::View{huxerui::Row{}};
    const auto firstLine = text.substr(0, text.find('\n'));
    return huxerui::Text(firstLine)
        .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
                                  color})
        .With(huxerui::Frame{.max_width = 190.0F}, huxerui::Tooltip(text));
}

// 条目标题行：显示名（缺失回退到条目键）+ 徽章组。
huxerui::View DshRowTitle(const std::string& name,
                          const std::vector<huxerui::View>& badges,
                          huxerui::Color nameColor) {
    std::vector<huxerui::View> items;
    items.push_back(huxerui::Text(name).Style(huxerui::TextStyle{
        huxerui::Font::System(font_size::kBody)
            .WithWeight(huxerui::FontWeight::SemiBold),
        nameColor}));
    for (const auto& badge : badges) items.push_back(badge);
    return huxerui::Row(std::move(items))
        .With(huxerui::Spacing(6.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

// 等宽次要行（条目键 / 模型 / 访问地址）。
huxerui::View DshMonoLine(const std::string& text, huxerui::Color color) {
    if (text.empty()) return huxerui::Row{};
    return huxerui::Text(text).Style(
        huxerui::TextStyle{huxerui::Font::Monospace(font_size::kChip), color});
}

} // namespace

// 左列一行：DSH 实际配置里的一条手写路由。
[[huxerui::composable]] huxerui::View DshLiveRow(
    store::DshLiveProvider live, huxerui::State<int> revision,
    huxerui::ToastHandle toast) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    auto dialog = huxerui::UseDialog();

    const std::string name =
        live.displayName.empty() ? live.key : live.displayName;
    std::vector<huxerui::View> badges;
    if (live.isDefault) {
        badges.push_back(DshBadge("使用中", theme.colors.primary,
                                  theme.colors.on_primary,
                                  islands.nested_radius));
    }
    if (!live.providerId.empty()) {
        badges.push_back(DshBadge("已纳管", islands.success, islands.on_success,
                                  islands.nested_radius));
    } else if (live.key != kDshOfficialKey) {
        badges.push_back(DshBadge("未纳管", islands.raised,
                                  theme.colors.on_surface_variant,
                                  islands.nested_radius));
    } else {
        badges.push_back(DshBadge("dsh 内置", islands.raised,
                                  theme.colors.on_surface_variant,
                                  islands.nested_radius));
    }

    auto bump = [revision] { revision = revision.Get() + 1; };
    const bool adoptable = live.providerId.empty() &&
                           live.key != kDshOfficialKey;

    // 收编：live 条目 → 本地供应商卡（store 负责把条目接管成 llmswitch-<id>）。
    auto adopt = [toast, live, bump] {
        try {
            const auto imported = providerStore().adoptDshProvider(live.key);
            toast.Show(std::format("已收编 {}", imported.name));
        } catch (const std::exception& e) {
            toast.Show(e.what());
        }
        bump();
    };

    // 删除：内置确认框；弹窗会卸载点击路径上的节点，推迟到任务队列。
    const auto tasks = huxerui::UseTaskScope();
    auto showDeleteConfirm = [dialog, toast, live, name, bump] {
        dialog.Show(
            "从 dsh 删除条目",
            std::format("确定从 settings.yaml 删除「{}」？只删这一条，本应用"
                        "列表里对应的供应商卡会变成「未写入」。",
                        name),
            "删除", "取消",
            [toast, live, name, bump] {
                try {
                    providerStore().removeDshProvider(live.key);
                    toast.Show(std::format("已从 dsh 删除 {}", name));
                } catch (const std::exception& e) {
                    toast.Show(e.what());
                }
                bump();
            },
            {});
    };

    return QuietCard(huxerui::Column {
        DshRowTitle(name, badges, theme.colors.on_surface),
        DshMonoLine(live.model.empty()
                        ? live.key
                        : std::format("{} · {}", live.key, live.model),
                    theme.colors.on_surface_variant),
        DshMonoLine(live.baseUrl, theme.colors.on_surface_variant),
        huxerui::Row {
            huxerui::IconButton(app::images::import, "收编到本应用")
                .OnClick([adopt] { adopt(); })
                .With(huxerui::Enabled(adoptable),
                      huxerui::Tooltip(adoptable
                                           ? "收编成本应用供应商（写成本应用"
                                             "管理的条目，dsh 行为不变）"
                                           : "该条目已在应用中或为 dsh 内置")),
            huxerui::IconButton(app::images::trash, "从 dsh 删除")
                .OnClick([tasks, showDeleteConfirm] {
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        showDeleteConfirm();
                        co_return;
                    });
                })
                .With(huxerui::Tooltip("只从 dsh 配置删除这一条")),
        }.With(huxerui::Spacing(4.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
    }.With(huxerui::Spacing(6.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start)))
        .Key("dsh-live:" + live.key);
}

// 右列一行：本应用留存的一个 dsh 供应商。
[[huxerui::composable]] huxerui::View DshLocalRow(
    models::Provider provider, bool present, bool same, bool isDefault,
    huxerui::TaskScope tasks, huxerui::ToastHandle toast,
    huxerui::State<int> revision, UsageCache usageCache,
    huxerui::State<std::size_t> formToolIndex,
    huxerui::State<std::string> formTarget) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    auto dialog = huxerui::UseDialog();
    const auto http = huxerui::UseService<huxerui::HttpClient>();

    const std::string id = provider.id;
    const std::string name = provider.name;
    const std::string accessUrl = models::effectiveBaseUrl(provider);
    auto checking = huxerui::UseState(false);
    auto latency = huxerui::UseState<std::string>({});
    auto usageRefreshing = huxerui::UseState(false);

    std::vector<huxerui::View> badges;
    if (isDefault) {
        badges.push_back(DshBadge("使用中", theme.colors.primary,
                                  theme.colors.on_primary,
                                  islands.nested_radius));
    }
    if (!present) {
        badges.push_back(DshBadge("未写入 dsh", islands.raised,
                                  theme.colors.on_surface_variant,
                                  islands.nested_radius));
    } else if (same) {
        badges.push_back(DshBadge("已同步", islands.success, islands.on_success,
                                  islands.nested_radius));
    } else {
        badges.push_back(DshBadge("与 dsh 有差异", islands.raised,
                                  islands.warning, islands.nested_radius));
    }

    // 用量展示（与供应商卡同源缓存）：启用且配置了 URL 才显示。
    std::string usageText;
    bool usageError = false;
    const bool usageConfigured =
        provider.usageEnabled && !provider.usageUrl.empty();
    if (usageConfigured) {
        const auto& cache = usageCache.Get();
        if (const auto it = cache.find(id); it != cache.end()) {
            usageText = it->second;
            usageError = it->second.starts_with("查询失败");
        } else {
            usageText = "用量待查询";
        }
    }
    if (usageConfigured && !usageText.empty() &&
        usageText.find('\n') != std::string::npos) {
        usageText = usageText.substr(0, usageText.find('\n'));
    }
    const std::string latencyText =
        checking.Get() ? "连通检测中…" : latency.Get();
    // 用量文本本身就是刷新入口（点一下重新查询）：两列布局宽度有限，额外再挂
    // 一枚刷新图标会把操作组顶出行外。
    huxerui::View usageView = DshStatusText(
        usageText, usageError ? theme.colors.error
                              : theme.colors.on_surface_variant);
    if (usageConfigured) {
        usageView = std::move(usageView)
                        .OnClick([tasks, usageCache, provider, http,
                                  usageRefreshing] {
                            // 已在查询中就不再叠加请求。
                            if (usageRefreshing.Get()) return;
                            usageRefreshing = true;
                            tasks.Launch([usageCache, provider, http,
                                          usageRefreshing]() -> huxerui::Task<void> {
                                WriteUsageCache(
                                    usageCache, provider.id,
                                    co_await FetchUsageText(http, provider));
                                usageRefreshing = false;
                            });
                        })
                        .With(huxerui::Tooltip("点击重新查询用量"),
                              huxerui::PointerCursor(
                                  huxerui::PointerCursorKind::Hand));
    }

    auto bump = [revision] { revision = revision.Get() + 1; };
    auto showEdit = [formToolIndex, formTarget, id] {
        formToolIndex = ToolRegistryIndex("dsh");
        formTarget = id;
    };
    auto showUsage = [formToolIndex, formTarget, id] {
        formToolIndex = ToolRegistryIndex("dsh");
        formTarget = "usage:" + id;
    };
    auto showDeleteConfirm = [dialog, toast, id, name, bump] {
        dialog.Show("删除供应商",
                    std::format("确定删除「{}」？它在 dsh 里的条目会一并清掉。",
                                name),
                    "删除", "取消",
                    [toast, id, name, bump] {
                        try {
                            providerStore().removeProvider("dsh", id);
                            toast.Show(std::format("已删除 {}", name));
                        } catch (const std::exception& e) {
                            toast.Show(e.what());
                        }
                        bump();
                    },
                    {});
    };

    return QuietCard(huxerui::Column {
        DshRowTitle(name, badges, theme.colors.on_surface),
        DshMonoLine(provider.model.empty()
                        ? accessUrl
                        : std::format("{} · {}", accessUrl, provider.model),
                    theme.colors.on_surface_variant),
        huxerui::Row {
            DshStatusText(latencyText,
                          latency.Get().starts_with("不可达")
                              ? theme.colors.error
                              : theme.colors.on_surface_variant),
            std::move(usageView),
            huxerui::IconButton(app::images::swap, "设为 dsh 默认")
                .OnClick([toast, id, name, bump] {
                    try {
                        providerStore().switchTo("dsh", id);
                        toast.Show(std::format("已把 {} 设为 dsh 默认路由", name));
                    } catch (const std::exception& e) {
                        toast.Show(e.what());
                    }
                    bump();
                })
                .With(huxerui::Enabled(!isDefault),
                      huxerui::Tooltip(isDefault
                                           ? "已是 dsh 默认路由"
                                           : "改 agent-default-model 指向它")),
            huxerui::IconButton(app::images::upload, "写入 / 更新 dsh")
                .OnClick([toast, bump] {
                    try {
                        providerStore().syncDshProviders({});
                        toast.Show("已把本应用全部供应商增量写入 dsh");
                    } catch (const std::exception& e) {
                        toast.Show(e.what());
                    }
                    bump();
                })
                .With(huxerui::Enabled(!present || !same),
                      huxerui::Tooltip(present && same
                                           ? "dsh 里已是最新"
                                           : "增量写入 settings.yaml")),
            huxerui::IconButton(app::images::activity, "联通检测")
                .OnClick([tasks, checking, latency, http, url = accessUrl] {
                    checking = true;
                    latency = std::string{};
                    tasks.Launch([checking, latency, http,
                                  url]() -> huxerui::Task<void> {
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
                .With(huxerui::Enabled(!checking.Get() && !accessUrl.empty()),
                      huxerui::Tooltip("检测实际访问 URL 的连通性与延迟")),
            huxerui::IconButton(app::images::edit, "编辑")
                .OnClick([showEdit] { showEdit(); })
                .With(huxerui::Tooltip("编辑")),
            huxerui::IconButton(app::images::gauge, "用量查询配置")
                .OnClick([showUsage] { showUsage(); })
                .With(huxerui::Tooltip("用量查询配置")),
            huxerui::IconButton(app::images::copy, "复制")
                .OnClick([toast, id, bump] {
                    try {
                        const auto copy =
                            providerStore().duplicateProvider("dsh", id);
                        toast.Show(std::format("已复制为 {}", copy.name));
                    } catch (const std::exception& e) {
                        toast.Show(e.what());
                    }
                    bump();
                })
                .With(huxerui::Tooltip("复制")),
            huxerui::IconButton(app::images::trash, "删除")
                .OnClick([tasks, showDeleteConfirm] {
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        showDeleteConfirm();
                        co_return;
                    });
                })
                .With(huxerui::Tooltip("删除（含 dsh 里的条目）")),
        }.With(huxerui::Spacing(4.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
    }.With(huxerui::Spacing(6.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start)))
        .Key("dsh-local:" + id);
}

// dsh 供应商页：顶部批量动作 + 左右两列（live 实况 / 本地留存）。
[[huxerui::composable]] huxerui::View DshProvidersPage(
    huxerui::State<int> revision, UsageCache usageCache,
    huxerui::State<std::size_t> formToolIndex,
    huxerui::State<std::string> formTarget) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    // 订阅全局变更计数：切换 / 收编 / 写入 / 删除后本页重读 live 与本地列表。
    (void)revision.Get();

    const auto live = providerStore().dshLiveProviders();
    const auto& group = providerStore().group("dsh");

    // 本地 provider id → live 状态（是否存在 / 是否一致）。
    struct LocalMatch {
        bool present = false;
        bool same = false;
    };
    std::map<std::string, LocalMatch> matches;
    for (const auto& entry : live) {
        if (entry.providerId.empty()) continue;
        LocalMatch match;
        match.present = true;
        for (const auto& p : group.providers) {
            if (p.id != entry.providerId) continue;
            match.same = entry.baseUrl == models::effectiveBaseUrl(p) &&
                         entry.apiFormat ==
                             models::normalizeApiFormat(p.apiFormat) &&
                         entry.model == p.model && entry.apiKey == p.apiKey;
        }
        matches[entry.providerId] = match;
    }
    std::string defaultProviderId;
    for (const auto& entry : live) {
        if (entry.isDefault && !entry.providerId.empty()) {
            defaultProviderId = entry.providerId;
        }
    }

    // 顶部批量动作。
    huxerui::View toolbar = huxerui::Row {
        huxerui::Text("settings.yaml 的 llm-pi-ai.providers 是多路由并存的"
                      "增量列表：本应用条目恒为 llmswitch-<id>，别家条目不动。")
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
                                      theme.colors.on_surface_variant})
            .With(huxerui::Grow(1.0F)),
        huxerui::Button("全部写入 dsh").OnClick([toast, revision] {
            try {
                providerStore().syncDshProviders({});
                toast.Show("已把本应用全部供应商增量写入 dsh");
            } catch (const std::exception& e) {
                toast.Show(e.what());
            }
            revision = revision.Get() + 1;
        }),
        huxerui::Button("全部收编").OnClick([toast, revision] {
            std::size_t adopted = 0;
            try {
                for (const auto& entry :
                     providerStore().dshLiveProviders()) {
                    if (!entry.providerId.empty() ||
                        entry.key == kDshOfficialKey) {
                        continue;
                    }
                    providerStore().adoptDshProvider(entry.key);
                    ++adopted;
                }
                toast.Show(adopted == 0
                               ? "没有需要收编的条目"
                               : std::format("已收编 {} 条", adopted));
            } catch (const std::exception& e) {
                toast.Show(e.what());
            }
            revision = revision.Get() + 1;
        }),
    }.With(huxerui::Spacing(theme.spacing.small),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));

    // 左列：DSH 实际配置。
    const std::string liveCount =
        std::format("DSH 实际配置 · {} 条", live.size());
    huxerui::View liveList = live.empty()
                                 ? huxerui::View{
                                       huxerui::Text("settings.yaml 里还没有手写"
                                                     "路由。")
                                           .Style(huxerui::TextStyle{
                                               huxerui::Font::System(
                                                   font_size::kCaption),
                                               theme.colors
                                                   .on_surface_variant})}
                                 : huxerui::View{huxerui::VirtualList(
                                       live,
                                       [revision, toast](
                                           const store::DshLiveProvider& entry) {
                                           return DshLiveRow(entry, revision,
                                                             toast);
                                       })
                                       .EstimatedItemExtent(120.0F)
                                       .CacheExtent(480.0F)
                                       .With(huxerui::Spacing(8.0F),
                                             huxerui::Grow(1.0F))};
    huxerui::View liveColumn = huxerui::Column {
        huxerui::Text(liveCount).Style(
            huxerui::TextStyle{huxerui::Font::System(font_size::kChip)
                                   .WithWeight(huxerui::FontWeight::SemiBold),
                               theme.colors.on_surface_variant}),
        std::move(liveList),
    }.With(huxerui::Spacing(8.0F), huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    // 右列：本应用留存。
    const std::string localCount =
        std::format("本应用留存 · {} 条", group.providers.size());
    huxerui::View localList =
        group.providers.empty()
            ? huxerui::View{
                  huxerui::Text("还没有供应商。点击右上角 + 新增。")
                      .Style(huxerui::TextStyle{
                          huxerui::Font::System(font_size::kCaption),
                          theme.colors.on_surface_variant})}
            : huxerui::View{huxerui::VirtualList(
                  group.providers,
                  [matches, tasks, toast, revision, usageCache, formToolIndex,
                   formTarget, defaultProviderId](
                      const models::Provider& provider) {
                      const auto it = matches.find(provider.id);
                      const bool present =
                          it != matches.end() && it->second.present;
                      const bool same = it != matches.end() && it->second.same;
                      return DshLocalRow(provider, present, same,
                                         provider.id == defaultProviderId,
                                         tasks, toast, revision, usageCache,
                                         formToolIndex, formTarget);
                  })
                  .EstimatedItemExtent(120.0F)
                  .CacheExtent(480.0F)
                  .With(huxerui::Spacing(8.0F), huxerui::Grow(1.0F))};
    huxerui::View localColumn = huxerui::Column {
        huxerui::Text(localCount).Style(
            huxerui::TextStyle{huxerui::Font::System(font_size::kChip)
                                   .WithWeight(huxerui::FontWeight::SemiBold),
                               theme.colors.on_surface_variant}),
        std::move(localList),
    }.With(huxerui::Spacing(8.0F), huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    return huxerui::Column {
        std::move(toolbar),
        huxerui::Row {
            std::move(liveColumn),
            std::move(localColumn),
        }.With(huxerui::Spacing(theme.spacing.medium),
               huxerui::Grow(1.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
    }.With(huxerui::Spacing(theme.spacing.small),
           huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace llmswitch::ui
