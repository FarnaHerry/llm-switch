// about_page.cpp — 关于页：应用名 + 版本（LLMSWITCH_VERSION 编译期常量）+ 一句话
// 简介、链接列表、技术栈卡、许可与致谢卡。SDK 无打开浏览器 API（platform 层只有
// 文本剪贴板接口，未暴露给 app），链接以等宽纯文本展示。
#include <huxerui/huxerui.h>

#include <string>

#include "ui.h"
#include "app_resources.h"

namespace llmswitch::ui {
namespace {

// 版本号编译期常量由顶层 CMake 注入（与 settings_page.cpp 同一约定）。
const std::string kVersionLine = std::format("llm-switch v{}", LLMSWITCH_VERSION);

[[huxerui::composable]] huxerui::View AboutSectionTitle(const std::string& title) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Text(title).Style(huxerui::TextStyle{
        huxerui::Font::System(font_size::kChip).WithWeight(huxerui::FontWeight::Bold),
        theme.colors.primary});
}

// 链接行：标签 + 等宽地址（纯文本展示，见文件头注释）。
[[huxerui::composable]] huxerui::View LinkRow(const std::string& label,
                                              const std::string& url) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Column {
        huxerui::Text(label).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kChip), theme.colors.on_surface}),
        huxerui::Text(url).Style(huxerui::TextStyle{
            huxerui::Font::Monospace(font_size::kCaption),
            theme.colors.on_surface_variant}),
    }.With(huxerui::Spacing(2.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View BodyLine(const std::string& text) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Text(text).Style(huxerui::TextStyle{
        huxerui::Font::System(font_size::kChip), theme.colors.on_surface_variant});
}

} // namespace

[[huxerui::composable]] huxerui::View AboutPage() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return PageScaffold(
        "关于",
        huxerui::Row{},
        huxerui::ScrollView(
            huxerui::Column {
                Card(huxerui::Row {
                    huxerui::Image(app::images::taiji)
                        .With(huxerui::Frame{.width = 44.0F, .height = 44.0F}),
                    huxerui::Column {
                        huxerui::Text(kVersionLine).Style(huxerui::TextStyle{
                            huxerui::Font::System(font_size::kTitle)
                                .WithWeight(huxerui::FontWeight::Bold),
                            theme.colors.on_surface}),
                        BodyLine("AI 编程工具（Claude Code / Codex / opencode / pi 等）"
                                 "的供应商切换器：一个配置库，多工具一键切换，"
                                 "附本地路由、用量查询与请求统计。"),
                    }.With(huxerui::Spacing(8.0F),
                           huxerui::Grow(1.0F),
                           huxerui::CrossAlign(
                               huxerui::CrossAxisAlignment::Stretch)),
                }.With(huxerui::Spacing(14.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))),

                Card(huxerui::Column {
                    AboutSectionTitle("链接"),
                    LinkRow("GitHub 仓库",
                            "https://github.com/FarnaHerry/llm-switch"),
                    LinkRow("上游参考 cc-switch",
                            "https://github.com/farion1231/cc-switch"),
                    LinkRow("HuxerUI", "https://huxerui.dev"),
                }.With(huxerui::Spacing(8.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),

                Card(huxerui::Column {
                    AboutSectionTitle("技术栈"),
                    BodyLine("C++23 modules · HuxerUI 0.3.0 · cpp-httplib · "
                             "nlohmann::json"),
                }.With(huxerui::Spacing(8.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),

                Card(huxerui::Column {
                    AboutSectionTitle("许可与致谢"),
                    BodyLine("nlohmann::json — MIT License"),
                    BodyLine("cpp-httplib — MIT License"),
                    BodyLine("simple-icons（Claude / Codex / opencode 图标）— CC0 1.0"),
                    BodyLine("pi.dev logo（pi-mono）— MIT License"),
                }.With(huxerui::Spacing(6.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),
            }.With(huxerui::Spacing(12.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
            .With(huxerui::Grow(1.0F)));
}

} // namespace llmswitch::ui
