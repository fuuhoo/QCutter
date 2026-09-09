# QCutter Release 部署脚本.
# 只在 Release (NDEBUG) 配置下由 POST_BUILD 触发.
# 把运行所需的 Qt DLL / vcpkg 运行时 / 平台插件拷到 package/, 保持
# 目录干净, 用户双击 QCutter.exe 即可运行.
#
# 传参 (cmake -DKEY=VAL 调用):
#   EXE             - 源 exe 路径 (例如 .../Release/QCutter.exe)
#   PACKAGE_DIR     - 输出目录 (例如 .../Release/package)
#   QT_BIN_DIR      - Qt 安装的 bin/ 目录 (含 Qt5Core.dll 等)
#   VCPKG_BIN_DIR   - vcpkg installed bin 目录 (含 libsqlite3.dll 等)
#   VCPKG_FALLBACK  - Release vcpkg 缺失时用 Debug 的 (同 triplet DLL 相同)
#   MINGW_BIN_DIR   - mingw730_64 bin 目录 (含 libwinpthread-1.dll 等)
#                     OR MSYS2 mingw64/bin (链接了 MSYS2 libsqlite3 时)
#   CONDA_BIN_DIR   - conda 的 Library/bin (链接了 conda libtiff 时)
#                     启发式 fallback: C:/Users/<user>/miniconda3/Library/bin
#   MSYS2_BIN_DIR   - MSYS2 的 mingw64/bin (链接了 MSYS2 libsqlite3 / libtiff 时
#                     运行时 dll 在这里, 如 libsqlite3-0.dll / libtiff-6.dll)
#   ASSETS_SRC      - 仓库 assets/ 源目录 (可缺失, 走 CopyAssetsIfPresent)
#
# 设计原则:
#   - 只列已知必需的 DLL, 不跑 windeployqt, 避免 50MB+ 冗余
#   - platforms/qwindows.dll 是 QApplication 在 Windows 启动必需,
#     拷到 package/platforms/qwindows.dll
#   - assets 复用 CopyAssetsIfPresent 防止源码树为空时报错
#   - 出错用 message(FATAL_ERROR) 阻断构建 (缺关键依赖比静默更安全)

# ---- 参数预处理 ----
# cmake -DEXE="..." 在 cmd.exe /C 嵌套下不会自动去掉两端引号
# (bash 直接调 cmake 时 bash 会去引号, 但 ninja 通过 cmd.exe 调
#  cmake 时不会), 导致 EXISTS / GLOB 把整个带引号字符串当路径.
# 对所有路径参数统一 strip 两端引号, 兼容性最好.
foreach(_strip_v EXE PACKAGE_DIR QT_BIN_DIR VCPKG_BIN_DIR VCPKG_FALLBACK
                  MINGW_BIN_DIR MSYS2_BIN_DIR CONDA_BIN_DIR ASSETS_SRC)
    if(DEFINED ${_strip_v})
        # cmake -DEXE="path" 在 cmd.exe /C 嵌套下保留两端的字面 "
        # (bash 直接调 cmake 时 bash 会去掉). STRIP 不去引号 (它只去
        # 空白), 用 REGEX 显式剥离两端的双引号 (有或没有).
        string(REGEX REPLACE "^\"+|\"+$" "" ${_strip_v} "${${_strip_v}}")
    endif()
endforeach()

# ---- sanity checks ----
# VCPKG_BIN_DIR / VCPKG_FALLBACK 允许为空, 其他目录必须存在且非空.
foreach(_v EXE PACKAGE_DIR QT_BIN_DIR MINGW_BIN_DIR)
    if(NOT DEFINED ${_v} OR "${${_v}}" STREQUAL "")
        message(FATAL_ERROR "DeployRelease: 缺少参数 ${_v}")
    endif()
endforeach()

if(NOT EXISTS "${EXE}")
    message(FATAL_ERROR "DeployRelease: EXE 不存在: ${EXE}")
endif()

# vcpkg 路径可选: 主路径空/不存在时试 fallback, 都不行就跳过 vcpkg DLL
set(_vcpkg_enabled FALSE)
if(DEFINED VCPKG_BIN_DIR AND NOT "${VCPKG_BIN_DIR}" STREQUAL ""
        AND EXISTS "${VCPKG_BIN_DIR}")
    set(_vcpkg_enabled TRUE)
    set(_vcpkg_used "${VCPKG_BIN_DIR}")
