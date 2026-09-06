// settings_page.cpp — 设置页：外观主题（跟随系统/玄墨/宣纸，存 AppConfig.themeMode
// 的 system/dark/light 并即时生效）、用量查询（总开关 usageEnabled + 刷新间隔
// usageRefreshMinutes，变更即落盘）、live 配置文件路径展示、导入/导出、关于。
//
// 导入/导出优先走 FilePicker 系统文件对话框（SaveFileAsync/OpenFileAsync）；
// 平台不可用（CanSaveFiles/CanOpenFiles 为 false，如无 xdg-desktop-portal）时
// 回落：导出到 dataDir()/exports/ 并展示路径，导入提供路径输入框。
#include <huxerui/huxerui.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "ui.h"

import llmswitch.config;
import llmswitch.models;
import llmswitch.store;

namespace llmswitch::ui {
namespace {

// 主题显示名：水墨风命名（玄墨=深色、宣纸=浅色），存值仍是 system/dark/light。
const std::vector<huxerui::StringVariant> kThemeNames{"跟随系统", "玄墨（深色）",
                                                      "宣纸（浅色）"};
const std::vector<std::string> kThemeModes{"system", "dark", "light"};

// 用量查询刷新间隔选项（下标 ↔ AppConfig.usageRefreshMinutes 分钟数，0=仅手动）。
const std::vector<huxerui::StringVariant> kUsageIntervals{"5 分钟", "10 分钟",
                                                          "30 分钟", "仅手动"};
const std::vector<int> kUsageMinutes{5, 10, 30, 0};

int UsageIntervalIndex(int minutes) {
    for (std::size_t i = 0; i < kUsageMinutes.size(); ++i) {
        if (kUsageMinutes[i] == minutes) return static_cast<int>(i);
    }
    return 3;  // 未知值按「仅手动」显示
}

// 版本号编译期常量由顶层 CMake 注入（hcg 不支持 composable 内条件编译，
// 字符串在文件作用域先拼好）。
const std::string kAboutText =
    std::format("llm-switch v{} · Claude Code / Codex 供应商切换", LLMSWITCH_VERSION);

[[huxerui::composable]] huxerui::View SettingRow(const std::string& label,
                                                 const std::string& hint,
                                                 huxerui::View control) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Row {
        huxerui::Column {
            huxerui::Text(label).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody), theme.colors.on_surface}),
            hint.empty()
                ? huxerui::View{huxerui::Row{}}
                : huxerui::View{huxerui::Text(hint).Style(huxerui::TextStyle{
                      huxerui::Font::System(font_size::kCaption),
                      theme.colors.on_surface_variant})},
        }.With(huxerui::Spacing(2.0F)),
        huxerui::Spacer(),
        std::move(control),
    }.With(huxerui::Spacing(12.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

[[huxerui::composable]] huxerui::View SectionTitle(const std::string& title) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Text(title).Style(huxerui::TextStyle{
        huxerui::Font::System(font_size::kChip).WithWeight(huxerui::FontWeight::Bold),
        theme.colors.primary});
}

// 配置文件路径行（只读展示解析结果，等宽小字）。
[[huxerui::composable]] huxerui::View PathRow(const std::string& label,
                                              const std::string& path) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Column {
        huxerui::Text(label).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kChip), theme.colors.on_surface}),
        huxerui::Text(path).Style(huxerui::TextStyle{
            huxerui::Font::Monospace(font_size::kCaption),
            theme.colors.on_surface_variant}),
    }.With(huxerui::Spacing(2.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace

[[huxerui::composable]] huxerui::View SettingsPage(huxerui::State<int> themeMode,
                                                   huxerui::State<int> revision) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    const auto picker = huxerui::UseService<huxerui::FilePicker>();
    auto lastExport = huxerui::UseState<std::string>({});
    auto importPath = huxerui::UseState(huxerui::TextEditingValue{""});
    // 用量查询设置（Switch/SegmentedButton 是受控组件，本地 State 为权威值，
    // 变更即落盘）。
    auto usageEnabled =
        huxerui::UseState(providerStore().config().usageEnabled);
    auto usageInterval = huxerui::UseState(
        UsageIntervalIndex(providerStore().config().usageRefreshMinutes));

    const bool canSave = picker && picker->CanSaveFiles();
    const bool canOpen = picker && picker->CanOpenFiles();

    // 导出：有系统对话框走 SaveFileAsync（先落临时文件做源），否则固定导出到
    // dataDir()/exports/ 并在页面上展示路径。
    auto doExport = [=] {
        tasks.Launch([=]() -> huxerui::Task<void> {
            if (canSave) {
                const auto tmp = std::filesystem::temp_directory_path() /
                                 "llm-switch-export.json";
                try {
                    providerStore().exportTo(tmp);
                } catch (const std::exception& e) {
                    toast.Show(e.what());
                    co_return;
                }
                const bool ok = co_await picker->SaveFileAsync(
                    huxerui::File(tmp.string()),
                    huxerui::SaveFileOptions{
                        .suggested_name = "llm-switch-config.json",
                        .filter = {.name = "JSON", .extensions = {"json"}}});
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                if (ok) toast.Show("配置已导出");
            } else {
                const auto dest = cfg::dataDir() / "exports" /
                                  "llm-switch-config.json";
                try {
                    providerStore().exportTo(dest);
                    lastExport = dest.string();
                    toast.Show("已导出到 " + dest.string());
                } catch (const std::exception& e) {
                    toast.Show(e.what());
                }
            }
        });
    };

    // 导入：有系统对话框走 OpenFileAsync → 落到临时文件 → importFrom；
    // 否则用手动输入的路径。成功/失败都 toast；成功后 revision+1 刷新全界面。
    auto doImportPicked = [=] {
        tasks.Launch([=]() -> huxerui::Task<void> {
            const auto ref = co_await picker->OpenFileAsync(
                huxerui::FilePickerFilter{.name = "JSON", .extensions = {"json"}});
            if (!ref) co_return;
            const auto tmp = std::filesystem::temp_directory_path() /
                             "llm-switch-import.json";
            if (!co_await ref->ImportToAsync(huxerui::File(tmp.string()),
                                             true)) {
                toast.Show("无法读取所选文件");
                co_return;
            }
            try {
                providerStore().importFrom(tmp);
                toast.Show("导入完成");
                revision = revision.Get() + 1;
            } catch (const std::exception& e) {
                toast.Show(e.what());
            }
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
        });
    };
    auto doImportPath = [=] {
        const std::string path = importPath.Get().text;
        if (path.empty()) {
            toast.Show("请输入配置文件路径");
            return;
        }
        try {
            providerStore().importFrom(path);
            toast.Show("导入完成");
            revision = revision.Get() + 1;
        } catch (const std::exception& e) {
            toast.Show(e.what());
        }
    };

    return PageScaffold(
        "设置",
        huxerui::Row{},
        huxerui::ScrollView(
            huxerui::Column {
                Card(huxerui::Column {
                    SectionTitle("外观"),
                    SettingRow(
                        "主题", "",
                        huxerui::SegmentedButton(
                            kThemeNames,
                            static_cast<std::size_t>(themeMode.Get()))
                            .OnChanged([themeMode](std::size_t idx) {
                                themeMode = static_cast<int>(idx);
                                providerStore().setThemeMode(kThemeModes[idx]);
                            })),
                }.With(huxerui::Spacing(10.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),

                Card(huxerui::Column {
                    SectionTitle("用量查询"),
                    SettingRow(
                        "启用用量查询",
                        "为配置了用量 URL 的供应商在卡片上显示余额/用量",
                        huxerui::Switch(usageEnabled.Get())
                            .OnChanged([usageEnabled](bool on) {
                                usageEnabled = on;
                                providerStore().setUsageEnabled(on);
                            })),
                    SettingRow(
                        "自动刷新间隔",
                        "「仅手动」时只在供应商卡片上点「刷新」才查询",
                        huxerui::SegmentedButton(
                            kUsageIntervals,
                            static_cast<std::size_t>(usageInterval.Get()))
                            .OnChanged([usageInterval](std::size_t idx) {
                                usageInterval = static_cast<int>(idx);
                                providerStore().setUsageRefreshMinutes(
                                    kUsageMinutes[idx]);
                            })),
                }.With(huxerui::Spacing(10.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),

                Card(huxerui::Column {
                    SectionTitle("配置文件"),
                    PathRow("Claude Code 设置（live）",
                            cfg::claudeSettingsFile().string()),
                    PathRow("Codex 凭据（live）", cfg::codexAuthFile().string()),
                    PathRow("Codex 主配置（live）", cfg::codexConfigFile().string()),
                    PathRow("llm-switch 配置库", cfg::configFile().string()),
                    PathRow("备份目录", cfg::backupsDir().string()),
                }.With(huxerui::Spacing(8.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),

                Card(huxerui::Column {
                    SectionTitle("导入 / 导出"),
                    huxerui::Text("导出整个配置库为 JSON；导入按供应商 id 合并"
                                  "（导入前自动备份当前配置到 backups/）。")
                        .Style(huxerui::TextStyle{
                            huxerui::Font::System(font_size::kCaption),
                            theme.colors.on_surface_variant}),
                    huxerui::Row {
                        huxerui::Button("导出配置").OnClick([doExport] {
                            doExport();
                        }),
                        canOpen
                            ? huxerui::View{huxerui::Button("导入配置")
                                                .OnClick([doImportPicked] {
                                                    doImportPicked();
                                                })}
                            : huxerui::View{huxerui::Row{}},
                    }.With(huxerui::Spacing(8.0F)),
                    // 无文件对话框平台：手动路径导入。
                    canOpen
                        ? huxerui::View{huxerui::Row{}}
                        : huxerui::View{
                              huxerui::Row {
                                  huxerui::TextField(importPath.Get())
                                      .Placeholder("配置文件路径")
                                      .Variant(huxerui::TextFieldVariant::Outlined)
                                      .OnChanged(
                                          [importPath](
                                              const huxerui::TextEditingValue& v) {
                                              importPath = v;
                                          })
                                      .With(huxerui::Grow(1.0F)),
                                  huxerui::Button("导入").OnClick([doImportPath] {
                                      doImportPath();
                                  }),
                              }.With(huxerui::Spacing(8.0F))},
                    lastExport.Get().empty()
                        ? huxerui::View{huxerui::Row{}}
                        : huxerui::View{huxerui::Text("上次导出：" +
                                                      lastExport.Get())
                                            .Style(huxerui::TextStyle{
                                                huxerui::Font::Monospace(
                                                    font_size::kCaption),
                                                theme.colors.on_surface_variant})},
                }.With(huxerui::Spacing(8.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),

                Card(huxerui::Column {
                    SectionTitle("关于"),
                    huxerui::Text(kAboutText)
                        .Style(huxerui::TextStyle{
                            huxerui::Font::System(font_size::kChip),
                            theme.colors.on_surface_variant}),
                }.With(huxerui::Spacing(6.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),
            }.With(huxerui::Spacing(12.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
            .With(huxerui::Grow(1.0F)));
}

} // namespace llmswitch::ui
