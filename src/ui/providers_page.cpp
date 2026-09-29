// providers_page.cpp — 供应商列表页：各 agent 工具组共用同一组件（参数化
// tool，注册表见 models::toolRegistry()）。工具选择由 AgentPage 的
// Agent 工具栏 + Pager 负责，新增动作也由 AgentPage 顶部 action group 触发；
// 本页只有列表一种形态——新增/编辑与用量查询配置是 AgentPage 自己的第二个
// 页面（整页覆盖工具栏 + Pager，不受分页器左右拖动影响），本页提供的只是
// 进入入口（formTool + formTarget 两个 State 由 AgentPage 持有）：
// 卡片的编辑图标写 formTarget = id，用量 gauge 图标写 "usage:" + id，
// AgentPage 据此换页并把供应商数据回填给表单。
// 每个供应商一张卡片三段式：左信息列（名称 / 实际访问 URL / 备注 /
// 「使用中」徽章（group.current 或 detectCurrent 命中），
// Grow 吃满剩余宽度）｜ 中间状态列（连通检测延迟 + 用量文本/刷新图标，
// 垂直居中落在内容与操作组之间，两者皆无时塌缩为零宽）｜ 右侧操作图标组
// （切换 swap / 联通检测 activity / 编辑 edit / 用量查询配置 gauge /
// 复制 copy / 删除 trash，自绘 SVG + Tooltip，删除走内置确认框）。联通检测经
// HuxerUI HttpClient（平台原生异步 HTTP），连通后卡片显示
// 「延迟 N ms」，失败显示「不可达：…」（error 色）。有官方厂商的工具
// （claude-code / claude / codex，models::officialVendorName）列表第一位固定
// 一张「官方」常驻卡：切换 = store.restoreOfficial 还原厂商原生状态，
// active = 组 current 与 detectCurrent 均为空。头部只有一个加号 IconButton
// 进入新增页。
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

#include <array>
#include <chrono>
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
    huxerui::State<std::string> formTool,
    huxerui::State<std::string> formTarget,
    huxerui::State<std::size_t> navPage,
    huxerui::State<std::size_t> selectedTool, std::size_t toolIndex) {
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
    const bool usageVisible = navPage.Get() == 0 &&
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
                        if (!p.usageEnabled || p.usageUrl.empty() ||
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

    // 卡片列表：官方常驻卡（有官方厂商的工具）排第一，其后是供应商卡；
    // 「使用中」= 组内 current 或 detectCurrent 命中。
    const auto& g = providerStore().group(tool);
    const bool hasOfficial = !models::officialVendorName(tool).empty();
    const std::size_t providerCount = g.providers.size();
    const std::string currentProvider = g.current;
    const std::string detectedProvider = detected.Get();
    huxerui::View providerCards = huxerui::Row{};
    if (hasOfficial) {
        providerCards = OfficialCard(
            tool, currentProvider.empty() && detectedProvider.empty(), toast,
            revision);
    }
    if (providerCount > 0) {
        const huxerui::View providerList =
            huxerui::VirtualList(
                g.providers,
                [tool, currentProvider, detectedProvider, tasks, toast, revision,
                 usageCache, formTool, formTarget](const models::Provider& provider) {
                    const bool isCurrent = currentProvider == provider.id;
                    const bool active =
                        isCurrent || detectedProvider == provider.id;
                    return ProviderCard(tool, provider, active, isCurrent, tasks,
                                        toast, revision, usageCache, formTool,
                                        formTarget);
                })
                .EstimatedItemExtent(150.0F)
                .CacheExtent(480.0F)
                .With(huxerui::Spacing(10.0F), huxerui::Grow(1.0F));
        providerCards = hasOfficial
                            ? huxerui::Column {
                                  providerCards,
                                  providerList,
                              }
                                  .With(huxerui::Spacing(10.0F),
                                        huxerui::CrossAlign(
                                            huxerui::CrossAxisAlignment::Stretch))
                            : providerList;
    }
    const bool hasCards = hasOfficial || providerCount > 0;

    // 列表模式只提供 Agent 岛屿内的 page 内容；Agent 工具栏、action group
    // 与 Pager 的外层岛屿由 AgentPage 统一拥有，避免每个 page 各自形成岛屿。
    const IslandTheme islands = ResolveIslandTheme(theme);
    std::vector<huxerui::View> listItems;

    // Claude Desktop 平台提示条（仅 macOS / Windows 可用；图标照常显示，
    // 增删改可用但本平台无法切换生效）。
    if (tool == "claude" && cfg::claudeDesktopDir().empty()) {
        listItems.push_back(
            huxerui::Text("Claude Desktop 仅支持 macOS / "
                          "Windows，当前平台切换不可用")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.on_surface_variant})
                .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 6.0F)),
                      huxerui::Background(islands.raised),
                      huxerui::CornerRadius(islands.nested_radius)));
    }
    listItems.push_back(
        !hasCards
            ? huxerui::View{
                  huxerui::Column {
                      huxerui::Text("还没有供应商。点击右上角 + 新增。")
                          .Style(huxerui::TextStyle{
                              huxerui::Font::System(font_size::kBody),
                              theme.colors.on_surface_variant}),
                  }.With(huxerui::Padding(32.0F),
                         huxerui::Grow(1.0F),
                         huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                         huxerui::CrossAlign(
                             huxerui::CrossAxisAlignment::Center))}
            : providerCards);

    huxerui::View root = huxerui::Column(std::move(listItems))
        .With(huxerui::Spacing(theme.spacing.medium),
              huxerui::Grow(1.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    return root;
}
} // namespace llmswitch::ui
