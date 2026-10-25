# 历史里程碑归档（截至 2026-10-03）

> 本文从旧版 AGENTS.md 迁出，仅作历史检索，不是当前开发规范。
> 旧工具数量、配色、依赖、同步网络接口、窗口行为与待办可能已被后续实现替代。
> 当前约束从根 AGENTS.md 和相应项目技能进入；当前产品进度查既有桌宠专题文档。

- ✅ M1：构建脚手架（HuxerUI 双通道、llmswitch_json、test_smoke/test_store）+
  领域层三模块（config/models/store）+ 领域层测试全绿。
- ✅ M2：完整 UI（应用壳+托盘切换菜单、Claude/Codex 供应商页 CRUD/切换/预设
  模板、设置页主题/路径/导入导出）、GUI 冒烟通过。
- ✅ M3：跨平台 CI（三桌面 job + tag release）+ Windows/macOS 平台入口补齐。
- ✅ 阶段A（2026-09-06）：领域层泛化到 5 工具注册表（claude-code / claude
  desktop / codex / opencode / pi / gemini / qwen / zcode），config.json 改 groups map（旧格式自动
  迁移），store 按工具 id 分发八个 writer + 各自 detectCurrent/importLive，
  UI 仅做最小适配（侧栏/供应商页仍只有 Claude Code / Codex，托盘菜单已
  通用列出全部组）——导航重写属阶段B。
- ✅ 阶段B（2026-09-06）：两级侧边栏导航（顶级 Agent 管理/设置 + Agent 页内
  二级工具图标栏，遍历注册表）、5 工具图标补齐（claudecode 自绘终端 /
  claude 官方星芒 / codex 结绳不动 / opencode simple-icons / pi pi.dev
  logo）、表单按 ToolSpec 适配（API 协议分段选择、needsModel 必填）。
  另修正阶段A 遗留：Claude Desktop profile 的 inferenceModels name 必须是
  桌面端白名单 route id（claude-(sonnet|opus|haiku|fable)-*，对齐 cc-switch
  上游 is_claude_safe_model_id），非白名单模型名借用 claude-sonnet-4-6 并把
  真名放 labelOverride。
- ✅ 拉模型列表 阶段A（2026-09-06）：curl/OpenSSL 依赖回归（vendor curl
  8.22.0 静态库 + OpenSSL 系统优先/Linux x86_64 静态包回落；Windows
  POST_BUILD 补 OpenSSL/CRT/huxerui.dll 旁挂，抄自 apitab）；新模块
  llmswitch.net（fetchModels 阻塞调用 + parseModelIds 纯函数）+ test_net。
  UI 入口（表单「拉取模型」按钮、任务线程包裹）属阶段B。
- ✅ 拉模型列表 阶段B（2026-09-06）：供应商表单模型字段旁加「获取模型」
  按钮——用表单当前 baseUrl/apiKey/apiFormat（无 apiFormat 字段的工具按
  工具推断：claude-code/claude→anthropic，codex→OpenAI 兼容）经
  `huxerui::RunWorker` 调 llmswitch.net::fetchModels（阻塞调用全程不在 UI
  线程）；拉取中按钮转「获取中…」禁用态，baseUrl/apiKey 空缺时禁用 +
  tooltip 提示；成功弹 ModelPickerContent 选择列表点选回填 model，失败
  toast 中文错误。未复制 Clash-Flux 的 task_bridge.h——SDK 0.2.0 自带的
  RunWorker 就是同一语义（skill fundamentals.md 推荐），task_bridge 是
  Clash-Flux 在 RunWorker 出现前的自造轮子。
- ✅ 大版本（2026-09-06，并行任务 A–G 整合）：
  - 新领域模块四个：`llmswitch.router`（本地路由引擎：cpp-httplib 服务器
    监听 127.0.0.1，`/<tool>/` 前缀转发到当前供应商 + 鉴权头替换 + 故障
    转移 + RequestLog/StatsSnapshot 统计，JSONL 落盘
    dataDir()/router/requests.jsonl）、`llmswitch.mcp`（MCP 统一清单
    dataDir()/mcp.json + claude-code/codex/opencode 三工具 live 写入，
    codex 走 TOML 行级 section 重写）、`llmswitch.skills`（中央库
    skills-store/ + 符号链接同步到 ~/.claude/skills 与 ~/.codex/skills）、
    `llmswitch.sessions`（扫描 ~/.claude/projects 与 ~/.codex/sessions 的
    jsonl 会话，列表/删除/导出，删除限已知根之下）。
  - apiFormat 三档化（openai-chat / openai-responses / anthropic，
    models::normalizeApiFormat + apiFormatLabel；opencode npm 与 pi api
    映射同步三档）。
  - 用量查询：Provider.usageEnabled + usageRefreshMinutes + usageUrl/usagePath/usageLabel +
    net::fetchUsage / extractByPath + 各供应商独立轮询 + DeepSeek 内置
    模板 suggestUsageQuery + 卡片用量显示与页面轮询（RunWorker）。
  - AppConfig 新增 routerEnabled / routerPort（15731）/ routerFailover +
    store setter；路由自启在 AppRoot 首组合 Lifecycle。
  - UI 顶级侧边栏扩为 8 区块（Agent 管理 / 本地路由 / 使用统计 /
    MCP 服务器 / Skills / 会话 / 设置 / 关于），新增 router_page /
    stats_page / mcp_page / skills_page / sessions_page / about_page 六个
    页面与 router/stats/mcp/skills/sessions/about 六对自绘图标。
  - 依赖新增 cpp-httplib 0.20.1（third_party/httplib 单头，INTERFACE 目标
    llmswitch_httplib）；测试目标增至 7 个（smoke/store/net/router/mcp/
    skills/sessions）。