elseif(DEFINED VCPKG_FALLBACK AND NOT "${VCPKG_FALLBACK}" STREQUAL ""
        AND EXISTS "${VCPKG_FALLBACK}")
    set(_vcpkg_enabled TRUE)
    set(_vcpkg_used "${VCPKG_FALLBACK}")
    message(STATUS "DeployRelease: 主 vcpkg 路径不可用, 用 fallback: ${VCPKG_FALLBACK}")
endif()

# ---- 清空并重建 package 目录 ----
# 保证旧构建遗留的 dll 不会污染新版本 (例如某次去掉了某个 Qt 模块)
file(REMOVE_RECURSE "${PACKAGE_DIR}")
file(MAKE_DIRECTORY "${PACKAGE_DIR}")

# ---- 1. 拷贝 exe 本身 ----
get_filename_component(_exe_name "${EXE}" NAME)
configure_file("${EXE}" "${PACKAGE_DIR}/${_exe_name}" COPYONLY)

# ---- 2. 拷贝 Qt 核心 DLL (Release 配置, 不带 d 后缀) ----
set(_qt_dlls
    Qt5Core.dll
    Qt5Gui.dll
    Qt5Widgets.dll
    Qt5Network.dll
    Qt5Sql.dll
    Qt5PrintSupport.dll
)
foreach(_dll ${_qt_dlls})
    set(_src "${QT_BIN_DIR}/${_dll}")
    if(EXISTS "${_src}")
        file(COPY "${_src}" DESTINATION "${PACKAGE_DIR}")
    else()
        message(FATAL_ERROR "DeployRelease: 找不到 Qt DLL: ${_src}")
    endif()
endforeach()

# ---- 3. 拷贝 vcpkg 提供的运行时 DLL (可选) ----
# (sqlite3 等的 .dll 在 vcpkg_installed/<triplet>/bin)
if(_vcpkg_enabled)
    file(GLOB _vcpkg_dlls LIST_DIRECTORIES false "${_vcpkg_used}/*.dll")
    foreach(_dll ${_vcpkg_dlls})
        file(COPY "${_dll}" DESTINATION "${PACKAGE_DIR}")
    endforeach()
    message(STATUS "DeployRelease: copied vcpkg DLLs from ${_vcpkg_used}")
else()
    message(WARNING "DeployRelease: vcpkg DLLs 不可用, package/ 缺少 libtiff/sqlite3/zlib 等运行 DLL, "
                    "exe 启动可能失败. 检查 VCPKG_INSTALLED_DIR 或先在 Debug 跑一次让 vcpkg 实例化.")
endif()

# ---- 3.5. 拷贝 conda bin (可选) ----
# 如果链接时 conda 的 libtiff.lib (导入 tiff.dll) 被选, 这里就要拷
# tiff.dll / sqlite3.dll / 等. 启发式 fallback: cmake 配置时如果
# 没传 CONDA_BIN_DIR (没设 CONDA_PREFIX), 尝试常见路径.
set(_conda_used "")
if(DEFINED CONDA_BIN_DIR AND NOT "${CONDA_BIN_DIR}" STREQUAL ""
        AND EXISTS "${CONDA_BIN_DIR}")
    set(_conda_used "${CONDA_BIN_DIR}")
else()
    # 兜底: 当前用户的 miniconda3/anaconda3
    if(DEFINED ENV{USERPROFILE})
        foreach(_candidate
                "$ENV{USERPROFILE}/miniconda3/Library/bin"
                "$ENV{USERPROFILE}/anaconda3/Library/bin")
            if(EXISTS "${_candidate}")
                set(_conda_used "${_candidate}")
                message(STATUS "DeployRelease: 启发式找到 conda bin: ${_candidate}")
                break()
            endif()
        endforeach()
    endif()
endif()

if(NOT "${_conda_used}" STREQUAL "")
    # 只拷可能需要的: tiff (导入 tiff.dll), sqlite (导入 sqlite3.dll)
    foreach(_dll tiff.dll sqlite3.dll)
        set(_src "${_conda_used}/${_dll}")
        if(EXISTS "${_src}")
            file(COPY "${_src}" DESTINATION "${PACKAGE_DIR}")
        endif()
    endforeach()
    message(STATUS "DeployRelease: copied conda DLLs from ${_conda_used}")
endif()

