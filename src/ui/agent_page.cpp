// agent_page.cpp — Agent 管理页：顶部 Agent 工具栏与 action group 共用一行，
// 下方 Pager 支持左右拖动；各工具供应商列表页保持挂载，保留列表滚动位置与
// 卡片局部状态。
//
// 编辑/新增供应商与用量查询配置是本页自己的第二个页面（IndexedPages 第二页，
// 见下方 agentForm）：打开表单时它整页覆盖工具栏 + Pager，分页器不参与布局与
// 命中——左右拖动没有可作用的滚动节点，拖不走正在编辑的表单，也切不到别的
// Agent。IndexedPages 只测量与摆放选中页，所以 Pager 与各列表页仍保持挂载，
// 返回列表时用量刷新时间戳、连通检测结果等局部状态都还在。
// 表单页在 Agent 页这一层用 PageScaffold 的内边距，只缩进一次（不再叠加在
// 已带壳层内边距的 Pager 页里），表单标题与工具栏、其他页面标题同一条左边线。
#include <huxerui/huxerui.h>

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ui.h"
#include "providers_internal.h"
#include "app_resources.h"

import llmswitch.models;
import llmswitch.store;

namespace llmswitch::ui {

// Agent 图标按钮（Agent 页工具栏与新增供应商页的「目标 Agent」选择器共用同一
// 控件）：点击把 selectedIndex 写成 index，选中态由承载底块表达。选中下标在
// 按钮自己的组合体里读取，外层缓存 View 声明也能跟随选择变化重组。
[[huxerui::composable]] huxerui::View AgentToolButton(
    huxerui::ImageResource icon, std::string label,
    huxerui::State<std::size_t> selectedIndex, std::size_t index) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    huxerui::View toolButton =
        huxerui::IconButton(icon, label)
            .OnClick([selectedIndex, index] { selectedIndex = index; })
            .With(huxerui::Tooltip(label));
    if (selectedIndex.Get() == index) {
        toolButton = std::move(toolButton).With(
            huxerui::Background(islands.overlay),
            huxerui::CornerRadius(islands.nested_radius));
    }
    return toolButton;
}

