# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.
根目录 `AGENTS.md` 是所有 AI agent 共用且优先级更高的强制规范；开始工作前必须
完整读取。本文件提供架构细节与实现上下文，不重复维护跨工具规则。

llm-switch 是 **cc-switch**（GitHub: farion1231/cc-switch）的 **C++23 模块化重写**：
管理多款 AI agent 工具的供应商配置切换——领域层已泛化到 10 个工具
（claude-code / claude desktop / codex / opencode / pi / dsh / hermes / gemini /
qwen / zcode，注册表见
`models::toolRegistry()`），把选中的供应商写进工具的 live 配置文件
（`~/.claude/settings.json` 的 env 块 / `~/.codex/auth.json` + `config.toml` /
opencode.json additive upsert / pi 的 models.json + settings.json /
dsh 的 settings.yaml + .credentials.yaml 行级改写 / hermes 的 config.yaml 行级改写 /
Claude Desktop 3p profile 组），并提供收编、备份、导入导出。在此之上还有：
本地路由引擎（127.0.0.1 反代到当前供应商 + 请求统计）、供应商用量查询、
MCP 服务器统一清单、Skills 中央库同步、历史会话管理。用 **HuxerUI**（组件式
声明 UI）做桌面壳，全程 C++；供应商页面的模型列表、用量查询和连通检测使用
HuxerUI HttpClient（按平台使用 WinHTTP/libsoup/Foundation），本地路由服务器
用 cpp-httplib 监听 127.0.0.1、出站转发同样走平台 HttpClient（UpstreamSession
桥接），无数据库。
构建系统 CMake（脚手架与姊妹项目 `../Clash-Flux` 同源）。分层：
UI（src/ui/*.cpp 普通源走 hcg codegen）/ 领域层（llmswitch.config/models/store/
net/router/mcp/skills/sessions 八个 C++23 模块）。

## HuxerUI 开发参考

UI 工作先读 skill：`.claude/skills/huxerui-app-development/SKILL.md`（references/
含 dsl-style、components、fundamentals、layout-and-ui 等分册）。要点：

- HuxerUI 双通道（顶层 CMakeLists 固定优先级）：显式 `HUXERUI_HOME` 源码 →
  `third_party/huxerui` 源码（git clone 上游，add_subdirectory 编译，不入库）→
  已安装 SDK（`HUXERUI_HOME` 指向含 lib/cmake/HuxerUI 的前缀）→
  `third_party/tarballs` 的 Linux 0.2.0 离线包。源码通道缺 GTK ≥4.14 /
  libepoxy ≥1.5 / libsoup ≥3.0 开发包时自动回落 SDK。强制 SDK：
  `-DLLMSWITCH_HUXERUI_FORCE_SDK=ON`。本机走 **third_party/huxerui 源码**通道
  （git clone 上游，跟主干拉取；当前钉在 `0c51262`，保留 `64264cb` 的 Linux
  有界 LRU 文本布局缓存，并包含 Application/Window 所有权重构与跨平台 HTTP
  流式请求修复；完整 SHA 见 `third_party/README.md`）。
- 剪贴板通过 composable 内的 `UseApplication().Clipboard()` 获取；事件处理器可捕获
  service 并同步调用 `IsAvailable()` / `ReadText()` / `WriteText()`，不要从 worker
  线程调用。TreeView 使用 `TreeView<Node>(roots, factory, item_info)`，必须放在有界
  的垂直视口中，并通过状态重组声明来反映节点变化。
- 源码通道优先用本地构建的宿主工具（`third_party/huxerui-tools/linux/x86_64/
  {hcg,hrc}`；本仓库暂无此目录，缺失时自然回落上游预置，逻辑保留）。
- UI 层是**普通 .cpp**（不要 .cppm：codegen 只扫 .cpp/.cc/.cxx）；composable
  函数不加 `inline`；入口根在 src/app.cpp + src/ui/app.cpp。
- **composable 函数体内不能有条件编译**（hcg 不支持 #ifdef 穿行）——版本号等
  宏在文件作用域先展开成常量（settings_page.cpp 的 kAboutText）。
- 供应商编辑/用量配置的进入、取消、返回属于纯 State 导航，点击回调直接写
  `formTarget`；State 通知仅标记重组并请求后续帧，不同步卸载点击节点。
  不要为纯导航套 `TaskScope::Launch` / `Delay(0)`：Linux dispatcher 使用低优先级
  idle 队列，可能被持续绘制推迟；网络和文件 IO 仍使用相应异步 API。该机制
  已上游报告 HuxerUI issue #144（调度优先级/公平性复查）；若新版 baseline 重做
  调度，需重测饥饿风险，但直写规则与是否饥饿无关。
- **hcg 的组合函数限制**：`UseTheme()` 等只能在 `[[huxerui::composable]]` 函数
  体内调用；普通函数里的 dialog 工厂 lambda 也不行。解法见
  providers_page.cpp：内容拆成 `ProviderFormContent` composable，普通函数
  `ShowProviderForm` 的工厂 lambda 只做转发。
- `nlohmann::json` 经 `import nlohmann.json` 模块：**不要用 `.items()` 结构化
  绑定**（迭代代理的 `get<>` 不在导出集里，模块下编译失败），用迭代器
  `it.key()`/`it.value()`。
- 存进 `State<T>` 的类型需要 `operator==`（`= default` 即可），否则 State
  写回时编译失败。注意 `router::RequestLog` **没有** operator==（领域层刻意
  不加），`State<std::vector<RequestLog>>` 编译不过——页面要包一层自定义
  包装/只存标量快照（router_page 的处理方式可作参考）。
- **Runtime 生命周期的一次性注册只能放 `AppOptions::application_hooks`，不能放
  composable**：0c51262 的 `08acc36 refactor(runtime): separate application and
  window ownership` 之后，`SystemTrayHandle::OnActivate` 保留单个处理器直到
  Runtime 关闭并**拒绝重复注册**（`std::logic_error "system tray activation
  handler is already connected"`），`ApplicationContext::Provide` 同样按类型
  拒绝重复。composable 体会随订阅的 State 重组，放进去必然第二次注册并
  直接 terminate。托盘激活的落点是 `TrayActivationTarget`（`src/ui/app.h`）：
  ApplicationHook 里 `Provide` + 注册一次，`SystemTrayPresentation` 用
  `UseService` 取回并在 Lifecycle 里绑定/解绑当前 `WindowHandle`——这也是
  skill `navigation-and-window.md` 要求的「不要捕获组合期窗口上下文」。

## 构建 / 运行 / 测试

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release  # 配置
cmake --build build --parallel 4                         # 编译（app + 7 个测试目标）
ctest --test-dir build             # 冒烟 + 领域层测试（smoke/store/net/router/mcp/skills/sessions）
./run.sh                           # 启动 GUI（INTEL_FORCE_PROBE=1）
huxerui run linux                  # HuxerUI CLI 流程（构建到 .huxerui/build/linux/）
```

调试构建把配置命令中的 `Release` 改为 `Debug`。项目默认生成器是 Ninja，
也可使用 `cmake --preset ninja-release` 和对应的 build preset。

每批修改的强制收尾流程以仓库根目录 `AGENTS.md` 为准：完整编译、运行测试、
`git diff --check`、创建本地 commit，并尝试推送当前分支。推送失败时保留本地
commit，不回滚已经验证的修改，并在最终回复中报告失败原因。

- 工具链：系统 GCC（本机 16.2.1）+ libstdc++，CMake ≥ 4.4（`import std` 仍是
  experimental：UUID 表在 `cmake/CxxImportStdGate.cmake`）。
- **依赖极简**：nlohmann::json 3.12.0 以 single header 提交在
  `third_party/json/`（配 `cmake/nlohmann.json.cppm` 提供 `import nlohmann.json`，
  静态库目标 `llmswitch_json`）；cpp-httplib 0.56.0 单头提交在
  `third_party/httplib/`（INTERFACE 目标 `llmswitch_httplib`，仅
  llmswitch.router 用）；HuxerUI 0.2.0 走双通道（见上）。
  网络（模型列表/用量/连通检测/路由出站转发）统一走 HuxerUI 平台
  HttpClient，不 vendor curl/OpenSSL。无 SQLite/IXWebSocket。
- 测试目标独立（7 个，均无框架、断言失败计数非零即败）：`test_smoke`
  （编译+运行冒烟）、`test_store`（领域层；全程 setenv 隔离到临时目录）、
  `test_net`（parseModelIds / extractByPath 纯函数；不测真实网络）、
  `test_router`（httplib 假上游：转发/换 key/故障转移/统计日志）、
  `test_mcp` / `test_skills` / `test_sessions`。测试目标经 FILE_SET 显式
  追加领域模块接口 + 对应实现单元（glob 只进 app 目标），链接
  `llmswitch_json`（test_router 另链 `llmswitch_httplib`）。

## 架构

| 模块 | 文件 | 职责 |
|------|------|------|
| `llmswitch.config` | `src/config.cppm` | 数据目录（~/.local/share/llm-switch）/ config.json、backups/、mcp.json、skills-store/、router/requests.jsonl 路径 / live 配置与会话/技能目录解析（全部 LLMSWITCH_* 环境变量可覆盖）/ 深色检测 |
| `llmswitch.models` | `src/models.cppm` | 工具注册表（ToolSpec/toolRegistry/findTool：claude-code/claude/codex/opencode/pi/dsh/hermes/gemini/qwen/zcode；needsModel/hasApiFormat/hasModelMappings 三标记驱动表单适配）+ Provider/ProviderGroup/AppConfig（groups 以注册表 id 为键的 map，旧格式顶层 claude/codex 自动迁移；router/usage 设置字段，Provider.usageEnabled 控制单个供应商是否查询，routerTools 保存逐 Agent 代理选择；Provider 含 modelFetchUrl、haiku/sonnet/opusModel 三档映射与 upstreamFormat/fullUrl URL 模式）+ JSON 序列化 + 内置预设（builtinPresets）+ 官方厂商名（officialVendorName：claude 系/codex/zcode/dsh 有官方常驻卡）+ apiFormat 三档归一（normalizeApiFormat/apiFormatLabel）+ 上游 URL 归一/后缀（normalizeUpstreamFormat/upstreamFormatSuffix/effectiveBaseUrl）+ 用量模板表纯解析与匹配（parseUsageTemplates/suggestUsageQuery：数据在 resources/raw/usage_templates.json 资源包内置 + dataDir 用户覆盖，不硬编码） |
| `llmswitch.store` | `src/store.cppm` + `src/store.cpp` | ProviderStore：config.json 读写、CRUD、switchTo 按工具 id 分发十个 writer（原子写+备份；gemini/qwen 走 <dir>/.env 行级 upsert + settings.json 深合并 auth 类型，zcode 走 provider map upsert + enabled 互斥，dsh 走 settings.yaml 行级 upsert + .credentials.yaml 密钥库，hermes 走 config.yaml custom_providers 列表 upsert + model 节指向）、restoreOfficial 恢复厂商原生状态（claude-code/claude/codex/gemini/qwen/zcode/dsh）、detectCurrent/importLive、导出导入、theme/usage/router 与逐 Agent 路由设置 setter、用量模板用户覆盖表读取（loadUsageTemplatesOverride） |
| `llmswitch.net` | `src/net.cppm` + `src/net.cpp` | 纯函数：模型列表 URL 拼接 `modelListUrl`/候选推导、响应解析 `parseModelIds`（data/models 两种形状，去重保序）和用量取值 `extractByPath`（点分路径+数组下标取标量）；实际网络请求不在此层——供应商页面走 HuxerUI HttpClient（provider_network.cpp），路由出站走 UpstreamSession |
| `llmswitch.router` | `src/router.cppm` + `src/router.cpp` | LocalRouter：cpp-httplib 服务器监听 127.0.0.1，`/<tool>/` 前缀路由到该组 current 供应商的实际 URL（按 upstreamFormat 追加 /anthropic 或 /v1，fullUrl 时原样），替换鉴权头，线程安全的逐工具开关运行中即时生效（禁用返回 403，不访问上游/统计），可选故障转移（429/5xx/连接失败按组内顺序试下一个）；RequestLog/StatsSnapshot 统计，每请求追加 JSONL（dataDir()/router/requests.jsonl），启动回填内存环形缓冲（最多 1000 条） |
| `llmswitch.mcp` | `src/mcp.cppm` + `src/mcp.cpp` | MCP 服务器统一清单（SSOT = dataDir()/mcp.json）；启停 = 写/删工具 live 配置条目：claude-code → ~/.claude.json 顶层 mcpServers 深合并、codex → config.toml 行级 [mcp_servers.*] section 重写、opencode → opencode.json 顶层 mcp；claude/pi 不支持（抛中文错） |
| `llmswitch.skills` | `src/skills.cppm` + `src/skills.cpp` | Skills 中央库（dataDir()/skills-store/<name>/：SKILL.md + 附带文件）+ create_symlink 同步到 ~/.claude/skills 与 ~/.codex/skills；合并视图（中央库/已链接/仅工具侧） |
| `llmswitch.sessions` | `src/sessions.cppm` + `src/sessions.cpp` | 历史会话扫描：~/.claude/projects/<项目>/*.jsonl 与 ~/.codex/sessions/<年>/<月>/<日>/*.jsonl；列表 Worker 在一次批次中枚举 mtime/size 并读取每个文件头尾固定大小摘要，VirtualList 行只展示已完成的摘要；删除（限已知 sessions 根之下，越界抛错）/导出/详情从文件尾反向分页读取；摘要 best-effort，详情按需解析 |
| `llmswitch::ui`（普通 C++） | `src/ui/*.cpp` | app（壳：冷调石板主题 AppDark「深海」/AppLight「晴石」+程序化环境光 AmbientGlow+带对称线条节点与 Hover 辉光的标题栏莲花导航+顶级 8 区块径向导航+IndexedPages+莲花托盘图标（单一盛放态）+关闭最小化到托盘+路由自启）/ agent_page（薄宿主：持 currentTool State + ProvidersPage .Key(tool) 宿主）/ router_page（路由总开关+逐 Agent 代理开关+已启用接入地址+最近请求日志，含 routerInstance() 单例）/ stats_page（统计汇总）/ mcp_page / skills_page / sessions_page（会话管理：右上角 Agent 图标组与 Agent 管理页同一注册表、无“全部”混合页；Pager 切换独立 Agent 历史，只有当前 Agent 扫描对应历史，单次 Worker 批量读取头尾固定大小摘要并缓存 30 秒，可见行只展示已完成的摘要，切换时丢弃过期结果；扫描/导出/删除/详情分页读取全程 RunWorker 入 worker 线程；详情首次从文件尾读取最近 50 条并定位末条，实际向上滚动接近顶部时才反向加载上一页并保持视口锚点，消息通过 StateList + VirtualList 虚拟化且以文件偏移保持稳定身份，行使用无阴影的不透明轻量表面）/ about_page（关于，顶部莲花 logo 卡）/ common（岛屿原语、页面骨架/卡片/弹窗卡片、providerStore() 全局实例、ToolIcon 图标资源对）/ providers_page（5 工具共用供应商页：工具图标栏在岛屿内部顶部（ToolBar）+ 官方常驻卡首位 + 卡片列表，卡片三段式：左信息列 ｜ 中间状态列（延迟/用量，内容与操作组之间，空则塌缩）｜ 右操作图标组（切换/联通检测/编辑/用量配置/复制/删除为自绘图标 IconButton + Tooltip，swap/activity/edit/gauge/copy/trash.svg，用量刷新 refresh.svg），模型列表/用量/连通检测经 HuxerUI HttpClient；编辑/新增是整页表单 ProviderFormPage（完整 URL switch + 上游格式 Select；关闭 switch 时按格式追加 /anthropic 或 /v1），用量查询配置是独立整页 UsageFormPage（每个供应商独立 usageEnabled switch 与 usageRefreshMinutes，统一配置字段；formTarget 多模式：""/"new"/"usage:"+id/id），新增页内嵌预设区、模型行内 Select 下拉 + 卡片用量显示/轮询）/ settings_page（主题/路径/导入导出/关于）/ ui.h（内部声明） |
| `src/app.cpp` | 普通 TU | `Application{AppRoot, AppOptions}`（Custom chrome，标题栏 24pt，1080×720 / min 800×600） |
| 平台入口 | `platform/{linux,windows,macos}/main.cpp` | 薄入口 `huxerui::RunApplication()`（无 CLI 分流；顶层 CMake 按 WIN32/APPLE/Linux 分支选用） |

顶级页面导航已从左侧图标栏收拢到自定义标题栏：莲花锚点在标题栏正中，
hover 时在屏幕中央展开径向导航盘，全部 8 个顶级页面图标围绕莲花排列，
点击更新共享的 `navPage` 并由 `IndexedPages` 切换页面；主内容区不再为侧栏
预留宽度。

## 领域层设计要点

- **live 文件**（被切换工具实际读取的文件）：claude-code 切换 = 深合并
  `~/.claude/settings.json` 的 `env.ANTHROPIC_BASE_URL` / `ANTHROPIC_AUTH_TOKEN`
  （model 非空时写 `ANTHROPIC_MODEL`；haiku/sonnet/opusModel 三档映射非空时
  写 `ANTHROPIC_DEFAULT_HAIKU/SONNET/OPUS_MODEL`），permissions 等其余字段
  原样保留；codex
  切换 = 深合并 `~/.codex/auth.json` 的 `OPENAI_API_KEY`，且
  `Provider.codexConfigToml` 非空时**整体替换** `~/.codex/config.toml`（TOML
  不做结构化合并，原文即模板），`Provider.model` 非空时再**行级重写**
  config.toml 顶层 `model` 键（只动第一个 `[` 节之前：替换未注释行/注释行，
  都没有则开头插入；`applyCodexModel`/`parseCodexModel`，importLive 也会
  收回顶层 model）。opencode = opencode.json 顶层 provider map
  additive upsert（npm 段按 apiFormat 三档选 `@ai-sdk/anthropic` /
  `@ai-sdk/openai` / `@ai-sdk/openai-compatible`；官方文件允许 JSON5 注释，
  改写用 readJsonStrict 解析失败抛错、绝不碰原文件）；pi = models.json
  providers upsert（api 字段三档映射 anthropic→anthropic-messages /
  openai-responses→openai-responses / 其余→openai-completions）+
  settings.json 深合并 defaultProvider/defaultModel（目录 0700、文件 0600）；
  dsh = `~/.dsh/settings.yaml` 行级改写：llm-pi-ai.providers 下 upsert
  `llmswitch-<id>` 手写路由条目（api 字段复用 pi 三档映射）+ 文件头
  agent-default-model 指向，密钥只写 `~/.dsh/.credentials.yaml`（顶层
  env 名→密钥 map，apiKeyEnv 引用，目录 0700、文件 0600）；两份 YAML
  都被 dsh 热监听 → 切换即时生效（needsRestart=false），restoreOfficial
  删 agent-default-model 块与 llmswitch-* 条目回到内置 deepseek-official
  路由；hermes = `~/.hermes/config.yaml` 行级改写：custom_providers 列表删
  旧 llmswitch-* 条目后追加新条目（api_mode 三档映射
  anthropic→anthropic_messages / openai-responses→codex_responses /
  其余→chat_completions），顶层 model 节写 provider（总是）与
  default（model 非空时），agent / mcp_servers / v12+ providers dict
  等其余节原样保留（目录 0700、文件 0600）；hermes 无官方默认态，
  restoreOfficial 抛错；
  claude desktop = 3p 直连（两份 claude_desktop_config.json 置
  deploymentMode=3p + configLibrary 固定 id profile/_meta.json，
  **Linux 不支持**）。inferenceModels = 主模型 + 三档映射条目：模型名是
  白名单 route id（claude-(sonnet|opus|haiku|fable)-*）直写 name，否则借
  该档安全 route id（haiku-4-5/sonnet-4-6/opus-4-8）；菜单显示名写入
  labelOverride，1M 能力按 supports1m 声明，实际请求模型保存在 Provider 的
  三档 *Model 字段（`claudeDesktopModelEntry`）。
- **apiFormat 三档**（models.cppm）：`openai-chat`（默认；`""`/旧值
  `"openai"`/未知值都归此档）/ `openai-responses` / `anthropic`。序列化存
  原值，判定/显示/映射一律经 `models::normalizeApiFormat` 归一 +
  `apiFormatLabel` 显示名，各处不各自解释字符串。
- **上游 URL 格式**：Provider 的 `upstreamFormat` 归一为 `anthropic` 或
  `openai`，关闭 `fullUrl` 时 `effectiveBaseUrl` 分别追加 `/anthropic` 或
  `/v1`；`fullUrl` 打开时保留输入地址原样。缺少新字段的旧配置按完整 URL
  兼容，避免历史地址重复追加。
- **用量查询**：Provider.usageEnabled + usageRefreshMinutes + usageUrl/usagePath/usageLabel
  （开关关闭或 usageUrl 空 = 不查，刷新间隔 0 = 仅手动，旧配置缺字段自动取默认
  10）；每个供应商在独立用量配置页控制开关和刷新间隔，HuxerUI HttpClient 异步 GET 后由 `extractByPath` 按点分
  路径（支持数组下标）取标量；官方用量模板表不写在代码里——数据随资源包发布
  （`resources/raw/usage_templates.json`，只收有官方文档的端点），用户可在
  `dataDir()/usage_templates.json` 放同格式文件整体覆盖（设置页展示路径），
  `models::parseUsageTemplates` 容错解析、`suggestUsageQuery` 按 baseUrl
  子串匹配。UI 侧：用量三字段在独立的 UsageFormPage
  （卡片 gauge 图标进入，formTarget = "usage:" + id；启用开关 + 编辑表单只保留其余
  字段、保存时沿用原用量配置），卡片显示 + 惰性 HuxerUI HTTP 查询：仅当前 Agent 列表进入或配置变更时
  检查刷新间隔，隐藏页与表单不查询，离开列表取消任务；返回时复用未到期缓存，
  持续停留可手动刷新。路由/统计页的定时刷新仅在对应顶级导航可见时运行
  （State 只在 UI 线程写）。
- **本地路由设置**：AppConfig.routerEnabled（启动自启）/ routerPort（默认
  15731）/ routerFailover / routerTools（逐 Agent 代理选择，旧配置默认全开），
  store 有对应 setter；AppRoot 首组合时自启（见 src/ui/app.cpp 的 Lifecycle）。
  LocalRouter 内部以 mutex 保护启用集合，运行中切换无需重启；禁用路径返回 403，
  且在 resolver、上游与统计之前短路。
- **文件安全约定**（store.cpp 匿名命名空间三件套）：`atomicWrite`（`.tmp` →
  rename，失败回落 remove+rename）；`readJsonOrNull`（解析失败把坏文件挪到
  `<file>.corrupt-<毫秒>` 再按无内容继续，绝不崩溃）；`backupLiveFile`（改写
  任何 live 文件前复制到 `backups/<tool>/<文件名>.<毫秒>.bak`，每工具每文件
  只留最近 10 份）。importFrom 前额外备份 config.json 本体。
- **首次导入**：`ProviderStore::load()` 在组为空且 live 文件存在时自动
  `importLive`——把当前生效配置收编成名为「当前配置」的 provider 并设为
  current（已有匹配项则复用不重复建）。
- **detectCurrent**：读 live 文件与组内 provider 匹配（claude 按
  baseUrl+apiKey，codex 按 apiKey），只读不改配置。
- **restoreOfficial**：恢复厂商原生状态（UI 入口 = 供应商列表首位的
  「官方」常驻卡，claude-code / claude / codex / zcode / dsh 等
  officialVendorName 非空的工具；opencode / pi / hermes 无官方
  默认态抛错）——claude-code 删 settings.json env 块的 ANTHROPIC_* 六键；
  codex 删 auth.json 的 OPENAI_API_KEY（删完为空对象则删文件），
  config.toml 仅当内容与组内某 provider 的模板（应用 model 后）完全一致
  才删除（用户手改过的文件不动）；claude desktop 删两份 config 的
  deploymentMode 键 + _meta.json 移除本应用条目/清 appliedId；dsh 删
  settings.yaml 的 agent-default-model 块与 llmswitch-* 手写路由条目，
  回到内置 deepseek-official 路由（.credentials.yaml 的密钥不代清）。
  改前照常备份，组 current 清空。
- **内置预设**：只收第三方兼容端点（官方厂商由常驻卡承担，不进预设）；
  opencode / pi / hermes 无官方厂商故只有第三方。
- **测试友好**：live 路径全部支持环境变量覆盖——
  `LLMSWITCH_CLAUDE_SETTINGS` / `LLMSWITCH_CODEX_AUTH` / `LLMSWITCH_CODEX_CONFIG`
  / `LLMSWITCH_OPENCODE_CONFIG` / `LLMSWITCH_PI_DIR` /
  `LLMSWITCH_DSH_SETTINGS` / `LLMSWITCH_DSH_CREDENTIALS` /
  `LLMSWITCH_HERMES_CONFIG` /
  `LLMSWITCH_CLAUDE_DESKTOP_DIR`（live 配置）；
  `LLMSWITCH_CLAUDE_JSON`（~/.claude.json，MCP）/
  `LLMSWITCH_SKILLS_STORE` / `LLMSWITCH_CLAUDE_SKILLS` / `LLMSWITCH_CODEX_SKILLS`
  （skills）/ `LLMSWITCH_CLAUDE_PROJECTS` / `LLMSWITCH_CODEX_SESSIONS`
  （会话）/ `LLMSWITCH_STATS_DIR`（路由统计目录，默认 dataDir()/router/）
  ——`~` 展开只认 HOME，dataDir 走 XDG_DATA_HOME/HOME；test_store 等用
  setenv 指到 `temp/llmswitch-test-<pid>` 即可完全隔离。
- ProviderStore **不强制单例**（测试可实例化）；UI 侧的全局实例在
  `src/ui/common.cpp::providerStore()`（首次访问即 load）。

## C++23 命名模块内联约定

`.cppm` 的命名模块 purview 不使用 `#include` 头文件的全局模块语义。class 体内定义的
成员函数不会自动获得全局模块下的隐式 `inline`；如果成员函数定义留在模块接口中且
希望作为接口内联函数使用，必须显式写 `inline`（`constexpr` / `consteval` 除外）。
普通命名空间函数在头文件和 `.cppm` 中都不会自动 `inline`，不能据此给所有函数加关键字。
`inline` 不是性能保证，实际是否内联需结合 `-O`、LTO/IPO、反汇编和基准确认。重量级
I/O、解析和 JSON 函数默认保留在 `.cpp` 中。

## 关键约定（改代码前必读）

1. **`import std;` 后禁止再 `#include` 标准头**。C/系统头放全局模块片段
   （`module;` 与 `module llmswitch.x;` 之间）。普通 UI .cpp / ui.h 头用哪个
   std 设施就自己 `#include` 哪个（注意 `std::println(stderr,...)` 的 `stderr`
   不在 std 模块里，需 `<cstdio>`）。
2. **UI 层遵守 skill 的 DSL 风格**：普通 .cpp、composable 不加 inline、View 按值
   传递、具名 View 链式调用前 `std::move`（`.With` 等是右值限定）。
3. **受控值以应用状态为权威**；`UseState` 只能在 `[[huxerui::composable]]`
   函数体内调用（普通函数/dialog 工厂 lambda 里建 State 是未定义行为），
   `State::Get()` 只读不改、改值走 `operator=` 写回；TextField 保留完整
   TextEditingValue；动态兄弟用稳定 `.Key(...)`（供应商卡 `.Key(id)`）。
4. **线程契约**：State 只在 UI 线程读写。store 无内部锁，**UI 线程独占**是
   设计前提——live 文件读写是微秒级本地 IO，CRUD/切换/导入导出直接在 UI
   线程回调里做；FilePicker 的 async API 恢复点本就在 UI 线程。阻塞网络调用
   （llmswitch.net 的 fetchModels / fetchUsage）经 SDK 自带的
   `huxerui::RunWorker` 派到 worker 线程（co_await 恢复点恒为 UI 线程，
   组合卸载自动取消）——**不要**从 Clash-Flux 复制 task_bridge.h，那是它
   在 SDK 提供 RunWorker 之前的自造轮子。**事件处理器内禁止同步写会导致
   点击节点被卸载的 State**——经 `tasks.Launch` + `co_await Delay(0)` 推迟
   （开弹窗、切页同理）。
5. **占位不能用 Spacer().With(Frame)**（Spacer 自带 Grow(1) 会平分空间）——
   用空 `Row{}`/`Column{}`；页面根要 `Grow(1.0F)` + `CrossAlign(Stretch)`。
6. **全局 revision 计数**：AppRoot 持有 `State<int> revision`，任何写库操作
   （含托盘切换、设置页导入）后 +1，驱动供应商页重读与托盘菜单重建
   （托盘 Lifecycle 以 revision 为依赖）。
7. **岛屿风**（对齐 Clash-Flux）：冷调石板主题（ui/app.cpp `AppDarkThemeSpec`
   「石墨」= 中性石墨海面 #101214 + 青蓝强调 #38BDF8 / `AppLightThemeSpec`
   「晴石」= 冷白海面 #FDFDFD + 天蓝强调 #2870D6；深色表面为中性冷灰阶、
   强调色是唯一彩色源；浅色强调色比参考图实测的亮蓝
   #4595F7 深一档，白字与描边对比度双双过 AA 正文线）。**强调色（primary）只用于可交互
   状态**——按钮/分段选中/开关/标题栏 hover 描边与辉光；分组标题等非交互
   文字用 on_surface / on_surface_variant，避免整页变蓝。设置页以单个太极
   选择器循环切换跟随系统/石墨/晴石（平衡态/石墨外环/晴石外环，悬停持续
   旋转、移出冻结当前角度），存值仍 system/dark/light。**顶级页面没有自己的
   卡片**：`PageScaffold` / `AgentPage` 只做内边距与滚动，页面与标题栏共用同一块
   窗口表面，两者的左右边距取同一个 `shellInset`（app.cpp），应用名与页面标题因此
   对齐；纵向同样不留缝隙——页面顶部内边距为 0、壳层标题栏与页面之间不声明
   `Spacing`，页面内容紧接标题栏下沿（标题栏高度由平台解析：Linux 把声明的 24
   抬到 32 最小高度，Windows 取 max(24, 系统标题高度)，`WindowTitleBar` 自己按
   解析值测量，页面排在它之后自然贴合，不要用硬编码高度去对齐）；层次感靠
   AmbientGlow + 下面分层的分区/卡片表达。浮层/弹窗 10pt、
   二级岛 6pt 圆角，表面色经 `ResolveIslandTheme(theme)` 语义层级取，
   不直接用 surface_container_*。删除确认用内置
   `dialog.Show(title, message, positive, negative, ...)`（DialogStyle 已在
   MinimalThemed 里主题化）。
   卡片克制使用：页面常规分区用 `PageSection`（标题 + 内容平铺）+
   `SectionDivider` 发丝线划分；重复列表条目（供应商/技能/MCP/会话行）用
   无边框 `QuietCard`（同 raised 表面/6pt 圆角/内边距）靠表面色差分层；
   通用 `Card`（raised 表面、6pt 圆角、1pt `outline_soft` 描边、内边距的
   单层轻量样式）只用于少数强调块（如关于页头部），不叠加裁剪层；弹窗
   仍保留规则边框以保证浮层识别度。
8. **图标契约**：所有 24×24 功能 SVG 必须遵守
   `resources/README.md` 的规范——每个语义只保留一套 `#FFFFFF` 无色
   alpha-mask，深浅主题由运行时 tint 自适应，选中态由承载底块表达；禁止
   `_selected/_dark/_light` 重复轮廓；品牌图标不得改变官方轮廓。
   CMake 配置期强制校验（无色 mask + 无重复轮廓）。
9. **响应式**：`UseViewportClass()` Compact(<600) 收窄页面内边距
   （`PageScaffold` / `AgentPage` 的 medium↔large，顶部恒为 0）与壳层
   `shellInset`（app.cpp，必须同值）；窗口最小 800×600。
10. **资源一律 RAII 包裹**（权威规则见 `AGENTS.md` 的「资源与 RAII 硬约束」）：
    禁止裸 `new`/`delete`/`malloc`/`free`；锁只用 `lock_guard`/`unique_lock`/
    `scoped_lock`；POSIX fd、`popen` 管道、Windows `HKEY`、临时文件、线程都
    必须先有所有者再使用，错误分支只 `return`，由析构释放。盘点与范本见下节
    「资源所有权与 RAII」。

## 资源所有权与 RAII

全项目盘点（2026-09 复核）：**所有获取即需释放的资源都有 RAII 所有者**，
新增资源类型必须照此先写所有者再写使用者。

| 资源 | 所有者 | 位置 |
|------|--------|------|
| 堆内存 | `std::unique_ptr<Impl>` / `shared_ptr` / 标准容器（无裸 new/delete） | `router.cpp`、`usage.cpp`、`ui/router_transport.cpp` |
| POSIX fd（open/socket/accept/connect） | `UniqueFd`（析构 `close`，可移动、禁拷贝；`Valid()` 判失败） | `src/single_instance.cpp` |
| `popen` 管道 | `UniquePipe = unique_ptr<FILE, PipeCloser>`（析构 `pclose`） | `src/config.cppm` 的 `systemPrefersDark()` |
| Windows 注册表键 | `UniqueRegKey`（析构 `RegCloseKey`） | `src/config.cppm`、`platform/windows/package/src/app.cpp` |
| 临时文件 `<file>.tmp` | `TempFileGuard`（成功 rename 后 `Release()`，否则析构删除） | `src/store.cpp`、`src/mcp.cpp`、`src/skills.cpp`、`src/router.cpp`、`src/usage.cpp` |
| 线程 | 持有者析构 `stop()` + `join()`；创建失败回滚绑定状态 | `LocalRouter::Impl`（`src/router.cpp`）、`single_instance::Coordinator`、测试 `FakeUpstream` |
| 锁 / 条件变量 | `std::lock_guard` / `std::unique_lock`（永不手写 lock/unlock） | `router.cpp`、`usage.cpp`、`ui/router_transport.cpp`、`single_instance.cpp` |
| HuxerUI 句柄 / 任务 / 订阅 | SDK 的 `TaskScope`、`ApplicationHandle`、`WindowHandle`、`SystemTrayHandle` 与 `Lifecycle` 返回的 disposer；composable 卸载自动取消 | `src/ui/*.cpp`（`UseTaskScope`、`Lifecycle`、`UseApplication().Clipboard()`） |

要点与易错处：

- 文件流 `std::ifstream` / `std::ofstream` 本身就是 RAII，不要为了「统一」再包一层；
  真正需要守卫的是它们的**副作用残留**（`.tmp` 半成品、备份、锁文件）。
- 平台句柄优先「带自定义 deleter 的 `unique_ptr`」，只有需要 `Out()` 参数风格
  （如 `RegOpenKeyExA` 的 `HKEY*`）才写极小 Guard 类；不要引入堆分配以外开销。
- **平台专用 closer 必须和它的 `#if` 分支绑在一起**：`UniquePipe` 调用的
  `popen`/`pclose` 是 POSIX，MSVC 全局命名空间里没有这两个名字。0.1.37 首轮
  CI 就是把 `PipeCloser` 提到 `#if` 之外，导致 `config.cppm` 在
  `test_store` 里编译失败（`C2039` / `C3861`），两个 Windows job 全挂而
  Linux/macOS 全绿——本地是 Linux 时这类错误只能靠 CI 暴露，改动跨平台
  代码后要人工核对每个 `#if` 分支里的符号在目标平台是否存在。
- 线程创建本身可能抛（资源耗尽）：`LocalRouter::Impl::start` 在 catch 里复位
  `isRunning` / `boundPort` 并 `server.stop()`，避免「报错但端口已占」的假运行态。
- `UniqueFd::Reset(int fd = -1)` 先关旧的再接管新的，`StartOrActivate` 重入安全由此保证。
- 已知的有意例外：`window.OnCloseRequest` 消费关闭、`Lifecycle` 返回清理闭包这类
  回调注册，必须返回可逆的 disposer（如 `single_instance::ClearActivationHandler`），
  不得注册后无人撤销。

## 已知取舍（读代码遇到别当 bug 修）

- **store 无锁与 router resolver 竞态窗口**：ProviderStore 无内部锁、UI 线程
  独占是全局契约；router_page.cpp 的 routerInstance() resolver 会被 router
  的 httplib 后台线程调用，此刻对 providerStore() 做只读组快照拷贝，与 UI
  线程写操作存在理论并发窗口（已知的极小概率取舍，router.cpp 要求 resolver
  线程安全且快速；代码注释已标记）。
- **codex MCP 的 TOML 重写会丢未收编的手写 [mcp_servers.*] 节**：llmswitch.mcp
  按行级 section 重写 config.toml——丢弃全部既有 [mcp_servers.*] 节后按当前
  启用清单重新生成（其余内容原样保留）。用户手写的 mcp_servers 若未先
  importFromTool 收编进 mcp.json，首次启停后丢失。
- **opencode 配置不支持 JSON5 注释**：opencode.json / 顶层 mcp 改写都走严格
  JSON 解析，带注释的官方文件会抛中文错且绝不碰原文件（让用户手动去注释）。
- **MCP 不支持 claude desktop / pi / dsh / hermes**：setEnabled/importFromTool 对这些
  工具抛「该工具暂不支持 MCP 管理」。
- **尚未接入剪贴板 / 打开浏览器交互**：上游 `d1d2daa` 已提供应用层
  `Clipboard` 服务，但路由页的各工具接入地址目前仍是等宽纯文本；关于页链接
  也仍不可点击跳转（尚无应用层打开浏览器入口）。接入复制按钮时使用
  `UseApplication().Clipboard()`，并按 `IsAvailable()` 控制可用状态。

## 多平台 / CI

- 平台入口：platform/{linux,windows,macos}/main.cpp 均为薄 GUI 入口（无 CLI
  分流）；顶层 CMakeLists 的 WIN32/APPLE 分支按平台把对应入口追加进 SOURCES。
- Windows 自定义安装向导（蓝本 Clash-Flux 同名机制）：platform/windows/
  huxerui.cmake 定义 `huxerui_configure_windows_project_package`，
  `huxerui package windows`（HUXERUI_PACKAGE=ON）时把 MSI + Burn 捆绑包
  （package/Package.wxs.in / Bundle.wxs.in）与 HuxerUI 托管安装器 UI
  （package/src/，品牌面板 + 简中/繁中/英文 strings）接进构建；日常构建
  零开销（函数内 `if (NOT HUXERUI_PACKAGE) return()`）。需要含
  `huxerui_add_windows_installer` 的 HuxerUI 源码/SDK（0.2.0 之后；当前 CI
  固定的 `0c51262` 已满足）。
- **触发策略：推 main、开 PR、推 `v*` tag 都构建**（tag 额外触发 release job）。
  不要为"省额度"把 main 的构建收掉——本仓库是 public，**标准 GitHub-hosted
  runner 的 Actions 用量对 public 仓库免费**（计费文档：*usage is free for
  self-hosted runners and for public repositories that use standard
  GitHub-hosted runners*；额度表里的 500MB artifact / 2000 minutes 是 private
  仓库的 plan 配额），收费的是 **larger runners**（4-core 起，*not free for
  public repositories*）——本仓库只用标准 runner。所以每次 push 构建的边际成本
  是 0，换来 main 上每笔提交的三平台回执；只有 `docs/**`、`**.md` 走
  `paths-ignore` 跳过。`workflow_dispatch` 手动触发只构建（release job 自带
  `startsWith(github.ref, 'refs/tags/v')` 判断，不会因为手动跑而发布）。
- `.github/workflows/build.yml`（蓝本 Clash-Flux 同名文件，按其已跑通配方
  适配）：三个桌面 job + release。build-linux（ubuntu:26.04 容器 + clang-21/
  libc++-21 + pip cmake==4.4.2 + libc++.modules.json 路径改写 + gtk4/epoxy/
  libsoup3 开发包，正式）；build-windows（MSVC + choco ninja）与 build-macos
  （brew llvm + 手写 libc++.modules.json + 仓库跟踪的 P0960 补丁）；三个平台均为
  发布门禁，必须完成编译、测试和打包。
- 三个平台构建都把 HuxerUI 上游钉在 commit `0c5126235d43c2b703166bcc00781b850f2d1c39`
  （含 Application/Window 所有权重构与跨平台 HTTP 流式请求修复）
  clone 到 third_party/huxerui 走源码通道；TLS 由平台栈提供，CI 不再安装
  OpenSSL；无 mihomo/Android（蓝本相关步骤已删）。
- Windows 主窗口图标和 macOS Objective-C++ `CrossAlign` 兼容修改分别保存在
  `cmake/patches/huxerui-windows-icon.patch` 与
  `cmake/patches/huxerui-macos-p0960.patch`；性能探针补丁仅按需手动应用。
- 打包：Linux tar.gz（二进制 + llm-switch.resources + lib/libhuxerui.so +
  libc++ 三件套 + patchelf `$ORIGIN/lib`）、Windows zip（exe + 旁挂 dll +
  resources）、macOS tar.gz（.app bundle）；push tag `v*` 时 release job
  （job 级 `contents: write`）下载三个平台产物，经
  softprops/action-gh-release 挂到 release；只有三个平台 job 全部成功且产物存在
  时才发布。

## 里程碑状态（2026-09-06）

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
- ✅ 新增 Gemini CLI / Qwen Code / ZCode 三个 agent（2026-09-13）：注册表
  扩到 8 工具（routerTools 默认清单同步）。gemini/qwen（gemini-cli 系）：
  认证与端点写 `~/.{gemini,qwen}/.env` 行级 upsert（GEMINI_API_KEY/
  GOOGLE_GEMINI_BASE_URL/GEMINI_MODEL 与 OPENAI_* 三键，保留其余变量与
  注释），settings.json 深合并 security.auth.selectedType（gemini-api-key/
  openai）；zcode：`~/.zcode/v2/config.json` provider map upsert
  `llmswitch:<id>` 条目（apiFormat 映射 kind）并 enabled 互斥停用其余，
  restoreOfficial 重启 builtin:*。均接入 detect/import/restore 与 router
  逐工具开关；MCP/技能/会话暂不涉及（页面自动优雅降级）。新增三枚自绘
  图标（gemini 四角星 / qwen 六边环 / zcode Z 字）。zcode 的 load() 首次
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
- ⬜ 待做：订阅站端点可能随各家调整，升级版本时需复核；无 CLI 分流、
  无单实例/开机自启。
