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

[[huxerui::composable]] huxerui::View AgentToolButton(
    std::string iconName, std::string label,
    huxerui::State<std::size_t> selectedTool, std::size_t toolIndex) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    huxerui::View toolButton =
        huxerui::IconButton(ToolIcon(iconName), label)
            .OnClick([selectedTool, toolIndex] { selectedTool = toolIndex; })
            .With(huxerui::Tooltip(label));
    if (selectedTool.Get() == toolIndex) {
        toolButton = std::move(toolButton).With(
            huxerui::Background(islands.overlay),
            huxerui::CornerRadius(islands.nested_radius));
    }
    return toolButton;
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

    // 表单页状态：formTool 记录表单属于哪个 Agent 组（供应商 id 只在组内唯一）；
    // formTarget 的取值与表单页约定一致——"" = 无表单；"new" = 新增；
    // "usage:" + id = 用量查询配置；否则 = 编辑的供应商 id。
    auto formTool = huxerui::UseState<std::string>({});
    auto formTarget = huxerui::UseState<std::string>({});
    auto formInitial = huxerui::UseState<models::Provider>({});
    auto formDataTarget = huxerui::UseState<std::string>({});
    auto formLoading = huxerui::UseState(false);

    // 顶部 action group 的新增请求（AppRoot 共享）由本页消费，随后清空，避免
    // 同一次点击在后续重组中重复打开表单。三个 State 一起写：下一帧的表单以
    // 新的 formInitial 挂载。
    huxerui::Lifecycle(
        [addProviderRequest, formTool, formTarget, formInitial] {
            if (!addProviderRequest.Get().empty()) {
                formTool = addProviderRequest.Get();
                addProviderRequest = {};
                formInitial = models::Provider{};
                formTarget = "new";
            }
            return [] {};
        },
        addProviderRequest.Get());

    const std::string target = formTarget.Get();
    const std::string formToolId = formTool.Get();
    // 打开表单不必先构造整棵供应商卡片树；目标供应商的拷贝经任务线程在本次
    // 事件之后完成，formDataTarget 同时用来丢弃旧目标的迟到结果。
    huxerui::Lifecycle(
        [tasks, formToolId, target, formTool, formTarget, formInitial,
         formDataTarget, formLoading] {
            formDataTarget = "";
            formLoading = !target.empty() && target != "new";
            if (target.empty()) return;
            if (target == "new") {
                formDataTarget = target;
                formLoading = false;
                return;
            }
            tasks.Launch([formToolId, target, formTool, formTarget, formInitial,
                          formDataTarget,
                          formLoading]() -> huxerui::Task<void> {
                // 不经 Delay(0)（帧调度）：任务体在工厂返回后经事件队列
                // 立即执行，此时组合已结束，写状态安全。
                if (formTarget.Get() != target ||
                    formTool.Get() != formToolId) {
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
                    formTool.Get() != formToolId) {
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
                AgentToolButton(std::string(spec.iconName),
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
                ProvidersPage(id, revision, usageCache, formTool, formTarget,
                              navPage, selectedTool, index)
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

    huxerui::View navigationContainer = huxerui::Row {
        huxerui::Row(*cachedButtons)
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
                                 target == "new", revision, formTarget)
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
