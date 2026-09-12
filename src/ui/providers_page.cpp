// providers_page.cpp — 供应商列表页：各 agent 工具组共用同一组件（参数化
// tool，注册表见 models::toolRegistry()）。工具选择由 AgentPage 的
// Agent 工具栏 + Pager 负责，新增动作也由 AgentPage 顶部 action group 触发；
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
// 进入新增页；编辑/新增都是整页表单（ProviderFormPage，字段太多弹窗太挤），
// 新增页顶部内嵌预设模板区（点选预填）；用量查询配置是独立整页
// UsageFormPage（formTarget = "usage:" + id 进入）。
// 表单按 ToolSpec 适配：完整 URL switch 与上游格式 Select 控制 URL 后缀，codex
// 显示 config.toml 原文、needsModel（opencode/pi）模型必填、hasApiFormat 显示
// API 协议分段选择；hasModelMappings（claude-code /
// claude）额外显示三档模型映射行（Haiku/Sonnet/Opus）。模型字段旁「获取模型」
// 默认按当前实际 URL/apiKey/上游格式经 HuxerUI HttpClient 拉取模型列表，也支持在
// 高级选项中覆盖完整模型列表 URL；平台异步请求完成后结果回 UI 线程写 State；拉取成功后模型行在按钮前出现 Select
// 下拉，点选回填该行的模型字段（不弹窗）。
// 用量查询：启用开关打开且 usageUrl 非空的卡片显示用量文本 + 手动刷新按钮；AgentPage
// 共享缓存，且只允许一个保留页启动按 config 的 usageRefreshMinutes
// 轮询，避免 Pager 保留多页后重复请求。
//
// 数据流：所有 store 读写都在 UI 线程（store 无内部锁，UI 线程独占是契约；
// live 文件读写为微秒级本地 IO，不经任务线程）。写操作后 revision+1，
// 驱动本页重读、托盘菜单重建。detectCurrent 在页面首组合跑一次。
// 列表多模式：formTarget State（"" = 列表；"new" = 新增；"usage:"+id = 用量
// 配置页；否则 = 编辑的 provider id）驱动末尾单 return 多选一，所有
// UseState 都在分支之前。
#include <huxerui/huxerui.h>

