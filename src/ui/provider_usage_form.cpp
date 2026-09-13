// provider_usage_form.cpp — 用量查询配置页（UsageFormPage）：从供应商卡片的
// gauge 图标进入，formTarget = "usage:" + id。字段：启用开关 / 用量 URL /
// 取值路径 / 单位标签 / 刷新间隔；「自动填充」按 baseUrl 匹配
// models::suggestUsageQuery 的内置端点模板。保存 = updateProvider 只改用量字段。
#include <huxerui/huxerui.h>

#include <string>
#include <vector>

#include "providers_internal.h"

import llmswitch.models;
import llmswitch.store;

namespace llmswitch::ui {

// 用量查询刷新间隔选项（0 = 仅手动）；刷新周期属于 Provider，不再是全局设置。
// 表单页与用量页共用，声明在 providers_internal.h。
const std::vector<huxerui::StringVariant> kUsageIntervals{
    "1 分钟", "5 分钟", "10 分钟", "30 分钟", "仅手动"};

const std::vector<int> kUsageMinutes{1, 5, 10, 30, 0};

int UsageIntervalIndex(int minutes) {
    for (std::size_t i = 0; i < kUsageMinutes.size(); ++i) {
        if (kUsageMinutes[i] == minutes) return static_cast<int>(i);
    }
    return 2;  // 未知值按默认「10 分钟」显示
}

// 用量查询配置页（整页，从卡片的 gauge 图标进入，formTarget = "usage:" + id）。
// 字段：启用开关 / 用量 URL / 取值路径 / 单位标签；「自动填充」按供应商
// baseUrl 匹配 models::suggestUsageQuery 的内置端点模板。关闭开关会保留已填
// 配置，重新打开即可恢复查询。保存 = updateProvider 只改用量字段。
[[huxerui::composable]] huxerui::View UsageFormPage(
    std::string tool, models::Provider initial, huxerui::State<int> revision,
    huxerui::State<std::string> formTarget, huxerui::TaskScope closeTasks) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto usageUrl = huxerui::UseState(huxerui::TextEditingValue{initial.usageUrl});
    auto usagePath =
        huxerui::UseState(huxerui::TextEditingValue{initial.usagePath});
    auto usageLabel =
        huxerui::UseState(huxerui::TextEditingValue{initial.usageLabel});
    auto usageEnabled = huxerui::UseState(initial.usageEnabled);
    auto usageInterval =
        huxerui::UseState(UsageIntervalIndex(initial.usageRefreshMinutes));

    // 返回列表（写 formTarget 会卸载本页与点击路径上的节点：推迟出指针
    // 事件路径；任务挂父级 closeTasks——本页 scope 随写入一起销毁）。
    auto goBack = [closeTasks, formTarget] {
        closeTasks.Launch([formTarget]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            formTarget = "";
        });
    };

    std::vector<huxerui::View> fields;
    fields.push_back(huxerui::Row {
        huxerui::Text("启用用量查询")
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                      theme.colors.on_surface}),
        huxerui::Spacer(),
        huxerui::Switch(usageEnabled.Get())
            .OnChanged([usageEnabled](bool enabled) {
                usageEnabled = enabled;
            }),
    }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
    fields.push_back(huxerui::Row {
        huxerui::Text("自动刷新间隔")
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                      theme.colors.on_surface}),
        huxerui::Spacer(),
        huxerui::SegmentedButton(
            kUsageIntervals, static_cast<std::size_t>(usageInterval.Get()))
            .OnChanged([usageInterval](std::size_t index) {
                usageInterval = static_cast<int>(index);
            }),
    }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
    fields.push_back(huxerui::Row {
        huxerui::TextField(usageUrl.Get())
            .Label("用量 URL")
            .Placeholder("https://...")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([usageUrl](const huxerui::TextEditingValue& v) {
                usageUrl = v;
            })
            .With(huxerui::Grow(1.0F)),
        huxerui::Button("自动填充")
            .OnClick([usageUrl, usagePath, toast, initial] {
                const auto suggested =
                    models::suggestUsageQuery(initial.baseUrl);
                if (!suggested) {
                    toast.Show("该供应商暂无内置用量端点模板，请手动填写");
                    return;
                }
                usageUrl = huxerui::TextEditingValue{suggested->first};
                usagePath = huxerui::TextEditingValue{suggested->second};
            })
            .With(huxerui::Tooltip("按 Base URL 匹配内置用量端点模板")),
    }.With(huxerui::Spacing(8.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
    fields.push_back(huxerui::TextField(usagePath.Get())
        .Label("取值路径")
        .Placeholder("balance_infos.0.total_balance")
        .Variant(huxerui::TextFieldVariant::Outlined)
        .OnChanged([usagePath](const huxerui::TextEditingValue& v) {
            usagePath = v;
        }));
    fields.push_back(huxerui::TextField(usageLabel.Get())
        .Label("单位标签")
        .Placeholder("如 CNY")
        .Variant(huxerui::TextFieldVariant::Outlined)
        .OnChanged([usageLabel](const huxerui::TextEditingValue& v) {
            usageLabel = v;
        }));
    fields.push_back(huxerui::Text(
        "开关、刷新间隔和查询端点均按供应商单独保存；查询带供应商 API Key 做 Bearer 鉴权。")
        .Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kCaption),
            theme.colors.on_surface_variant}));

    return PageScaffold(
        "用量查询 — " + initial.name,
        huxerui::Row {
            huxerui::Button("返回").OnClick([goBack] { goBack(); }),
        },
        huxerui::Column {
            huxerui::ScrollView(
                huxerui::Column(std::move(fields))
                    .With(huxerui::Spacing(12.0F),
                          huxerui::CrossAlign(
                              huxerui::CrossAxisAlignment::Stretch)))
                .With(huxerui::Grow(1.0F)),
            huxerui::Row {
                huxerui::Button("取消").OnClick([goBack] { goBack(); }),
                huxerui::Button("保存")
                    .OnClick([=] {
                        const std::string url = usageUrl.Get().text;
                        const std::string path = usagePath.Get().text;
                        if (usageEnabled.Get() && !url.empty() && path.empty()) {
                            toast.Show("启用查询时取值路径不能为空");
                            return;
                        }
                        models::Provider p = initial;
                        p.usageEnabled = usageEnabled.Get();
                        p.usageRefreshMinutes =
                            kUsageMinutes[static_cast<std::size_t>(
                                usageInterval.Get())];
                        p.usageUrl = url;
                        p.usagePath = path;
                        p.usageLabel = usageLabel.Get().text;
                        try {
                            providerStore().updateProvider(tool, p);
                        } catch (const std::exception& e) {
                            toast.Show(e.what());
                            return;
                        }
                        revision = revision.Get() + 1;
                        toast.Show("用量查询配置已保存");
                        goBack();
                    }),
            }.With(huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween)),
        }.With(huxerui::Spacing(12.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

} // namespace llmswitch::ui
