# CMakeLists.txt — llm-switch 顶层构建（HuxerUI CLI 生成格式 + 本仓库 vendor 依赖）。
#
# 形状对齐 `huxerui create app` 产物（脚手架移植自姊妹项目 Clash-Flux）：顶部项目
# 自省块（CLI plan-only 配置读取）、platform/<平台>/main.cpp 平台入口、src/ 递归
# 源码发现、resources/ 以 app 命名空间注册。差异仅在于：依赖 vendor 在
# third_party/（HuxerUI 优先 third_party/huxerui 源码编译，tarballs 里的预编译
# SDK 兜底），领域层以 C++23 模块（FILE_SET）追加到同一 app 目标。
#
# 常用命令：
#   huxerui build linux [--profile debug|release]   # CLI 构建（.huxerui/build/linux/...）
#   huxerui run linux                               # CLI 构建并运行
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --parallel 4                 # 直接 Ninja 流程（默认）
#   ctest --test-dir build                          # 冒烟 + 领域层测试
#   ./run.sh                                        # 运行 build/ 出的 GUI
cmake_minimum_required(VERSION 3.30)

# ---- HuxerUI CLI 项目自省 --------------------------------------------------
# `huxerui build/run/package` 先用 -DHUXERUI_PROJECT_PLAN_ONLY=ON 配置本项目，
# 读本块写出的 JSON 获知 kind/name/id/target；本块必须位于 project() 之前，
# 且不依赖任何语言/包解析（plan-only 配置会直接 return）。
if (HUXERUI_PROJECT_PLAN_OUTPUT)
    get_filename_component(HUXERUI_PROJECT_PLAN_DIRECTORY
            "${HUXERUI_PROJECT_PLAN_OUTPUT}"
            DIRECTORY
    )
    file(MAKE_DIRECTORY "${HUXERUI_PROJECT_PLAN_DIRECTORY}")
    file(WRITE "${HUXERUI_PROJECT_PLAN_OUTPUT}" [=[
{
  "schema": 1,
  "kind": "app",
  "name": "llm-switch",
  "id": "dev.farna.llmswitch",
  "target": "llm-switch"
}
]=])
endif ()
if (HUXERUI_PROJECT_PLAN_ONLY)
    return()
endif ()

project(llm-switch VERSION 0.1.3 DESCRIPTION "Claude Code / Codex provider config switcher (cc-switch C++ rewrite)" LANGUAGES NONE)

# HUXERUI_LIBRARY_GRAPH_ONLY：CLI 扫描 huxerui 库依赖图时的特殊配置——
# 不启用语言、不构建，huxerui_add_app 只写出库图 JSON 即返回（见 SDK
# HuxerUIApp.cmake）。本项目的重活（第三方依赖/模块/链接）全部在该模式下跳过。
if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    # `import std` 的 experimental 门控必须在 enable_language(CXX) 前设置
    # （UUID 随 CMake 版本变，见 cmake/CxxImportStdGate.cmake 顶部注释）。
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
    include(CxxImportStdGate)
    enable_language(C)
    enable_language(CXX)
    if (WIN32)
        enable_language(RC)
    endif ()

    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS ON)            # gnu++23：模块实现与 GCC 标准库组合最顺
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    # 只给用 import std 的目标开 CXX_MODULE_STD（llm-switch / 测试目标），
    # 不影响可能的 C++17 子项目。
    set(CMAKE_CXX_MODULE_STD OFF)

    # 默认 Release（单配置生成器）；调试请显式 -DCMAKE_BUILD_TYPE=Debug
    # （huxerui CLI 会按 --profile 显式传入，不受此默认影响）
    if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
    endif ()

    find_package(Threads REQUIRED)
endif ()