- ✅ 恢复官方 + 官方预设 + codex 模型（2026-09-06）：store 新增
  restoreOfficial（claude-code / claude / codex 撤掉本应用写入的 live
  覆盖回到厂商原生状态，供应商页「恢复官方」确认按钮）；builtinPresets
  各组首位加官方条目（Anthropic 官方 / OpenAI 官方 wire_api=responses）；
  codex 表单补齐模型字段与「获取模型」（模型切换时行级重写 config.toml
  顶层 model 键，importLive 收回）。
- ✅ 表单按 agent 区分 + 下拉选模型（2026-09-06）：Provider 新增
  haiku/sonnet/opusModel 三档映射字段 + ToolSpec.hasModelMappings
  （claude-code / claude）——claude-code 切换写 ANTHROPIC_DEFAULT_*_MODEL
  env（restoreOfficial 一并擦除、importLive 收回）；claude desktop 每档
  映射写成 inferenceModels 条目（非白名单借该档 route id + labelOverride，
  claudeDesktopModelEntry）。表单模型选择从弹窗改为行内 Select 下拉
  （获取模型成功后出现在「获取模型」按钮前，主模型与三档映射行共享一份
  fetchedModels）；ModelPickerContent 弹窗删除。注：hcg 要求 composable
  返回 View（不能抽返回表单状态结构的 composable 辅助），两处 UseState
  初始化靠 LLMSWITCH_FORM_STATES_INIT 宏在调用点展开。
- ✅ Agent 页结构重排（2026-09-06）：编辑/新增供应商从弹窗改为整页表单
  （ProviderFormPage，ProvidersPage 内 formTarget State 双模式切换 +
  .Key("form:" + target) 重建状态）；agent 二级图标栏从左侧移到 Agent 页
  顶部横排；「恢复官方」按钮删除，官方订阅改为列表首位常驻卡
  （models::officialVendorName，切换 = restoreOfficial；官方条目同时撤出
  builtinPresets）；预设模板并入新增页顶部预设区（点选 FillForm 预填）；
  新增供应商按钮改为自绘加号图标（resources/images/add.svg）。
- ✅ 卡片操作图标化 + 托盘增强（2026-09-06）：供应商卡/官方卡操作组移到
  卡片右侧（左信息右操作、垂直居中），文字按钮换自绘图标 + Tooltip
  （swap/edit/copy/trash.svg，用量刷新 refresh.svg；命名避开 C++ 关键字
  switch/delete）；窗口最小分辨率 560×480 → 800×600；托盘菜单扩充（组头
  显示当前生效名、官方条目入菜单勾选 = restoreOfficial、本地路由开关项
  start/stop routerInstance）；关闭最小化到托盘（AppRoot 注册
  window.OnCloseRequest：托盘可用时 window.Hide() + return true 消费关闭，
  托盘「显示主窗口」经 Activate 召回，「退出」走 application.Quit() 绕过
  关闭处理器；托盘不可用返回 false 走平台默认关闭）。
- ✅ 工具栏入岛 + 连通检测 + 用量配置页（2026-09-06）：agent 工具图标栏从
  页面顶部移到供应商岛屿内部顶部（ToolBar 在 providers_page；AgentPage 变
  薄宿主持 currentTool State，点击图标经 Launch+Delay(0) 推迟写——写它会
  让 .Key(tool) 重建含被点击图标的整棵子树）；选中图标垫 raised 圆角底块
  作选中态，岛屿标题文字「X 供应商」删除（列表模式改自定义一级岛：头部
  一行 = 工具图标栏左 + 新增按钮右）；卡片操作组新增联通检测
  （activity 图标 → net::pingLatencyMs，连通显示「延迟 N ms」、失败
  「不可达：…」error 色，baseUrl 空禁用）与用量查询配置入口（gauge 图标 →
  独立整页 UsageFormPage，formTarget = "usage:"+id；用量三字段 +
  自动填充从编辑表单迁入，编辑保存保留原用量配置）；API Key 输入框加
  眼睛图标切换明文/掩码（eye/eye_off.svg）。上游 `3eb827d` 后改用 SDK 内置
  可交互 TrailingIcon（TrailingIcon(icon, 语义标签) + OnTrailingIconClick +
  Secure(bool)），不再外裹 Row + 独立 IconButton；且眼睛仅悬停输入框
  （ViewEvents::Hover，只在 Enter/Leave 写 State）或已明文时挂载显示。
