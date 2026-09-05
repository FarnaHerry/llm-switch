# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

llm-switch 是 **cc-switch**（GitHub: farion1231/cc-switch）的 **C++23 模块化重写**：
管理 Claude Code / Codex 两款 AI CLI 的供应商配置切换——把选中的供应商写进
工具的 live 配置文件（`~/.claude/settings.json` 的 env 块 / `~/.codex/auth.json`
+ `config.toml`），并提供收编、备份、导入导出。用 **HuxerUI**（组件式声明 UI）
做桌面壳，全程 C++，无网络/数据库依赖。构建系统 CMake（脚手架与姊妹项目
`../Clash-Flux` 同源）。分层：UI（src/ui/*.cpp 普通源走 hcg codegen）/
领域层（llmswitch.config/models/store 三个 C++23 模块）。

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
  写回时编译失败。

## 构建 / 运行 / 测试

```bash
cmake -B build -G Ninja            # 配置（默认 Release；调试加 -DCMAKE_BUILD_TYPE=Debug）
cmake --build build -j             # 编译（app + test_smoke + test_store）
ctest --test-dir build             # 冒烟 + 领域层测试
./run.sh                           # 启动 GUI（INTEL_FORCE_PROBE=1）
huxerui run linux                  # HuxerUI CLI 流程（构建到 .huxerui/build/linux/）
```

- 工具链：系统 GCC（本机 16.2.1）+ libstdc++，CMake ≥ 4.4（`import std` 仍是
  experimental：UUID 表在 `cmake/CxxImportStdGate.cmake`）。
- **依赖极简**：nlohmann::json 3.12.0 以 single header 提交在
  `third_party/json/`（配 `cmake/nlohmann.json.cppm` 提供 `import nlohmann.json`，
  静态库目标 `llmswitch_json`）；HuxerUI 0.2.0 走双通道（见上）。无
  curl/SQLite/OpenSSL（本项目无网络与数据库需求）。
- 测试目标独立：`test_smoke`（编译+运行冒烟）、`test_store`（领域层断言式
  测试，无框架，非零即败；全程 setenv 隔离到临时目录）。测试目标经
  FILE_SET 显式追加领域模块接口 + `src/store.cpp` 实现单元（glob 只进 app
  目标），链接 `llmswitch_json`。

## 架构

| 模块 | 文件 | 职责 |
|------|------|------|
| `llmswitch.config` | `src/config.cppm` | 数据目录（~/.local/share/llm-switch）/ config.json 与 backups/ 路径 / live 配置文件解析（含 LLMSWITCH_* 环境变量覆盖）/ 深色检测 |
| `llmswitch.models` | `src/models.cppm` | Provider/ProviderGroup/AppConfig 数据模型 + JSON 序列化 + 内置预设（builtinPresets） |
| `llmswitch.store` | `src/store.cppm` + `src/store.cpp` | ProviderStore：config.json 读写、CRUD、switchTo 切换（原子写+备份）、detectCurrent/importLive、导出导入 |
| `llmswitch::ui`（普通 C++） | `src/ui/*.cpp` | app（壳：MinimalDark/Light 主题+标题栏+图标侧栏+IndexedPages+托盘）/ common（岛屿原语、页面骨架/卡片/弹窗卡片、providerStore() 全局实例）/ providers_page（Claude/Codex 共用供应商页）/ settings_page（主题/路径/导入导出/关于）/ ui.h（内部声明） |
| `src/app.cpp` | 普通 TU | `Application{AppRoot, AppOptions}`（Custom chrome，标题栏 24pt，1080×720 / min 560×480） |
| 平台入口 | `platform/{linux,windows,macos}/main.cpp` | 薄入口 `huxerui::RunApplication()`（无 CLI 分流；顶层 CMake 按 WIN32/APPLE/Linux 分支选用） |

## 领域层设计要点

- **live 文件**（被切换工具实际读取的文件）：claude 切换 = 深合并
  `~/.claude/settings.json` 的 `env.ANTHROPIC_BASE_URL` / `ANTHROPIC_AUTH_TOKEN`
  （model 非空时写 `ANTHROPIC_MODEL`），permissions 等其余字段原样保留；codex
  切换 = 深合并 `~/.codex/auth.json` 的 `OPENAI_API_KEY`，且
  `Provider.codexConfigToml` 非空时**整体替换** `~/.codex/config.toml`（TOML
  不做结构化合并，原文即模板）。
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
- **测试友好**：live 路径全部支持环境变量覆盖（`LLMSWITCH_CLAUDE_SETTINGS` /
  `LLMSWITCH_CODEX_AUTH` / `LLMSWITCH_CODEX_CONFIG`），`~` 展开只认 HOME，
  dataDir 走 XDG_DATA_HOME/HOME——test_store 用 setenv 指到
  `temp/llmswitch-test-<pid>` 即可完全隔离。
- ProviderStore **不强制单例**（测试可实例化）；UI 侧的全局实例在
  `src/ui/common.cpp::providerStore()`（首次访问即 load）。

## 关键约定（改代码前必读）

1. **`import std;` 后禁止再 `#include` 标准头**。C/系统头放全局模块片段
   （`module;` 与 `module llmswitch.x;` 之间）。普通 UI .cpp / ui.h 头用哪个
   std 设施就自己 `#include` 哪个（注意 `std::println(stderr,...)` 的 `stderr`
   不在 std 模块里，需 `<cstdio>`）。
2. **UI 层遵守 skill 的 DSL 风格**：普通 .cpp、composable 不加 inline、View 按值
   传递、具名 View 链式调用前 `std::move`（`.With` 等是右值限定）。
3. **受控值以应用状态为权威**；TextField 保留完整 TextEditingValue；动态兄弟用
   稳定 `.Key(...)`（供应商卡 `.Key(id)`）。
4. **线程契约**：State 只在 UI 线程读写。store 无内部锁，**UI 线程独占**是
   设计前提——live 文件读写是微秒级本地 IO，CRUD/切换/导入导出直接在 UI
   线程回调里做，没有 task_bridge；FilePicker 的 async API 恢复点本就在 UI
   线程。**事件处理器内禁止同步写会导致点击节点被卸载的 State**——经
   `tasks.Launch` + `co_await Delay(0)` 推迟（开弹窗、切页同理）。
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

## 多平台 / CI

- 平台入口：platform/{linux,windows,macos}/main.cpp 均为薄 GUI 入口（无 CLI
  分流）；顶层 CMakeLists 的 WIN32/APPLE 分支按平台把对应入口追加进 SOURCES。
- `.github/workflows/build.yml`（蓝本 Clash-Flux 同名文件，按其已跑通配方
  适配）：三个桌面 job + release。build-linux（ubuntu:26.04 容器 + clang-21/
  libc++-21 + pip cmake==4.4.2 + libc++.modules.json 路径改写 + gtk4/epoxy/
  libsoup3 开发包，正式）；build-windows（MSVC + choco ninja）与 build-macos
  （brew llvm + 手写 libc++.modules.json + 内联 P0960 补丁）为实验性
  continue-on-error——首次全量编译未在 CI 验证过，连续绿后再摘标记。
- 三个 job 都把 HuxerUI 上游钉在 commit `c00e72a`（"refresh prebuilt host
  tools"）clone 到 third_party/huxerui 走源码通道；本项目无 OpenSSL/mihomo/
  Android（蓝本相关步骤已删）。
- 打包：Linux tar.gz（二进制 + llm-switch.resources + lib/libhuxerui.so +
  libc++ 三件套 + patchelf `$ORIGIN/lib`）、Windows zip（exe + 旁挂 dll +
  resources）、macOS tar.gz（.app bundle）；push tag `v*` 时 release job
  （`if: always()`，job 级 `contents: write`）下载已存在的产物经
  softprops/action-gh-release 挂到 release。

## 里程碑状态（2026-09-05）

- ✅ M1：构建脚手架（HuxerUI 双通道、llmswitch_json、test_smoke/test_store）+
  领域层三模块（config/models/store）+ 领域层测试全绿。
- ✅ M2：完整 UI（应用壳+托盘切换菜单、Claude/Codex 供应商页 CRUD/切换/预设
  模板、设置页主题/路径/导入导出）、GUI 冒烟通过。
- ✅ M3：跨平台 CI（三桌面 job + tag release）+ Windows/macOS 平台入口补齐。
- ⬜ 待做：托盘图标是灰色双向箭头占位（正式图标待设计）；codex 内置预设仅
  OpenRouter/DeepSeek 两家可扩充；未做「关闭最小化到托盘」（SDK 有
  `OnCloseRequest` 范式，sdk 文档 navigation-and-window.md）；无 CLI 分流、
  无单实例/开机自启。
