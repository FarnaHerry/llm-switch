// providers_zcode.cpp — ZCode 供应商页：左右两列对照.
//
// 与 dsh 页同一套模型（live 是「多供应商并存」的累加 map，本应用另留一份），
// 两个工具的差别在这里体现：
//   左列 = ZCode 实际配置（~/.zcode/v2/config.json 的 provider map 里每一条：
//          本应用的 llmswitch:<id>、ZCode 自己页面上建的条目、官方 builtin:*）
//   右列 = 本应用留存（config.json 组内供应商）
// ZCode 的「当前用哪条」是条目自己的 enabled 开关（它允许多条同时启用，本应用
// 只保证自己托管的那几条互斥），密钥就在条目里（options.apiKey），不像 dsh
// 那样另有凭据文档。
//
// 「保留 ZCode 自己的配置」是这一页的硬约束：ZCode 原生条目（键由它自己生成）
// 收编时**不改名也不改写**，只是开始被本应用记录；写入/更新原位合并，它自己
// 维护的字段（options 里的其它键、systemDisabledReason 等）原样保留；builtin:*
// 是官方套餐条目，既不收编也不删除。写侧每个入口只动自己那一条，整组重建只
// 发生在「全部写入 ZCode」这一个显式按钮上——否则用户在 ZCode 侧整理过的条目，
// 下一次本地增删就会被补回来。
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

// 小徽章：品牌/语义底 + 小字。与官方卡、供应商卡的「使用中」一致。
huxerui::View ZcodeBadge(const std::string& text, huxerui::Color background,
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
huxerui::View ZcodeStatusText(const std::string& text, huxerui::Color color) {
    if (text.empty()) return huxerui::View{huxerui::Row{}};
    const auto firstLine = text.substr(0, text.find('\n'));
    return huxerui::Text(firstLine)
        .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
                                  color})
        .With(huxerui::Frame{.max_width = 190.0F}, huxerui::Tooltip(text));
}