- ✅ 会话页异步加载 + 过滤图标组（2026-09-07）：会话扫描从 UI 线程同步改为
  RunWorker 入 worker 线程（首载/切过滤/刷新/删除后重载；此前 countLines
  getline 逐行读全文件 + 标题提取都在 UI 线程，文件一多切换即卡顿），
  请求代次保证快速切过滤时旧结果不会覆盖新选择，异常路径会正确结束 loading
  并反馈原因；导出/删除文件 IO 也移入 RunWorker，成功删除后才触发重载。
  重载期间旧列表保持显示 + 刷新图标自转指示（无限 Tween 360°，无文字提示）；countLines 改 64KB 块读数
  换行（口径不变：末行无换行算一行、10000 封顶）；过滤 SegmentedButton
  改为图标组（全部=agents / claudecode / codex，选中态 raised 底块，与
  Agent 管理页一致）；ToolIcon/IconPair 从 providers_page 提到
  common.cpp + ui.h 共享。
- ✅ 本地路由逐 Agent 开关（2026-09-07）：保留服务总开关，新增 Claude Code /
  Claude Desktop / Codex / opencode / Pi 五个独立代理开关；选择持久化到
  AppConfig.routerTools（旧配置缺字段默认全开，显式空数组允许全部关闭），
  LocalRouter 以 mutex 保护运行时启用集合，切换不重启服务。禁用工具请求返回
  403，且不调用 resolver、不访问上游、不计入统计；接入地址只展示已启用项。
- ✅ 太极八卦水墨风（2026-09-06）：主题从极简黑白重构为水墨配色——InkDark
  「玄墨」（暖调近黑海面 #161411 + 宣纸白主色 #E6E0D2）/ InkLight「宣纸」
  （米白纸面 #EFEAE0 + 浓墨主色 #2B2823），文本/描边全部暖调墨色阶
  （浓墨/淡墨），error 改朱砂红；自绘太极图 taiji.svg（resources/images，
  宣纸底 + 玄墨双鱼反色眼），标题栏与关于页顶部卡各一枚，并 rasterize 为
  tray.png/@2x/@3x 替换灰色双向箭头占位托盘图标；设置页主题选项改名
  跟随系统/玄墨（深色）/宣纸（浅色）。按「墨韵 INK UI」参考图深化：
  浅色纸面再亮一档、二级岛近白 + 一级岛/卡片细墨边（Border outline_soft
  1pt + ClipChildren）、整窗背景改用深浅两套全景水墨画卷
  （ink_backdrop_dark/light.svg：远山、朱砂落日、归鸟、竹枝和飞瀑；
  Stack 底层以 Cover 覆盖窗口边缘，中央留白，轻岛屿允许环境景物隐约透出）。
  书法标题字未做：系统无 CJK 衬线字体，
  不为标题打包字体文件。
- ✅ 退役 curl/OpenSSL（2026-09-13）：网络统一 HuxerUI 平台 HttpClient
  （Linux libsoup / Windows WinHTTP / macOS NSURLSession，TLS 由平台栈负责）。
  net 同步 curl 接口（fetchModels/fetchUsage/pingLatencyMs）删除，只留纯
  函数；router 出站新增 `UpstreamSession` 抽象（Send + AbortInFlight），
  生产实现是 UI 层 PlatformUpstreamSession（httplib 工作线程经
  TaskScope::Post + Launch 桥到 UI 线程 SendAsync，条件变量 + 截止时间
  等待），app.cpp 根组合 `BindRouterUpstreamSession()` 绑定，测试注入
  httplib::Client 版会话。vendor 只剩 json/httplib（curl/openssl tarball
  与 huxerui-linux-openssl-system.patch 删除），CI 三平台不再安装 OpenSSL，
  Windows 打包不再带 libssl/libcrypto。
- ✅ 订阅供应商预设（2026-09-13）：builtinPresets 改为 PresetGroups 两分组
  （subscription 订阅制中转 / metered 按量官方 API），供应商新增页预设区
  分两行展示（Row 不换行）。claude-code 订阅组新增 PackyCode / AICodeMirror /
  88code / DuckCoding / Kimi For Coding（端点核实自 cc-switch 官方预设与各家
  官方客户端源码；Kimi For Coding 主模型与三档映射都填端点别名
  kimi-for-coding）；codex 订阅组新增 PackyCode（Responses 协议中转）。
- ✅ Canvas 泼墨伪元素（2026-09-13）：src/ui/ink_splash.cpp 用 Canvas+Path
  程序化泼墨（副墨团/主墨团 Catmull-Rom Blob + DrawPathShadow 洇边 + 浓墨
  内斑 + 飞白细枝 + 圆点/旋转椭圆飞墨），固定种子 splitmix32 保证同 seed
  形态恒定；墨色取主题 on_surface 深浅自适应。app.cpp 根 Stack 在画卷与
  内容之间叠两处（右上 TopEnd 为主、左下 BottomStart 呼应）。坑：Canvas
  外再包 Align 修饰符会被撑满父约束——画布本身铺满宿主，落点用 anchor
  参数在画布内定位，尺度与窗口高度解耦限幅。
