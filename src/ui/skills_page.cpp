// skills_page.cpp — Skills 管理页：中央库（dataDir()/skills-store/）+ 符号链接
// 同步状态的合并视图。每卡：名称 + 描述（空显示「无描述」）+ 状态徽章
// （中央库 / 已同步 claude-code / 已同步 codex / 仅某工具安装）+ 同步开关
// （claude-code / codex 两个 Switch → setLinked，建链失败的中文错直接 toast，
// Windows 上会带开发者模式/管理员提示）+ 编辑（描述 + 正文多行 → updateBody）
// + 删除（确认框）。顶部：新建 Skill + 收编工具已有 Skill。
//
// 数据流：SkillsStore 无长期持有价值（扫描即视图），页面用 UseState 持有一份，
// 每次操作后重新 load() 刷新（微秒级本地 IO，UI 线程直接跑）。
#include <huxerui/huxerui.h>

#include <chrono>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui.h"

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
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            dialog.Show(
                [=](huxerui::DialogContext ctx) -> huxerui::View {
                    return SkillEditContent(name, description, body, ctx, toast,
                                            store);
                },
                huxerui::DialogOptions{});
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

    // 同步开关：仅中央库中的 Skill 可同步；未收编的需先「收编工具已有 Skill」。
    std::vector<huxerui::View> toggles;
    for (const std::string_view toolId : kSkillTools) {
        toggles.push_back(
            huxerui::Switch(std::string(ToolName(toolId)),
                            IsLinked(skill, toolId))
                .OnChanged([toast, name, toolId = std::string(toolId), store,
                            reload](bool on) {
                    try {
                        skills::SkillsStore::load().setLinked(name, toolId, on);
                    } catch (const std::exception& e) {
                        toast.Show(e.what());
                    }
                    reload();
                })
                .With(huxerui::Enabled(skill.inStore),
                      huxerui::Tooltip(skill.inStore
                                           ? "同步到该工具的 skills 目录"
                                           : "仅中央库中的 Skill 可同步")));
    }

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
        huxerui::Row(std::move(toggles))
            .With(huxerui::Spacing(16.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Row {
            huxerui::Button("编辑")
                .OnClick([showEdit] { showEdit(); })
                .With(huxerui::Enabled(skill.inStore),
                      huxerui::Tooltip(skill.inStore ? "编辑描述与正文"
                                                     : "仅中央库中的 Skill 可编辑")),
            huxerui::Button("删除").OnClick([tasks, showDeleteConfirm] {
                // 弹窗会卸载点击路径上的节点：推迟出指针事件路径。
                tasks.Launch([=]() -> huxerui::Task<void> {
                    co_await huxerui::Delay(std::chrono::duration<double>{0});
                    showDeleteConfirm();
                });
            }),
        }.With(huxerui::Spacing(8.0F)),
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
    std::vector<huxerui::View> items;
    for (const auto& s : store.Get().skills()) {
        for (const auto& tool : s.installedOnlyTools) {
            items.push_back(
                huxerui::Column {
                    huxerui::Text(s.name).Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kBody)
                            .WithWeight(huxerui::FontWeight::SemiBold),
                        theme.colors.on_surface}),
                    huxerui::Text("来自 " + std::string(ToolName(tool)))
                        .Style(huxerui::TextStyle{
                            huxerui::Font::System(font_size::kCaption),
                            theme.colors.on_surface_variant}),
                }.With(huxerui::Spacing(2.0F),
                       huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 8.0F)),
                       huxerui::CornerRadius(8.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
                    .OnClick([ctx, toast, store, name = s.name, tool] {
                        try {
                            skills::SkillsStore::load().importFromTool(tool, name);
                            toast.Show("已收编 " + name);
                        } catch (const std::exception& e) {
                            toast.Show(e.what());
                        }
                        store = skills::SkillsStore::load();
                        ctx.Dismiss();
                    }));
        }
    }
    if (items.empty()) {
        items.push_back(huxerui::Text("没有可收编的 Skill。")
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                      theme.colors.on_surface_variant}));
    }
    return DialogCard(huxerui::Column {
        huxerui::Text("收编工具已有 Skill", huxerui::TextRole::Title),
        huxerui::Text("把工具目录里已安装的 Skill 复制进中央库，之后可统一同步。")
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                theme.colors.on_surface_variant}),
        huxerui::ScrollView(huxerui::Column(std::move(items))
            .With(huxerui::Spacing(4.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
            .With(huxerui::Frame{.height = 320.0F}),
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
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            dialog.Show(
                [=](huxerui::DialogContext ctx) -> huxerui::View {
                    return SkillCreateContent(newName, newDesc, newBody, ctx, toast,
                                              store);
                },
                huxerui::DialogOptions{});
        });
    };

    auto showImportDialog = [=] {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            dialog.Show(
                [=](huxerui::DialogContext ctx) -> huxerui::View {
                    return SkillImportContent(ctx, toast, store);
                },
                huxerui::DialogOptions{});
        });
    };

    std::vector<huxerui::View> cards;
    for (const auto& s : store.Get().skills()) {
        cards.push_back(SkillCard(s, tasks, toast, store));
    }

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
        cards.empty()
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
            : huxerui::View{huxerui::ScrollView(
                                huxerui::Column(std::move(cards))
                                    .With(huxerui::Spacing(10.0F),
                                          huxerui::CrossAlign(
                                              huxerui::CrossAxisAlignment::Stretch)))
                                .With(huxerui::Grow(1.0F))});
}

} // namespace llmswitch::ui
