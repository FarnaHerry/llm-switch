// ui.h — llm-switch HuxerUI 前端内部声明（UI 层是普通 C++ 源，经 huxerui_add_app
// codegen；composable 定义只在 .cpp，见 .claude/skills/huxerui-app-development）。
#pragma once

#include <huxerui/huxerui.h>

#include <string>
#include <string_view>

import llmswitch.models;
import llmswitch.router;  // 任务E：routerInstance() 声明需要 router::LocalRouter
import llmswitch.store;

namespace llmswitch::ui {

// 全项目统一字号阶梯（pt）：控件/正文跟随 SDK 默认 14，不再散落硬编码字面量。
namespace font_size {
inline constexpr float kCaption = 11.0F;  // 徽标、状态小字
inline constexpr float kChip = 12.0F;     // 紧凑部件文字
inline constexpr float kBody = 14.0F;     // 正文/按钮/输入框（SDK 默认）
inline constexpr float kTitle = 20.0F;    // 页面/弹窗标题
} // namespace font_size

// 标题栏内容统一高度（= AppOptions.window.title_bar_height）。
inline constexpr float kTitleBarContentHeight = 24.0F;

// ---- 状态管理 ----
// 全局唯一 ProviderStore（UI 线程独占：store 无内部锁，所有读写都发生在
// UI 线程的组合/回调里；live 文件读写是微秒级本地 IO，不需要任务线程）。
store::ProviderStore& providerStore();

// 工具显示名（侧边栏提示 / 托盘菜单分组标题 / 页面标题）。
std::string_view ToolName(std::string_view tool);

// ToolSpec.iconName → 单套无色 alpha-mask 图标；深浅主题由组件运行时 tint
// 自适应，选中态由承载容器表达。未知名回退 agents 图标。
huxerui::ImageResource ToolIcon(std::string_view iconName);

// ---- 岛屿结构（对齐 Clash-Flux island 模型）----
// 语义层级：页面通过层级选表面，不直接依赖 Material 的 surface_container_* 命名；
// 颜色仍由当前 ThemeSpec 派生，深浅主题共用组件。
enum class IslandLevel {
    Base,    // 一级岛（页面根）
    Raised,  // 二级岛（卡片/分组）
    Overlay, // 浮动面（弹层、徽章）
};

struct IslandTheme {
    float page_gap;        // 岛间缝隙（透出窗口底色「海面」）
    float island_padding;  // 一级岛内边距
    float island_radius;   // 一级轻岛圆角（当前 10pt）
    float nested_radius;   // 二级岛/浮动菜单圆角（当前 6pt）
    huxerui::Color ocean;   // 海面（窗口背景）
    huxerui::Color base;    // 一级岛表面
    huxerui::Color raised;  // 二级岛表面
    huxerui::Color overlay; // 浮动面
    huxerui::Color outline_soft;
};

IslandTheme ResolveIslandTheme(const huxerui::ThemeSpec& theme);

// 岛屿原语：Surface 负责语义表面/圆角/内边距；Section 在其上提供标题+内容排版。
huxerui::View IslandSurface(huxerui::View content, IslandLevel level = IslandLevel::Base);

// ---- 页面（定义在各自 .cpp，均为 [[huxerui::composable]]）----
// Agent 管理页：薄宿主，持有当前工具 State 并转交 ProvidersPage（工具图标栏
// 在供应商岛屿内部顶部）。
huxerui::View AgentPage(huxerui::State<int> revision);
// 供应商列表页：各工具组共用同一组件，currentTool 取 models::toolRegistry()
// 的注册表 id（claude-code / codex / ...），岛屿内部顶部渲染工具图标栏。
// revision 是全局变更计数（AppRoot 持有）：任何写库操作后 +1，驱动本页重读
// 与托盘菜单重建。
huxerui::View ProvidersPage(huxerui::State<std::string> currentTool,
                            huxerui::State<int> revision);
// 设置页持有主题模式 State（AppRoot 传入）。
huxerui::View SettingsPage(huxerui::State<int> themeMode, huxerui::State<int> revision);

// ---- 通用部件（common.cpp）----

// 页面骨架（一级岛）：标题行（标题 + 右缘动作）+ 内容区，整体为低对比轻岛，
// 落在窗口海面底色上（岛间缝隙经壳层 Spacing 透出）。
huxerui::View PageScaffold(const std::string& title, huxerui::View actions,
                           huxerui::View content);

// 卡片容器（二级岛）：raised 表面 + 断续墨线/飞白纹理 + 内边距。
huxerui::View Card(huxerui::View content);

// 自定义内容弹窗的卡片包裹：SDK 的 dialog.Show(ViewFactory/DialogFactory) 不给
// 内容加底板（只有标题+消息的内置形态才有 DialogStyle），统一包一层：
// overlay 表面 + 阴影 + 描边 + 16pt 圆角 + 内边距。
huxerui::View DialogCard(huxerui::View content);

// ---- 路由/统计页面（router_page.cpp / stats_page.cpp）----
// 本地路由页：运行状态/启用开关/端口/故障转移控制、各工具接入地址、最近请求日志。
huxerui::View RouterPage();
// 使用统计页：汇总指标、按供应商分布、刷新/清空统计。
huxerui::View StatsPage();

// 进程级 LocalRouter 单例（定义在 router_page.cpp）：resolver 回调读
// providerStore() 的组快照，首次访问按 config().routerFailover 设置故障转移。
// app.cpp 首组合时若 config().routerEnabled 且 !routerInstance().running()，
// 则 try { routerInstance().start(config().routerPort); } catch 后 toast 中文错误。
router::LocalRouter& routerInstance();

// ---- MCP/Skills/会话页面（mcp_page.cpp / skills_page.cpp / sessions_page.cpp）----
// MCP 服务器管理页（页面级持有 mcp::McpStore）。
huxerui::View McpPage();
// Skills 管理页（页面级持有 skills::SkillsStore）。
huxerui::View SkillsPage();
// 历史会话管理页（页面级持有会话列表 State）。
huxerui::View SessionsPage();

// ---- 关于页（about_page.cpp）----
// 应用名/版本/简介 + 链接 + 技术栈 + 许可致谢。
huxerui::View AboutPage();

} // namespace llmswitch::ui