- ✅ 会话详情卡顿修复（2026-09-13）：19MB 合成会话实测复现（UI 线程 99.5%
  冻结，gdb 定位在 VirtualList 重测量 + Pango 全量重排）——loadOlder 逐项
  `Insert(0)` 的每次通知都同步触发一次页面重组。详情页消息列表改不可变
  快照（`shared_ptr<const vector>`，与列表页同模式），worker 结果一次 O(1)
  写入；`parseMessageLine` 对进入 UI 的文本设 16KB 上限（UTF-8 安全截断，
  完整内容走导出）。复测：每页 50 条历史在 UI 线程合并 0.0-0.1ms。
  SDK 层遗留问题（VirtualList 每帧重新 factory 可视行且 PangoTextLayout
  无跨帧缓存，详情页静止即 99% CPU）已报上游 HuxerUI/HuxerUI#136 并经
  PR HuxerUI/HuxerUI#137 合入（有界 LRU 文本布局缓存 + ScopedTextLayout
  跟进修复），本地补丁已撤、基线升至 64264cb。
- ✅ 新增 ZCode（2026-09-13；同批加入的 Gemini CLI / Qwen Code 已于
  2026-10-03 移除，见后）：`~/.zcode/v2/config.json` provider map upsert
  `llmswitch:<id>` 条目（apiFormat 映射 kind）并 enabled 互斥停用其余，
  restoreOfficial 重启 builtin:*。接入 detect/import/restore 与 router
  逐工具开关；MCP/技能/会话暂不涉及（页面自动优雅降级）。新增自绘
  图标（zcode Z 字）。zcode 的 load() 首次
  自动收编为**全量**（liveFileExists 补 zcode 分支；importLive 把每个带
  凭据的 provider 条目都收进列表——同端点+密钥原位更新保留 id，enabled
  条目设为 current，无凭据的 OAuth 条目跳过），避免打开页面为空。
  zcode 的 models 是**不定长清单**：Provider 新增 `models` 字段（序列化为
  每供应商 `models` 数组；空 = 仅 model 一个）——导入全量读出、切换全量
  写回（默认模型保证在列）、表单「获取模型」拉到的列表整体写入（未拉取
  则保留原清单），多模型条目不再丢模型。
- ✅ 冷调石板主题重构（2026-09-17）：按参考图把两套主题从暖调水墨换成冷调
  石板/海军蓝——`AppDarkThemeSpec`「深海」（海面 #101923 + 青蓝 #38BDF8）/
  `AppLightThemeSpec`「晴石」（海面 #FDFDFD + 天蓝 #2F7BE6），并补齐此前
  一直沿用 Material 紫色默认值的 `primary_container` / `on_primary_container`
  / `tertiary_container` / `on_tertiary_container` / `scrim`。强调色语义收紧：
  `primary` 只用于可交互状态，四处 SectionTitle 从 primary 改到
  `on_surface_variant`。标题栏莲花本体固定 `on_surface`（浅色深石板/深色冷
  白），强调色只走悬停圆底 + 描边 + 三层同心环与加宽底描边构成的辉光。
  水墨身份退役：删除 `src/ui/ink_splash.cpp`（泼墨）与 `ink_backdrop_dark/
  light.svg`（含早已无引用的 `ink_landscape.svg`），环境层改为程序化
  `AmbientGlow(dark)`（RadialGradient 冷调光晕）。太极选择器三枚 glyph 重新
  配色（冷白 #E8F0F8 / 深蓝 #1B2836），主题名改「深海/晴石」（仅悬停提示）。
  `Card` 增加 1pt `outline_soft` 描边（冷调下卡片与页面色差小，靠描边划界）。
  资源同步冷调化：托盘 10 张 PNG 重渲染、`platform/windows/app.ico` 与
  `platform/linux/package/llm-switch.svg` 重新生成、Windows 安装器重复的
  主题定义同步。实测取值：浅色底 #FDFDFD、hover #E9F0F8、亮蓝 #4595F7；
  深色底 #101923、卡片 #1A2431、hover #22303F、青蓝辉光 #287AA9~#38BDF8。
  注：参考图标题栏圆角实测 ≈26px/条高 105px ≈ 0.25，映射到 24 DIP 标题栏
  就是 ~6pt，即现有 nested_radius——**未放宽** AGENTS.md 的圆角约束。
- ✅ 主体配色对比度达标（2026-09-19）：量化评估两套主题（岛屿表面按 alpha
  合成到海面后算 WCAG 对比度）后收紧浅色「晴石」——primary #2F7BE6→#2870D6
  （白字 4.12→4.80:1、作描边 3.94→4.58:1，过 AA 正文线）、error
  #D64545→#C63A3A（4.18→4.93:1）；AmbientGlow 浅色洗色与 Windows 安装器
  重复主题定义同步。语义状态色进 `IslandTheme`（success/on_success/warning，
  按 `ResolveIslandTheme` 内海面亮度分深浅取值）：router 页 `StatusColor`
  与「运行中」徽章改走语义层，删除硬编码绿/黄（浅色下警告黄仅 2.81:1、
  徽章白字绿底 3.3:1，深色徽章改翻墨青字 4.97:1）。内置确认框底色
  surface_container_high→highest，与 `DialogCard` overlay 同源，两套弹窗底色
  一致。深色「深海」全项 ≥6:1，未改动。
