# third_party — vendored 依赖

nlohmann::json 以 single header 直接提交在 `json/`；cpp-httplib 以 single
header 提交在 `httplib/`；HuxerUI 0.2.0 的 Linux 离线 SDK 包提交在
`tarballs/` 兜底。日常源码构建跟随 HuxerUI v0.3.0 发布线之后的主干，CI 固定到
已验证的 commit `445488a6672f225d73b6bf093fe7e004f354a3c0`。构建
离线、可复现；清单与姊妹项目 Clash-Flux 对齐（无 IXWebSocket / SQLiteCpp）。
网络（模型列表/用量/连通检测/本地路由出站）统一走 HuxerUI 平台 HttpClient
（Linux libsoup / Windows WinHTTP / macOS NSURLSession，TLS 由平台栈负责），
不 vendor curl/OpenSSL。

项目对 Windows 主窗口图标保留一个独立补丁
`cmake/patches/huxerui-windows-icon.patch`，由 Windows CI 在检出 HuxerUI
后应用；不要把这个项目补丁直接提交到第三方仓库。

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