// 工具栏岛：一排图标按钮 + raised 表面 / 6pt 圆角 / 内边距。Agent 页工具栏与
// 新增供应商页的目标选择器外观完全一致。
[[huxerui::composable]] huxerui::View AgentToolBar(huxerui::View buttons) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    // 形参在 hcg 生成的组合体里按值捕获（const），链式修饰符右值限定。
    huxerui::View row = buttons;
    return huxerui::Row {
        std::move(row)
            .With(huxerui::Spacing(theme.spacing.extra_small),
                  huxerui::MainAlign(huxerui::MainAxisAlignment::Start),
                  huxerui::CrossAlign(
                      huxerui::CrossAxisAlignment::Center)),
    }.With(huxerui::Padding(theme.spacing.extra_small),
           huxerui::Background(islands.raised),
           huxerui::CornerRadius(islands.nested_radius),
           huxerui::ClipChildren(),
           huxerui::MainAlign(huxerui::MainAxisAlignment::Start),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View AgentPager(
    std::shared_ptr<std::vector<huxerui::View>> pages,
    huxerui::State<std::size_t> selectedTool, std::size_t pageCount) {
    auto selectTool = [selectedTool, pageCount](std::size_t index) {
        if (index < pageCount) {
            selectedTool = index;
        }
    };
    return huxerui::Pager(*pages, selectedTool)
        .ScrollAxis(huxerui::Axis::Horizontal)
        .DragEnabled(true)
        .OnChanged(selectTool);
}

[[huxerui::composable]] huxerui::View AgentPage(
    huxerui::State<int> revision, huxerui::State<std::size_t> navPage,
    huxerui::State<std::size_t> selectedTool,
    huxerui::State<std::string> addProviderRequest) {
    const auto& registry = models::toolRegistry();
    if (registry.empty()) {
        return huxerui::Text("没有可用的 Agent 工具");
    }

    auto usageCache = huxerui::UseState<std::map<std::string, std::string>>({});
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    const huxerui::TaskScope tasks = huxerui::UseTaskScope();

    // 表单页状态：formToolIndex 是表单的目标 Agent 在注册表里的下标，
    // registry.size() = 特殊项「所有 Agent」（同一份配置写进每个组）；
    // formTarget 的取值与表单页约定一致——"" = 无表单；"new" = 新增；
    // "usage:" + id = 用量查询配置；否则 = 编辑的供应商 id。
    // 用下标而不是 tool id 作为唯一真源：新增页的左上角就是这条注册表工具栏，
    // 选中态与点击落点天然一致（ToolRegistryIndex 供卡片入口换算）。
    const std::size_t allToolsIndex = registry.size();
    auto formToolIndex = huxerui::UseState<std::size_t>(0);
    auto formTarget = huxerui::UseState<std::string>({});
    auto formInitial = huxerui::UseState<models::Provider>({});
    auto formDataTarget = huxerui::UseState<std::string>({});
    auto formLoading = huxerui::UseState(false);

    // 顶部 action group 的新增请求（AppRoot 共享）由本页消费，随后清空，避免
    // 同一次点击在后续重组中重复打开表单。写目标下标 + 目标 + 初值：下一帧的
    // 表单以新的 formInitial 挂载。
    huxerui::Lifecycle(
        [addProviderRequest, formToolIndex, formTarget, formInitial] {
            if (!addProviderRequest.Get().empty()) {
                const std::string requested = addProviderRequest.Get();
                addProviderRequest = {};
                formToolIndex = ToolRegistryIndex(requested);
                formInitial = models::Provider{};
                formTarget = "new";
            }
            return [] {};
        },
        addProviderRequest.Get());

    const std::string target = formTarget.Get();
    const std::size_t toolIndex =
        std::min(formToolIndex.Get(), allToolsIndex);
    // 「所有 Agent」没有单一 tool id：表单退回注册表中立的通用策略（不出现
    // 任何 agent 专属区块），保存时把同一份配置写进每个注册表组。
    const bool allTools = toolIndex == allToolsIndex;
    const std::string formToolId =
        allTools ? std::string{} : std::string(registry[toolIndex].id);
    // 打开表单不必先构造整棵供应商卡片树；目标供应商的拷贝经任务线程在本次
    // 事件之后完成，formDataTarget 同时用来丢弃旧目标的迟到结果。
    huxerui::Lifecycle(
        [tasks, formToolId, target, toolIndex, formToolIndex, formTarget,
         formInitial, formDataTarget, formLoading] {
            formDataTarget = "";
            formLoading = !target.empty() && target != "new";
            if (target.empty()) return;
            if (target == "new") {
                formDataTarget = target;
                formLoading = false;
                return;
            }
            tasks.Launch([formToolId, target, toolIndex, formToolIndex,
                          formTarget, formInitial, formDataTarget,
                          formLoading]() -> huxerui::Task<void> {
                // 不经 Delay(0)（帧调度）：任务体在工厂返回后经事件队列
                // 立即执行，此时组合已结束，写状态安全。
                if (formTarget.Get() != target ||
                    formToolIndex.Get() != toolIndex) {
                    co_return;
                }
                const std::string id =
                    target.starts_with("usage:") ? target.substr(6) : target;
                models::Provider loaded;
                for (const auto& provider :
                     providerStore().group(formToolId).providers) {
                    if (provider.id == id) {
                        loaded = provider;
                        break;
                    }
                }
                if (formTarget.Get() != target ||
                    formToolIndex.Get() != toolIndex) {
                    co_return;
                }
                formInitial = loaded;
                formDataTarget = target;
                formLoading = false;
                co_return;
            });
        },
        target, formToolId);

    auto toolButtonCache =
        huxerui::UseState<std::shared_ptr<std::vector<huxerui::View>>>({});
    std::shared_ptr<std::vector<huxerui::View>> cachedButtons =
        toolButtonCache.Get();
    if (!cachedButtons || cachedButtons->size() != registry.size()) {
        auto nextButtons = std::make_shared<std::vector<huxerui::View>>();
        nextButtons->reserve(registry.size());
        for (std::size_t index = 0; index < registry.size(); ++index) {
            const auto& spec = registry[index];
            const std::string id(spec.id);
            nextButtons->push_back(
                AgentToolButton(ToolIcon(spec.iconName),
                                std::string(spec.displayName), selectedTool, index)
                    .Key("agent-tool:" + id));
        }
        toolButtonCache = nextButtons;
        cachedButtons = std::move(nextButtons);
    }

    auto providerPageCache =
        huxerui::UseState<std::shared_ptr<std::vector<huxerui::View>>>({});
    std::shared_ptr<std::vector<huxerui::View>> cachedPages =
        providerPageCache.Get();
    if (!cachedPages || cachedPages->size() != registry.size()) {
        auto nextPages = std::make_shared<std::vector<huxerui::View>>();
        nextPages->reserve(registry.size());
        for (std::size_t index = 0; index < registry.size(); ++index) {
            const auto& spec = registry[index];
            const std::string id(spec.id);
            nextPages->push_back(
                ProvidersPage(id, revision, usageCache, formToolIndex,
                              formTarget, navPage, selectedTool, index)
                    .Key("agent-providers:" + id)
                    .With(huxerui::Grow(1.0F)));
        }
        providerPageCache = nextPages;
        cachedPages = std::move(nextPages);
    }

    auto requestAddProvider = [selectedTool, addProviderRequest] {
        const auto& currentRegistry = models::toolRegistry();
        const std::size_t index = selectedTool.Get();
        if (index < currentRegistry.size()) {
            addProviderRequest = std::string(currentRegistry[index].id);
        }
    };

    huxerui::View navigationContainer = AgentToolBar(huxerui::Row(*cachedButtons));
    // 与 PageScaffold 同一套壳层约束：顶部不留内边距（top = 0，壳层也不留
    // Spacing），Agent 工具栏紧接标题栏下沿；左右边距是壳层标题栏的同一个
    // shellInset，应用名与工具栏左对齐。
    const float inset = compact ? theme.spacing.medium : theme.spacing.large;
    // 页面 0：工具栏 + Pager（各工具列表页常驻）。
    huxerui::View agentPage = huxerui::Column {
        huxerui::Row {
            navigationContainer,
            huxerui::Row {
                huxerui::IconButton(app::images::add, "新增供应商")
                    .OnClick(requestAddProvider)
                    .With(huxerui::Tooltip("新增供应商")),
            }.With(huxerui::Spacing(theme.spacing.small),
                   huxerui::CrossAlign(
                       huxerui::CrossAxisAlignment::Center)),
        }.With(huxerui::Spacing(theme.spacing.small),
               huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        AgentPager(cachedPages, selectedTool, registry.size())
            .With(huxerui::Grow(1.0F)),
    }.With(huxerui::Padding(huxerui::EdgeInsets{.top = 0.0F,
                                               .right = inset,
                                               .bottom = inset,
                                               .left = inset}),
           huxerui::Spacing(theme.spacing.medium),
           huxerui::ClipChildren(),
           huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    // 新增页左上角的目标 Agent 选择器：与 Agent 页工具栏同一控件、同一外观，
    // 但行为是「这份配置写给谁」——点选切换目标 Agent，最后一项「所有 Agent」
    // 表示写进每个注册表组。只在新增时有意义（编辑的条目属于固定组）。
    huxerui::View targetSelector;
    if (target == "new") {
        std::vector<huxerui::View> selectorButtons;
        selectorButtons.reserve(registry.size() + 1);
        for (std::size_t index = 0; index < registry.size(); ++index) {
            const auto& spec = registry[index];
            selectorButtons.push_back(
                AgentToolButton(ToolIcon(spec.iconName),
                                std::string(spec.displayName), formToolIndex,
                                index)
                    .Key("form-tool:" + std::string(spec.id)));
        }
        selectorButtons.push_back(
            AgentToolButton(app::images::all, "所有 Agent", formToolIndex,
                            allToolsIndex)
                .Key("form-tool:all"));
        targetSelector =
            AgentToolBar(huxerui::Row(std::move(selectorButtons)));
    }

    // 页面 1：新增/编辑供应商或用量查询配置。未打开时是空占位（IndexedPages
    // 要求子 View 非空）；选中页才参与布局，占位不会被测量。
    huxerui::View agentForm = huxerui::Row{};
    if (!target.empty()) {
        const bool formReady =
            formDataTarget.Get() == target && !formLoading.Get();
        auto goBack = [formTarget] { formTarget = ""; };
        if (!formReady) {
            agentForm = PageScaffold(
                            target.starts_with("usage:") ? "用量查询"
                                                         : "编辑供应商",
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
        } else if (target.starts_with("usage:")) {
            agentForm =
                UsageFormPage(formToolId, formInitial.Get(), revision,
                              formTarget)
                    .Key("usage:" + target.substr(6));
        } else {
            agentForm =
                ProviderFormPage(formToolId, formInitial.Get(),
                                 target == "new", revision, formTarget,
                                 std::move(targetSelector))
                    .Key("form:" + target);
        }
    }

    // IndexedPages 只测量与摆放选中页：表单打开时 Pager 整体不参与布局与命中
    // （左右拖动切 Agent 的路径不存在），但它与各列表页仍保持挂载。
    return huxerui::IndexedPages(
               std::vector<huxerui::View>{std::move(agentPage),
                                          std::move(agentForm)},
               target.empty() ? 0U : 1U)
        .With(huxerui::Grow(1.0F));
}

} // namespace llmswitch::ui