- ✅ 深色主题换「石墨」中性灰阶（2026-09-19）：深色「深海」的海军蓝底
  （#101923 一族）整体换为中性石墨冷灰阶（海面 #101214、卡片 #1A1E22、
  描边 #2D333A、正文 #E8EAED、次要 #9AA1AA），强调青蓝 #38BDF8 与语义红
  不变——表面去蓝后强调色成为唯一彩色源。对比度全项过 AA 且余量优于旧值
  （正文 15.3:1、次要 7.1:1）。主题名「深海」→「石墨」（悬停提示），太极
  三枚 glyph 去蓝（#1D222B/#E9ECEE），Windows 安装器暗色 spec 同步。
  应用图标（linux svg / windows ico 的品牌底色）保持不动。
- ✅ 切页动画减压（2026-09-19）：针对 GTK 后端「动画期间全窗内容逐帧
  重光栅化、无跨帧缓存」的实测结论（无头探针：运行时侧每帧亚毫秒，卡顿
  全在后端光栅），切页收合 0.18→0.12s、展开 0.30→0.18s（昂贵帧数 -38%）；
  `AmbientGlow` 只把光斑实际覆盖的顶部画进 cairo 批次（深 72%/浅 56% 窗高，
  渐变按裁剪矩形归一化、在裁剪线上恰好衰减到 0），削减每帧最大单件光栅
  面积。根治（cairo 批次按 PaintSequence revision 缓存为 GSK 纹理）待上游
  PR。切页时序常量：`kCollapseSeconds`（app.cpp）。
- ✅ 卡片减负（2026-09-19）：诊断是卡片滥用而非线宽——描边保持 1pt/
  0.62α 不动。新增 `PageSection`（标题+内容平铺）+ `SectionDivider`（主题
  Divider 发丝线），router（4）/settings（3）/stats（2）/about（3，头部
  英雄卡保留）分区卡全部拍平；重复列表条目换无边框 `QuietCard`：供应商
  卡 ×2、Skills 行、MCP 行、会话行。
- ✅ 0.2.0 用量账本迁 SQLite（2026-09-26）：会话用量账本从 append-only 的
  `usage.jsonl` + `scan-state.json` 换成 SQLite，经 `huxerui_use_library` 引入
  **Lib-SQLite**（独立仓库，不在主仓也不在 SDK 里；经下游 fork
  `FarnaHerry/Lib-SQLite` 的 `farna/main` 钉 commit
  `88f610f`，FetchContent 在 configure 期拉取，CI 无需改动）。因为 Lib-SQLite
  的公开 API 只有异步（`Database::*Async` 返回 `Task`，同步入口只在
  `Transaction` 回调内），而 `llmswitch.usage` 是只 `import std` 的纯模块，
  所以切分成：域模块只留解析/`ScanUsageLogs`/聚合口径，`usage.db`
  （`usage_records` 主键 key + `(agent,ts)` 索引、`scan_state` 表）与异步编排
  放在 `stats_page.cpp`。跨轮次合并靠 `ON CONFLICT(key) DO UPDATE SET
  x=max(x,excluded.x)`，与原「字段级取 max」一致；**不兼容旧数据**（旧 JSONL
  不再读）。代价（v0.1.38 → v0.2.0 同工具链实测）：Linux tar.gz/deb +19.8%/
  +19.6%（+832/+823KB）、rpm +22.3%、Windows zip +15.2%、setup.exe +7.7%、
  macOS tar.gz +47.8%（基数仅 1.53MB）。同时定下「高频日志直接写文件、
  不进 SQLite」的载体规则（见该节）。
- ✅ 表单页脱离 Pager（2026-09-29）：新增/编辑供应商与用量查询配置从
  ProvidersPage 的页内模式提升为 AgentPage 自己的第二个页面（IndexedPages
  双页，只有选中页参与测量与摆放）——表单打开时工具栏 + Pager 不参与布局
  与命中，横向拖动既滑不走表单也切不了 Agent；Pager 与各列表页仍保持挂载，
  返回列表保留列表滚动位置、卡片状态与用量刷新时间戳。表单在 Agent 页这一
  层只缩进一次，标题与工具栏、其他页面标题同一条左边线（此前叠在已带壳层
  内边距的 Pager 页里，缩进两次）。无头 UiTest 实测量：列表模式横向拖动
  仍能切 Agent（0↔1），表单打开后同一拖动被表单页挡住（selectedTool 不变、
  返回/取消仍在）。
