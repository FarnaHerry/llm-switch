# third_party/CMakeLists.txt — vendor 依赖。
#
# nlohmann::json 以 single header 直接提交在 json/（配 cmake/nlohmann.json.cppm
# 提供 `import nlohmann.json`）；cpp-httplib 以 single header 提交在 httplib/。
# HuxerUI 由顶层 CMakeLists 在源码/已安装 SDK/离线 tarball 通道间选择，不在
# 此构建。网络请求统一走 HuxerUI 平台 HttpClient（Linux libsoup / Windows
# WinHTTP / macOS NSURLSession），不 vendor curl/OpenSSL。
#
# 产物 target：
#   llmswitch_json    nlohmann::json 3.12.0（single header）+ `import nlohmann.json`
#   llmswitch_httplib cpp-httplib 0.56.0（single header，MIT；路由本地监听用）
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/llmswitch_vendor.cmake")

# ---- nlohmann::json 3.12.0（single header + `import nlohmann.json` 模块）------
add_library(llmswitch_json STATIC)
target_sources(llmswitch_json PUBLIC FILE_SET json_modules TYPE CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/.."
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/nlohmann.json.cppm")
set_target_properties(llmswitch_json PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_MODULE_STD ON)
target_include_directories(llmswitch_json PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/json")

# ---- cpp-httplib 0.56.0（single header，MIT）---------------------------------
# 用途：llmswitch.router 的本地 HTTP 代理服务器（仅监听 127.0.0.1，无 TLS 服务端
# 需求；出站转发走 HuxerUI 平台 HttpClient）。头文件直接提交在 httplib/，
# INTERFACE 目标导出包含目录。
add_library(llmswitch_httplib INTERFACE)
target_include_directories(llmswitch_httplib INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/httplib")
target_compile_features(llmswitch_httplib INTERFACE cxx_std_17)
