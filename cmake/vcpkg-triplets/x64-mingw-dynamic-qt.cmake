# vcpkg overlay triplet for QCutter + Qt 5.
#
# 基于 vcpkg 内置的 x64-mingw-dynamic (triplets/community/x64-mingw-dynamic.cmake),
# 加上 _GLIBCXX_USE_CXX11_ABI=0 强制, 与 QCutter 和 Qt 5.12 mingw73 的
# libstdc++ 旧 ABI 一致, 避免运行时 "filesystem::path::parent_path() 无法
# 定位程序输入点" (cxx11 双下划线符号缺失).
#
# 用法:
#   cmake -DCMAKE_TOOLCHAIN_FILE=.../vcpkg.cmake \
#         -DVCPKG_OVERLAY_TRIPLETS=<repo>/cmake/vcpkg-triplets

set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_ENV_PASSTHROUGH PATH)
set(VCPKG_CMAKE_SYSTEM_NAME MinGW)
set(VCPKG_POLICY_DLLS_WITHOUT_LIBS enabled)

# 强制旧 ABI, 与 Qt 5 + GCC 7.3 自带的 libstdc++ 兼容.
set(VCPKG_CXX_FLAGS "-D_GLIBCXX_USE_CXX11_ABI=0 ${VCPKG_CXX_FLAGS}")
set(VCPKG_C_FLAGS   "-D_GLIBCXX_USE_CXX11_ABI=0 ${VCPKG_C_FLAGS}")
