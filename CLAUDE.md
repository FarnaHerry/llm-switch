# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

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
  `-DLLMSWITCH_HUXERUI_FORCE_SDK=ON`。本机走**已安装 SDK** 通道。
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
cmake -B build -G Ninja            # 配置（默认 Release；调试加 -DCMAKE_BUILD_TYPE=Debug）
cmake --build build -j             # 编译（app + 7 个测试目标）
ctest --test-dir build             # 冒烟 + 领域层测试（smoke/store/net/router/mcp/skills/sessions）
./run.sh                           # 启动 GUI（INTEL_FORCE_PROBE=1）
huxerui run linux                  # HuxerUI CLI 流程（构建到 .huxerui/build/linux/）
```

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
| `llmswitch.models` | `src/models.cppm` | 工具注册表（ToolSpec/toolRegistry/findTool：claude-code/claude/codex/opencode/pi）+ Provider/ProviderGroup/AppConfig（groups 以注册表 id 为键的 map，旧格式顶层 claude/codex 自动迁移；router/usage 设置字段）+ JSON 序列化 + 内置预设（builtinPresets）+ apiFormat 三档归一（normalizeApiFormat/apiFormatLabel）+ 用量端点模板（suggestUsageQuery） |
| `llmswitch.store` | `src/store.cppm` + `src/store.cpp` | ProviderStore：config.json 读写、CRUD、switchTo 按工具 id 分发五个 writer（原子写+备份）、detectCurrent/importLive、导出导入、theme/usage/router 设置 setter |
| `llmswitch.net` | `src/net.cppm` + `src/net.cpp` | fetchModels（curl 阻塞调用，调用方负责线程；anthropic 走 {base}/v1/models 双鉴权头，其余 {base}/models Bearer；10s 超时）+ parseModelIds 纯函数（data/models 两种形状，去重保序）+ fetchUsage（GET+Bearer 拉用量）/ extractByPath（点分路径+数组下标取标量） |
| `llmswitch.router` | `src/router.cppm` + `src/router.cpp` | LocalRouter：cpp-httplib 服务器监听 127.0.0.1，`/<tool>/` 前缀路由到该组 current 供应商的 baseUrl，替换鉴权头，可选故障转移（429/5xx/连接失败按组内顺序试下一个）；RequestLog/StatsSnapshot 统计，每请求追加 JSONL（dataDir()/router/requests.jsonl），启动回填内存环形缓冲（最多 1000 条） |
| `llmswitch.mcp` | `src/mcp.cppm` + `src/mcp.cpp` | MCP 服务器统一清单（SSOT = dataDir()/mcp.json）；启停 = 写/删工具 live 配置条目：claude-code → ~/.claude.json 顶层 mcpServers 深合并、codex → config.toml 行级 [mcp_servers.*] section 重写、opencode → opencode.json 顶层 mcp；claude/pi 不支持（抛中文错） |
| `llmswitch.skills` | `src/skills.cppm` + `src/skills.cpp` | Skills 中央库（dataDir()/skills-store/<name>/：SKILL.md + 附带文件）+ create_symlink 同步到 ~/.claude/skills 与 ~/.codex/skills；合并视图（中央库/已链接/仅工具侧） |
| `llmswitch.sessions` | `src/sessions.cppm` + `src/sessions.cpp` | 历史会话扫描：~/.claude/projects/<项目>/*.jsonl 与 ~/.codex/sessions/<年>/<月>/<日>/*.jsonl；列表（mtime 倒序）/删除（限已知 sessions 根之下，越界抛错）/导出；标题/行数提取 best-effort 不抛 |
| `llmswitch::ui`（普通 C++） | `src/ui/*.cpp` | app（壳：MinimalDark/Light 主题+标题栏+顶级 8 区块图标侧栏+IndexedPages+托盘+路由自启）/ agent_page（Agent 管理：二级工具图标栏+ProvidersPage 宿主）/ router_page（路由控制+接入地址+最近请求日志，含 routerInstance() 单例）/ stats_page（统计汇总）/ mcp_page / skills_page / sessions_page / about_page（关于）/ common（岛屿原语、页面骨架/卡片/弹窗卡片、providerStore() 全局实例）/ providers_page（5 工具共用供应商页，表单按 ToolSpec 适配 + 用量三字段与卡片用量显示/轮询）/ settings_page（主题/用量查询/路径/导入导出/关于）/ ui.h（内部声明） |
| `src/app.cpp` | 普通 TU | `Application{AppRoot, AppOptions}`（Custom chrome，标题栏 24pt，1080×720 / min 560×480） |
| 平台入口 | `platform/{linux,windows,macos}/main.cpp` | 薄入口 `huxerui::RunApplication()`（无 CLI 分流；顶层 CMake 按 WIN32/APPLE/Linux 分支选用） |

## 领域层设计要点

- **live 文件**（被切换工具实际读取的文件）：claude-code 切换 = 深合并
  `~/.claude/settings.json` 的 `env.ANTHROPIC_BASE_URL` / `ANTHROPIC_AUTH_TOKEN`
  （model 非空时写 `ANTHROPIC_MODEL`），permissions 等其余字段原样保留；codex
  切换 = 深合并 `~/.codex/auth.json` 的 `OPENAI_API_KEY`，且
  `Provider.codexConfigToml` 非空时**整体替换** `~/.codex/config.toml`（TOML
  不做结构化合并，原文即模板）。opencode = opencode.json 顶层 provider map
  additive upsert（npm 段按 apiFormat 三档选 `@ai-sdk/anthropic` /
  `@ai-sdk/openai` / `@ai-sdk/openai-compatible`；官方文件允许 JSON5 注释，
  改写用 readJsonStrict 解析失败抛错、绝不碰原文件）；pi = models.json
  providers upsert（api 字段三档映射 anthropic→anthropic-messages /
  openai-responses→openai-responses / 其余→openai-completions）+
  settings.json 深合并 defaultProvider/defaultModel（目录 0700、文件 0600）；
  claude desktop = 3p 直连（两份 claude_desktop_config.json 置
  deploymentMode=3p + configLibrary 固定 id profile/_meta.json，
  **Linux 不支持**）。
- **apiFormat 三档**（models.cppm）：`openai-chat`（默认；`""`/旧值
  `"openai"`/未知值都归此档）/ `openai-responses` / `anthropic`。序列化存
  原值，判定/显示/映射一律经 `models::normalizeApiFormat` 归一 +
  `apiFormatLabel` 显示名，各处不各自解释字符串。
- **用量查询**：Provider.usageUrl/usagePath/usageLabel（usageUrl 空 = 不查）+
  全局 AppConfig.usageEnabled/usageRefreshMinutes（0=仅手动，旧配置缺字段
  自动取默认 true/10）；net::fetchUsage 同步阻塞 10s，extractByPath 按点分
  路径（支持数组下标）取标量；`suggestUsageQuery` 只内置有官方文档的
  DeepSeek（/user/balance）。UI 侧卡片显示 + RunWorker 轮询（State 只在
  UI 线程写）。
- **本地路由设置**：AppConfig.routerEnabled（启动自启）/ routerPort（默认
  15731）/ routerFailover，store 有对应 setter；AppRoot 首组合时自启
  （见 src/ui/app.cpp 的 Lifecycle）。
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
7. **岛屿风**（对齐 Clash-Flux）：极简黑白主题（ui/app.cpp MinimalDark/Light）；
   一级岛 16pt / 二级岛 8pt 圆角，表面色经 `ResolveIslandTheme(theme)` 语义
   层级取，不直接用 surface_container_*。删除确认用内置
   `dialog.Show(title, message, positive, negative, ...)`（DialogStyle 已在
   MinimalThemed 里主题化）。
8. **响应式**：`UseViewportClass()` Compact(<600) 收窄侧栏(44pt)/一级岛内边距
   （PageScaffold）；窗口最小 560×480。

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
- **SDK 无剪贴板 / 打开浏览器 API**：路由页的各工具接入地址与关于页的链接
  都是等宽纯文本展示，不可点击复制/跳转（SDK 0.2.0 只暴露平台层
  PlatformClipboard 接口，无 app 侧入口）。

## 多平台 / CI

- 平台入口：platform/{linux,windows,macos}/main.cpp 均为薄 GUI 入口（无 CLI
  分流）；顶层 CMakeLists 的 WIN32/APPLE 分支按平台把对应入口追加进 SOURCES。
- Windows 自定义安装向导（蓝本 Clash-Flux 同名机制）：platform/windows/
  huxerui.cmake 定义 `huxerui_configure_windows_project_package`，
  `huxerui package windows`（HUXERUI_PACKAGE=ON）时把 MSI + Burn 捆绑包
  （package/Package.wxs.in / Bundle.wxs.in）与 HuxerUI 托管安装器 UI
  （package/src/，品牌面板 + 简中/繁中/英文 strings）接进构建；日常构建
  零开销（函数内 `if (NOT HUXERUI_PACKAGE) return()`）。需要含
  `huxerui_add_windows_installer` 的 HuxerUI 源码/SDK（0.2.0 之后；
  CI 钉的 c00e72a 如需出安装包要前移）。
- `.github/workflows/build.yml`（蓝本 Clash-Flux 同名文件，按其已跑通配方
  适配）：三个桌面 job + release。build-linux（ubuntu:26.04 容器 + clang-21/
  libc++-21 + pip cmake==4.4.2 + libc++.modules.json 路径改写 + gtk4/epoxy/
  libsoup3 开发包 + libssl-dev，正式）；build-windows（MSVC + choco ninja +
  choco openssl）与 build-macos
  （brew llvm + 手写 libc++.modules.json + 内联 P0960 补丁）为实验性
  continue-on-error——首次全量编译未在 CI 验证过，连续绿后再摘标记。
- 三个 job 都把 HuxerUI 上游钉在 commit `c00e72a`（"refresh prebuilt host
  tools"）clone 到 third_party/huxerui 走源码通道；OpenSSL 三平台各自提供
  （linux apt libssl-dev / windows choco openssl + `-DOPENSSL_ROOT_DIR` /
  macos brew openssl@3 + `-DOPENSSL_ROOT_DIR`）；无 mihomo/Android
  （蓝本相关步骤已删）。
- 打包：Linux tar.gz（二进制 + llm-switch.resources + lib/libhuxerui.so +
  libc++ 三件套 + patchelf `$ORIGIN/lib`）、Windows zip（exe + 旁挂 dll +
  resources）、macOS tar.gz（.app bundle）；push tag `v*` 时 release job
  （`if: always()`，job 级 `contents: write`）下载已存在的产物经
  softprops/action-gh-release 挂到 release。

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
- ⬜ 待做：托盘图标是灰色双向箭头占位（正式图标待设计）；codex 内置预设仅
  OpenRouter/DeepSeek 两家可扩充；未做「关闭最小化到托盘」（SDK 有
  `OnCloseRequest` 范式，sdk 文档 navigation-and-window.md）；无 CLI 分流、
  无单实例/开机自启。
