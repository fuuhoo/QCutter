cmake_minimum_required(VERSION 3.21)
project(QCutter VERSION 0.1.0 LANGUAGES CXX)

# ----------------------------------------------------------------------------
# 全局编译设置
# ----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
    # MinGW 7.3.0 (Qt 5.12.12 自带) 需要手动链接 stdc++fs 才能用 std::filesystem
    add_compile_options(-lstdc++fs)
    # 把 stdc++fs 加到 link options; 注意该选项要放在最后才生效，
    # 在 target_link_libraries 里也会再次追加以保证链接顺序。
    add_link_options(-lstdc++fs)
    # Qt 5.15.2 mingw81 自带的 libstdc++-6.dll 是 GCC 8.1.0 编的 (默认 cxx11 ABI),
    # 但 GCC 8/9 在不同 minor 版本上 _GLIBCXX_USE_CXX11_ABI 默认值不一致,
    # 加上 vcpkg mingw 用的 GCC 11+ 默认 ABI=1, 跨版本运行时容易出现
    # "filesystem::path::parent_path() 无法定位程序输入点" 错误.
    # 解决: 静态链接 libgcc/libstdc++ 进 exe, 这样 QCutter.exe 完全不依赖
    # Qt 自带的 libstdc++-6.dll (避免 ABI mismatch).
    add_link_options(-static-libgcc -static-libstdc++)
    # mingw GCC 7.3.0 的 libstdc++.a 在旧 ABI (_GLIBCXX_USE_CXX11_ABI=0)
    # 下, cow-stdexcept.o 里 std::runtime_error/logic_error 的拷贝构造函数
    # 是 strong 符号 (T), 而任何带 try/catch (const std::exception&) 或
    # 继承 std::runtime_error 的 TU 在编译时也会生成同名 strong 符号.
    # 链接时两边都进 exe, 报 multiple definition.
    # 允许重复定义, ld 会保留先遇到的版本 (来自 libstdc++.a 的官方实现).
    add_link_options(-Wl,--allow-multiple-definition)
endif()

# Qt 5.15.2 mingw81 是用 GCC 8.1.0 编译的, 默认 _GLIBCXX_USE_CXX11_ABI=0
# (旧 ABI, 符号不带 _cxx11 双下划线). 我们的 vcpkg mingw 用 GCC 11+ 编译时
# 默认 _GLIBCXX_USE_CXX11_ABI=1 (新 ABI), 链接 Qt 自带的 libstdc++-6.dll 时
# 找不到 path::parent_path() 等 _cxx11 符号, 程序启动报
# "ZNKS[t]10filesystem7_cxx11path11parent_pathEv 无法定位程序输入点".
# 强制关掉 cxx11 ABI 让 QCutter 用与 Qt5 相同的符号命名.
# 这只影响 mingw build (非 MSVC), 也确保 FetchContent 路径的 nlohmann_json
# 用相同 ABI 编译.
if(NOT MSVC)
    add_compile_definitions(_GLIBCXX_USE_CXX11_ABI=0)
endif()

# ----------------------------------------------------------------------------
# 依赖项
# ----------------------------------------------------------------------------
# 优先使用 Qt5_DIR / CMAKE_PREFIX_PATH (Qt Creator Kit 设置会传入)
find_package(Qt5 5.12 REQUIRED COMPONENTS Core Gui Widgets Network Concurrent Sql PrintSupport)

# libtiff + sqlite3: 通过 vcpkg toolchain 自动可用, 或系统已安装
find_package(TIFF REQUIRED)

# sqlite3: 标准 CMake 包名是 "SQLite3" (大写), 不是 "Sqlite3"
find_package(SQLite3 REQUIRED)

