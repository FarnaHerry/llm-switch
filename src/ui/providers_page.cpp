// providers_page.cpp — 供应商列表页：各 agent 工具组共用同一组件（参数化
// tool，注册表见 models::toolRegistry()）。工具选择由 AgentPage 的
// Agent 工具栏 + Pager 负责；每个工具列表顶部有「直连 / 路由 / 聚合」三段
// 页签，页签只切换视图，不写配置。新增动作在页签行右侧，按当前页签打开普通
// 表单或 aggregate:new。新增/编辑与用量查询配置是 AgentPage 自己的第二个
// 页面（整页覆盖工具栏 + Pager，不受分页器左右拖动影响），本页提供的只是
// 进入入口（formToolIndex + formTarget 两个 State 由 AgentPage 持有）：
// 卡片的编辑图标写 formTarget = id，用量 gauge 图标写 "usage:" + id，
// formToolIndex 定位目标 Agent 组（ToolRegistryIndex），AgentPage 据此换页
// 并把供应商数据回填给表单。
// 每个供应商一张卡片三段式：左信息列（名称 / 实际访问 URL / 备注 /
// 「使用中」徽章（group.current 或 detectCurrent 命中），
// Grow 吃满剩余宽度）｜ 中间状态列（连通检测延迟 + 用量文本/刷新图标，
// 垂直居中落在内容与操作组之间，两者皆无时塌缩为零宽）｜ 右侧操作图标组
// （切换 swap / 联通检测 activity / 编辑 edit / 用量查询配置 gauge /
// 复制 copy / 删除 trash，自绘 SVG + Tooltip，删除走内置确认框）。中间列是
// 固定尺寸段，所以状态文本在 provider_card 里统一限宽（首行 + 预览上限 +
// Frame.max_width）：用量失败文本带 URL 与响应体摘要，任其按 intrinsic 宽度
// 参与布局会挤垮信息列并把操作组顶出卡片；被截掉的完整文本进 Tooltip。
// 联通检测经
// HuxerUI HttpClient（平台原生异步 HTTP），连通后卡片显示
// 「延迟 N ms」，失败显示「不可达：…」（error 色）。有官方厂商的工具
// （claude-code / claude / codex，models::officialVendorName）列表第一位固定
// 一张「官方」常驻卡：切换 = store.restoreOfficial 还原厂商原生状态，
// active = 组 current 与 detectCurrent 均为空。页签行右侧的加号进入当前
// 页签对应的新增页；直连/路由页签的主操作分别是切回真实地址/切换路由目标，
// 聚合页签进入聚合模式。
// 表单按 ToolSpec 适配：完整 URL switch 与上游格式 Select 控制 URL 后缀，codex
// 显示 config.toml 原文、needsModel（opencode/pi）模型必填、hasApiFormat 显示
// API 协议分段选择；hasModelMappings（claude-code /
// claude）额外显示三档模型映射行（Haiku/Sonnet/Opus）。模型字段旁「获取模型」
// 默认按当前实际 URL/apiKey/上游格式经 HuxerUI HttpClient 拉取模型列表，也支持在
// 高级选项中覆盖完整模型列表 URL；平台异步请求完成后结果回 UI 线程写 State；拉取成功后模型行在按钮前出现 Select
// 下拉，点选回填该行的模型字段（不弹窗）。
// 用量查询：启用开关打开且 usageUrl 非空的卡片显示用量文本 + 手动刷新按钮；AgentPage
// 共享缓存，仅可见列表进入（且没有打开表单）或配置改变时按供应商刷新间隔惰性检查。
//
// 数据流：所有 store 读写都在 UI 线程（store 无内部锁，UI 线程独占是契约；
// live 文件读写为微秒级本地 IO，不经任务线程）。写操作后 revision+1，
// 驱动本页重读、托盘菜单重建。detectCurrent 在页面首组合跑一次。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ui.h"
#include "providers_internal.h"
#include "app_resources.h"

import llmswitch.config;
import llmswitch.models;
import llmswitch.net;
import llmswitch.store;

