# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.
根目录 `AGENTS.md` 是所有 AI agent 共用且优先级更高的强制规范；开始工作前必须
完整读取。本文件提供架构细节与实现上下文，不重复维护跨工具规则。

llm-switch 是 **cc-switch**（GitHub: farion1231/cc-switch）的 **C++23 模块化重写**：
管理多款 AI agent 工具的供应商配置切换——领域层已泛化到 5 个工具
（claude-code / claude desktop / codex / opencode / pi，注册表见
`models::toolRegistry()`），把选中的供应商写进工具的 live 配置文件
（`~/.claude/settings.json` 的 env 块 / `~/.codex/auth.json` + `config.toml` /
opencode.json additive upsert / pi 的 models.json + settings.json /
Claude Desktop 3p profile 组），并提供收编、备份、导入导出。在此之上还有：
本地路由引擎（127.0.0.1 反代到当前供应商 + 请求统计）、供应商用量查询、
MCP 服务器统一清单、Skills 中央库同步、历史会话管理。用 **HuxerUI**（组件式
声明 UI）做桌面壳，全程 C++；网络依赖 curl/OpenSSL（拉模型列表与用量查询，
llmswitch.net）+ cpp-httplib（本地路由服务器，llmswitch.router），无数据库。
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
  （git clone 上游，跟主干拉取；当前钉在 `9eff3c8`，含应用层 Clipboard
  服务、TreeView，以及 TextField 可交互 TrailingIcon——密码框内置眼睛按钮）。
- 剪贴板通过 composable 内的 `UseService<Clipboard>()` 获取；事件处理器可捕获
  service 并同步调用 `IsAvailable()` / `ReadText()` / `WriteText()`，不要从 worker
  线程调用。TreeView 使用 `TreeView<Node>(roots, factory, item_info)`，必须放在有界
  的垂直视口中，并通过状态重组声明来反映节点变化。
- 源码通道优先用本地构建的宿主工具（`third_party/huxerui-tools/linux/x86_64/
  {hcg,hrc}`；本仓库暂无此目录，缺失时自然回落上游预置，逻辑保留）。
- UI 层是**普通 .cpp**（不要 .cppm：codegen 只扫 .cpp/.cc/.cxx）；composable
  函数不加 `inline`；入口根在 src/app.cpp + src/ui/app.cpp。
- **composable 函数体内不能有条件编译**（hcg 不支持 #ifdef 穿行）——版本号等
  宏在文件作用域先展开成常量（settings_page.cpp 的 kAboutText）。
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
  静态库目标 `llmswitch_json`）；curl 8.22.0 以 tarball vendor 构建
  （OpenSSL 后端静态库；OpenSSL 优先系统包，Linux x86_64 回落
  `third_party/tarballs/openssl-3.5.1-linux-x86_64.tar.gz` 静态包，解析段在
  `add_subdirectory(third_party)` 之前）；cpp-httplib 0.54.1 单头提交在
  `third_party/httplib/`（INTERFACE 目标 `llmswitch_httplib`，仅
  llmswitch.router 用）；HuxerUI 0.2.0 走双通道（见上）。
  无 SQLite/IXWebSocket。
- 测试目标独立（7 个，均无框架、断言失败计数非零即败）：`test_smoke`
  （编译+运行冒烟）、`test_store`（领域层；全程 setenv 隔离到临时目录）、
  `test_net`（parseModelIds / extractByPath 纯函数；不测真实网络）、
  `test_router`（httplib 假上游：转发/换 key/故障转移/统计日志）、
  `test_mcp` / `test_skills` / `test_sessions`。测试目标经 FILE_SET 显式
  追加领域模块接口 + 对应实现单元（glob 只进 app 目标），链接
  `llmswitch_json`（test_net 另链 curl/OpenSSL，test_router 另链
  `llmswitch_httplib`）。

## 架构