# ---- 4. 拷贝 mingw 运行时 + MSYS2 依赖 ----
# -static-libgcc / -static-libstdc++ 已经在链接阶段做掉了, 但 Qt 的
# libQt5Core.dll 等会用 mingw 线程模型, 仍依赖 libwinpthread-1.dll.
# libstdc++-6.dll / libgcc_s_seh-1.dll 在静态链时不需要, 但留着也无害.
#
# 如果 exe 链接时使用了 MSYS2 mingw64 的 libsqlite3.dll.a / libtiff.dll.a
# (而不是 vcpkg 的), 运行时 DLL (libsqlite3-0.dll / libtiff-6.dll) 也必须
# 从同一来源拷过来.
#
# 关键洞察: 链接器可能从多个源链了同一个 lib (Qt Creator Kit 不一致),
# 例如 libtiff 来自 conda (导入 tiff.dll), libsqlite3 来自 MSYS2 (导入
# libsqlite3-0.dll). 缺哪个 dll, package 就启动失败. 这里把每个 dll
# 在多个候选路径搜一遍, 找到就拷.
set(_dll_search_paths
    "${MINGW_BIN_DIR}"
    "${_conda_used}"
)
foreach(_vcpkg_candidate ${VCPKG_BIN_DIR} ${VCPKG_FALLBACK})
    if(NOT "${_vcpkg_candidate}" STREQUAL "")
        list(APPEND _dll_search_paths "${_vcpkg_candidate}")
    endif()
endforeach()
# MSYS2 mingw64/bin (链接 MSYS2 提供的 libsqlite3 / libtiff 时 dll 在这里)
if(DEFINED MSYS2_BIN_DIR AND NOT "${MSYS2_BIN_DIR}" STREQUAL ""
        AND EXISTS "${MSYS2_BIN_DIR}")
    list(APPEND _dll_search_paths "${MSYS2_BIN_DIR}")
endif()

# mingw 基础 DLL (Qt 必需): 缺失报错, 不接受 fallback
foreach(_dll libwinpthread-1.dll libgcc_s_seh-1.dll libstdc++-6.dll)
    set(_found FALSE)
    foreach(_path ${_dll_search_paths})
        if(EXISTS "${_path}/${_dll}")
            file(COPY "${_path}/${_dll}" DESTINATION "${PACKAGE_DIR}")
            set(_found TRUE)
            break()
        endif()
    endforeach()
    if(NOT _found AND EXISTS "${MINGW_BIN_DIR}/${_dll}")
        # 已在 mingw bin 查找过, 上面漏掉, 直接报错
        message(WARNING "DeployRelease: 找不到 mingw 基础 DLL: ${_dll}")
    endif()
endforeach()

# MSYS2 mingw64 风格的 dll (链接了 MSYS2 libsqlite3 时): 缺失警告
foreach(_dll libsqlite3-0.dll libtiff-6.dll libtiffxx-6.dll)
    set(_found FALSE)
    foreach(_path ${_dll_search_paths})
        if(EXISTS "${_path}/${_dll}")
            file(COPY "${_path}/${_dll}" DESTINATION "${PACKAGE_DIR}")
            set(_found TRUE)
            break()
        endif()
    endforeach()
endforeach()

# ---- 5. 拷贝 Qt 平台插件 (platforms/qwindows.dll) ----
# 没这个 QApplication 直接 segfault 在启动阶段.
set(_qt_plugins_src "${QT_BIN_DIR}/../plugins")
if(EXISTS "${_qt_plugins_src}/platforms/qwindows.dll")
    file(MAKE_DIRECTORY "${PACKAGE_DIR}/platforms")
    file(COPY "${_qt_plugins_src}/platforms/qwindows.dll"
         DESTINATION "${PACKAGE_DIR}/platforms")
else()
    message(FATAL_ERROR "DeployRelease: 找不到 qwindows.dll: ${_qt_plugins_src}/platforms")
endif()

# ---- 6. 拷贝 assets (maplibre 等) ----
if(DEFINED ASSETS_SRC AND EXISTS "${ASSETS_SRC}")
    set(SRC "${ASSETS_SRC}")
    set(DST "${PACKAGE_DIR}/assets")
    include("${CMAKE_CURRENT_LIST_DIR}/CopyAssetsIfPresent.cmake")
endif()

# ---- 完成 ----
message(STATUS "")
message(STATUS "==== QCutter Release package ready ====")
message(STATUS "  ${PACKAGE_DIR}")
message(STATUS "======================================")
message(STATUS "")