// 条目标题行：显示名（缺失回退到条目键）+ 徽章组。
huxerui::View ZcodeRowTitle(const std::string& name,
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
huxerui::View ZcodeMonoLine(const std::string& text, huxerui::Color color) {
    if (text.empty()) return huxerui::Row{};
    return huxerui::Text(text).Style(
        huxerui::TextStyle{huxerui::Font::Monospace(font_size::kChip), color});
}

// 右列一条的删除动作：eraseLive=false 走 store 的「只删本应用留存」路径
// （config.json 一字不动），true 才把条目也从 ZCode 里摘掉。
void RemoveZcodeLocal(huxerui::ToastHandle toast, const std::string& name,
                      const std::string& id, huxerui::State<int> revision,
                      bool eraseLive) {
    try {
        providerStore().removeProvider("zcode", id, eraseLive);
        toast.Show(eraseLive
                       ? std::format("已删除 {}（含 ZCode 里的条目）", name)
                       : std::format("已删除 {}（ZCode 里的条目保留）", name));
    } catch (const std::exception& e) {
        toast.Show(e.what());
    }
    revision = revision.Get() + 1;
}

// 右列删除确认：条目同时在 config.json 里时问清删哪边——只收回本应用留存
// （条目原地留下，左列随即显示成未纳管），还是连 ZCode 里那一条一起摘掉。
// 内置确认框只有正/负两个动作，所以条目在 live 里时用自定义内容给三个选择。
[[huxerui::composable]] huxerui::View ZcodeDeleteConfirmContent(
    std::string name, std::string id, std::string key,
    huxerui::DialogContext context, huxerui::ToastHandle toast,
    huxerui::State<int> revision) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const huxerui::TextStyle hint{
        huxerui::Font::System(font_size::kCaption),
        theme.colors.on_surface_variant};
    return DialogCard(huxerui::Column {
        huxerui::Text("删除供应商", huxerui::TextRole::Title),
        huxerui::Text(std::format(
            "「{}」在本应用和 ZCode 的 config.json 里都有条目，这次删除要不要连 "
            "ZCode 里那一条一起摘掉？",
            name)),
        huxerui::Text(std::format("只删本应用：config.json 里的 {} 原样留下，左列"
                                  "会把它显示成未纳管的条目，需要时还能再收编"
                                  "回来。",
                                  key))
            .Style(hint),
        huxerui::Text("连同 ZCode 一起删：config.json 里那一条也被摘掉；它正启用"
                      "着的话，ZCode 里就少一个可选供应商。")
            .Style(hint),
        huxerui::Row {
            huxerui::Spacer(),
            huxerui::Button("取消").OnClick([context] { context.Dismiss(); }),
            huxerui::Button("只删本应用")
                .OnClick([context, toast, name, id, revision] {
                    context.Dismiss();
                    RemoveZcodeLocal(toast, name, id, revision,
                                     /*eraseLive=*/false);
                }),
            huxerui::Button("连同 ZCode 一起删")
                .OnClick([context, toast, name, id, revision] {
                    context.Dismiss();
                    RemoveZcodeLocal(toast, name, id, revision,
                                     /*eraseLive=*/true);
                }),
        }.With(huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
    }.With(huxerui::Spacing(12.0F),
           huxerui::Frame{.width = 460.0F},
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

// 打开右列删除确认（普通函数：dialog.Show 的工厂转发到 composable 内容）。
// 条目还没写进 live 时两边其实只有一份，照旧走内置单动作确认框。
void ShowZcodeDeleteConfirm(huxerui::DialogHandle dialog,
                            huxerui::ToastHandle toast,
                            const std::string& name, const std::string& id,
                            const std::string& liveKey,
                            huxerui::State<int> revision) {
    if (liveKey.empty()) {
        dialog.Show(
            "删除供应商",
            std::format("确定删除「{}」？ZCode 的 config.json 里没有它的条目。",
                        name),
            "删除", "取消",
            [toast, name, id, revision] {
                RemoveZcodeLocal(toast, name, id, revision, /*eraseLive=*/true);
            },
            {});
        return;
    }
    dialog.Show(
        [=](huxerui::DialogContext ctx) -> huxerui::View {
            return ZcodeDeleteConfirmContent(name, id, liveKey, ctx, toast,
                                             revision);
        },
        huxerui::DialogOptions{});
}

} // namespace

// 左列一行：ZCode 实际配置里的一条条目。
[[huxerui::composable]] huxerui::View ZcodeLiveRow(
    store::ZcodeLiveProvider live, huxerui::State<int> revision,
    huxerui::ToastHandle toast) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    auto dialog = huxerui::UseDialog();
    const auto tasks = huxerui::UseTaskScope();

    const std::string name =
        live.displayName.empty() ? live.key : live.displayName;
    std::vector<huxerui::View> badges;
    if (live.enabled) {
        badges.push_back(ZcodeBadge("启用中", theme.colors.primary,
                                    theme.colors.on_primary,
                                    islands.nested_radius));
    }
    if (live.builtin) {
        badges.push_back(ZcodeBadge("ZCode 内置", islands.raised,
                                    theme.colors.on_surface_variant,
                                    islands.nested_radius));
    } else if (!live.providerId.empty()) {
        badges.push_back(ZcodeBadge("已纳管", islands.success,
                                    islands.on_success,
                                    islands.nested_radius));
    } else {
        badges.push_back(ZcodeBadge("未纳管", islands.raised,
                                    theme.colors.on_surface_variant,
                                    islands.nested_radius));
    }

    auto bump = [revision] { revision = revision.Get() + 1; };
    // builtin:* 是 ZCode 官方套餐条目，不归本应用管；没有密钥的条目（OAuth）
    // 收编进列表也没法用，与 importLive 的口径一致，不提供收编。
    const bool adoptable =
        live.providerId.empty() && !live.builtin && !live.apiKey.empty();

    // 「当前用哪条」是条目自己的 enabled 开关，所以这个动作放在左列：ZCode
    // 原生条目也能被启用，不必先收编成本应用供应商；反过来，只翻 enabled 就是
    // 一次完整切换，条目内容一字不动。builtin:* 的启停由 ZCode 自己管。
    const bool canEnable = !live.builtin && !live.enabled;
    auto enable = [toast, live, name, bump] {
        try {
            providerStore().enableZcodeKey(live.key);
            toast.Show(std::format("已在 ZCode 中启用 {}", name));
        } catch (const std::exception& e) {
            toast.Show(e.what());
        }
        bump();
    };

    // 收编：记进本地列表。live 一字不动——ZCode 原生条目保持它自己的键与内容。
    auto adopt = [toast, live, bump] {
        try {
            const auto imported = providerStore().adoptZcodeProvider(live.key);
            toast.Show(std::format("已收编 {}", imported.name));
        } catch (const std::exception& e) {
            toast.Show(e.what());
        }
        bump();
    };

    // 删除：只删 live 这一条；弹窗会卸载点击路径上的节点，推迟到任务队列。
    auto showDeleteConfirm = [dialog, toast, live, name, bump] {
        dialog.Show(
            "从 ZCode 删除条目",
            live.providerId.empty()
                ? std::format("确定从 config.json 删除「{}」？只删这一条，别的"
                              "条目与本应用列表都不受影响。",
                              name)
                : std::format("确定从 config.json 删除「{}」？只删这一条，本应用"
                              "列表里对应的供应商会变成「未写入」。",
                              name),
            "删除", "取消",
            [toast, live, name, bump] {
                try {
                    providerStore().removeZcodeProvider(live.key);
                    toast.Show(std::format("已从 ZCode 删除 {}", name));
                } catch (const std::exception& e) {
                    toast.Show(e.what());
                }
                bump();
            },
            {});
    };

    const std::string modelText =
        live.models.size() > 1
            ? std::format("{} 等 {} 个模型", live.model, live.models.size())
            : live.model;
    return QuietCard(huxerui::Column {
        ZcodeRowTitle(name, badges, theme.colors.on_surface),
        ZcodeMonoLine(modelText.empty()
                          ? live.key
                          : std::format("{} · {}", live.key, modelText),
                      theme.colors.on_surface_variant),
        ZcodeMonoLine(live.baseUrl, theme.colors.on_surface_variant),
        huxerui::Row {
            huxerui::IconButton(app::images::swap, "在 ZCode 中启用")
                .OnClick([enable] { enable(); })
                .With(huxerui::Enabled(canEnable),
                      huxerui::Tooltip(
                          live.enabled
                              ? "已在 ZCode 中启用"
                              : (live.builtin
                                     ? "ZCode 官方套餐条目的启停由 ZCode 自己管"
                                     : "翻 config.json 里这一条的 enabled"
                                       "（其余字段一字不动）"))),
            huxerui::IconButton(app::images::import, "收编到本应用")
                .OnClick([adopt] { adopt(); })
                .With(huxerui::Enabled(adoptable),
                      huxerui::Tooltip(
                          adoptable
                              ? "收编成本应用供应商（ZCode 里的条目原样保留）"
                              : "该条目已在应用中、是 ZCode 内置，或没有密钥")),
            huxerui::IconButton(app::images::trash, "从 ZCode 删除")
                .OnClick([tasks, showDeleteConfirm] {
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        showDeleteConfirm();
                        co_return;
                    });
                })
                .With(huxerui::Enabled(!live.builtin),
                      huxerui::Tooltip(live.builtin
                                           ? "ZCode 官方套餐条目不能删除"
                                           : "只从 config.json 删除这一条")),
        }.With(huxerui::Spacing(4.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
    }.With(huxerui::Spacing(6.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start)))
        .Key("zcode-live:" + live.key);
}

// 右列一行：本应用留存的一个 ZCode 供应商。
[[huxerui::composable]] huxerui::View ZcodeLocalRow(
    models::Provider provider, bool present, bool same, bool enabled,
    std::string liveKey, huxerui::TaskScope tasks, huxerui::ToastHandle toast,
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
    if (present && enabled) {
        badges.push_back(ZcodeBadge("启用中", theme.colors.primary,
                                    theme.colors.on_primary,
                                    islands.nested_radius));
    }
    if (!present) {
        badges.push_back(ZcodeBadge("未写入 ZCode", islands.raised,
                                    theme.colors.on_surface_variant,
                                    islands.nested_radius));
    } else if (same) {
        badges.push_back(ZcodeBadge("已同步", islands.success,
                                    islands.on_success,
                                    islands.nested_radius));
    } else {
        badges.push_back(ZcodeBadge("与 ZCode 有差异", islands.raised,
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
    huxerui::View usageView = ZcodeStatusText(
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
        formToolIndex = ToolRegistryIndex("zcode");
        formTarget = id;
    };
    auto showUsage = [formToolIndex, formTarget, id] {
        formToolIndex = ToolRegistryIndex("zcode");
        formTarget = "usage:" + id;
    };
    auto showDeleteConfirm = [dialog, toast, id, name, liveKey, revision] {
        ShowZcodeDeleteConfirm(dialog, toast, name, id, liveKey, revision);
    };

    const std::string modelText =
        provider.models.size() > 1
            ? std::format("{} 等 {} 个模型", provider.model,
                          provider.models.size())
            : provider.model;
    return QuietCard(huxerui::Column {
        ZcodeRowTitle(name, badges, theme.colors.on_surface),
        ZcodeMonoLine(modelText.empty() ? accessUrl
                                        : std::format("{} · {}", accessUrl,
                                                      modelText),
                      theme.colors.on_surface_variant),
        huxerui::Row {
            ZcodeStatusText(latencyText,
                            latency.Get().starts_with("不可达")
                                ? theme.colors.error
                                : theme.colors.on_surface_variant),
            std::move(usageView),
            huxerui::IconButton(app::images::write, "写入 / 更新 ZCode")
                .OnClick([toast, name, id, bump] {
                    try {
                        providerStore().writeZcodeProvider(id);
                        toast.Show(std::format("已把 {} 写入 ZCode", name));
                    } catch (const std::exception& e) {
                        toast.Show(e.what());
                    }
                    bump();
                })
                .With(huxerui::Enabled(!present || !same),
                      huxerui::Tooltip(present && same
                                           ? "ZCode 里已是最新"
                                           : "只把这一条写进 config.json")),
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
                            providerStore().duplicateProvider("zcode", id);
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
                .With(huxerui::Tooltip("删除（先问只删本应用还是连同 ZCode 一起删）")),
        }.With(huxerui::Spacing(4.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
    }.With(huxerui::Spacing(6.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start)))
        .Key("zcode-local:" + id);
}

// ZCode 供应商页：顶部批量动作 + 左右两列（live 实况 / 本地留存）。
[[huxerui::composable]] huxerui::View ZcodeProvidersPage(
    huxerui::State<int> revision, UsageCache usageCache,
    huxerui::State<std::size_t> formToolIndex,
    huxerui::State<std::string> formTarget) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    // 订阅全局变更计数：启用 / 收编 / 写入 / 删除后本页重读 live 与本地列表。
    (void)revision.Get();

    const auto live = providerStore().zcodeLiveProviders();
    const auto& group = providerStore().group("zcode");

    // 本地 provider id → live 状态（条目键 / 是否存在 / 是否一致 / 是否启用）。
    struct LocalMatch {
        std::string key;
        bool present = false;
        bool same = false;
        bool enabled = false;
    };
    std::map<std::string, LocalMatch> matches;
    for (const auto& entry : live) {
        if (entry.providerId.empty()) continue;
        LocalMatch match;
        match.key = entry.key;
        match.present = true;
        match.enabled = entry.enabled;
        for (const auto& p : group.providers) {
            if (p.id != entry.providerId) continue;
            // 写侧的形状：清单为空时只写主模型一条。
            std::vector<std::string> expected = p.models;
            if (expected.empty() && !p.model.empty()) expected = {p.model};
            match.same = entry.baseUrl == models::effectiveBaseUrl(p) &&
                         entry.apiFormat ==
                             models::normalizeApiFormat(p.apiFormat) &&
                         entry.apiKey == p.apiKey && entry.models == expected;
        }
        matches[entry.providerId] = match;
    }

    // 顶部批量动作。
    huxerui::View toolbar = huxerui::Row {
        huxerui::Text("config.json 的 provider map 是多条并存的增量列表：本应用"
                      "优先沿用已有条目键，ZCode 自己的条目一律原样保留。")
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
                                      theme.colors.on_surface_variant})
            .With(huxerui::Grow(1.0F)),
        huxerui::Button("全部写入 ZCode").OnClick([toast, revision] {
            try {
                providerStore().syncZcodeProviders();
                toast.Show("已把本应用全部供应商写入 ZCode（并清理孤儿条目）");
            } catch (const std::exception& e) {
                toast.Show(e.what());
            }
            revision = revision.Get() + 1;
        }),
        huxerui::Button("全部收编").OnClick([toast, revision] {
            std::size_t adopted = 0;
            try {
                for (const auto& entry :
                     providerStore().zcodeLiveProviders()) {
                    if (entry.builtin || !entry.providerId.empty() ||
                        entry.apiKey.empty()) {
                        continue;
                    }
                    providerStore().adoptZcodeProvider(entry.key);
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

    // 左列：ZCode 实际配置。
    const std::string liveCount =
        std::format("ZCode 实际配置 · {} 条", live.size());
    huxerui::View liveList =
        huxerui::VirtualList(live,
                             [revision, toast](
                                 const store::ZcodeLiveProvider& entry) {
                                 return ZcodeLiveRow(entry, revision, toast);
                             })
            .EstimatedItemExtent(120.0F)
            .CacheExtent(480.0F)
            .With(huxerui::Spacing(8.0F), huxerui::Grow(1.0F));
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
                   formTarget](const models::Provider& provider) {
                      const auto it = matches.find(provider.id);
                      const bool present =
                          it != matches.end() && it->second.present;
                      const bool same = it != matches.end() && it->second.same;
                      const bool enabled =
                          it != matches.end() && it->second.enabled;
                      const std::string key =
                          it != matches.end() ? it->second.key : std::string{};
                      return ZcodeLocalRow(provider, present, same, enabled, key,
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