#include <array>
#include <chrono>
#include <map>
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
    huxerui::State<std::string> addProviderRequest, bool enableUsagePolling) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    const auto http = huxerui::UseService<huxerui::HttpClient>();
    // 首组合时探测 live 文件命中（本地文件读，UI 线程直接跑）。
    auto detected = huxerui::UseState<std::string>({});
    huxerui::Lifecycle(
        [tool, detected] {
            detected = providerStore().detectCurrent(tool);
            return [] {};
        },
        0);
    // 列表/表单多模式："" = 列表；"new" = 新增；"usage:" + id = 用量查询
    // 配置页；否则 = 编辑的 provider id。子页以 .Key 组合，换目标即重建状态。
    auto formTarget = huxerui::UseState<std::string>({});
    // AgentPage 顶部 action group 发来的新增请求只由对应工具页消费，随后
    // 清空请求，避免同一次点击在后续重组中重复打开表单。
    huxerui::Lifecycle(
        [tool, addProviderRequest, formTarget] {
            if (addProviderRequest.Get() == tool) {
                addProviderRequest = {};
                formTarget = "new";
            }
            return [] {};
        },
        addProviderRequest.Get());
    // 编辑表单先显示轻量加载页，再异步拷贝目标供应商。这样切换编辑目标
    // 不必先构造整棵供应商卡片树；formDataTarget 也用来丢弃旧目标的迟到结果。
    auto formInitial = huxerui::UseState<models::Provider>({});
    auto formDataTarget = huxerui::UseState<std::string>({});
    auto formLoading = huxerui::UseState(false);
    const std::string target = formTarget.Get();
    huxerui::Lifecycle(
        [tasks, tool, target, formTarget, formInitial, formDataTarget,
         formLoading] {
            formDataTarget = "";
            formLoading = !target.empty() && target != "new";
            if (target.empty()) return;
            if (target == "new") {
                formDataTarget = target;
                formLoading = false;
                return;
            }
            tasks.Launch([tool, target, formTarget, formInitial, formDataTarget,
                          formLoading]() -> huxerui::Task<void> {
                co_await huxerui::Delay(std::chrono::duration<double>{0});
                if (formTarget.Get() != target) co_return;

                models::Provider loaded;
                const auto& group = providerStore().group(tool);
                for (const auto& provider : group.providers) {
                    if (provider.id == target) {
                        loaded = provider;
                        break;
                    }
                }
                if (formTarget.Get() != target) co_return;
                formInitial = loaded;
                formDataTarget = target;
                formLoading = false;
            });
        },
        target);

    // 用量缓存由 AgentPage 共享；自动轮询只由第一个保留页启动（TaskScope
    // 随 Agent 页卸载取消）。每个周期在 UI 线程重读 config：
    // usageRefreshMinutes>0 时立即拉一轮所有启用且配置了 usageUrl 的供应商（全部分组，
    // 不只当前工具）再睡一个间隔；仅手动时按 30s 轻量再检查（设置页改动至多
    // 30s 生效，避免睡死在一个长间隔里）。State 只在 UI 线程写。
    huxerui::Lifecycle(
        [tasks, usageCache, http, enableUsagePolling] {
            if (enableUsagePolling) {
                tasks.Launch([usageCache, http]() -> huxerui::Task<void> {
                    while (true) {
                        const auto& config = providerStore().config();
                        if (config.usageRefreshMinutes <= 0) {
                            co_await huxerui::Delay(
                                std::chrono::duration<double>{30});
                            continue;
                        }
                        std::vector<models::Provider> targets;
                        for (const auto& [toolId, grp] : config.groups) {
                            for (const auto& p : grp.providers) {
                                if (p.usageEnabled && !p.usageUrl.empty()) {
                                    targets.push_back(p);
                                }
                            }
                        }
                        // 顺序拉取（每次最长 10s），每个完成即回写缓存。
                        for (const auto& p : targets) {
                            WriteUsageCache(usageCache, p.id,
                                            co_await FetchUsageText(http, p));
                        }
                        co_await huxerui::Delay(std::chrono::duration<double>{
                            config.usageRefreshMinutes * 60.0});
                    }
                });
            }
            return [] {};
        },
        enableUsagePolling);

    // 订阅全局变更计数：托盘切换 / 设置页导入后本页重读。
    (void)revision.Get();

    // 表单模式必须在构造列表卡片之前返回。编辑时只先挂载一个轻量页面，
    // 目标数据回填完成后再创建真正的表单，避免等待所有列表内容完成组合。
    const bool formReady = formDataTarget.Get() == target && !formLoading.Get();
    if (!target.empty()) {
        if (!formReady) {
            auto goBack = [tasks, formTarget] {
                tasks.Launch([formTarget]() -> huxerui::Task<void> {
                    co_await huxerui::Delay(std::chrono::duration<double>{0});
                    formTarget = "";
                });
            };
            const std::string title = target.starts_with("usage:")
                                          ? "用量查询"
                                          : "编辑供应商";
            return PageScaffold(
                       title,
                       huxerui::Row {
                           huxerui::Button("返回")
                               .OnClick([goBack] { goBack(); }),
                       },
                       huxerui::Column {
                           huxerui::Text("正在加载供应商配置…")
                               .Style(huxerui::TextStyle{
                                   huxerui::Font::System(font_size::kBody),
                                   theme.colors.on_surface_variant}),
                       }
                           .With(huxerui::Grow(1.0F),
                                 huxerui::MainAlign(
                                     huxerui::MainAxisAlignment::Center),
                                 huxerui::CrossAlign(
                                     huxerui::CrossAxisAlignment::Center)))
                .Key("form-loading:" + target);
        }

        const models::Provider initial = formInitial.Get();
        if (target.starts_with("usage:")) {
            return UsageFormPage(tool, initial, revision, formTarget)
                .Key("usage:" + target.substr(6));
        }
        const bool isNew = target == "new";
        return ProviderFormPage(tool, initial, isNew, revision, formTarget)
            .Key("form:" + target);
    }

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
                 usageCache, formTarget](const models::Provider& provider) {
                    const bool isCurrent = currentProvider == provider.id;
                    const bool active =
                        isCurrent || detectedProvider == provider.id;
                    return ProviderCard(tool, provider, active, isCurrent, tasks,
                                        toast, revision, usageCache, formTarget);
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
