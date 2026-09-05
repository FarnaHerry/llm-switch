# llmswitch_vendor.cmake — vendor 解包助手（移植自姊妹项目 Clash-Flux）。
# 被 顶层 CMakeLists 与 third_party/CMakeLists 共同 include；重复 include 无害。
set(LLMSWITCH_VENDOR_DIR "${CMAKE_BINARY_DIR}/vendor")
file(MAKE_DIRECTORY "${LLMSWITCH_VENDOR_DIR}")

# llmswitch_extract(<name> <archive> <sha256> <topdir>)
# 解包（带 sha 记录，tarball 变更时自动重解）；topdir 的路径存到 <name>_SOURCE_DIR。
function(llmswitch_extract name archive sha256 topdir)
    set(dest "${LLMSWITCH_VENDOR_DIR}/${name}")
    set(recorded "")
    if(EXISTS "${dest}/.vendor-sha256")
        file(READ "${dest}/.vendor-sha256" recorded)
    endif()
    string(STRIP "${recorded}" recorded)
    if(NOT recorded STREQUAL sha256 OR NOT EXISTS "${dest}/${topdir}")
        message(STATUS "llmswitch vendor: extracting ${archive} -> ${dest}")
        file(REMOVE_RECURSE "${dest}")
        file(MAKE_DIRECTORY "${dest}")
        # INPUT/DESTINATION 是 file(ARCHIVE_EXTRACT) 的正式签名（3.30+ 均一致；
        # 不要写成位置参数 + TO —— 那个形式任何版本都不存在，会报
        # "Unrecognized argument: <archive>"）
        file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${dest}")
        if(NOT EXISTS "${dest}/${topdir}")
            message(FATAL_ERROR "llmswitch vendor: ${archive} 里找不到 ${topdir}")
        endif()
        file(WRITE "${dest}/.vendor-sha256" "${sha256}")
    endif()
    set(${name}_SOURCE_DIR "${dest}/${topdir}" PARENT_SCOPE)
endfunction()
