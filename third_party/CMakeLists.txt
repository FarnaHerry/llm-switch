# third_party/CMakeLists.txt — vendor 依赖。
#
# nlohmann::json 以 single header 直接提交在 json/（配 cmake/nlohmann.json.cppm
# 提供 `import nlohmann.json` 模块）；curl 8.22.0 以 tarball 提交在 tarballs/，
# configure 期校验 SHA256 后解包到 ${CMAKE_BINARY_DIR}/vendor/ 再
# add_subdirectory（OpenSSL 后端，静态库）；HuxerUI 由顶层 CMakeLists 在源码/
# 已安装 SDK/离线 tarball 通道间选择，不在此构建。
#
# 产物 target：
#   HuxerUI::huxerui  HuxerUI 0.2.0 SDK（shared；由顶层 find_package 引入）
#   CURL::libcurl     curl 8.22.0（OpenSSL 后端，静态库，不编 curl.exe）
#   llmswitch_json    nlohmann::json 3.12.0（single header）+ `import nlohmann.json`
#
# OpenSSL 在顶层 CMakeLists 解析（imported target 需要全局可见），
# 本文件假定 find_package(OpenSSL) 已成功。
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/llmswitch_vendor.cmake")

if(NOT OpenSSL_FOUND)
    message(FATAL_ERROR "third_party：顶层应已完成 OpenSSL 解析")
endif()

# ---- nlohmann::json 3.12.0（single header + `import nlohmann.json` 模块）------
add_library(llmswitch_json STATIC)
target_sources(llmswitch_json PUBLIC FILE_SET json_modules TYPE CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/.."
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/nlohmann.json.cppm")
set_target_properties(llmswitch_json PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_MODULE_STD ON)
target_include_directories(llmswitch_json PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/json")

# ---- curl 8.22.0（静态库，OpenSSL 后端，仅 lib）------------------------------
# 用途：llmswitch.net 按供应商 baseUrl+key 拉取模型列表（GET /models）。
llmswitch_extract(curl
    "${CMAKE_CURRENT_SOURCE_DIR}/tarballs/curl-8.22.0.tar.gz"
    "d54dd598bf05927a726deb38df31c6a255ba83ff1de57c5d1464dac3ed8f44a1"
    "curl-8.22.0")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
add_subdirectory("${curl_SOURCE_DIR}" "curl-build")
