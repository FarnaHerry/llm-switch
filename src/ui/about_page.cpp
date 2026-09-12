// about_page.cpp — 关于页：应用名 + 版本（LLMSWITCH_VERSION 编译期常量）+ 一句话
// 简介、链接列表、技术栈卡、许可与致谢卡。SDK 无打开浏览器 API（platform 层只有
// 文本剪贴板接口，未暴露给 app），链接以等宽纯文本展示。
#include <huxerui/huxerui.h>

#include <string>
#include <vector>

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

// 图标库展示：58 枚 24×24 水墨图标（#FFFFFF alpha-mask，经 Tint 走主题
// 墨色，悬停 Tooltip 显示语义名）。Flow 自动换行。
[[huxerui::composable]] huxerui::View IconGallery() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    struct Entry {
        huxerui::ImageResource icon;
        const char* name;
    };
    const std::array<Entry, 58> kIcons{{
        {app::images::home, "首页"},       {app::images::agents, "Agent"},
        {app::images::providers, "供应商"}, {app::images::models, "模型"},
        {app::images::router, "路由"},     {app::images::skills, "Skills"},
        {app::images::mcp, "MCP"},         {app::images::sessions, "会话"},
        {app::images::stats, "用量"},      {app::images::settings, "设置"},
        {app::images::back, "返回"},       {app::images::forward, "前进"},
        {app::images::trash, "删除"},      {app::images::download, "下载"},
        {app::images::upload, "上传"},     {app::images::search, "搜索"},
        {app::images::refresh, "刷新"},    {app::images::add, "添加"},
        {app::images::edit, "编辑"},       {app::images::import, "导入"},
        {app::images::resource_export, "导出"},
        {app::images::backup, "备份"},
        {app::images::restore, "恢复"},    {app::images::file, "文件"},
        {app::images::folder, "文件夹"},   {app::images::image, "图片"},
        {app::images::video, "视频"},      {app::images::audio, "音频"},
        {app::images::link, "链接"},       {app::images::copy, "复制"},
        {app::images::user, "用户"},       {app::images::api_key, "API Key"},
        {app::images::group, "群组"},       {app::images::message, "消息"},
        {app::images::bell, "通知"},       {app::images::star, "收藏"},
        {app::images::heart, "喜欢"},      {app::images::more, "更多"},
        {app::images::success, "成功"},    {app::images::warning, "警告"},
        {app::images::error, "错误"},      {app::images::info, "信息"},
        {app::images::loading, "加载"},    {app::images::disabled, "禁用"},
        {app::images::processing, "进行中"}, {app::images::help, "帮助"},
        {app::images::lock, "锁定"},       {app::images::unlock, "解锁"},
        {app::images::options, "选项"},    {app::images::logout, "退出"},
        {app::images::calendar, "日历"},   {app::images::clock, "时间"},
        {app::images::location, "定位"},   {app::images::filter, "筛选"},
        {app::images::sort, "排序"},       {app::images::eye, "显示"},
        {app::images::eye_off, "隐藏"},    {app::images::menu, "菜单"},
    }};
    std::vector<huxerui::View> tiles;
    tiles.reserve(kIcons.size());
    for (const auto& entry : kIcons) {
        tiles.push_back(
            huxerui::Image(entry.icon)
                .Tint(theme.colors.on_surface_variant)
                .With(huxerui::Frame{.width = 22.0F, .height = 22.0F},
                      huxerui::Padding(6.0F),
                      huxerui::Tooltip(entry.name)));
    }
    return huxerui::Flow(std::move(tiles))
        .With(huxerui::Spacing(4.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

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
                             "curl · nlohmann::json"),
                }.With(huxerui::Spacing(8.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),

                Card(huxerui::Column {
                    AboutSectionTitle("图标库"),
                    IconGallery(),
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
