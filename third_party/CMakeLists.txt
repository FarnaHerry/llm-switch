# third_party/CMakeLists.txt — vendor 依赖。
#
# llm-switch 的第三方依赖极少：nlohmann::json 以 single header 直接提交在 json/
# （配 cmake/nlohmann.json.cppm 提供 `import nlohmann.json` 模块），HuxerUI 由
# 顶层 CMakeLists 在源码/已安装 SDK/离线 tarball 通道间选择，不在此构建。
#
# 产物 target：
#   HuxerUI::huxerui  HuxerUI 0.2.0 SDK（shared；由顶层 find_package 引入）
#   llmswitch_json    nlohmann::json 3.12.0（single header）+ `import nlohmann.json`
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/llmswitch_vendor.cmake")

# ---- nlohmann::json 3.12.0（single header + `import nlohmann.json` 模块）------
add_library(llmswitch_json STATIC)
target_sources(llmswitch_json PUBLIC FILE_SET json_modules TYPE CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/.."
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/nlohmann.json.cppm")
set_target_properties(llmswitch_json PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_MODULE_STD ON)
target_include_directories(llmswitch_json PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/json")