namespace llmswitch::ui {

using provider_detail::FetchUsageText;
using provider_detail::WriteUsageCache;

[[huxerui::composable]] huxerui::View ProvidersPage(
    std::string tool, huxerui::State<int> revision, UsageCache usageCache,
    huxerui::State<std::size_t> formToolIndex,
    huxerui::State<std::string> formTarget,
    huxerui::State<std::size_t> navPage,
    huxerui::State<std::size_t> selectedTool, std::size_t toolIndex,
    huxerui::State<std::size_t> workspacePage, bool inWorkspace) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    const auto http = huxerui::UseService<huxerui::HttpClient>();
    // 探测 live 文件命中（只读本地文件，UI 线程直接跑）。依赖 revision 而不只是
    // 首组合：切换 / 恢复官方 / 删除都会改写（或撤掉）live 配置，只有重算
    // detected 才能让「使用中」跟着走——否则组 current 已经指向新供应商，而
    // detected 仍停在旧命中项，两张卡会同时带上徽章，直到页面重新挂载。
    auto detected = huxerui::UseState<std::string>({});
    huxerui::Lifecycle(
        [tool, detected] {
            detected = providerStore().detectCurrent(tool);
            return [] {};
        },
        revision.Get());

    // 保留页不会卸载：仅在当前列表进入、没有打开表单或配置变更时惰性检查，
    // 不设后台计时器。时间戳跨表单/导航切换保留，取消返回不会重新查询尚未
    // 到期的供应商。表单打开时列表被 AgentPage 的表单页覆盖，也就不再查询。
    using UsageTimes =
        std::map<std::string, std::chrono::steady_clock::time_point>;
    auto checkedAt = huxerui::UseState(std::make_shared<UsageTimes>());
    const bool formOpen = !formTarget.Get().empty();
    const bool agentPageVisible =
        inWorkspace
            ? navPage.Get() == pages::kWorkspace &&
                  workspacePage.Get() == workspace_pages::kAgents
            : navPage.Get() == pages::kAgents;
    const bool usageVisible = agentPageVisible &&
                              selectedTool.Get() == toolIndex && !formOpen;
    huxerui::Lifecycle(
        [tasks, usageCache, http, tool, checkedAt, usageVisible] {
            auto active = std::make_shared<bool>(usageVisible);
            huxerui::TaskHandle request;
            if (usageVisible) {
                request = tasks.Launch([=]() -> huxerui::Task<void> {
                    if (!*active) co_return;
                    // 拷贝当前组，避免跨 await 持有 store 内部引用。
                    const auto providers = providerStore().group(tool).providers;
                    for (const auto& p : providers) {
                        if (!*active) co_return;
                        if (models::isAggregateProvider(p) ||
                            !p.usageEnabled || p.usageUrl.empty() ||
                            p.usageRefreshMinutes <= 0) continue;
                        const auto now = std::chrono::steady_clock::now();
                        const auto previous = checkedAt.Get()->find(p.id);
                        if (previous != checkedAt.Get()->end() &&
                            now - previous->second < std::chrono::minutes{p.usageRefreshMinutes}) continue;
                        auto text = co_await FetchUsageText(http, p);
                        if (!*active) co_return;
                        (*checkedAt.Get())[p.id] = std::chrono::steady_clock::now();
                        WriteUsageCache(usageCache, p.id, std::move(text));
                    }
                });
            }
            // 离开列表即取消任务，阻止旧配置/隐藏页结果回写。
            return [active, request] { *active = false; request.Cancel(); };
        },
        usageVisible, revision.Get());

    // 订阅全局变更计数：托盘切换 / 设置页导入后本页重读。
    (void)revision.Get();

    // 增量多供应商工具（dsh / zcode）不走单列卡片：它们的 live 配置是多条目
    // 并存的累加 map，页面改由左右两列渲染「live 实况 / 本地留存」。分流本身
    // 走注册表标记 ToolSpec::additiveProviders，具体布局按工具分派——两个工具
    // 的条目形状与默认语义不同（dsh 用 agent-default-model + 凭据文档，ZCode 用
    // 条目自己的 enabled + options.apiKey），共用的是「左右两列对照」这套交互。
    if (const auto* spec = models::findTool(tool);
        spec != nullptr && spec->additiveProviders) {
        return tool == "zcode"
                   ? ZcodeProvidersPage(revision, usageCache, formToolIndex,
                                        formTarget)
                   : DshProvidersPage(revision, usageCache, formToolIndex,
                                      formTarget);
    }

    // 连接模式分类（对齐 cc-switch 的「直连 / 路由 / 聚合」三段）：
    // 页签只决定正在看哪一类，不改变生效模式；切换模式必须点卡片上的主
    // 操作或横幅按钮。current 是当前生效目标（直连供应商 / 路由目标 /
    // 聚合项），direct 是回到直连要恢复的普通供应商。
    const auto& g = providerStore().group(tool);
    const std::string mode =
        g.mode == "route" || g.mode == "aggregate" ? g.mode : "direct";
    const std::string currentProvider = g.current;
    const std::string directProvider = g.direct;
    const std::string detectedProvider = detected.Get();
    const bool hasOfficial = !models::officialVendorName(tool).empty();

