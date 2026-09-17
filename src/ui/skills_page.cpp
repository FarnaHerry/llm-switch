// skills_page.cpp — Skills 管理页：中央库（dataDir()/skills-store/）+ 符号链接
// 同步状态的合并视图。每卡：名称 + 描述（空显示「无描述」）+ 状态徽章
// （中央库 / 已同步 claude-code / 已同步 codex / 仅某工具安装）+ Agent 图标行
// （仅显示已同步或仅安装在该工具的 Agent）+ 编辑（描述 + 正文多行 → updateBody）
// + 删除（确认框）。顶部：新建 Skill + 收编工具已有 Skill；列表上方按 Agent
// 过滤（SegmentedButton：全部 + 各同步目标，命中 = 已同步或仅安装在该工具）。
//
// 数据流：SkillsStore 无长期持有价值（扫描即视图），页面用 UseState 持有一份，
// 每次操作后重新 load() 刷新（微秒级本地 IO，UI 线程直接跑）。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui.h"

import llmswitch.models;
import llmswitch.skills;

namespace llmswitch::ui {
namespace {

constexpr std::string_view kSkillTools[] = {"claude-code", "codex"};

bool IsLinked(const skills::SkillInfo& skill, std::string_view toolId) {
    for (const auto& t : skill.linkedTools) {
        if (t == toolId) return true;
    }
    return false;
}

// Skill 是否与某 Agent 相关：已同步到该工具，或仅安装在该工具目录。
bool InvolvesTool(const skills::SkillInfo& skill, std::string_view toolId) {
    if (IsLinked(skill, toolId)) return true;
    for (const auto& t : skill.installedOnlyTools) {
        if (t == toolId) return true;
    }
    return false;
}

// 状态徽章行：中央库 / 已同步 <工具> / 仅 <工具> 安装。
[[huxerui::composable]] huxerui::View SkillBadges(const skills::SkillInfo& skill) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    std::vector<huxerui::View> badges;
    auto badge = [&](std::string text, huxerui::Color bg, huxerui::Color fg) {
        badges.push_back(
            huxerui::Text(std::move(text)).Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption), fg})
                .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(8.0F, 2.0F)),
                      huxerui::Background(bg),
                      huxerui::CornerRadius(islands.nested_radius)));
    };
    if (skill.inStore) badge("中央库", theme.colors.primary, theme.colors.on_primary);
    for (const auto& t : skill.linkedTools) {
        badge("已同步 " + t, theme.colors.secondary, theme.colors.on_secondary);
    }
    for (const auto& t : skill.installedOnlyTools) {
        badge("仅 " + t + " 安装", theme.colors.secondary_container,
              theme.colors.on_secondary_container);
    }
    return huxerui::Row(std::move(badges))
        .With(huxerui::Spacing(6.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

// Agent 状态行：只显示已同步或仅安装在该工具的 Agent 图标，不显示文字开关。
[[huxerui::composable]] huxerui::View SkillAgentIcons(const skills::SkillInfo& skill) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    std::vector<huxerui::View> icons;
    for (const std::string_view toolId : kSkillTools) {
        if (!InvolvesTool(skill, toolId)) continue;
        const auto* spec = models::findTool(toolId);
        if (spec == nullptr) continue;
        const std::string label(ToolName(toolId));
        icons.push_back(
            huxerui::Image(ToolIcon(spec->iconName))
                .Tint(theme.colors.on_surface_variant)
                .With(huxerui::Frame{.width = 24.0F, .height = 24.0F},
                      huxerui::Semantics{.role = huxerui::SemanticRole::Image,
                                         .label = label},
                      huxerui::Tooltip(label)));
    }
    return huxerui::Row(std::move(icons))
        .With(huxerui::Spacing(8.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

// 编辑弹窗内容（composable）：描述 + 正文多行 → updateBody（frontmatter 用新
// 描述，正文整体替换）。
[[huxerui::composable]] huxerui::View SkillEditContent(
    std::string name, huxerui::State<huxerui::TextEditingValue> description,
    huxerui::State<huxerui::TextEditingValue> body, huxerui::DialogContext ctx,
    huxerui::ToastHandle toast, huxerui::State<skills::SkillsStore> store) {
    return DialogCard(huxerui::ScrollView(huxerui::Column {
        huxerui::Text("编辑 Skill — " + name, huxerui::TextRole::Title),
        huxerui::TextField(description.Get())
            .Label("描述")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([description](const huxerui::TextEditingValue& v) {
                description = v;
            }),
        huxerui::TextField(body.Get())
            .Label("正文（SKILL.md）")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .LineLimits(huxerui::TextFieldLineLimits::MultiLine(8, 16))
            .OnChanged([body](const huxerui::TextEditingValue& v) { body = v; }),
        huxerui::Row {
            huxerui::Button("取消").OnClick([ctx] { ctx.Dismiss(); }),
            huxerui::Button("保存").OnClick([=] {
                try {
                    // State::Get() 只给 const 引用；变更操作走 load() 出的
                    // 临时可变副本（磁盘即 SSOT，成功后整页重新 load 刷新）。
                    skills::SkillsStore::load().updateBody(
                        name, description.Get().text, body.Get().text);
                } catch (const std::exception& e) {
                    toast.Show(e.what());
                    return;
                }
                ctx.Dismiss();
                store = skills::SkillsStore::load();
                toast.Show("已保存");
            }),
        }.With(huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween)),
    }.With(huxerui::Spacing(12.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
        .With(huxerui::Frame{.width = 480.0F}));
}

// 新建弹窗内容（composable）：名称/描述/正文 → create（名字非法或已在中央库
// 时 store 抛中文错）。
[[huxerui::composable]] huxerui::View SkillCreateContent(
    huxerui::State<huxerui::TextEditingValue> name,
    huxerui::State<huxerui::TextEditingValue> description,
    huxerui::State<huxerui::TextEditingValue> body, huxerui::DialogContext ctx,
    huxerui::ToastHandle toast, huxerui::State<skills::SkillsStore> store) {
    return DialogCard(huxerui::ScrollView(huxerui::Column {
        huxerui::Text("新建 Skill", huxerui::TextRole::Title),
        huxerui::TextField(name.Get())
            .Label("名称")
            .Placeholder("my-skill")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([name](const huxerui::TextEditingValue& v) { name = v; }),
        huxerui::TextField(description.Get())
            .Label("描述（可选）")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([description](const huxerui::TextEditingValue& v) {
                description = v;
            }),
        huxerui::TextField(body.Get())
            .Label("正文（SKILL.md，可选）")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .LineLimits(huxerui::TextFieldLineLimits::MultiLine(6, 14))
            .OnChanged([body](const huxerui::TextEditingValue& v) { body = v; }),
        huxerui::Row {
            huxerui::Button("取消").OnClick([ctx] { ctx.Dismiss(); }),
            huxerui::Button("创建").OnClick([=] {
                if (name.Get().text.empty()) {
                    toast.Show("名称不能为空");
                    return;
                }
                try {
                    skills::SkillsStore::load().create(
                        name.Get().text, description.Get().text, body.Get().text);
                } catch (const std::exception& e) {
                    toast.Show(e.what());
                    return;
                }
                ctx.Dismiss();
                store = skills::SkillsStore::load();
                toast.Show("已创建 " + name.Get().text);
            }),
        }.With(huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween)),
    }.With(huxerui::Spacing(12.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
        .With(huxerui::Frame{.width = 480.0F}));
}

[[huxerui::composable]] huxerui::View SkillCard(
    const skills::SkillInfo& skill, huxerui::TaskScope tasks,
    huxerui::ToastHandle toast, huxerui::State<skills::SkillsStore> store) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto dialog = huxerui::UseDialog();
    // 编辑表单状态归卡片作用域（弹窗内容随卡片存活）。
    auto description = huxerui::UseState(huxerui::TextEditingValue{""});
    auto body = huxerui::UseState(huxerui::TextEditingValue{""});
    const std::string name = skill.name;

    auto reload = [store] { store = skills::SkillsStore::load(); };

    // 编辑：读正文预填后开弹窗（弹窗会卸载点击路径上的节点：推迟出指针事件路径）。
    auto showEdit = [dialog, tasks, toast, name, description, body, store] {
        try {
            body = huxerui::TextEditingValue{store.Get().readBody(name)};
        } catch (const std::exception& e) {
            toast.Show(e.what());
            return;
        }
        for (const auto& s : store.Get().skills()) {
            if (s.name == name) {
                description = huxerui::TextEditingValue{s.description};
                break;
            }
        }
        tasks.Launch([=]() -> huxerui::Task<void> {
            // 弹窗会卸载点击路径上的节点：经事件队列推迟，不走帧调度。
            dialog.Show(
                [=](huxerui::DialogContext ctx) -> huxerui::View {
                    return SkillEditContent(name, description, body, ctx, toast,
                                            store);
                },
                huxerui::DialogOptions{});
            co_return;
        });
    };

    auto showDeleteConfirm = [dialog, toast, name, store, reload] {
        dialog.Show(
            "删除 Skill",
            std::format("确定删除「{}」？将从中央库及各工具的同步链接中移除。", name),
            "删除", "取消",
            [toast, name, store, reload] {
                try {
                    skills::SkillsStore::load().remove(name);
                    toast.Show(std::format("已删除 {}", name));
                } catch (const std::exception& e) {
                    toast.Show(e.what());
                }
                reload();
            },
            {});
    };

    return Card(huxerui::Column {
        huxerui::Row {
            huxerui::Text(name).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody)
                    .WithWeight(huxerui::FontWeight::SemiBold),
                theme.colors.on_surface}),
            SkillBadges(skill),
            huxerui::Spacer(),
        }.With(huxerui::Spacing(6.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Text(skill.description.empty() ? "无描述" : skill.description)
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
                                      theme.colors.on_surface_variant}),
        SkillAgentIcons(skill),
        huxerui::Row {
            huxerui::Button("编辑")
                .OnClick([showEdit] { showEdit(); })
                .With(huxerui::Enabled(skill.inStore),
                      huxerui::Tooltip(skill.inStore ? "编辑描述与正文"
                                                     : "仅中央库中的 Skill 可编辑")),
            huxerui::Button("删除").OnClick([tasks, showDeleteConfirm] {
                // 弹窗会卸载点击路径上的节点：经事件队列推迟，不走帧调度。
                tasks.Launch([=]() -> huxerui::Task<void> {
                    showDeleteConfirm();
                    co_return;
                });
            }),
            // 操作组靠卡片右缘；卡片内容按 Stretch 排布，主轴靠 MainAlign 收尾。
        }.With(huxerui::Spacing(8.0F),
               huxerui::MainAlign(huxerui::MainAxisAlignment::End)),
    }.With(huxerui::Spacing(6.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
        .Key(name);
}

// 「收编工具已有 Skill」弹窗内容：列出只装在某工具目录、未入中央库的项，
// 点选 importFromTool 复制进中央库。
[[huxerui::composable]] huxerui::View SkillImportContent(
    huxerui::DialogContext ctx, huxerui::ToastHandle toast,
    huxerui::State<skills::SkillsStore> store) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    struct ImportItem {
        std::string name;
        std::string tool;
    };
    std::vector<ImportItem> items;
    for (const auto& s : store.Get().skills()) {
        for (const auto& tool : s.installedOnlyTools) {
            items.push_back(ImportItem{s.name, std::string(tool)});
        }
    }
    const huxerui::Color itemTextColor = theme.colors.on_surface;
    const huxerui::Color itemHintColor = theme.colors.on_surface_variant;
    const huxerui::View itemList =
        huxerui::VirtualList(
            items,
            [ctx, toast, store, itemTextColor,
             itemHintColor](const ImportItem& item) {
                return huxerui::Column {
                    huxerui::Text(item.name).Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kBody)
                            .WithWeight(huxerui::FontWeight::SemiBold),
                        itemTextColor}),
                    huxerui::Text("来自 " + std::string(ToolName(item.tool)))
                        .Style(huxerui::TextStyle{
                            huxerui::Font::System(font_size::kCaption),
                            itemHintColor}),
                }.With(huxerui::Spacing(2.0F),
                       huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 8.0F)),
                       huxerui::CornerRadius(8.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
                    .OnClick([ctx, toast, store, name = item.name,
                              tool = item.tool] {
                        try {
                            skills::SkillsStore::load().importFromTool(tool, name);
                            toast.Show("已收编 " + name);
                        } catch (const std::exception& e) {
                            toast.Show(e.what());
                        }
                        store = skills::SkillsStore::load();
                        ctx.Dismiss();
                    })
                    .Key(item.name + "/" + item.tool);
            })
            .EstimatedItemExtent(58.0F)
            .CacheExtent(160.0F)
            .With(huxerui::Spacing(4.0F));
    const huxerui::View itemsView =
        items.empty()
            ? huxerui::View{huxerui::Text("没有可收编的 Skill。").Style(
                  huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                     theme.colors.on_surface_variant})}
            : itemList;
    return DialogCard(huxerui::Column {
        huxerui::Text("收编工具已有 Skill", huxerui::TextRole::Title),
        huxerui::Text("把工具目录里已安装的 Skill 复制进中央库，之后可统一同步。")
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                theme.colors.on_surface_variant}),
        huxerui::View{itemsView}.With(huxerui::Frame{.height = 320.0F}),
        huxerui::Row {
            huxerui::Spacer(),
            huxerui::Button("取消").OnClick([ctx] { ctx.Dismiss(); }),
        },
    }.With(huxerui::Spacing(12.0F),
           huxerui::Frame{.width = 380.0F},
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

} // namespace

[[huxerui::composable]] huxerui::View SkillsPage() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto dialog = huxerui::UseDialog();
    // 页面级持有 SkillsStore：首组合加载，每次操作后重新 load 刷新。
    auto store = huxerui::UseState<skills::SkillsStore>({});
    auto loaded = huxerui::UseState(false);
    if (!loaded.Get()) {
        loaded = true;
        store = skills::SkillsStore::load();
    }
    // 新建表单状态归页面作用域。
    auto newName = huxerui::UseState(huxerui::TextEditingValue{""});
    auto newDesc = huxerui::UseState(huxerui::TextEditingValue{""});
    auto newBody = huxerui::UseState(huxerui::TextEditingValue{""});

    auto showCreateDialog = [=] {
        newName = huxerui::TextEditingValue{""};
        newDesc = huxerui::TextEditingValue{""};
        newBody = huxerui::TextEditingValue{""};
        tasks.Launch([=]() -> huxerui::Task<void> {
            // 弹窗会卸载点击路径上的节点：经事件队列推迟，不走帧调度。
            dialog.Show(
                [=](huxerui::DialogContext ctx) -> huxerui::View {
                    return SkillCreateContent(newName, newDesc, newBody, ctx, toast,
                                              store);
                },
                huxerui::DialogOptions{});
            co_return;
        });
    };

    auto showImportDialog = [=] {
        tasks.Launch([=]() -> huxerui::Task<void> {
            dialog.Show(
                [=](huxerui::DialogContext ctx) -> huxerui::View {
                    return SkillImportContent(ctx, toast, store);
                },
                huxerui::DialogOptions{});
            co_return;
        });
    };

    // Agent 过滤：0 = 全部（通用图标），其余按 kSkillTools 顺序对应各同步
    // 目标。只显示 agent 图标（与 Agent 管理页同一注册表），文字仅作无障碍
    // 语义标签。
    auto agentFilter = huxerui::UseState<std::size_t>(0);
    std::vector<huxerui::SegmentedButtonItem> filterItems;
    filterItems.push_back(
        huxerui::SegmentedButtonItem::IconOnly(ToolIcon("agents"), "全部"));
    for (const std::string_view toolId : kSkillTools) {
        const auto* spec = models::findTool(toolId);
        filterItems.push_back(huxerui::SegmentedButtonItem::IconOnly(
            ToolIcon(spec != nullptr ? spec->iconName : ""),
            std::string(ToolName(toolId))));
    }

    const auto& skillItems = store.Get().skills();
    const std::size_t skillCount = skillItems.size();
    const std::size_t filterIndex =
        std::min(agentFilter.Get(), std::size(kSkillTools));
    std::vector<skills::SkillInfo> filteredItems;
    if (filterIndex == 0) {
        filteredItems = skillItems;
    } else {
        const std::string_view toolId = kSkillTools[filterIndex - 1];
        for (const auto& skill : skillItems) {
            if (InvolvesTool(skill, toolId)) filteredItems.push_back(skill);
        }
    }

    const huxerui::View filterRow = huxerui::Row {
        huxerui::Text("Agent").Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kCaption),
            theme.colors.on_surface_variant}),
        huxerui::SegmentedButton(std::move(filterItems), agentFilter)
            .OnChanged([agentFilter](std::size_t index) { agentFilter = index; }),
        huxerui::Spacer(),
    }.With(huxerui::Spacing(10.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));

    const huxerui::View skillList =
        huxerui::VirtualList(
            filteredItems,
            [tasks, toast, store](const skills::SkillInfo& skill) {
                return SkillCard(skill, tasks, toast, store);
            })
            .EstimatedItemExtent(180.0F)
            .CacheExtent(480.0F)
            .With(huxerui::Spacing(10.0F), huxerui::Grow(1.0F));

    const huxerui::View filteredEmpty =
        huxerui::View{huxerui::Column {
            huxerui::Text("该 Agent 下还没有 Skill。同步开关打开或收编后会出现在这里。")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kBody),
                    theme.colors.on_surface_variant}),
        }.With(huxerui::Padding(32.0F),
               huxerui::Grow(1.0F),
               huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))};

    return PageScaffold(
        "Skills",
        huxerui::Row {
            huxerui::Button("收编工具已有 Skill").OnClick([showImportDialog] {
                showImportDialog();
            }),
            huxerui::Button("新建 Skill").OnClick([showCreateDialog] {
                showCreateDialog();
            }),
        }.With(huxerui::Spacing(8.0F)),
        huxerui::Column {
            filterRow,
            skillCount == 0
                ? huxerui::View{
                      huxerui::Column {
                          huxerui::Text("还没有 Skill。点击右上角「新建 Skill」创建，"
                                        "或「收编工具已有 Skill」把工具里已装的收进中央库。")
                              .Style(huxerui::TextStyle{
                                  huxerui::Font::System(font_size::kBody),
                                  theme.colors.on_surface_variant}),
                      }.With(huxerui::Padding(32.0F),
                             huxerui::Grow(1.0F),
                             huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                             huxerui::CrossAlign(
                                 huxerui::CrossAxisAlignment::Center))}
                : (filteredItems.empty() ? filteredEmpty : skillList),
        }.With(huxerui::Spacing(10.0F),
               huxerui::Grow(1.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

} // namespace llmswitch::ui