- ✅ 新增供应商页的目标 Agent 选择器（2026-09-29）：新增页左上角的标题换成
  与 Agent 页完全相同的注册表工具栏（AgentToolButton/AgentToolBar 提到
  agent_page.cpp 共用），但行为是「这份配置写给谁」——点选切换目标 Agent，
  表单策略随之切换（codex 出现 config.toml 区块等）；工具栏最后一项是新的
  `resources/images/all.svg`「所有 Agent」，保存时把同一份配置写进每个注册表
  组（各组各自生成 id，用量配置随预设带过去）。编辑/用量页没有选择器，仍走
  普通标题行（PageScaffoldWithHeader 让标题行整行可由调用方提供，左右内边距
  与 PageScaffold 同一套壳层约束）。表单目标改用注册表下标
  `formToolIndex`（registry.size() = 所有 Agent）作为唯一真源，卡片入口经
  `ToolRegistryIndex` 换算；切换目标保留已填内容，只把「上游格式」重置成新
  agent 的原生默认（agent 派生初值，跨 agent 沿用会给出错误后缀）。
  无头 UiTest 实测：选择器 11 项齐全、切到 codex 后只在 codex 表单出现
  config.toml 区块、预设填好的数据在切换目标后保留、切到「所有 Agent」保存后
  10 个注册表组各出现一条该供应商。
  修复同日发现的两个「闪一帧旧内容」：①新增页打开时先闪一帧「编辑供应商 /
  正在加载供应商配置…」的旧脚手架（左上角还不是选择器）——`formReady` 对
  `"new"` 不该再等 `formDataTarget`，那是编辑表单等异步拷贝的就绪标志，而关闭
  上一次表单时它已被清空；②在新增页里切换目标 Agent 时，「上游格式」会闪一帧
  旧 agent 的默认后缀——那次重置原本写在 `Lifecycle` 里，而 Lifecycle 在组合
  之后才跑；改成组合期按 `FormStates.upstreamFormatTool`（已为哪个 agent 定过）
  现算，编辑表单不重置、沿用保存值。无头 UiTest 逐帧实测：打开新增页的第一帧
  就已是目标 agent 的选择器与策略，切换目标当帧即为新默认
  （Anthropic/OpenAI 不再滞后一帧），编辑页仍先加载并保留保存的 `upstreamFormat`。
- ✅ 关于页去掉唯一卡片（2026-09-29）：头部（莲花 + 版本 + 简介）原来是全应用
  最后一个用通用 `Card` 包的页面区块，现在与其他顶级页面一样直接平铺在窗口表面、
  由 SectionDivider 分段（内部结构、字号与间距一字未动）。`Card` 因此只剩会话详情
  的「用户消息导航」在用。
- ✅ dsh 手写路由的推理档位（2026-09-29）：dsh 的 llm-pi-ai 适配器把「模型支持
  思考」当 per-model 能力——手写路由不在模型条目里声明 `reasoningEfforts`，
  dsh 就把模型当成不支持推理，模型菜单里连「推理等级」都不出现。`models`
  新增规范档位表（off/minimal/low/medium/high/xhigh/max，取自 pi-ai 的 thinking
  level）与归一/位掩码互转，`Provider.reasoningEfforts` 作为 per-供应商清单
  （JSON 往返时归一）；写入端在 `llmswitch-<id>` 的首个模型条目下声明这些档位
  （键固定加引号，`"off":` 留空 = 不发思考参数），收编端从同一条目读回；
  dsh 表单新增「推理档位」chip 区块（provider_form_dsh.cpp 的 DshFormPolicy +
  DshReasoningFields，此前 dsh 复用 opencode/pi 的通用策略）。
  同时补上 dsh 的 **flow 风格 providers 摊平**：settings.yaml 允许
  `providers: { c: { ... } }` 这种写法，而行级改写只懂块风格，直接往 flow 块后
  插条目会写出非法 YAML、dsh 的 watcher 保留旧文档导致切换静默失效——现在读写
  前先把 flow 值摊平成块风格（含 `providers: {…}` 与换行两种写法、任意嵌套的
  map/seq、引号内的 `#`/`{}` 不误判），摊平只改排版；裸 null/~/Null 与空值同义，
  统一写成空值。收编时「首个模型条目的档位」计数按每条路由重置（此前是整篇
  文档一份计数，第二条及以后的路由档位会被静默丢掉）。test_store 覆盖：
  档位全集/归一/位掩码往返、写入拼写与顺序、flow 摊平后键值保留且字节稳定、
  多条路由各自读回、`providers: {…}` 同行写法。
- ✅ pi 支持精确化（2026-09-29）：三处对 pi（`~/.pi/agent/models.json` +
  settings.json）的精确性修复。①`detectCurrent("pi")` 原先在
  defaultProvider 不是组内 id 时**遍历 models.json 全部条目**找凭据命中——
  而本应用切换只 upsert、从不删旧条目，历史条目会一直躺在文件里，于是「pi
  实际用着用户手写的别的路由」被误判成某个历史供应商正在生效；现在改为只按
  defaultProvider 指向的那一条条目匹配，仅当该键为空或悬空（配置已不自洽）
  才退回遍历全部条目（保留原行为与既有测试）。探测同时改用 readJsonPassive，
  坏文件不再被 detect 挪走。②切换原先把 providers[id].models 覆盖成
  `[主模型]`，用户在该 provider 下配的其它模型被静默删掉；现在保留既有清单，
  只在缺少主模型时补一条，且**按原元素形状**补（裸标量或 `{"id": ...}` 对象
  都认，新增 `modelEntryId`/`modelListContains` 共享助手）。③收编时
  defaultModel 缺失则回退到条目第一个模型（与 dsh 一致），否则收编出来的
  供应商没有模型、再切换就把清单写丢。