    const std::size_t initialView =
        mode == "aggregate" ? 2 : (mode == "route" ? 1 : 0);
    auto modeTab = huxerui::UseState<std::size_t>(std::size_t{initialView});
    huxerui::Lifecycle(
        [modeTab, mode] {
            const std::size_t desired =
                mode == "aggregate" ? 2 : (mode == "route" ? 1 : 0);
            if (modeTab.Get() != desired) modeTab = desired;
            return [] {};
        },
        mode);
    const std::size_t view = std::min<std::size_t>(modeTab.Get(), 2);

    auto nameOf = [&g](const std::string& id) {
        if (id.empty()) return std::string{};
        for (const auto& provider : g.providers) {
            if (provider.id != id) continue;
            return provider.name.empty() ? provider.id : provider.name;
        }
        return std::string{};
    };
    const std::string effectiveName =
        currentProvider.empty()
            ? (hasOfficial ? std::string("官方原生") : std::string("未设置"))
            : nameOf(currentProvider);
    const std::string modeName = mode == "route"   ? "路由"
                                 : mode == "aggregate" ? "聚合"
                                                       : "直连";
    const IslandTheme islands = ResolveIslandTheme(theme);

    std::vector<models::Provider> directProviders;
    std::vector<models::Provider> aggregateProviders;
    for (const auto& provider : g.providers) {
        (models::isAggregateProvider(provider) ? aggregateProviders
                                                : directProviders)
            .push_back(provider);
    }

    auto bumpRevision = [revision] { revision = revision.Get() + 1; };
    auto backToDirect = [toast, tool, modeTab, bumpRevision] {
        try {
            StopRoute(tool);
            modeTab = 0;
            bumpRevision();
            toast.Show("已回到直连");
        } catch (const std::exception& e) {
            toast.Show(e.what());
        }
    };

    // 模式页签 + 生效状态 + 当前页签的新增入口。页签只切换视图，不写配置。
    std::vector<huxerui::SegmentedButtonItem> modeItems;
    modeItems.push_back(
        huxerui::SegmentedButtonItem(app::images::link, "直连"));
    modeItems.push_back(
        huxerui::SegmentedButtonItem(app::images::router, "路由"));
    modeItems.push_back(
        huxerui::SegmentedButtonItem(app::images::subrouter, "聚合"));
    huxerui::View modeTabs =
        huxerui::SegmentedButton(std::move(modeItems), view)
            .OnChanged([modeTab](std::size_t index) {
                modeTab = std::min<std::size_t>(index, 2);
            });
    huxerui::View addButton =
        huxerui::IconButton(app::images::add,
                            view == 2 ? "新增聚合供应商" : "新增供应商")
            .OnClick([view, tool, formToolIndex, formTarget] {
                formToolIndex = ToolRegistryIndex(tool);
                formTarget = view == 2 ? "aggregate:new" : "new";
            })
            .With(huxerui::Tooltip(view == 2 ? "新增聚合供应商"
                                             : "新增供应商"));

