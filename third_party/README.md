# third_party — vendored 依赖

nlohmann::json 以 single header 直接提交在 `json/`；cpp-httplib 以 single
header 提交在 `httplib/`；HuxerUI 0.2.0 的 Linux 离线 SDK 包提交在
`tarballs/` 兜底。日常源码构建跟随下游 fork `FarnaHerry/HuxerUI` 的
`farna/main` 集成分支（官方 `main` 只作上游基线镜像），CI 固定到
已验证的 commit `22d7e3ac55920865b7ff41cf02e989677ae20d5f`。构建
离线、可复现；清单与姊妹项目 Clash-Flux 对齐（无 IXWebSocket）；SQLite 只经由
下游 fork `FarnaHerry/Lib-SQLite`（`farna/main` 钉 `88f610fe66c57260b1ac38e676fc36b23c900fc2`）
引入（见顶层 CMakeLists 的 huxerui_use_library），不额外 vendor。
网络（模型列表/用量/连通检测/本地路由出站）统一走 HuxerUI 平台 HttpClient
（Linux libsoup / Windows WinHTTP / macOS NSURLSession，TLS 由平台栈负责），
不 vendor curl/OpenSSL。

项目专属的 HuxerUI 修改已并入 fork 的 `farna/main`：Windows 主窗口图标
（`HUXERUI_WINDOWS_APPLICATION_ICON_RESOURCE_ID` 编译宏，本仓库顶层
CMakeLists 传入 `=101`）与 macOS Objective-C++ 的 P0960 兼容修正，CI 不再
检出后打补丁；不要把项目补丁直接提交到第三方 checkout。可选的 Linux UI 性能
探针（`LLMSWITCH_PROBE_CLICK` 合成输入）维护在 fork 的 `probe/ui-perf`
分支，只在人工诊断时合入本地 checkout，不参与正常构建。
Linux 文本布局缓存
（有界 LRU，修 VirtualList 每帧全量重排版）已由上游合入
（HuxerUI/HuxerUI#137），不再需要本地补丁。

## 清单与来源

| 包 | 版本 | tarball | 来源 |
|----|------|---------|------|
| HuxerUI | 0.3.0 | `huxerui-sdk-0.2.0-linux-x86_64.tar.gz`（离线回落） | 默认使用 `third_party/huxerui/` 的 0.3.0 主干源码（本地 clone，不入库）；`HUXERUI_HOME` 可指向源码根目录，`LLMSWITCH_HUXERUI_FORCE_SDK=ON` 时使用 Linux 0.2.0 离线包。Linux 源码模式需 GTK ≥4.14、libepoxy ≥1.5、libsoup ≥3.0（Fedora：`gtk4-devel libepoxy-devel libsoup3-devel`）；macOS/Windows 通过源码或 `HUXERUI_HOME` 提供 SDK。Windows 自定义安装向导（`platform/windows/package/`）需要含 `cmake/HuxerUIWindowsInstaller.cmake` 的源码/SDK。 |
| nlohmann::json | 3.12.0 | `json/nlohmann/json.hpp`（single header） | 上游 `nlohmann/json` v3.12.0 `single_include`；配 `cmake/nlohmann.json.cppm` 提供 `import nlohmann.json` 模块 |
| cpp-httplib | 0.56.0 | `httplib/httplib.h`（single header，MIT） | 上游 `yhirose/cpp-httplib` v0.56.0；`llmswitch_httplib` INTERFACE 目标导出包含目录，供 llmswitch.router 本地代理服务器（仅监听 127.0.0.1；出站转发走 HuxerUI 平台 HttpClient，无 TLS 服务端需求） |

## 更新某个依赖

1. 用新版本源码打 tarball（保持顶层目录名，或同步改引用处的 `topdir` 参数）；
2. `sha256sum` 新值写回顶层 `CMakeLists.txt` 的 `llmswitch_extract` 调用；
3. 跑一次 configure 验证解包与 SHA 校验。