# ---- HuxerUI（0.2.0 SDK / 源码双通道，HUXERUI_HOME 契约）-------------------
# 优先级（固定顺序，与 Clash-Flux 一致）：
#   1. 显式 HUXERUI_HOME 源码（有 CMakeLists.txt + include/huxerui/huxerui.h）。
#   2. third_party/huxerui 源码（本地 git clone 的上游仓库，add_subdirectory 编译，
#      跟踪上游 `git pull` 即可；不入库，.gitignore 已忽略）。
#      CLI 自动注入的已安装 SDK 前缀（HUXERUI_HOME 指向 SDK）不得越过前两级。
#   3. 已安装 SDK（HUXERUI_HOME 指向含 lib/cmake/HuxerUI 的 SDK 目录）。
#   4. 仓库离线 SDK（tarball，Linux 专用；macOS/Windows 不提供离线包）。
#
# LLMSWITCH_HUXERUI_FORCE_SDK=ON：唯一明确跳过源码的开关，只走第 3/4 通道（发布门禁
# 与兼容测试用）。源码模式编译 Linux 后端需要 gtk4-devel libsoup3-devel。
include(cmake/llmswitch_vendor.cmake)
set(HUXERUI_HOME "$ENV{HUXERUI_HOME}" CACHE PATH "HuxerUI 0.2.0 SDK or source directory")
set(LLMSWITCH_HUXERUI_BUNDLED_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/huxerui" CACHE PATH "Bundled HuxerUI source checkout (override for testing to simulate missing source without moving shared directory)")
set(LLMSWITCH_HUXERUI_SOURCE_DIR "")
option(LLMSWITCH_HUXERUI_FORCE_SDK "Skip source checkout; use SDK fallback only" OFF)
option(LLMSWITCH_HUXERUI_MOCK_MISSING_DEPS "Force missing gtk4/libsoup for selection-matrix testing" OFF)
set(LLMSWITCH_HUXERUI_FROM_SOURCE OFF)

# 优先级 1：显式 HUXERUI_HOME 源码（用户显式指向源码目录时优先于仓库内源码）
if (NOT LLMSWITCH_HUXERUI_FORCE_SDK
        AND HUXERUI_HOME
        AND EXISTS "${HUXERUI_HOME}/CMakeLists.txt"
        AND EXISTS "${HUXERUI_HOME}/include/huxerui/huxerui.h")
    set(LLMSWITCH_HUXERUI_SOURCE_DIR "${HUXERUI_HOME}")
endif ()

# 优先级 2：仓库内 third_party/huxerui 源码（默认 source-first）
if (NOT LLMSWITCH_HUXERUI_SOURCE_DIR
        AND NOT LLMSWITCH_HUXERUI_FORCE_SDK
        AND EXISTS "${LLMSWITCH_HUXERUI_BUNDLED_SOURCE_DIR}/CMakeLists.txt"
        AND EXISTS "${LLMSWITCH_HUXERUI_BUNDLED_SOURCE_DIR}/include/huxerui/huxerui.h")
    set(LLMSWITCH_HUXERUI_SOURCE_DIR "${LLMSWITCH_HUXERUI_BUNDLED_SOURCE_DIR}")
endif ()

# 源码依赖检查（Linux）：与 HuxerUI 的 GTK Snapshot / GPU 纹理依赖保持一致。
if (LLMSWITCH_HUXERUI_SOURCE_DIR
        AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
        AND NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    if (LLMSWITCH_HUXERUI_MOCK_MISSING_DEPS)
        set(LLMSWITCH_HUXERUI_GTK4_FOUND OFF)
        set(LLMSWITCH_HUXERUI_EPOXY_FOUND OFF)
        set(LLMSWITCH_HUXERUI_LIBSOUP_FOUND OFF)
    else ()
        find_package(PkgConfig QUIET)
        if (PkgConfig_FOUND)
            pkg_check_modules(LLMSWITCH_HUXERUI_GTK4 QUIET gtk4>=4.14)
            pkg_check_modules(LLMSWITCH_HUXERUI_EPOXY QUIET epoxy>=1.5)
            pkg_check_modules(LLMSWITCH_HUXERUI_LIBSOUP QUIET libsoup-3.0>=3.0)
        endif ()
    endif ()
    if (NOT LLMSWITCH_HUXERUI_GTK4_FOUND OR NOT LLMSWITCH_HUXERUI_EPOXY_FOUND
            OR NOT LLMSWITCH_HUXERUI_LIBSOUP_FOUND)
        message(WARNING "llm-switch: ${LLMSWITCH_HUXERUI_SOURCE_DIR} 存在但缺 GTK >=4.14 / epoxy >=1.5 / libsoup >=3.0 开发包"
            "（sudo dnf install gtk4-devel libepoxy-devel libsoup3-devel），回落预编译 SDK —— 本次未使用源码")
        set(LLMSWITCH_HUXERUI_SOURCE_DIR "")
    endif ()
endif ()

# 源码模式
if (LLMSWITCH_HUXERUI_SOURCE_DIR)
    set(LLMSWITCH_HUXERUI_FROM_SOURCE ON)
    # 上游顶层作为子项目时 tests/examples/CLI 默认 OFF；static 切片必须关掉——
    # _huxerui_select_framework_target 偏好 static，留着会让 app 链到静态库。
    set(HUXERUI_BUILD_STATIC OFF)
    # 上游预置宿主工具可能落后于源码（linux 预置 hrc 旧 → 新运行时解旧资源包直接
    # 抛 "unknown path operation"）。本地若有用当前源码编出的工具
    # （third_party/huxerui-tools/<platform>/<arch>/{hcg,hrc}），优先使用；
    # 缺失时回落上游预置（llm-switch 暂无本地 huxerui-tools，目录不存在自然回落）。
    set(LLMSWITCH_HUXERUI_LOCAL_TOOLS
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/huxerui-tools")
    if (LLMSWITCH_HUXERUI_SOURCE_DIR STREQUAL LLMSWITCH_HUXERUI_BUNDLED_SOURCE_DIR
            AND EXISTS "${LLMSWITCH_HUXERUI_LOCAL_TOOLS}/linux/x86_64/hrc"
            AND EXISTS "${LLMSWITCH_HUXERUI_LOCAL_TOOLS}/linux/x86_64/hcg")
        set(HUXERUI_HOST_TOOL_ROOT "${LLMSWITCH_HUXERUI_LOCAL_TOOLS}")
        message(STATUS "llm-switch: 宿主工具用本地源码构建（third_party/huxerui-tools）")
    endif ()
    add_subdirectory("${LLMSWITCH_HUXERUI_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/huxerui-sdk"
        EXCLUDE_FROM_ALL)
    # 打印实际来源与 commit，使 `huxerui build/run` 默认输出可见
    execute_process(COMMAND git -C "${LLMSWITCH_HUXERUI_SOURCE_DIR}" rev-parse --short HEAD
        OUTPUT_VARIABLE LLMSWITCH_HUXERUI_SOURCE_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if (LLMSWITCH_HUXERUI_SOURCE_COMMIT)
        message(STATUS "llm-switch: HuxerUI 源码编译（${LLMSWITCH_HUXERUI_SOURCE_DIR} @ ${LLMSWITCH_HUXERUI_SOURCE_COMMIT}）")
    else ()
        message(STATUS "llm-switch: HuxerUI 源码编译（${LLMSWITCH_HUXERUI_SOURCE_DIR}）")
    endif ()
else ()
    # SDK 模式：优先级 3（HUXERUI_HOME SDK）→ 优先级 4（离线 tarball，Linux 专用）
    # find_package 不依赖语言启用，graph-only 模式照常执行。
    if (HUXERUI_HOME AND EXISTS "${HUXERUI_HOME}/lib/cmake/HuxerUI/HuxerUIConfig.cmake")
        set(HUXERUI_SDK_DIR "${HUXERUI_HOME}" CACHE INTERNAL "HuxerUI SDK root" FORCE)
        message(STATUS "llm-switch: HuxerUI SDK（已安装，${HUXERUI_SDK_DIR}）")
    elseif (APPLE OR WIN32)
        message(FATAL_ERROR
            "llm-switch: macOS/Windows 未提供离线 SDK 包；"
            "请用 HUXERUI_HOME 指向 0.2.0 源码或已安装 SDK，"
            "或 clone third_party/huxerui 后默认以源码模式编译")
    else ()
        llmswitch_extract(huxerui
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tarballs/huxerui-sdk-0.2.0-linux-x86_64.tar.gz"
            "e184a62a0e5e22cd71835d5278034d65f8a30a2c48273513633f88719a1243e5"
            "huxerui-sdk-0.2.0-linux-x86_64")
        set(HUXERUI_SDK_DIR "${huxerui_SOURCE_DIR}" CACHE INTERNAL "HuxerUI SDK root" FORCE)
        message(STATUS "llm-switch: HuxerUI SDK（离线 tarball，${HUXERUI_SDK_DIR}）")
    endif ()
    if (NOT HUXERUI_SDK_DIR)
        set(HUXERUI_SDK_DIR "${huxerui_SOURCE_DIR}" CACHE INTERNAL "HuxerUI SDK root" FORCE)
    endif ()
    set(HuxerUI_DIR "${HUXERUI_SDK_DIR}/lib/cmake/HuxerUI" CACHE PATH "HuxerUI package" FORCE)
    set(HUXERUI_RESOURCES_BIN "${HUXERUI_SDK_DIR}/share/huxerui/resources/huxerui/resources.bin" CACHE INTERNAL "")
    # shared 组件：桌面 Linux/Windows 运行时动态库（static 切片在 Linux 要 gtk4
    # 开发包，本仓库不用）。
    find_package(HuxerUI 0.2.0 CONFIG REQUIRED COMPONENTS shared
        PATHS "${HUXERUI_SDK_DIR}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
endif ()

# 平台壳的 HuxerUI 配置钩子（生成格式：platform/<平台>/huxerui.cmake 可选）
if (WIN32)
    include("${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/huxerui.cmake" OPTIONAL)
elseif (APPLE AND NOT IOS)
    include("${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/huxerui.cmake" OPTIONAL)
endif ()

if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    # ---- OpenSSL 解析（必须在 add_subdirectory(third_party) 前：imported target
    # 要对 curl 子项目全局可见）------------------------------------------------
    # 优先系统 OpenSSL；系统没有时，linux x86_64 使用 vendor 的静态 3.5.1。
    find_package(OpenSSL QUIET)
    if (NOT OpenSSL_FOUND)
        if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64)")
            llmswitch_extract(openssl
                "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tarballs/openssl-3.5.1-linux-x86_64.tar.gz"
                "d749ee7ec6bda301dba4a67dc7196d4a04b310f664dc360d6ab2e49298b731cf"
                "openssl-3.5.1-linux-x86_64")
            set(OPENSSL_ROOT_DIR "${openssl_SOURCE_DIR}" CACHE PATH "vendored OpenSSL" FORCE)
            set(OPENSSL_USE_STATIC_LIBS TRUE CACHE BOOL "" FORCE)
            find_package(OpenSSL REQUIRED)
            message(STATUS "llm-switch: using vendored OpenSSL ${OPENSSL_VERSION} (static)")
        else ()
            message(FATAL_ERROR
                "未找到 OpenSSL 开发头文件/库。请安装系统 OpenSSL（libssl-dev / "
                "openssl@3 / vcpkg openssl），或为该平台补充 third_party/tarballs 下的静态包。")
        endif ()
    endif ()

    add_subdirectory(third_party)
endif ()

# ---- llm-switch app 目标 ----------------------------------------------------
# 源码发现对齐生成格式：src/ 下递归 glob 普通 C++ 源（UI composable 源 + 领域
# 模块的实现单元 .cpp；codegen 无 composable 标记时原样放行）。模块接口 .cppm
# 不经 glob，由 FILE_SET 显式追加。
file(GLOB_RECURSE APP_SOURCE_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)
# 平台入口（graph-only 配置不编译任何东西，跳过）
if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    if (WIN32)
        list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/main.cpp")
    elseif (APPLE AND NOT IOS)
        list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/main.cpp")
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/main.cpp")
    endif ()
endif ()

# CLI 依赖图输出位置（生成格式约定；huxerui_add_app 在存在该变量时写出）
set(HUXERUI_LIBRARY_GRAPH_OUTPUT
        "${CMAKE_CURRENT_SOURCE_DIR}/.huxerui/generated/libraries.json"
)

# 领域层 C++23 模块清单（.cppm 接口；对应 .cpp 实现单元已随 glob 进 SOURCES）。
# 测试目标需要 import 领域模块时复用同一份清单（见下方 test_store）。
set(LLMSWITCH_MODULE_UNITS
    src/config.cppm
    src/models.cppm
    src/net.cppm
    src/store.cppm
    src/router.cppm
    src/mcp.cppm
    src/skills.cppm
    src/sessions.cppm
)

# 24×24 界面图标必须是单套无色 alpha-mask 水墨资源。深浅与交互状态全部由
# 运行时 tint/承载容器表达，不允许再维护 *_selected.svg 分叉资源。
file(GLOB UI_ICON_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/images/*.svg")
foreach(UI_ICON_FILE IN LISTS UI_ICON_FILES)
    file(READ "${UI_ICON_FILE}" UI_ICON_SOURCE)
    if(UI_ICON_SOURCE MATCHES "width=\"24\"" AND
       UI_ICON_SOURCE MATCHES "height=\"24\"")
        get_filename_component(UI_ICON_NAME "${UI_ICON_FILE}" NAME)
        if(UI_ICON_NAME MATCHES "_selected\\.svg$")
            message(FATAL_ERROR
                "UI icon must use one adaptive asset; remove selected duplicate: ${UI_ICON_FILE}")
        endif()
        string(REPLACE "#FFFFFF" "" UI_ICON_NON_MASK_SOURCE "${UI_ICON_SOURCE}")
        if(UI_ICON_NON_MASK_SOURCE MATCHES "#[0-9A-Fa-f]")
            message(FATAL_ERROR
                "UI icon must be a colorless #FFFFFF alpha mask: ${UI_ICON_FILE}")
        endif()
    endif()
endforeach()

huxerui_add_app(llm-switch
    SOURCES
        ${APP_SOURCE_FILES}
    RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    RESOURCE_NAMESPACE app
    BUNDLE_NAME "llm-switch"
    BUNDLE_IDENTIFIER "dev.farna.llmswitch"
)

if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    # 领域层 C++23 模块（.cppm 接口；对应 .cpp 实现单元已随 glob 进 SOURCES）
    target_sources(llm-switch PRIVATE FILE_SET llmswitch_modules TYPE CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES ${LLMSWITCH_MODULE_UNITS})
    set_target_properties(llm-switch PROPERTIES CXX_MODULE_STD ON)
    # 显式打开全源扫描：模块实现单元（普通 .cpp 里的 module llmswitch.*;）必须经
    # 扫描拿到 -fmodules-ts。SDK 模式下 CMP0155 默认行为恰好扫描全部源，源码
    # 模式（add_subdirectory huxerui）下该默认不覆盖普通 .cpp，会丢模块标志。
    set_target_properties(llm-switch PROPERTIES CXX_SCAN_FOR_MODULES ON)
    target_compile_features(llm-switch PRIVATE cxx_std_23)
    # 应用版本编译期常量（设置页“关于”展示）：唯一来源是顶层 project(VERSION)。
    target_compile_definitions(llm-switch PRIVATE LLMSWITCH_VERSION="${PROJECT_VERSION}")
    if (WIN32)
        target_compile_definitions(llm-switch PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        # GUI 子系统入口
        if (MSVC)
            target_link_options(llm-switch PRIVATE
                /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
        else ()
            target_link_options(llm-switch PRIVATE -mwindows)
        endif ()
    endif ()
    # 源码注释含中文：MSVC 必须显式 /utf-8，否则按 codepage 误读
    if (MSVC)
        target_compile_options(llm-switch PRIVATE /utf-8)
    endif ()
    target_include_directories(llm-switch PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(llm-switch PRIVATE
        llmswitch_json
        llmswitch_httplib
        CURL::libcurl
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
    )

    # 生成格式钩子：平台清单 / Info.plist
    if (WIN32 AND HUXERUI_WINDOWS_MANIFEST)
        target_sources(llm-switch PRIVATE "${HUXERUI_WINDOWS_MANIFEST}")
    endif ()
    if (APPLE AND NOT IOS AND HUXERUI_MACOS_INFO_PLIST)
        set_target_properties(llm-switch PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${HUXERUI_MACOS_INFO_PLIST}")
    endif ()

    # ---- Windows 自定义安装向导 ---------------------------------------------
    # `huxerui package windows` 时生效（日常构建零开销）：platform/windows/
    # huxerui.cmake 定义配置函数，把 MSI + Burn 捆绑包 + 托管安装器 UI
    # （platform/windows/package/，品牌面板 + 简中/繁中/英文目录）接进构建。
    # install 规则是安装包文件唯一来源——图标资产在此显式声明。
    # 需要含 huxerui_add_windows_installer 的 HuxerUI 源码/SDK（0.2.0 之后，
    # 见 third_party/README.md）。
    if (WIN32 AND COMMAND huxerui_configure_windows_project_package)
        get_target_property(LLMSWITCH_APP_INSTALL_COMPONENT llm-switch
            HUXERUI_APPLICATION_INSTALL_COMPONENT)
        if (LLMSWITCH_APP_INSTALL_COMPONENT
                AND NOT LLMSWITCH_APP_INSTALL_COMPONENT MATCHES "-NOTFOUND$")
            huxerui_configure_windows_project_package(llm-switch
                "${LLMSWITCH_APP_INSTALL_COMPONENT}")
            install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets"
                DESTINATION .
                COMPONENT "${LLMSWITCH_APP_INSTALL_COMPONENT}")
        endif ()
    endif ()

    # 图标资源放 <exe>/assets；HuxerUI 资源包（内置+app 合并，由 huxerui_add_app 的
    # 资源构建生成）放 <exe>.resources。最新运行时除了 resources.bin 索引外，
    # 还会按索引读取同目录下的 .huxv/.png 等实际 payload，必须把整个 package
    # 目录复制到运行目录。
    add_custom_command(TARGET llm-switch POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:llm-switch>/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_BINARY_DIR}/huxerui-resources/llm-switch/package"
                "$<TARGET_FILE_DIR:llm-switch>/llm-switch.resources"
        COMMENT "Copying assets and the merged HuxerUI resource package next to llm-switch")
    if (WIN32)
        # HuxerUI 运行时 DLL 需在 exe 旁（抄自姊妹项目 apitab 顶层 WIN32 段）。
        # 源码通道没有 HUXERUI_SDK_DIR，且 dll 实际落在构建树 RUNTIME 输出
        # （多配置时还带 config 子目录），只有 $<TARGET_FILE:> 能跨两种通道、
        # 跨生成器解析；目标缺失（如静态框架构建）时回落 SDK 路径，两处都没有
        # 就跳过。
        set(LLMSWITCH_HUXERUI_DLL "")
        if (TARGET HuxerUI::huxerui)
            get_target_property(LLMSWITCH_HUXERUI_DLL_TYPE HuxerUI::huxerui TYPE)
            if (LLMSWITCH_HUXERUI_DLL_TYPE STREQUAL "SHARED_LIBRARY")
                set(LLMSWITCH_HUXERUI_DLL "$<TARGET_FILE:HuxerUI::huxerui>")
            endif ()
        endif ()
        if (NOT LLMSWITCH_HUXERUI_DLL AND HUXERUI_SDK_DIR
                AND EXISTS "${HUXERUI_SDK_DIR}/bin/huxerui.dll")
            set(LLMSWITCH_HUXERUI_DLL "${HUXERUI_SDK_DIR}/bin/huxerui.dll")
        endif ()
        if (LLMSWITCH_HUXERUI_DLL)
            add_custom_command(TARGET llm-switch POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${LLMSWITCH_HUXERUI_DLL}" "$<TARGET_FILE_DIR:llm-switch>"
                COMMENT "Copying huxerui.dll next to llm-switch")
        endif ()

        # OpenSSL 运行库：系统/安装版 OpenSSL（CI chocolatey）是共享库，
        # llm-switch.exe 直接导入 libssl-N-x64.dll/libcrypto-N-x64.dll，干净
        # Windows 上没有，必须随 exe 带出。静态 OpenSSL 时 bin 下无 dll，
        # glob 为空自然跳过。DLL 统一在 <OpenSSL根>/bin（根由 include 目录推导）。
        if (OPENSSL_INCLUDE_DIR)
            get_filename_component(_llmswitch_openssl_root "${OPENSSL_INCLUDE_DIR}" DIRECTORY)
            file(GLOB _llmswitch_openssl_dlls
                "${_llmswitch_openssl_root}/bin/libssl-*.dll"
                "${_llmswitch_openssl_root}/bin/libcrypto-*.dll")
            if (_llmswitch_openssl_dlls)
                add_custom_command(TARGET llm-switch POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            ${_llmswitch_openssl_dlls} "$<TARGET_FILE_DIR:llm-switch>"
                    COMMENT "Copying OpenSSL runtime DLLs next to llm-switch")
            elseif (NOT OPENSSL_USE_STATIC_LIBS)
                message(WARNING
                    "llm-switch: 未在 ${_llmswitch_openssl_root}/bin 找到 OpenSSL 运行库 dll，"
                    "发布包缺 libssl/libcrypto 将无法在干净 Windows 上启动")
            endif ()
        endif ()

        # MSVC 桌面 CRT：llm-switch.exe/huxerui.dll 都按 /MD 链接，导入
        # msvcp140/vcruntime140/vcruntime140_1；没装 VC++ redist 的机器直接
        # 起不来，随 exe 带出（许可允许 app-local 分发）。vcvars 环境（CI 的
        # ilammy/msvc-dev-cmd、本机 VS 开发者命令行）提供 VCToolsRedistDir；
        # 多版本目录并存时取最后一个（glob 字典序 = 版本序）。
        if (MSVC AND NOT CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "Static")
            set(_llmswitch_crt_dir "")
            if (DEFINED ENV{VCToolsRedistDir})
                file(TO_CMAKE_PATH "$ENV{VCToolsRedistDir}" _llmswitch_redist)
                file(GLOB _llmswitch_crt_dirs LIST_DIRECTORIES true
                    "${_llmswitch_redist}/x64/Microsoft.VC*.CRT")
                foreach (_dir IN LISTS _llmswitch_crt_dirs)
                    if (EXISTS "${_dir}/msvcp140.dll")
                        set(_llmswitch_crt_dir "${_dir}")
                    endif ()
                endforeach()
            endif ()
            if (_llmswitch_crt_dir)
                add_custom_command(TARGET llm-switch POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${_llmswitch_crt_dir}/msvcp140.dll"
                            "${_llmswitch_crt_dir}/vcruntime140.dll"
                            "${_llmswitch_crt_dir}/vcruntime140_1.dll"
                            "$<TARGET_FILE_DIR:llm-switch>"
                    COMMENT "Copying MSVC CRT runtime DLLs next to llm-switch")
            else ()
                message(WARNING
                    "llm-switch: 未定位 MSVC CRT redist（VCToolsRedistDir 未设置），"
                    "发布包缺 msvcp140/vcruntime140*.dll 将无法在干净 Windows 上启动")
            endif ()
        endif ()
    endif ()

    # 仓库根的 compile_commands.json 软链（给 clangd 用；.gitignore 已忽略）
    # 并行测试/多构建并发时多个 build 目录同时创建同一软链接会竞态；已存在则跳过
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json")
        file(CREATE_LINK "${CMAKE_BINARY_DIR}/compile_commands.json"
                         "${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json"
                         SYMBOLIC)
    endif()

    # ---- 测试 ---------------------------------------------------------------
    # 独立于 CTest 的 BUILD_TESTING 变量：第三方子项目可能把它改成 OFF，不能让它
    # 决定 llm-switch 自己的测试。
    option(LLMSWITCH_BUILD_TESTS "Build llm-switch test executables" ON)
    if (LLMSWITCH_BUILD_TESTS)
        enable_testing()
        # 测试断言匹配中文错误串（如「不支持 Linux」「该工具暂不支持 MCP 管理」），
        # MSVC 必须 /utf-8 才能正确读 UTF-8 源文字面量（对后续创建的全部测试目标生效）。
        if (MSVC)
            add_compile_options(/utf-8)
        endif ()
        # 测试目标直接编译 src/*.cpp（curl/httplib 头链会拉进 windows.h）：
        # 不定 NOMINMAX 时 min/max 宏会炸掉 std::min 调用（C2589）。
        if (WIN32)
            add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
        endif ()

        # 冒烟测试：只验证工程能编出可运行二进制。
        add_executable(test_smoke tests/test_smoke.cpp)
        set_target_properties(test_smoke PROPERTIES CXX_MODULE_STD ON)
        target_compile_features(test_smoke PRIVATE cxx_std_23)
        add_test(NAME smoke COMMAND test_smoke)

        # 领域层测试：import llmswitch.config/models/store + nlohmann.json。
        # glob 只进 app 目标，测试目标必须显式追加模块接口（FILE_SET）与实现
        # 单元（store.cpp 是普通 .cpp，须随源扫描拿到 -fmodules-ts）。
        add_executable(test_store tests/test_store.cpp src/store.cpp)
        target_sources(test_store PRIVATE FILE_SET llmswitch_modules TYPE CXX_MODULES
            BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
            FILES ${LLMSWITCH_MODULE_UNITS})
        set_target_properties(test_store PROPERTIES CXX_MODULE_STD ON CXX_SCAN_FOR_MODULES ON)
        target_compile_features(test_store PRIVATE cxx_std_23)
        target_link_libraries(test_store PRIVATE llmswitch_json Threads::Threads)
        add_test(NAME store COMMAND test_store)

        # 网络层测试：只测 parseModelIds 纯函数（不碰真实网络）；net.cpp 实现
        # 单元引用了 curl 符号，须链接 CURL::libcurl + OpenSSL。
        add_executable(test_net tests/test_net.cpp src/net.cpp)
        target_sources(test_net PRIVATE FILE_SET llmswitch_modules TYPE CXX_MODULES
            BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
            FILES src/net.cppm)
        set_target_properties(test_net PROPERTIES CXX_MODULE_STD ON CXX_SCAN_FOR_MODULES ON)
        target_compile_features(test_net PRIVATE cxx_std_23)
        target_link_libraries(test_net PRIVATE
            llmswitch_json CURL::libcurl OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
        add_test(NAME net COMMAND test_net)

        # 本地路由测试：httplib 假上游 + 路由转发/换 key/故障转移/统计日志。
        add_executable(test_router tests/test_router.cpp src/router.cpp)
        target_sources(test_router PRIVATE FILE_SET llmswitch_modules TYPE CXX_MODULES
            BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
            FILES src/router.cppm src/config.cppm src/models.cppm)
        set_target_properties(test_router PROPERTIES CXX_MODULE_STD ON CXX_SCAN_FOR_MODULES ON)
        target_compile_features(test_router PRIVATE cxx_std_23)
        target_link_libraries(test_router PRIVATE
            llmswitch_json llmswitch_httplib
            CURL::libcurl OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
        add_test(NAME router COMMAND test_router)

        # MCP 管理测试：统一清单读写 + 各工具 live 配置写入。
        add_executable(test_mcp tests/test_mcp.cpp src/mcp.cpp)
        target_sources(test_mcp PRIVATE FILE_SET llmswitch_modules TYPE CXX_MODULES
            BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
            FILES src/mcp.cppm src/config.cppm src/models.cppm)
        set_target_properties(test_mcp PROPERTIES CXX_MODULE_STD ON CXX_SCAN_FOR_MODULES ON)
        target_compile_features(test_mcp PRIVATE cxx_std_23)
        target_link_libraries(test_mcp PRIVATE llmswitch_json Threads::Threads)
        add_test(NAME mcp COMMAND test_mcp)

        # Skills 管理测试：中央库 + 符号链接同步 + 扫描。
        add_executable(test_skills tests/test_skills.cpp src/skills.cpp)
        target_sources(test_skills PRIVATE FILE_SET llmswitch_modules TYPE CXX_MODULES
            BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
            FILES src/skills.cppm src/config.cppm)
        set_target_properties(test_skills PROPERTIES CXX_MODULE_STD ON CXX_SCAN_FOR_MODULES ON)
        target_compile_features(test_skills PRIVATE cxx_std_23)
        target_link_libraries(test_skills PRIVATE llmswitch_json Threads::Threads)
        add_test(NAME skills COMMAND test_skills)

        # 会话管理测试：扫描/摘要/删除各工具历史会话。
        add_executable(test_sessions tests/test_sessions.cpp src/sessions.cpp)
        target_sources(test_sessions PRIVATE FILE_SET llmswitch_modules TYPE CXX_MODULES
            BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
            FILES src/sessions.cppm src/config.cppm)
        set_target_properties(test_sessions PROPERTIES CXX_MODULE_STD ON CXX_SCAN_FOR_MODULES ON)
        target_compile_features(test_sessions PRIVATE cxx_std_23)
        target_link_libraries(test_sessions PRIVATE llmswitch_json Threads::Threads)
        add_test(NAME sessions COMMAND test_sessions)
    endif ()
endif ()
