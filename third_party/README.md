# third_party — vendored 依赖

nlohmann::json 以 single header 直接提交在 `json/`；curl 与 OpenSSL（Linux
x86_64 回落用静态包）以 tarball 提交在 `tarballs/`，configure 期校验 SHA256
解包构建；HuxerUI 0.2.0 的 Linux 离线 SDK 包提交在 `tarballs/` 兜底。构建
离线、可复现；清单与姊妹项目 Clash-Flux 对齐（无 IXWebSocket / SQLiteCpp）。

## 清单与来源

| 包 | 版本 | tarball | 来源 |
|----|------|---------|------|
| HuxerUI | 0.2.0 | `huxerui-sdk-0.2.0-linux-x86_64.tar.gz` | 由官方 0.2.0 SDK 安装前缀归档（shared 库 + headers + CMake 包 + hcg/hrc + 内置资源）。`HUXERUI_HOME` 可指向 0.2.0 SDK 安装目录或源码根目录；未设置时优先 `third_party/huxerui/` 源码（本地 clone，不入库），`LLMSWITCH_HUXERUI_FORCE_SDK=ON` 时使用 Linux 离线包。Linux 源码模式需 GTK ≥4.14、libepoxy ≥1.5、libsoup ≥3.0（Fedora：`gtk4-devel libepoxy-devel libsoup3-devel`）；macOS/Windows 必须通过 `HUXERUI_HOME` 提供 0.2.0 源码或 SDK。 |
| curl | 8.22.0 | `curl-8.22.0.tar.gz` | 上游官方发布包；OpenSSL 后端静态库（不编 curl.exe，brotli/zstd/psl/ssh2/ldap 全关），供 llmswitch.net 拉取模型列表 |
| OpenSSL | 3.5.1 | `openssl-3.5.1-linux-x86_64.tar.gz` | 预编译静态库，仅作 Linux x86_64 找不到系统 OpenSSL 时的回落；其余平台用系统包（CI：libssl-dev / choco openssl / brew openssl@3） |
| nlohmann::json | 3.12.0 | `json/nlohmann/json.hpp`（single header） | 上游 `nlohmann/json` v3.12.0 `single_include`；配 `cmake/nlohmann.json.cppm` 提供 `import nlohmann.json` 模块 |
| cpp-httplib | 0.20.1 | `httplib/httplib.h`（single header，MIT） | 上游 `yhirose/cpp-httplib` v0.20.1；`llmswitch_httplib` INTERFACE 目标导出包含目录，供 llmswitch.router 本地代理服务器（仅监听 127.0.0.1；出站转发走 curl，无 TLS 服务端需求） |

## 更新某个依赖

1. 用新版本源码打 tarball（保持顶层目录名，或同步改引用处的 `topdir` 参数）；
2. `sha256sum` 新值写回顶层 `CMakeLists.txt` 的 `llmswitch_extract` 调用；
3. 跑一次 configure 验证解包与 SHA 校验。