    huxerui::View modeHeader =
        huxerui::Column {
            huxerui::Row {
                std::move(modeTabs),
                huxerui::Spacer(),
                std::move(addButton),
            }.With(huxerui::Spacing(theme.spacing.small),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
            huxerui::Text(std::format("生效：{} · {}", modeName,
                                      effectiveName))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kChip),
                    theme.colors.on_surface_variant}),
        }.With(huxerui::Spacing(6.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    // 路由横幅：生效时显示目标和故障转移开关；未生效时给出启动目标。
    huxerui::View routeBanner = huxerui::Row{};
    if (view == 1) {
        if (mode == "route") {
            huxerui::View failoverSwitch =
                huxerui::Switch(providerStore().config().routerFailover)
                    .OnChanged([toast, bumpRevision](bool on) {
                        routerInstance().setFailoverEnabled(on);
                        providerStore().setRouterFailover(on);
                        toast.Show(on ? "已开启故障转移" : "已关闭故障转移");
                        bumpRevision();
                    });
            routeBanner =
                huxerui::Row {
                    huxerui::Text(std::format("路由生效中 → {}", effectiveName))
                        .Style(huxerui::TextStyle{
                            huxerui::Font::System(font_size::kBody),
                            theme.colors.on_surface}),
                    huxerui::Spacer(),
                    huxerui::Text("故障转移")
                        .Style(huxerui::TextStyle{
                            huxerui::Font::System(font_size::kCaption),
                            theme.colors.on_surface_variant}),
                    std::move(failoverSwitch),
                    huxerui::Button("回到直连")
                        .OnClick([backToDirect] { backToDirect(); }),
                }.With(huxerui::Spacing(8.0F),
                       huxerui::CrossAlign(
                           huxerui::CrossAxisAlignment::Center));
        } else {
            std::string startTarget = directProvider;
            if (startTarget.empty() && !directProviders.empty()) {
                startTarget = directProviders.front().id;
            }
            huxerui::View startButton =
                startTarget.empty()
                    ? huxerui::View{huxerui::Text(
                          "还没有可路由的普通供应商，请先在「直连」页签新增。")
                          .Style(huxerui::TextStyle{
                              huxerui::Font::System(font_size::kCaption),
                              theme.colors.on_surface_variant})}
                    : huxerui::View{
                          huxerui::Button(
                              std::format("开始路由到「{}」",
                                          nameOf(startTarget)))
                              .OnClick([toast, tool, startTarget, modeTab,
                                        bumpRevision] {
                                  try {
                                      StartRoute(tool, startTarget);
                                      modeTab = 1;
                                      bumpRevision();
                                      toast.Show("已进入路由模式");
                                  } catch (const std::exception& e) {
                                      toast.Show(e.what());
                                  }
                              })};
            routeBanner =
                huxerui::Column {
                    huxerui::Text("当前未路由。开始后 Agent 的 base URL "
                                  "会指向本地路由，切换目标无需重启。")
                        .Style(huxerui::TextStyle{
                            huxerui::Font::System(font_size::kCaption),
                            theme.colors.on_surface_variant}),
                    std::move(startButton),
                }.With(huxerui::Spacing(8.0F),
                       huxerui::CrossAlign(
                           huxerui::CrossAxisAlignment::Stretch));
        }
        routeBanner = std::move(routeBanner).With(
            huxerui::Padding(huxerui::EdgeInsets::Symmetric(12.0F, 8.0F)),
            huxerui::Background(islands.raised),
            huxerui::CornerRadius(islands.nested_radius));
    }

    huxerui::View aggregateBanner = huxerui::Row{};
    if (view == 2) {
        aggregateBanner =
            mode == "aggregate"
                ? huxerui::Row {
                      huxerui::Text(
                          std::format("聚合生效中 · {}", effectiveName))
                          .Style(huxerui::TextStyle{
                              huxerui::Font::System(font_size::kBody),
                              theme.colors.on_surface}),
                      huxerui::Spacer(),
                      huxerui::Button("回到直连")
                          .OnClick([backToDirect] { backToDirect(); }),
                  }.With(huxerui::Spacing(8.0F),
                         huxerui::CrossAlign(
                             huxerui::CrossAxisAlignment::Center))
                : huxerui::View{
                      huxerui::Text(
                          "选择下方的聚合供应商卡片进入聚合模式；请求经本地"
                          "路由按 model 分发到成员供应商。")
                          .Style(huxerui::TextStyle{
                              huxerui::Font::System(font_size::kCaption),
                              theme.colors.on_surface_variant})};
        aggregateBanner = std::move(aggregateBanner).With(
            huxerui::Padding(huxerui::EdgeInsets::Symmetric(12.0F, 8.0F)),
            huxerui::Background(islands.raised),
            huxerui::CornerRadius(islands.nested_radius));
    }

    huxerui::View directNote = huxerui::Row{};
    if (view == 0 && mode != "direct") {
        directNote = huxerui::Row {
            huxerui::Text(std::format(
                              "当前生效：{}。在直连页签点击卡片会退出{}模式。",
                              modeName, modeName))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.on_surface_variant}),
        }.With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 6.0F)),
               huxerui::Background(islands.raised),
               huxerui::CornerRadius(islands.nested_radius));
    }

    huxerui::View providerCards = huxerui::Row{};
    if (view == 0) {
        huxerui::View official =
            hasOfficial
                ? OfficialCard(tool,
                               mode == "direct" &&
                                   currentProvider.empty() &&
                                   detectedProvider.empty(),
                               toast, revision)
                : huxerui::View{huxerui::Row{}};
        huxerui::View directList = huxerui::Row{};
        if (!directProviders.empty()) {
            directList =
                huxerui::VirtualList(
                    directProviders,
                    [tool, mode, currentProvider, directProvider,
                     detectedProvider, tasks, toast, revision, usageCache,
                     formToolIndex,
                     formTarget](const models::Provider& provider) {
                        const bool active =
                            mode == "direct" &&
                            (currentProvider == provider.id ||
                             detectedProvider == provider.id);
                        const bool current = active;
                        std::string label;
                        if (active) {
                            label = "使用中";
                        } else if (mode != "direct" &&
                                   directProvider == provider.id) {
                            label = "直连";
                        }
                        return ProviderCard(
                            tool, provider, active, current, std::move(label),
                            ProviderCardAction::SwitchDirect, tasks, toast,
                            revision, usageCache, formToolIndex, formTarget);
                    })
                    .EstimatedItemExtent(150.0F)
                    .CacheExtent(480.0F)
                    .With(huxerui::Spacing(10.0F), huxerui::Grow(1.0F));
        }
        if (hasOfficial && !directProviders.empty()) {
            providerCards = huxerui::Column {
                std::move(official),
                std::move(directList),
            }.With(huxerui::Spacing(10.0F),
                   huxerui::CrossAlign(
                       huxerui::CrossAxisAlignment::Stretch));
        } else {
            providerCards =
                !directProviders.empty() ? std::move(directList)
                                         : std::move(official);
        }
    } else if (view == 1) {
        if (!directProviders.empty()) {
            providerCards =
                huxerui::VirtualList(
                    directProviders,
                    [tool, mode, currentProvider, directProvider, tasks, toast,
                     revision, usageCache, formToolIndex,
                     formTarget](const models::Provider& provider) {
                        const bool target =
                            mode == "route" &&
                            currentProvider == provider.id;
                        std::string label;
                        if (target) {
                            label = "路由中";
                        } else if (directProvider == provider.id) {
                            label = "直连";
                        }
                        return ProviderCard(
                            tool, provider, target, target, std::move(label),
                            ProviderCardAction::RouteTo, tasks, toast,
                            revision, usageCache, formToolIndex, formTarget);
                    })
                    .EstimatedItemExtent(150.0F)
                    .CacheExtent(480.0F)
                    .With(huxerui::Spacing(10.0F), huxerui::Grow(1.0F));
        }
    } else {
        if (!aggregateProviders.empty()) {
            providerCards =
                huxerui::VirtualList(
                    aggregateProviders,
                    [tool, mode, currentProvider, tasks, toast, revision,
                     usageCache, formToolIndex,
                     formTarget](const models::Provider& provider) {
                        const bool active =
                            mode == "aggregate" &&
                            currentProvider == provider.id;
                        return ProviderCard(
                            tool, provider, active, active,
                            active ? std::string("聚合生效中") : std::string{},
                            ProviderCardAction::Aggregate, tasks, toast,
                            revision, usageCache, formToolIndex, formTarget);
                    })
                    .EstimatedItemExtent(150.0F)
                    .CacheExtent(480.0F)
                    .With(huxerui::Spacing(10.0F), huxerui::Grow(1.0F));
        }
    }

    const bool hasCards =
        view == 0 ? (hasOfficial || !directProviders.empty())
                  : (view == 1 ? !directProviders.empty()
                               : !aggregateProviders.empty());

    huxerui::View emptyState =
        huxerui::Column {
            huxerui::Text(view == 1
                              ? "还没有可路由的普通供应商。请先在「直连」页签新增。"
                              : view == 2
                                    ? "还没有聚合供应商。点击右上角 + 新增。"
                                    : "还没有供应商。点击右上角 + 新增。")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kBody),
                    theme.colors.on_surface_variant}),
        }.With(huxerui::Padding(32.0F),
               huxerui::Grow(1.0F),
               huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));

    std::vector<huxerui::View> content;
    if (tool == "claude" && cfg::claudeDesktopDir().empty()) {
        content.push_back(
            huxerui::Text("Claude Desktop 仅支持 macOS / "
                          "Windows，当前平台切换不可用")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.on_surface_variant})
                .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 6.0F)),
                      huxerui::Background(islands.raised),
                      huxerui::CornerRadius(islands.nested_radius)));
    }
    if (view == 0 && mode != "direct") content.push_back(std::move(directNote));
    if (view == 1) content.push_back(std::move(routeBanner));
    if (view == 2) content.push_back(std::move(aggregateBanner));
    content.push_back(hasCards ? std::move(providerCards)
                               : std::move(emptyState));

    huxerui::View root = huxerui::Column {
        std::move(modeHeader),
        huxerui::Column(std::move(content))
            .With(huxerui::Spacing(theme.spacing.medium),
                  huxerui::Grow(1.0F),
                  huxerui::CrossAlign(
                      huxerui::CrossAxisAlignment::Stretch)),
    }.With(huxerui::Spacing(theme.spacing.medium),
           huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    return root;
}
} // namespace llmswitch::ui