# nlohmann_json: 由 vcpkg 提供, 也可走 FetchContent (fallback)
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# ----------------------------------------------------------------------------
# QCutterCore 静态库 (无 Qt GUI 依赖, 纯 C++ 引擎, 可被独立测试/嵌入)
# ----------------------------------------------------------------------------
set(CORE_SOURCES
    src/util/paths.cpp
    src/util/logger.cpp
    src/engine/error.cpp
    src/engine/alpha.cpp
    src/engine/planner.cpp
    src/engine/mercator.cpp
    src/engine/meta.cpp
    src/engine/proj_engine.cpp
    src/engine/source.cpp
    src/engine/writer.cpp
    src/engine/cutter.cpp
    src/api/history_store.cpp
    src/api/preview_server.cpp
    src/api/task_manager.cpp
)
add_library(QCutterCore STATIC ${CORE_SOURCES})
target_include_directories(QCutterCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(QCutterCore
    PUBLIC
        Qt5::Core
        Qt5::Gui
        Qt5::Network
        Qt5::Sql
        Qt5::PrintSupport
        TIFF::TIFF
        SQLite::SQLite3
        nlohmann_json::nlohmann_json
)
find_package(Threads REQUIRED)
target_link_libraries(QCutterCore PUBLIC Threads::Threads)

# MinGW 7.3.0 链接 stdc++fs 必须放在最后才能解析 unresolved symbols。
if(NOT MSVC)
    target_link_libraries(QCutterCore PUBLIC stdc++fs)
endif()

# ----------------------------------------------------------------------------
# QCutter 可执行文件
# ----------------------------------------------------------------------------
set(APP_SOURCES
    src/main.cpp
    src/ui/main_window.cpp
    src/ui/pages/home_shell.cpp
    src/ui/pages/new_task_page.cpp
    src/ui/pages/tasks_page.cpp
    src/ui/pages/settings_page.cpp
    src/ui/widgets/task_card.cpp
    src/state/app_state.cpp
    src/ui/app_theme.cpp
)
add_executable(QCutter ${APP_SOURCES}
    src/test.h src/test.cpp)
# GUI 程序: 不分配 console 窗口, 启动时不会出现黑框
# (CLI 模式 --cut/--cli 仍走 stderr 输出, 不会影响)
if(WIN32)
    set_target_properties(QCutter PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
target_link_libraries(QCutter
    PRIVATE
        QCutterCore
        Qt5::Gui
        Qt5::Widgets
)
if (WIN32)
    # for crash handler in src/main.cpp (SetUnhandledExceptionFilter + StackWalk)
    target_link_libraries(QCutter PRIVATE dbghelp)
endif()
target_include_directories(QCutter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# MapLibre 资源 (运行时由 writer.cpp 从 assets/ 拷贝到输出目录).
# assets/maplibre/{*.js, *.css} 在 .gitignore 中 (运行时由代码按需下载),
# 所以源码树里 assets/ 可能为空或缺失.
# 注意: 不要在这里加 add_custom_command 拷贝 assets 到 build 目录 — 那样
# 会和下面的 DeployRelease 的 add_custom_command 一起被 ninja 合并成
# "cmd.exe /C A && B", 再被 link rule 包一层 cmd.exe /C 形成嵌套,
# cmd 解析嵌套引号失败导致整个 build 报 FAILED (即使 g++ 实际成功).
# DeployRelease.cmake 最后一步已经 include 了 CopyAssetsIfPresent,
# 一次 POST_BUILD 只有一个 cmd.exe /C 包装.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets")
    install(DIRECTORY assets DESTINATION share/qcutter)
endif()

# ----------------------------------------------------------------------------
# Release 打包: 把 QCutter.exe + Qt DLL + vcpkg 运行时 + assets
# 收集到 build/.../Release/package/, 用户双击 exe 即可运行.
# 只在 Release 配置 (NDEBUG) 下触发, Debug 保持不变 (用 IDE 启动更顺手).
# ----------------------------------------------------------------------------
if(WIN32 AND NOT MSVC AND CMAKE_BUILD_TYPE STREQUAL "Release")
    # Qt bin 目录: Qt5_DIR 是 .../mingw73_64/lib/cmake/Qt5, 回溯到 mingw73_64/
    get_filename_component(_qt_prefix "${Qt5_DIR}" DIRECTORY)   # .../lib/cmake
    get_filename_component(_qt_prefix "${_qt_prefix}" DIRECTORY) # .../lib
    get_filename_component(_qt_prefix "${_qt_prefix}" DIRECTORY) # .../mingw73_64
    set(_qt_bin "${_qt_prefix}/bin")

    # vcpkg bin 目录: $VCPKG_INSTALLED_DIR/$VCPKG_TARGET_TRIPLET/bin
    # 注意 vcpkg mingw dynamic 模板不带 release/debug 子目录, 所有 DLL
    # 都在 bin/ 下 (release 和 debug 的 .lib 同样混放, 链接器按需选).
    # 路径在 POST_BUILD 时可能还没实例化 (vcpkg_installed 在某些
    # Release build 里缺失), 脚本内部会做 fallback.
    set(_vcpkg_bin "")
    if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        set(_vcpkg_bin
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
    endif()
    # Debug 目录下的 vcpkg_installed 作为 fallback, 同一 triplet DLL 一致
    set(_vcpkg_fallback "")
    if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        get_filename_component(_build_root
            "${CMAKE_BINARY_DIR}" DIRECTORY)               # .../Desktop_...
        set(_vcpkg_fallback
            "${_build_root}-Debug/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/bin")
    endif()

    # mingw730_64 bin: 从编译器路径推. mingw730 g++ 在
    # .../Tools/mingw730_64/bin/g++.exe, bin 目录就是它的同级.
    get_filename_component(_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)

    # conda bin (libtiff / sqlite3 等可能从 conda 链接): 检测 CONDA_PREFIX
    # 环境变量 (conda activate 时会设). 若 cmake 配置时找不到, 脚本端
    # 用启发式找 C:/Users/<user>/miniconda3/Library/bin 作为兜底.
    set(_conda_bin "")
    if(DEFINED ENV{CONDA_PREFIX})
        set(_conda_bin "$ENV{CONDA_PREFIX}/Library/bin")
    endif()

    # MSYS2 mingw64 bin (链接器若用了 MSYS2 的 libsqlite3.dll.a /
    # libtiff.dll.a, 运行时 dll 在这里). 启发式: 找 PATH 上的 mingw64 bin.
    set(_msys2_bin "")
    if(DEFINED ENV{MSYS2_ROOT})
        set(_msys2_bin "$ENV{MSYS2_ROOT}/mingw64/bin")
    elseif(EXISTS "C:/msys64/mingw64/bin")
        set(_msys2_bin "C:/msys64/mingw64/bin")
    endif()

    # 用 add_custom_target 而不是 add_custom_command(POST_BUILD):
    # 后者会让 ninja 把 deploy 嵌入 CXX_EXECUTABLE_LINKER rule 的
    # $POST_BUILD 字段, 而 link rule 模板自身又包了一层
    # "cmd.exe /C $PRE_LINK && g++ ... && $POST_BUILD".
    # 如果 $POST_BUILD 内部还包含嵌套 cmd.exe /C (例如 Qt Creator
    # vcpkg integration 加的 "vcpkg z-applocal"), 整体会成为嵌套
    #   cmd.exe /C "cmd.exe /C ... && cmd.exe /C ..."
    # 未转义引号导致 cmd 解析失败, ninja 报 FAILED (虽然 g++ 实际成功).
    # 改成独立 target 后, ninja 给 link rule 一个空的 POST_BUILD,
    # 给 deploy target 单独一个 command edge, 没有嵌套.
    add_custom_target(deploy_release ALL
        COMMAND ${CMAKE_COMMAND}
            -DEXE="$<TARGET_FILE:QCutter>"
            -DPACKAGE_DIR="$<TARGET_FILE_DIR:QCutter>/package"
            -DQT_BIN_DIR="${_qt_bin}"
            -DVCPKG_BIN_DIR="${_vcpkg_bin}"
            -DVCPKG_FALLBACK="${_vcpkg_fallback}"
            -DMINGW_BIN_DIR="${_mingw_bin}"
            -DMSYS2_BIN_DIR="${_msys2_bin}"
            -DCONDA_BIN_DIR="${_conda_bin}"
            -DASSETS_SRC="${CMAKE_CURRENT_SOURCE_DIR}/assets"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/DeployRelease.cmake"
        DEPENDS QCutter
        COMMENT "Building portable Release package (Qt DLLs + vcpkg runtime + assets)"
        VERBATIM
    )
endif()

# ----------------------------------------------------------------------------
# 摘要信息
# ----------------------------------------------------------------------------
message(STATUS "")
message(STATUS "==== QCutter build configuration ====")
message(STATUS "  Qt5         : ${Qt5_VERSION}")
message(STATUS "  Compiler    : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Build type  : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Source dir  : ${CMAKE_SOURCE_DIR}")
message(STATUS "  Binary dir  : ${CMAKE_BINARY_DIR}")
message(STATUS "====================================")
message(STATUS "")
