# 视觉与资源契约

代码路径相对仓库根目录；主题唯一入口为 src/ui/theme_colors.h，主应用与 Windows
安装器共享。当前深色为“石墨”、浅色为“晴石”，不从历史截图抄旧主题常量。

## UI 视觉硬约束

- 总体风格是冷调石板的“岛屿风”：环境层只有一层主题派生的程序化光晕，内容
  仍以轻量、分层、可读的岛屿组织，不把环境装饰压到交互内容之上。
- **全部带标题栏的窗口一律自定义标题栏**（`WindowChromeMode::Custom` +
  `title_bar_height = 24` + `WindowTitleBar`：莲花图标 + 应用名 + 框架渲染的
  caption 控件，左右边距与正文同 `spacing.large`）：主窗口、桌宠信息卡、
  Windows 安装器向导都已如此；新增任何普通窗口不得出现平台原生标题栏。
  例外：桌宠球/胶囊是无边框透明饰件（Utility + transparent_surface，无标题
  栏），不属于此列。另注意框架的 `MenuHandle::ShowAt` 是**窗口内浮层**，
  会被窗口边界裁剪——挂在桌宠这类小窗口上的菜单必须精简到装得下（球形态
  菜单用紧凑行高 `minimum_item_height 30` + 行内边距 4 放 6 项无分隔短标签，
  默认行高只能放 5 项），胶囊态放不下菜单就改为打开真窗口（信息卡）承载操作；
  且 `UseMenu()` 与 `UseDialog()` 同款坑——必须在 Theme 的子作用域内获取
  （Scope 工厂在组合期执行），否则捕获默认环境、菜单落回框架默认浅色样式。
- **顶级页面没有自己的卡片**：页面与标题栏共用同一块窗口表面（`PageScaffold` /
  `AgentPage` 只负责内边距与滚动，不画背景与圆角），应用名与页面标题共享同一条
  左右边线（壳层 `shellInset` 必须与页面内边距取同一个值）。纵向同样不留缝隙：
  页面顶部内边距为 0、壳层标题栏与页面之间不声明 `Spacing`，页面内容紧接标题栏
  下沿；标题栏高度由平台解析（`WindowTitleBar` 按平台最小值测量），不得用硬编码
  高度去"对齐"页面。层次感由
  `AmbientGlow` 环境光 + 下面分层的分区与卡片表达。
- 卡片克制使用：页面常规分区用 `PageSection`（标题 + 内容平铺在页面表面上）
  配 `SectionDivider` 发丝线划分，不把一页串成一列盒子；重复列表条目
  （供应商/技能/MCP/会话行）用无边框 `QuietCard` 靠表面色差分层。通用
  `Card`（主题 raised 表面、6pt 圆角、1pt 语义描边、内边距的单层轻量样式）
  只用于少数需要强调的独立块；弹窗保留更强描边与阴影以保证浮层识别度。
- 圆角：浮层/弹窗 10pt、二级岛 6pt；颜色必须来自 `ResolveIslandTheme(theme)`
  等语义主题层，不在页面中散落硬编码明暗色。
- 强调色（`primary`）只用于可交互状态（按钮 / 选中 / 开关 / hover 描边与辉光）；
  分组标题、正文等非交互文字用 `on_surface` / `on_surface_variant`，不随主题变蓝。
- 辉光只用分层描边与阴影表达（SDK 无模糊滤镜）；标题栏悬停可由多层同心环 +
  低透明度宽描边 + 主题阴影构成。
- 主题只用太极图形选择器，不显示“深色/浅色/跟随系统”等文字提示。阴阳平衡表示
  跟随系统，深色外环突出表示深色，浅色外环突出表示浅色；点击循环切换
  `system → dark → light`。悬停时持续旋转，移出时冻结在当下角度并保留状态。
- 保证交互可辨识、文本对比度和响应式布局；环境装饰不能牺牲信息层级或功能识别。
- UseViewportClass 的 Compact（<600）同步收窄页面内边距和 shellInset，
  PageScaffold/AgentPage 使用 medium/large，顶部恒为 0；控制中心最小 800×600。
- 删除确认使用内置 dialog.Show(title, message, positive, negative, ...)，
  DialogStyle 由 AppThemed 主题化；dsh/ZCode live 删除采用对应三键确认。
- 关于页头部莲花、版本、简介直接平铺，链接/技术栈/许可用 PageSection 分段，
  不重新外裹通用 Card。品牌托盘保持单一盛放莲花，不恢复旧含苞状态。

## UI 资源约束

- 除 Agent 品牌身份图标外，24×24 功能图标遵守
  [资源规范](../../../resources/README.md) 的单套无色图标规范；深浅模式由运行时
  tint 自适应，禁止维护 `_selected/_dark/_light` 重复资源。CMake 会在配置期检查
  `#FFFFFF` alpha-mask 与 `_selected` 重复资源。
- Agent 身份图标（`agent_*.png`）是品牌色圆角方块 + 官方标志的 64px 栅格
  （含 `@2x`/`@3x`），直接呈现颜色，不参与 SVG tint；深浅模式共用一套，关闭态
  用整体透明度表达。
- 新增 `models::kToolRegistry` Agent 时，必须为 `ToolSpec::iconName` 添加对应的
  `resources/images/agent_*.png`（品牌色底 + 官方轮廓，64px 与 `@2x`/`@3x`），
  更新 `ToolIcon()` 映射、图标来源与许可表，并补充注册表/图标对应关系测试；
  UI 不得静默回退到通用 Agent 图标。
- 新增 `builtinPresets()` 默认供应商模板时，必须给每个厂商指定可复用的厂商
  图标（优先官方标志；没有合适素材时可自绘缩写），在
  `models::builtinPresetIconName()` 与 UI `PresetIcon()` 登记映射，并让订阅与按量
  模板入口都显示该图标；同一厂商跨 Agent/计费组复用一枚图标。新增资源时同步
  更新来源与许可表，并在测试中检查模板图标映射完整。
- 太极主题选择器、轻岛屿表面和卡片规范见本文“UI 视觉硬约束”。
- 除 Agent 品牌身份图标外，图标表现必须通过项目的 `IconButton`/主题 tint 管线
  完成，不为深浅模式复制资源，不依赖 SVG 自带的固定前景色。