| 模块 | 文件 | 职责 |
|------|------|------|
| `llmswitch.config` | `src/config.cppm` | 数据目录（~/.local/share/llm-switch）/ config.json、backups/、mcp.json、skills-store/、router/requests.jsonl 路径 / live 配置与会话/技能目录解析（全部 LLMSWITCH_* 环境变量可覆盖）/ 深色检测 |
| `llmswitch.models` | `src/models.cppm` | 工具注册表（ToolSpec/toolRegistry/findTool：claude-code/claude/codex/opencode/pi；needsModel/hasApiFormat/hasModelMappings 三标记驱动表单适配）+ Provider/ProviderGroup/AppConfig（groups 以注册表 id 为键的 map，旧格式顶层 claude/codex 自动迁移；router/usage 设置字段，routerTools 保存逐 Agent 代理选择；Provider 含 modelFetchUrl、haiku/sonnet/opusModel 三档映射与 upstreamFormat/fullUrl URL 模式）+ JSON 序列化 + 内置预设（builtinPresets）+ 官方厂商名（officialVendorName：claude 系/codex 有官方常驻卡）+ apiFormat 三档归一（normalizeApiFormat/apiFormatLabel）+ 上游 URL 归一/后缀（normalizeUpstreamFormat/upstreamFormatSuffix/effectiveBaseUrl）+ 用量端点模板（suggestUsageQuery） |
| `llmswitch.store` | `src/store.cppm` + `src/store.cpp` | ProviderStore：config.json 读写、CRUD、switchTo 按工具 id 分发五个 writer（原子写+备份）、restoreOfficial 恢复厂商原生状态（claude-code/claude/codex）、detectCurrent/importLive、导出导入、theme/usage/router 与逐 Agent 路由设置 setter |
| `llmswitch.net` | `src/net.cppm` + `src/net.cpp` | fetchModels（curl 阻塞调用，调用方负责线程；anthropic 走 {base}/v1/models 双鉴权头，其余 {base}/models Bearer；10s 超时）+ fetchModelsFromUrl（完整模型列表 URL 覆盖默认端点）+ parseModelIds 纯函数（data/models 两种形状，去重保序）+ fetchUsage（GET+Bearer 拉用量）/ extractByPath（点分路径+数组下标取标量）+ pingLatencyMs（连通检测：GET baseUrl 不带鉴权，任何 HTTP 响应算连通，返回 CURLINFO_TOTAL_TIME 毫秒） |
| `llmswitch.router` | `src/router.cppm` + `src/router.cpp` | LocalRouter：cpp-httplib 服务器监听 127.0.0.1，`/<tool>/` 前缀路由到该组 current 供应商的实际 URL（按 upstreamFormat 追加 /anthropic 或 /v1，fullUrl 时原样），替换鉴权头，线程安全的逐工具开关运行中即时生效（禁用返回 403，不访问上游/统计），可选故障转移（429/5xx/连接失败按组内顺序试下一个）；RequestLog/StatsSnapshot 统计，每请求追加 JSONL（dataDir()/router/requests.jsonl），启动回填内存环形缓冲（最多 1000 条） |
| `llmswitch.mcp` | `src/mcp.cppm` + `src/mcp.cpp` | MCP 服务器统一清单（SSOT = dataDir()/mcp.json）；启停 = 写/删工具 live 配置条目：claude-code → ~/.claude.json 顶层 mcpServers 深合并、codex → config.toml 行级 [mcp_servers.*] section 重写、opencode → opencode.json 顶层 mcp；claude/pi 不支持（抛中文错） |
| `llmswitch.skills` | `src/skills.cppm` + `src/skills.cpp` | Skills 中央库（dataDir()/skills-store/<name>/：SKILL.md + 附带文件）+ create_symlink 同步到 ~/.claude/skills 与 ~/.codex/skills；合并视图（中央库/已链接/仅工具侧） |
| `llmswitch.sessions` | `src/sessions.cppm` + `src/sessions.cpp` | 历史会话扫描：~/.claude/projects/<项目>/*.jsonl 与 ~/.codex/sessions/<年>/<月>/<日>/*.jsonl；列表（mtime 倒序）/删除（限已知 sessions 根之下，越界抛错）/导出；标题/行数提取 best-effort 不抛 |
| `llmswitch::ui`（普通 C++） | `src/ui/*.cpp` | app（壳：太极水墨主题 InkDark「玄墨」/InkLight「宣纸」+标题栏太极标+顶级 8 区块图标侧栏+IndexedPages+托盘（太极图图标）+关闭最小化到托盘+路由自启）/ agent_page（薄宿主：持 currentTool State + ProvidersPage .Key(tool) 宿主）/ router_page（路由总开关+逐 Agent 代理开关+已启用接入地址+最近请求日志，含 routerInstance() 单例）/ stats_page（统计汇总）/ mcp_page / skills_page / sessions_page（会话管理：过滤用图标组——全部/Claude Code/Codex 与 Agent 页同套图标+选中底块；扫描/导出/删除全程 RunWorker 入 worker 线程，加载请求代次阻止旧结果覆盖新筛选，重载不清空旧列表）/ about_page（关于，顶部太极 logo 卡）/ common（岛屿原语、页面骨架/卡片/弹窗卡片、providerStore() 全局实例、ToolIcon 图标资源对）/ providers_page（5 工具共用供应商页：工具图标栏在岛屿内部顶部（ToolBar）+ 官方常驻卡首位 + 卡片列表，卡片三段式：左信息列 ｜ 中间状态列（延迟/用量，内容与操作组之间，空则塌缩）｜ 右操作图标组（切换/联通检测/编辑/用量配置/复制/删除为自绘图标 IconButton + Tooltip，swap/activity/edit/gauge/copy/trash.svg，用量刷新 refresh.svg），联通检测经 net::pingLatencyMs；编辑/新增是整页表单 ProviderFormPage（完整 URL switch + 上游格式 Select；关闭 switch 时按格式追加 /anthropic 或 /v1），用量查询配置是独立整页 UsageFormPage（formTarget 多模式：""/"new"/"usage:"+id/id），新增页内嵌预设区、模型行内 Select 下拉 + 卡片用量显示/轮询）/ settings_page（主题/用量查询/路径/导入导出/关于）/ ui.h（内部声明） |
| `src/app.cpp` | 普通 TU | `Application{AppRoot, AppOptions}`（Custom chrome，标题栏 24pt，1080×720 / min 800×600） |
| 平台入口 | `platform/{linux,windows,macos}/main.cpp` | 薄入口 `huxerui::RunApplication()`（无 CLI 分流；顶层 CMake 按 WIN32/APPLE/Linux 分支选用） |

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
- **用量查询**：Provider.usageUrl/usagePath/usageLabel（usageUrl 空 = 不查）+
  全局 AppConfig.usageEnabled/usageRefreshMinutes（0=仅手动，旧配置缺字段
  自动取默认 true/10）；net::fetchUsage 同步阻塞 10s，extractByPath 按点分
  路径（支持数组下标）取标量；`suggestUsageQuery` 只内置有官方文档的
  DeepSeek（/user/balance）。UI 侧：用量三字段在独立的 UsageFormPage
  （卡片 gauge 图标进入，formTarget = "usage:" + id；编辑表单只保留其余
  字段、保存时沿用原用量配置），卡片显示 + RunWorker 轮询（State 只在
  UI 线程写）。
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
  「官方」常驻卡，仅 claude-code / claude / codex；opencode / pi 无官方
  默认态抛错）——claude-code 删 settings.json env 块的 ANTHROPIC_* 六键；
  codex 删 auth.json 的 OPENAI_API_KEY（删完为空对象则删文件），
  config.toml 仅当内容与组内某 provider 的模板（应用 model 后）完全一致
  才删除（用户手改过的文件不动）；claude desktop 删两份 config 的
  deploymentMode 键 + _meta.json 移除本应用条目/清 appliedId。改前照常
  备份，组 current 清空。
- **内置预设**：只收第三方兼容端点（官方厂商由常驻卡承担，不进预设）；
  opencode / pi 无官方厂商故只有第三方。
- **测试友好**：live 路径全部支持环境变量覆盖——
  `LLMSWITCH_CLAUDE_SETTINGS` / `LLMSWITCH_CODEX_AUTH` / `LLMSWITCH_CODEX_CONFIG`
  / `LLMSWITCH_OPENCODE_CONFIG` / `LLMSWITCH_PI_DIR` /
  `LLMSWITCH_CLAUDE_DESKTOP_DIR`（live 配置）；
  `LLMSWITCH_CLAUDE_JSON`（~/.claude.json，MCP）/
  `LLMSWITCH_SKILLS_STORE` / `LLMSWITCH_CLAUDE_SKILLS` / `LLMSWITCH_CODEX_SKILLS`
  （skills）/ `LLMSWITCH_CLAUDE_PROJECTS` / `LLMSWITCH_CODEX_SESSIONS`
  （会话）/ `LLMSWITCH_STATS_DIR`（路由统计目录，默认 dataDir()/router/）
  ——`~` 展开只认 HOME，dataDir 走 XDG_DATA_HOME/HOME；test_store 等用
  setenv 指到 `temp/llmswitch-test-<pid>` 即可完全隔离。
- ProviderStore **不强制单例**（测试可实例化）；UI 侧的全局实例在
  `src/ui/common.cpp::providerStore()`（首次访问即 load）。

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
7. **岛屿风**（对齐 Clash-Flux）：太极水墨主题（ui/app.cpp InkDark「玄墨」/
   InkLight「宣纸」：暖调墨色阶 + 宣纸白/浓墨主色 + 朱砂 error；设置页主题
   设置页以单个太极选择器循环切换跟随系统/玄墨/宣纸（平衡态/玄墨外环/
   宣纸外环，悬停持续旋转、移出冻结当前角度），存值仍 system/dark/light）；
   一级轻岛 10pt / 二级岛 6pt 圆角，半透明表面色经
   `ResolveIslandTheme(theme)` 语义
   层级取，不直接用 surface_container_*。删除确认用内置
   `dialog.Show(title, message, positive, negative, ...)`（DialogStyle 已在
   MinimalThemed 里主题化）。
   通用 `Card` 不画规整 Border：以 `ink_card_frame.svg` 的断续墨线、飞白和
   角部淡晕作为卡片自身边界；弹窗仍保留规则边框以保证浮层识别度。
8. **水墨图标契约**：所有 24×24 功能 SVG 必须遵守
   `resources/README.md` 的水墨规范——每个语义只保留一套 `#FFFFFF` 无色
   alpha-mask，深浅主题由运行时 tint 自适应，选中态由承载底块表达；禁止
   `_selected/_dark/_light` 重复轮廓；品牌图标不得改变官方轮廓。
   CMake 配置期强制校验（无色 mask + 无重复轮廓）。
9. **响应式**：`UseViewportClass()` Compact(<600) 收窄侧栏(44pt)/一级岛内边距
   （PageScaffold）；窗口最小 800×600。

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
- **MCP 不支持 claude desktop / pi**：setEnabled/importFromTool 对这两个工具
  抛「该工具暂不支持 MCP 管理」。
- **尚未接入剪贴板 / 打开浏览器交互**：上游 `9eff3c8` 已提供应用层
  `Clipboard` 服务，但路由页的各工具接入地址目前仍是等宽纯文本；关于页链接
  也仍不可点击跳转（尚无应用层打开浏览器入口）。接入复制按钮时使用
  `UseService<Clipboard>()`，并按 `IsAvailable()` 控制可用状态。

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
  固定的 `9eff3c8` 已满足）。
- `.github/workflows/build.yml`（蓝本 Clash-Flux 同名文件，按其已跑通配方
  适配）：三个桌面 job + release。build-linux（ubuntu:26.04 容器 + clang-21/
  libc++-21 + pip cmake==4.4.2 + libc++.modules.json 路径改写 + gtk4/epoxy/
  libsoup3 开发包 + libssl-dev，正式）；build-windows（MSVC + choco ninja +
  choco openssl）与 build-macos
  （brew llvm + 手写 libc++.modules.json + 内联 P0960 补丁）；三个平台均为
  发布门禁，必须完成编译、测试和打包。
- 三个 job 都把 HuxerUI 上游钉在 commit `9eff3c8`（含 Clipboard / TreeView）
  clone 到 third_party/huxerui 走源码通道；OpenSSL 三平台各自提供
  （linux apt libssl-dev / windows choco openssl + `-DOPENSSL_ROOT_DIR` /
  macos brew openssl@3 + `-DOPENSSL_ROOT_DIR`）；无 mihomo/Android
  （蓝本相关步骤已删）。
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
  desktop / codex / opencode / pi），config.json 改 groups map（旧格式自动
  迁移），store 按工具 id 分发五个 writer + 各自 detectCurrent/importLive，
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
  - 用量查询：Provider.usageUrl/usagePath/usageLabel + net::fetchUsage /
    extractByPath + 全局 usageEnabled/usageRefreshMinutes + DeepSeek 内置
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
  1pt + ClipChildren）、标题栏应用名旁加朱砂印章「易」（固定印泥红底
  宣纸白字，两主题通用）、整窗背景改用深浅两套全景水墨画卷
  （ink_backdrop_dark/light.svg：远山、朱砂落日、归鸟、竹枝和飞瀑；
  Stack 底层以 Cover 覆盖窗口边缘，中央留白，轻岛屿允许环境景物隐约透出）。
  书法标题字未做：系统无 CJK 衬线字体，
  不为标题打包字体文件。
- ⬜ 待做：codex 内置预设仅
  OpenRouter/DeepSeek 两家可扩充；无 CLI 分流、无单实例/开机自启。