- ✅ 卡片状态文本限宽（2026-10-01）：用量查询失败曾让整行不可用——失败文本是
  「查询失败：查询用量失败：HTTP 404（URL）—— 响应体前 200 字」，而卡片中间
  状态行是三段式「信息列（Grow）｜状态（固定）｜操作组（固定）」里的固定段，
  Row 的非 Grow 子项按 intrinsic 宽度参与布局，一条 1000+pt 的文本会把信息列
  挤到 0 宽（名称竖排）并把右侧六个操作图标整个顶出卡片（切换/编辑/复制/删除
  全部点不到）。修复两层：①显示层统一「取首行 + 预览列数上限（26 列 ≈ 13 个
  汉字 ≈ 143pt）+ Frame.max_width 150」，被截掉的完整文本进 Tooltip（预览与
  全文等价时不挂，避免无信息浮层）；②失败文本本身改成两行——首行是短原因
  （`HttpFailure::Summary()`：HTTP 失败即「HTTP 404」，传输失败即平台错误
  消息），次行是原来的完整上下文（URL + 响应体摘要），toast 与 Tooltip 仍看到
  全文。无头 UiTest 探针（tests/ui_layout_probe.cpp，未跟踪，见其文件头构建
  说明）实测：修复前受影响卡片的 6 个操作图标 x=[1080,1340] 全部
  in_viewport=false；修复后 800×600 与 1080×720 两个视口下图标全在卡内，且在
  图标中心真实点击能触发切换（该卡挂上「使用中」徽章）。
- ✅ dsh 官方模型条目字段（2026-10-01）：此前 dsh 新增/编辑表单只有推理档位，
  Provider 也只存 reasoningEfforts——收编一条手写路由后再切换，用户的容量与
  视觉声明就被写丢。字段清单与取值口径取自本机安装的 dsh 插件
  （`@deepseek-ai/dsh-llm-pi-ai` 的 `lib/types/config.d.ts` / `catalog.d.ts`：
  `PiAiModelProfile` 的 id / name / contextWindow / maxTokens / input /
  reasoningEfforts，以及 `MODALITIES = [text, image]`、
  `THINKING_LEVELS = [off…max]`——档位表与既有 `reasoningLevels()` 完全一致）。
  新增：Provider 的 `contextWindow` / `maxTokens`（0 = 不声明）与
  `inputModalities`（text/image）；表单多出一块「输入模态 + 容量」chips/输入框
  （容量接受 `131072` / `256K` / `1M`，与 dsh 官方设置页同一套 K/M 拼写，
  `models::parseTokenCount` / `formatTokenCount`）；写入端在模型条目里按官方
  顺序落盘 `contextWindow` → `maxTokens` → `input: [text, image]` →
  `reasoningEfforts`，未声明一律不写（dsh 退回 catalog 值/路由默认）；
  读取端认块风格、同行 flow（`input: [text, image]`、
  `reasoningEfforts: { "off": null, medium: medium }`）与 K 后缀容量。
  **text 是底座**：非空模态清单一律补齐 text（只声明 image 等于声明这个模型
  收不了文字），空清单仍是「不声明」；chips 里勾「图像」自动带上「文本」、
  取消「文本」整份声明一起灭。另修一处官方键丢失：agent-default-model 块
  重写时保留用户选的 `reasoningEffort`（dsh 的模型选择状态，每切一次都被清掉
  就是丢设置）。无头 UiTest 探针（tests/ui_dsh_form_probe.cpp，未跟踪）实测
  完整路径「打开 dsh 新增页 → 选 DeepSeek 预设 → 勾图像 → 上下文窗口填 1M、
  最大输出填 256K → 添加 → 切换」：config.json 装配出
  `inputModalities:[text,image]`、`contextWindow:1000000`、`maxTokens:256000`，
  live settings.yaml 落成官方形状；撤掉表单区块或撤掉装配代码，探针分别以
  退出码 1 / 2 复现。test_store 覆盖写入形状、未声明不写、块/flow 读回、
  容量拼写与模态底座规则（四处均做反向验证：撤掉修复后分别 4 / 10 / 7 / 1
  项断言失败）。
- ✅ dsh 凭据文档布局修复（2026-10-02）：`.credentials.yaml` 是 dsh 的
  **version 1 布局**——顶层只允许 version / refs / records，引用名在 `refs:`
  块内（缩进 2）。此前 `upsertDshCredential` 把 `LLMSWITCH_<ID>: "…"` 追加到
  顶层，dsh 解析时直接拒绝**整份文档**
  （`credentials-local: unknown top-level key "LLMSWITCH_…"`），于是所有密钥
  一起失效、连用户自己存的 C_API_KEY 都读不出来，所有手工路由都报凭据缺失
  ——dsh 完全不可用。修复：先认布局再落笔（有 `refs:` 块就写块内、`refs: {}`
  摊平成块风格、其它 flow 值明确报错不硬写；没有 version 的预发布 flat 布局
  仍写顶层；空文件建 version 1 骨架），并把旧版本写到顶层的 `LLMSWITCH_*`
  残留迁回 refs 块（值保留、refs 里已有的同名引用为准），下一次写入即自愈；
  读侧（adopt/importLive）只认 refs 块内的引用。test_store 8d10 覆盖布局/
  迁移/去重/骨架，实机文件已修复（原件备份在 backups/dsh/），并用 dsh 自己的
  `parseCredentialsDocument` 验证可解析。
