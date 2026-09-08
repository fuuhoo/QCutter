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
# 所以源码树里 assets/ 可能为空或缺失. 用 CMake script 守卫跳过防止 build 报错.
add_custom_command(
    TARGET QCutter POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DSRC="${CMAKE_CURRENT_SOURCE_DIR}/assets"
        -DDST="$<TARGET_FILE_DIR:QCutter>/assets"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyAssetsIfPresent.cmake"
    COMMENT "Copying MapLibre assets to build directory (skipped if absent)"
    VERBATIM
)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets")
    install(DIRECTORY assets DESTINATION share/qcutter)
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
