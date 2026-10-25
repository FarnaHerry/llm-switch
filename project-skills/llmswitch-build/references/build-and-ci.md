# 构建、依赖与跨平台发布

代码路径相对仓库根目录。具体版本和命令参数以顶层 CMakeLists、preset、
[依赖说明](../../../third_party/README.md) 与
[当前 workflow](../../../.github/workflows/build.yml) 为准，避免维护第二份 pin 清单。

## 本地构建与运行

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
./run.sh
```

Ninja 是默认生成器；已有目录若用其它生成器，不原地切换或删除，另建目录。
Debug 将 Release 改为 Debug，或用 `cmake --preset ninja-release` / ninja-debug
配置及对应 build/test preset。
./run.sh 优先运行已启用 Live2D 的 build-live2d，否则用 build；HuxerUI CLI 的
`huxerui run linux` 使用 .huxerui/build/linux/，不能混淆这几个构建来源。
固定收尾顺序以 [根规范](../../../AGENTS.md) 为准，文档变更也执行本地完整编译测试。

本机默认系统 GCC + libstdc++；CMake ≥4.4，import std 仍需 experimental gate。
支持的 CMake UUID 集中在 cmake/CxxImportStdGate.cmake，升级先核对 gate。
CI Linux 使用 clang/libc++，不要将本机 GCC 可用性当成 CI 配方。

## HuxerUI 双通道

顶层 CMake 固定优先级：

1. 显式 HUXERUI_HOME 指向的源码树。
2. third_party/huxerui 的本地下游 fork 源码。
3. HUXERUI_HOME 指向含 lib/cmake/HuxerUI 的安装 SDK。
4. third_party/tarballs 的 Linux 离线 SDK。

源码需要 GTK ≥4.14、libepoxy ≥1.5、libsoup ≥3.0 开发包，缺失时按 CMake 回落 SDK。
`-DLLMSWITCH_HUXERUI_FORCE_SDK=ON` 是明确跳过源码的开关。
当前项目支持源码 0.3.0 / 离线 SDK 0.2.0；实际版本查 CMakeCache 和配置输出。
API 只参考当前使用版本的公开契约及其 huxerui-app-development skill。

宿主工具优先本地 third_party/huxerui-tools/linux/x86_64/{hcg,hrc}，缺失回落上游预置。
若升级后预置与源码不同步（如 hrc 报 unknown path operation），从 tools/codegen、
tools/resource_compiler 重编；不要修改应用语义去兼容过期工具。
HuxerUI checkout 与宿主工具目录不入本项目版本管理，项目技能也不复制它们的手册。

## 依赖与测试入口

- nlohmann::json、cpp-httplib 单头随仓库提交，目标分别为 llmswitch_json 与
  llmswitch_httplib；JSON 经 cmake/nlohmann.json.cppm 模块包装。
- HuxerUI 与 Lib-SQLite 沿用下游 fork 的集成分支，固定 commit 查现有依赖说明和
  CMake/CI。Lib-SQLite 经 huxerui_use_library 获取，自带 SQLite amalgamation；
  不另引数据库栈、IXWebSocket、curl/OpenSSL。网络 TLS 由平台栈提供。
- 核心测试 smoke/store/net/router/mcp/skills/sessions，后续已有 usage/agents/
  single_instance；用 ctest -N 核对当前清单，不把旧“7 个”当完整测试集合。
  测试以失败断言计数非零退出，无额外测试框架。
- store 等测试通过环境变量隔离 live 与数据路径；net 测纯解析，不测真实网络；
  router 用 httplib 假上游覆盖转发、换 key、故障转移、开关和统计。
- 模块测试经 FILE_SET 显式追加领域接口与实现，应用 glob 不代表测试自动包含；
  JSON 模块链接 llmswitch_json，router 另链 llmswitch_httplib。

## 平台、CI 与发布

- platform/{linux,windows,macos}/main.cpp 是 GUI 入口，无 CLI 分流；单实例启动/
  激活在 src/single_instance.*，UI 收到激活后经应用任务队列处理。
- 推 main、开 PR、推 v* tag 都构建，tag 另触发 release；不要为省额度关闭 main/PR
  三平台门禁。docs/**、**.md 可被 paths-ignore 跳过；workflow_dispatch 只构建，
  release job 另有 tag 判断，不能把手动触发构建当成发布。
- build-linux 为 ubuntu:26.04 容器、clang/libc++、固定 pip CMake、模块 JSON 路径修正
  与 GTK/epoxy/libsoup 开发包；包名与版本以 workflow 为准。
- Windows 先 build-windows-sdk 构建同一 pin 的安装 SDK，应用和安装器两个消费者下载
  复用，避免重复构建框架；MSVC + Ninja。macOS 使用 brew LLVM 与模块 JSON 配方。
  Linux/macOS 源码构建；全部必要平台和安装器任务成功、产物存在才允许 release。
- `huxerui package windows` 的自定义向导从 platform/windows/huxerui.cmake 的
  huxerui_configure_windows_project_package 接入，HUXERUI_PACKAGE=ON 时生成 MSI、
  Burn bundle 和 package/src/ 的简中/繁中/英文 UI；日常构建函数早退，无打包开销。
  需要含 huxerui_add_windows_installer 的源码/SDK，配色共享 src/ui/theme_colors.h。
- 项目框架补丁已并入下游 fork，包括 Windows 图标宏
  HUXERUI_WINDOWS_APPLICATION_ICON_RESOURCE_ID=101 与 macOS P0960 修正；
  不在应用仓库内另堆第三方 checkout 补丁。Linux 性能探针的 probe/ui-perf 分支
  只按需人工诊断合入；文本布局 LRU 缓存已在上游，不再维护旧补丁。
- Linux tar.gz 包含程序、resources、libhuxerui.so、libc++ 运行库并设 $ORIGIN/lib；
  Windows zip 含 exe、旁挂 DLL、resources，另有安装器；macOS 为 .app bundle。
  release 下载各必需平台产物，通过 action-gh-release 发布，发布 job 单独赋写权限。