- ✅ ZCode 同步 dsh 的增量模式（2026-10-02）：`~/.zcode/v2/config.json` 的
  provider map 与 dsh 同为「多条目并存 + 一个当前指向」，供应商页改走左右
  两列（ToolSpec.additiveProviders，providers_zcode.cpp）。写侧单条增量：
  `writeZcodeEntry`（私有单条 upsert，原生条目走 mergeZcodeEntry 原位合并）与
  `eraseZcodeEntry`（私有单条删除）是全部实现，公开入口 `writeZcodeProvider` /
  `adoptZcodeProvider` / `removeZcodeProvider` / `enableZcodeKey`（左列「在 ZCode
  中启用」= 只翻 enabled）/ `syncZcodeProviders`（整组重建
  只此一处）+ 增改复制删与 `switchTo` 都走它们；`zcodeLiveProviders` 是左列
  实况快照（key / providerId / builtin / enabled / 模型清单）。与 dsh 的两处
  差别：**默认指向是条目自己的 enabled**（ZCode 允许多条同时启用，本应用只
  保证自己托管的 llmswitch:* 互斥——切换只翻 enabled，内容一字不动；正因它是
  live 自己的状态，这个动作与 dsh 一样放在**左列**，未纳管的原生条目也能直接
  启用，右列只留写入 / 收编 / 删除）、
  **密钥就在条目里**（没有独立凭据文档）。「保留 ZCode 自己的配置」是硬约束：
  原生条目（键由 ZCode 生成）收编时**不改名也不改写**、只是开始被本应用记录；
  写入/更新原位合并、保留它自己维护的字段（options 其它键、
  systemDisabledReason 等）；builtin:* 不收编不删除（启停也不代管）；整组重建只写成
  llmswitch:<id>（已有原生键的原位更新）并清孤儿 llmswitch:*。
  `removeProvider` 的 `eraseLive` 对 zcode 同样生效（右列删除确认的三键弹窗）。
  右列每行的「写入 / 更新」用 `write.svg`（与 dsh 同一枚：文件 + 插入的一条
  墨块 = 只写这一条）。
- ✅ dsh / zcode 双列页减压（2026-10-03）：两个工具页是全应用挂载节点最多的
  页面（一屏两个 VirtualList），而 linux 后端的 cairo/GSK 批次没有跨帧缓存——
  任何重绘都按已挂载节点重新光栅，所以「特别卡顿」的根因是**预取太贪 + 行内
  文本节点太多**，不是 store 读文件（`dshLiveProviders` 21 条 0.13ms、
  `zcodeLiveProviders` 36 条 0.18ms；页面体实测每次挂载只跑一次，不是逐帧）。
  两处改动：四个列表的 `CacheExtent(480.0F)` → `128.0F`；行内的「键 · 模型」与
  「访问地址」两行合成一条 `DshMetaLine` / `ZcodeMetaLine`（一条 Text，URL 长了
  照旧折行）。实测（无头探针逐帧 Pump，claude-code 单列页为对照）：一屏挂载
  dsh 250 → 201 节点、zcode 256 → 193，静止帧中位数 dsh 10.4 → 8.8ms、
  zcode 11.2 → 8.6ms（对照 5.2ms 未变），滚动帧均值 dsh 17.6 → 10.1ms、
  zcode 14.5 → 8.9ms。绝对值随机器负载浮动，看的是与对照页的比值。探针留在
  tests/（未跟踪，不进 CTest）。
- ✅ 退役 Gemini CLI / Qwen Code（2026-10-03）：两个工具从注册表整体移除
  （`kToolRegistry` 10 → 8 个，`routerTools` 默认清单同步），连带删掉它们的
  live 通道与全部支持面——`.env` 行级 upsert + settings.json 深合并
  auth 类型（store 的 switchTo / restoreOfficial / importLive /
  detectCurrent）、`cfg::gemini*/qwen*` 路径与环境变量、用量账本的 Qwen
  解析器（`ParseQwen`）与扫描根、工具图标映射、对应的领域层测试与
  `GEMINI.md`。工具组本身消失后：config.json 里遗留的 gemini/qwen 组不再
  渲染（数据保留、不主动删）；它们作为**其它工具的端点预设**（claude-code
  / codex / dsh 下的「千问（阿里百炼）」等）与厂商 logo `qwen.svg` 保留
  不动——那是端点，不是工具支持。`gemini.svg` 已无引用，暂留。
- ⬜ 待做：订阅站端点可能随各家调整，升级版本时需复核；无 CLI 分流、
  无单实例/开机自启；`usage.db` 的 WAL 一致性备份（当前不在 `backups/` 覆盖内）。
