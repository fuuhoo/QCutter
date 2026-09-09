# GitHub Actions 打包 Windows Qt + MinGW 程序指南

> 本文档总结 QCutter 项目 v2.0.x → v2.1.5 期间 CI 调试经验，沉淀为可复用 playbook。
> 目标：让 CI 出的 exe 与本地 Qt Creator Kit 完全一致（编译器、链接、运行时 DLL、ABI 全对齐）。

## 0. 一句话总结

CI 跑 Qt + MinGW 程序，最容易踩的坑是 **编译器不一致**：
本地 Kit 用 Qt 离线安装器 bundle 的 mingw73 (GCC 7.3)，CI 用 windows-2022 runner 自带的 MSYS2 mingw64 (GCC 14)，
exe 大小能差 12%，且有跨 GCC 版本 ABI 风险。

**修复路径**：CI 必须装**和本地一模一样的 MinGW 工具链**。Qt SDK 仓库里有这个 toolchain 的 7z（70 MB），
直接 curl + 7z x 解压即可。**`install-qt-action` 不装 toolchain，`aqtinstall` 不支持 GCC 7.3.0**，两条常见路径都走不通。

---

## 1. 环境对齐的关键差异

| 项 | 本地（Qt Creator Kit "Desktop Qt 5.12.12 MinGW 64-bit"） | CI runner 默认 |
|---|---|---|
| Qt 版本 | 5.12.12 | — |
| Qt arch | `win64_mingw73` | — |
| MinGW 编译器 | 7.3.0 (`D:\Qt\Qt5.12.12\Tools\mingw730_64\bin`) | 14.x (`C:\mingw64\bin`, MSYS2 mingw64) |
| vcpkg triplet | `x64-mingw-dynamic-qt` | — |
| cxx11 ABI | 0 | 1 (GCC 9+ 默认) |

### 1.1 为什么 ABI=0 必选

Qt 5.12 自带的 mingw73 libstdc++ 用 cxx11 ABI=0 编译（`_GLIBCXX_USE_CXX11_ABI=0`）。
如果 vcpkg 编译 tiff/sqlite 等依赖用 ABI=1，QCutter.exe 用 ABI=0 链接，运行时会出现：

```
filesystem::path::parent_path() 无法定位程序输入点
```

或更隐蔽的 heap corruption（std::list size 跨 ABI 边界变化）。
所以 vcpkg triplet 必须强制 ABI=0：

```cmake
# cmake/vcpkg-triplets/x64-mingw-dynamic-qt.cmake
set(VCPKG_CXX_FLAGS "-D_GLIBCXX_USE_CXX11_ABI=0 ${VCPKG_CXX_FLAGS}")
set(VCPKG_C_FLAGS   "-D_GLIBCXX_USE_CXX11_ABI=0 ${VCPKG_C_FLAGS}")
```

### 1.2 编译器版本对 exe 大小的影响

实测 QCutter (Qt 5.12.12 + Qt Creator Kit)：

| 编译器 | exe 大小 | 与本地差距 |
|---|---|---|
| mingw73 GCC 7.3（本地） | 3,716 KB | — |
| mingw81 GCC 8.1 | ~3,750 KB | ~+34 KB |
| MSYS2 mingw64 GCC 14 | 4,182 KB | **+466 KB** |

GCC 14 stdlib 引入更多 c++17/20 符号，即使强制 ABI=0 也无法消除。
**结论**：要 100% 对齐本地 dev，必须用 GCC 7.3。

---

## 2. CI 装 MinGW 工具链的正确路径

### 2.1 ❌ 错误路径 A：`install-qt-action@v4`

```yaml
- uses: jurplel/install-qt-action@v4
  with:
    version: '5.12'
    arch: win64_mingw73
    # 注意：arch: win64_mingw73 只装 Qt 库 (DLLs/prl/pc),
    # 不装 MinGW 工具链. $QT_ROOT_DIR/bin/g++.exe 不存在.
```

### 2.2 ❌ 错误路径 B：`aqtinstall`

```bash
aqt install-tool windows desktop tools_mingw730    # ❌ "Failed to locate XML data"
aqt install-tool windows desktop tools_mingw730_64 # ❌ "Failed to locate XML data"
aqt install-tool windows desktop qt.tools.win64_mingw730  # ❌ aqtinstall v3.3.0 metadata 不支持
```

`aqt list-tool windows desktop` 输出（v3.3.0）：

```
tools_mingw1310    # GCC 13.1
tools_mingw90      # GCC 9.0
tools_mingw81      # GCC 8.1
tools_mingw        # 老 (GCC 4.x)
tools_llvm_mingw2217 / tools_llvm_mingw1706  # LLVM MinGW
```

**GCC 7.3.0 不在 aqtinstall metadata 里**，原因是 aqtinstall 的 XML metadata 是手动维护的快照，不读 Qt 实时 Updates.xml。

### 2.3 ✅ 正确路径：直接 curl Qt SDK 仓库的 7z

Qt SDK 在线仓库（不依赖 aqtinstall）有 mingw730 64-bit 完整工具链：

```
URL: https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/tools_mingw/qt.tools.win64_mingw730/
大小: ~70 MB
文件: 7.3.0-1-202004170606x86_64-7.3.0-release-posix-seh-rt_v5-rev0.7z
解压后: C:/Qt/Tools/mingw730_64/bin/g++.exe
```

这是 Qt 5.12.12 离线安装器 bundle 的**同一份** mingw730_64（与本地 Kit 完全一致）。

```yaml
- name: Install mingw73 toolchain
  run: |
    MINGW_DIR="C:/Qt/Tools/mingw730_64"
    MINGW_ARCHIVE="7.3.0-1-202004170606x86_64-7.3.0-release-posix-seh-rt_v5-rev0.7z"
    MINGW_URL="https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/tools_mingw/qt.tools.win64_mingw730/${MINGW_ARCHIVE}"
    curl -fsSL -o /tmp/mingw730.7z "$MINGW_URL"
    7z x -y -o"C:/Qt" /tmp/mingw730.7z
```

archive 名 `x86_64-7.3.0-release-posix-seh-rt_v5-rev0.7z` 与 niXman/mingw-builds-binaries GCC 7.3.0 release
**一致**（niXman release 已被删，Qt SDK 仓库仍有镜像）。

---

## 3. vcpkg 用 mingw 编译器需要 target-prefixed 名

vcpkg 的 `scripts/toolchains/mingw.cmake` 用：

```cmake
find_program(CMAKE_C_COMPILER "${CMAKE_SYSTEM_PROCESSOR}-w64-mingw32-gcc")
find_program(CMAKE_CXX_COMPILER "${CMAKE_SYSTEM_PROCESSOR}-w64-mingw32-g++")
```

要找 **target-prefixed** 名（`x86_64-w64-mingw32-gcc.exe`），但 Qt mingw bin/ 里只有无前缀的 `gcc.exe`。

修复：复制成 vcpkg 找的 target-prefixed 名。GCC driver 看 `argv[0]` 含 `-w64-mingw32-` 自动用 target triple，
与原 gcc.exe 行为等价：

```bash
for tool in gcc g++ cpp cc c++ ld; do
  src="$MINGW_BIN/${tool}.exe"
  dst="$MINGW_BIN/x86_64-w64-mingw32-${tool}.exe"
  if [ -f "$src" ] && [ ! -f "$dst" ]; then
    cp -v "$src" "$dst"
  fi
done
```

**不要用** `mklink /H ...`（cmd 不在 Git Bash PATH，且需要 admin）。
**不要用** `ln -s ...`（MSYS 软链 Windows 应用不识别）。

---

## 4. 完整工作 workflow（可直接复用）

```yaml
name: Build & Release

on:
  push:
    tags: ['v*']
  workflow_dispatch:

jobs:
  build:
    name: windows-mingw
    runs-on: windows-2022
    defaults:
      run:
        shell: bash
    steps:
      - uses: actions/checkout@v4

      - name: Cache vcpkg binary caches
        uses: actions/cache@v4
        with:
          path: ~/vcpkg
          key: vcpkg-${{ matrix.vcpkg_triplet }}-qt${{ matrix.qt }}-v1

      - name: Install vcpkg
        run: |
          if [ ! -x "$GITHUB_WORKSPACE/vcpkg/vcpkg" ]; then
            # Need full clone (not --depth 1) because vcpkg.json's builtin-baseline
            # points at a specific commit that must be reachable from the clone.
            git clone https://github.com/microsoft/vcpkg.git "$GITHUB_WORKSPACE/vcpkg"
            "$GITHUB_WORKSPACE/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
          fi

      - name: Install Qt
        uses: jurplel/install-qt-action@v4
        with:
          version: '5.12'
          arch: win64_mingw73
          host: windows
          target: desktop
          cache: true

      # ★ 关键步骤：从 Qt SDK 仓库直接 curl mingw73 7z 解压
      - name: Install mingw73 toolchain
        run: |
          MINGW_DIR="C:/Qt/Tools/mingw730_64"
          MINGW_ARCHIVE="7.3.0-1-202004170606x86_64-7.3.0-release-posix-seh-rt_v5-rev0.7z"
          MINGW_URL="https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/tools_mingw/qt.tools.win64_mingw730/${MINGW_ARCHIVE}"
          curl -fsSL -o /tmp/mingw730.7z "$MINGW_URL"
          7z x -y -o"C:/Qt" /tmp/mingw730.7z
          MINGW_BIN="$MINGW_DIR/bin"
          "$MINGW_BIN/g++.exe" --version | head -1
          # 为 vcpkg 创建 target-prefixed wrappers
          for tool in gcc g++ cpp cc c++ ld; do
            src="$MINGW_BIN/${tool}.exe"
            dst="$MINGW_BIN/x86_64-w64-mingw32-${tool}.exe"
            if [ -f "$src" ] && [ ! -f "$dst" ]; then
              cp -v "$src" "$dst"
            fi
          done
          echo "MINGW73_TOOLCHAIN_ROOT=$MINGW_DIR" >> "$GITHUB_ENV"

      # ★ vcpkg install 必须把 mingw bin 放 PATH 最前，让 vcpkg find_program 找到
      # 否则会回退到 PATH 上的 MSVC cl.exe（detect_compiler 失败）
      - name: Install vcpkg dependencies
        env:
          VCPKG_DEFAULT_TRIPLET: x64-mingw-dynamic-qt
          VCPKG_OVERLAY_TRIPLETS: ${{ github.workspace }}/cmake/vcpkg-triplets
        run: |
          export PATH="/c/Qt/Tools/mingw730_64/bin:$PATH"
          # pipefail 保护：vcpkg install 失败也要保留 debug log
          "$GITHUB_WORKSPACE/vcpkg/vcpkg" install 2>&1 | tee vcpkg-install.log || true
          echo "vcpkg install exit=${PIPESTATUS[0]}"

      - name: Configure
        env:
          CMAKE_PREFIX_PATH: ${{ env.QT_ROOT_DIR }}
          VCPKG_DEFAULT_TRIPLET: x64-mingw-dynamic-qt
          VCPKG_OVERLAY_TRIPLETS: ${{ github.workspace }}/cmake/vcpkg-triplets
          CMAKE_C_COMPILER: 'C:/Qt/Tools/mingw730_64/bin/gcc.exe'
          CMAKE_CXX_COMPILER: 'C:/Qt/Tools/mingw730_64/bin/g++.exe'
        run: |
          export PATH="/c/Qt/Tools/mingw730_64/bin:$PATH"
          cmake -S . -B build -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_C_COMPILER='C:/Qt/Tools/mingw730_64/bin/gcc.exe' \
            -DCMAKE_CXX_COMPILER='C:/Qt/Tools/mingw730_64/bin/g++.exe' \
            -DCMAKE_TOOLCHAIN_FILE="$GITHUB_WORKSPACE/vcpkg/scripts/buildsystems/vcpkg.cmake" \
            -DVCPKG_TARGET_TRIPLET="x64-mingw-dynamic-qt" \
            -DCMAKE_PREFIX_PATH="${CMAKE_PREFIX_PATH}"

      - name: Build
        run: cmake --build build --config Release -j 2

      - name: Package
        env:
          QT_ROOT_DIR: ${{ env.QT_ROOT_DIR }}
        run: |
          rm -rf stage && mkdir -p stage
          # 1. exe
          find build -maxdepth 2 \( -name 'QCutter' -o -name 'QCutter.exe' \) \
            -type f -executable -exec cp {} stage/ \; 2>/dev/null || true
          # 2. Qt5 DLLs
          if [ -d "$QT_ROOT_DIR/bin" ]; then
            for d in Qt5Core Qt5Gui Qt5Widgets Qt5Network Qt5Sql Qt5PrintSupport Qt5Concurrent; do
              find "$QT_ROOT_DIR/bin" -maxdepth 1 -name "${d}.dll" -exec cp {} stage/ \; 2>/dev/null || true
            done
          fi
          # 3. platforms/qwindows.dll
          if [ -d "$QT_ROOT_DIR/plugins/platforms" ]; then
            mkdir -p stage/platforms
            cp "$QT_ROOT_DIR/plugins/platforms/qwindows.dll" stage/platforms/
          fi
          # ★ mingw runtime DLLs：从与编译器同源的 toolchain 取
          if [ -d /c/Qt/Tools/mingw730_64/bin ]; then
            for d in libwinpthread-1 libgcc_s_seh-1 libstdc++-6; do
              find /c/Qt/Tools/mingw730_64/bin -maxdepth 1 -name "${d}.dll" \
                -exec cp {} stage/ \; 2>/dev/null || true
            done
          fi
          # 4. vcpkg 运行时 DLLs
          for _vcpkg_dir in \
            "$GITHUB_WORKSPACE/QCutter/vcpkg_installed" \
            "$GITHUB_WORKSPACE/vcpkg_installed" \
            "$GITHUB_WORKSPACE/build/vcpkg_installed"; do
            if [ -d "$_vcpkg_dir" ]; then
              find "$_vcpkg_dir" -path '*/bin/*.dll' -not -path '*/debug/*' \
                -exec cp {} stage/ \; 2>/dev/null || true
              break
            fi
          done
          # 5. 打包
          (cd stage && 7z a "../${{ matrix.artifact_name }}.7z" .)

      - uses: actions/upload-artifact@v4
        with:
          name: ${{ matrix.artifact_name }}
          path: ${{ matrix.artifact_name }}.7z

      # Release 步骤省略
```

---

## 5. CMakeLists.txt 关键配置

```cmake
# 静态链 libstdc++ / libgcc，避免运行时 ABI 不匹配
if(MINGW)
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libstdc++ -static-libgcc")

  # mingw73 std::runtime_error 有 multiple definition 问题
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--allow-multiple-definition")

  # 强制 ABI=0，与 Qt 5.12 mingw73 libstdc++ 一致
  add_compile_definitions(_GLIBCXX_USE_CXX11_ABI=0)
endif()

# 找到 Qt 后, 用 add_subdirectory 或 find_package(Qt5) 都行
find_package(Qt5 COMPONENTS Core Gui Widgets Network Sql PrintSupport REQUIRED)
target_link_libraries(qcut PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets Qt5::Network Qt5::Sql Qt5::PrintSupport)
```

---

## 6. vcpkg overlay triplet 模板

```cmake
# cmake/vcpkg-triplets/x64-mingw-dynamic-qt.cmake
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_ENV_PASSTROUGH PATH)
set(VCPKG_CMAKE_SYSTEM_NAME MinGW)
set(VCPKG_POLICY_DLLS_WITHOUT_LIBS enabled)

# ★ 强制 ABI=0（Qt 5.x mingw 默认 ABI=0，必须对齐）
set(VCPKG_CXX_FLAGS "-D_GLIBCXX_USE_CXX11_ABI=0 ${VCPKG_CXX_FLAGS}")
set(VCPKG_C_FLAGS   "-D_GLIBCXX_USE_CXX11_ABI=0 ${VCPKG_C_FLAGS}")
```

---

## 7. 调试 checklist

### 7.1 vcpkg detect_compiler 失败

**症状**：
```
error: vcpkg was unable to detect the active compiler's information.
The log file content at ".../buildtrees/detect_compiler/stdout-x64-mingw-dynamic-qt.log" is:
-- Configuring x64-mingw-dynamic-qt
CMake Error at scripts/cmake/vcpkg_execute_required_process.cmake:127 (message):
    Command failed: .../ninja.exe -v
    Error code: 1
```

**根因诊断**：

1. **关键诊断**：vcpkg install 失败的 stderr 里只引用 buildtrees 里 .log 文件路径，**真正的 cmake/ninja 错误不会出现在 stdout 里**，必须把 buildtrees tar 出来看。

2. **调试技巧**：在 vcpkg install 步骤的 `|| true` 后保留 buildtrees 打包：
   ```bash
   "$GITHUB_WORKSPACE/vcpkg/vcpkg" install 2>&1 | tee vcpkg-install.log || true
   echo "vcpkg install exit=${PIPESTATUS[0]}"
   tar -czf vcpkg-buildtrees.tar.gz -C "$GITHUB_WORKSPACE/vcpkg" buildtrees 2>/dev/null || true
   mkdir -p vcpkg-debug-logs
   find "$GITHUB_WORKSPACE/vcpkg/buildtrees/detect_compiler" \
     \( -name '*.log' -o -name 'CMakeCache.txt' \) \
     -exec cp -v {} vcpkg-debug-logs/ \; 2>/dev/null || true
   ```

3. **GitHub Actions 默认 bash 用 `pipefail`**：vcpkg install 失败时 `tee` 退出非零，整个 pipeline 立刻终止，后续 tar/find/cp/zip 不会跑。**必须** 在 `tee` 后加 `|| true`，否则 debug artifact 永远是空的。

4. **artifact 上传**：
   ```yaml
   - name: Upload vcpkg debug info (on failure)
     if: failure()
     uses: actions/upload-artifact@v4
     with:
       name: vcpkg-debug-${{ matrix.name }}
       path: |
         vcpkg-install.log
         vcpkg-buildtrees.tar.gz
         vcpkg-debug-logs
   ```

5. **下载 artifact 排查**：
   ```bash
   gh run download <RUN_ID> -n vcpkg-debug-windows-mingw
   unzip vcpkg-debug-windows-mingw.zip
   # 重点看 detect_compiler/x64-mingw-dynamic-qt-{rel,dbg}/CMakeCache.txt.log
   # 和 detect_compiler/x64-mingw-dynamic-qt-{rel,dbg}-out.log
   ```

### 7.2 Configure 阶段编译器路径不存在

**症状**：
```
CMake Error: CMake was unable to find a build program corresponding to "Ninja".
...
The C++ compiler
  "D:/a/QCutter/Qt/5.12.12/mingw73_64/bin/g++.exe"
is not able to compile a simple test program.
```

**根因**：`install-qt-action` 把 Qt 装到 `D:/a/QCutter/Qt/.../mingw73_64/bin/`，但 bin/ 下只有 Qt DLLs，没有 gcc.exe/g++.exe。误以为 `arch: win64_mingw73` 装了 MinGW toolchain。

**修复**：见 §2.3，单独装 mingw73。

### 7.3 运行时缺 DLL

**症状**：双击 exe 弹"找不到 xxx.dll"。

**根因**：发布包里漏拷 vcpkg 依赖的运行时 DLL。

**排查方法**：

```bash
# 用 Qt mingw 的 objdump（支持 PE）查 exe 依赖
objdump -p QCutter.exe | grep "DLL Name" | sort -u
```

**预期**：
- Qt5Core/Gui/Widgets/Network/Sql/PrintSupport.dll
- libsqlite3-0.dll / libsqlite3.dll（看 vcpkg 还是 MSYS2）
- libtiff-6.dll / tiff.dll
- libgcc_s_seh-1.dll / libstdc++-6.dll（如果没 -static-libstdc++）
- libwinpthread-1.dll

**修复**：见 §4 Package 步骤，把所有候选路径都覆盖到。

### 7.4 vcpkg 安装完找不到 install dir

**症状**：Configure 时 cmake 找不到 vcpkg 装的包，要求重新 vcpkg install（用错误的 x64-windows triplet）。

**根因**：vcpkg manifest 模式默认 install 到 `<source>/vcpkg_installed/<triplet>/`，但 cmake 默认会去 `<build>/vcpkg_installed/<triplet>/` 找。如果 cmake 不知道 triplet，会用 x64-windows 重新装一遍。

**修复**：Configure 时显式传：
```bash
-DVCPKG_TARGET_TRIPLET=x64-mingw-dynamic-qt \
-DVCPKG_INSTALLED_DIR="$GITHUB_WORKSPACE/vcpkg_installed" \
```

或用环境变量（在 vcpkg install 步骤和 Configure 步骤都设）：
```yaml
env:
  VCPKG_DEFAULT_TRIPLET: x64-mingw-dynamic-qt
  VCPKG_OVERLAY_TRIPLETS: ${{ github.workspace }}/cmake/vcpkg-triplets
```

---

## 8. 关键概念速查

| 概念 | 解释 |
|---|---|
| **cxx11 ABI** | GCC 5+ 引入新 std::string/list 实现（ABI=1），与旧版（ABI=0）二进制不兼容。Qt 5.12 mingw73 用 ABI=0。 |
| **`-static-libstdc++ -static-libgcc`** | 把 stdlib 静态链进 exe，运行时不需要 libstdc++-6.dll。避免外部 stdlib ABI 版本不一致。 |
| **`-Wl,--allow-multiple-definition`** | mingw73 std::runtime_error 的 strong symbol 在 libstdc++.a 和 Qt5Core.a 里都出现，需要允许 multiple definition。 |
| **`x64-mingw-dynamic-qt`** | vcpkg community triplet，强制 ABI=0 + MinGW toolchain，给 Qt 5 用。 |
| **`x64-mingw-dynamic`** | vcpkg 内置 triplet，cxx11 ABI=1（GCC 默认），不能用于 Qt 5。 |
| **install-qt-action `arch: win64_mingw73`** | 只装 Qt 库，不装 MinGW toolchain。文档陷阱。 |
| **aqtinstall `tools_mingw*`** | metadata 只知道 8.1/9.0/13.1/llvm，不支持 7.3.0。要直接 curl Qt SDK 仓库的 7z。 |
| **vcpkg `mingw.cmake`** | `find_program(x86_64-w64-mingw32-gcc)`，需要 target-prefixed 名。Qt mingw bin/ 只有无前缀 gcc，要复制。 |
| **`pipefail`** | GitHub Actions bash 默认 `-eo pipefail`，vcpkg install 失败时阻断后续命令，必须 `|| true`。 |

---

## 9. 本项目 CI 演进史（v2.0.x → v2.1.5）

| 版本 | 问题 | 修复 | 结果 |
|---|---|---|---|
| v2.0.1–v2.0.6 | vcpkg detect_compiler 失败，env 实验没效果 | — | 失败 |
| v2.0.7 | 加 debug artifact 上传 | — | 失败（artifact 只有 920 字节，pipefail） |
| v2.0.8 | `VCPKG_CMAKE_CONFIGURE_OPTIONS` 强制 gcc.exe | — | 失败（find_program 覆盖） |
| v2.0.9 | `cmd //c mklink` 创建硬链 | — | 失败（cmd 不在 Git Bash PATH） |
| v2.1.0 | revert env 调整 + 加 `\|\| true` | pipefail 修 | 失败（Configure 路径不存在 gcc.exe） |
| v2.1.1 | 改用 MSYS2 mingw64 GCC 14 编 | 路径修 | ✅ 但 exe 4182 KB（与本地不符） |
| v2.1.2 | 用 aqtinstall 装 mingw73 | — | 失败（metadata 不支持） |
| v2.1.3 | 改包名 `tools_mingw730_64` | — | 失败 |
| v2.1.4 | 改包名 `qt.tools.win64_mingw730` | — | 失败（metadata 还是不支持） |
| **v2.1.5** | **直接 curl Qt SDK 仓库 7z** | **路径真正修** | **✅ exe 3722 KB（与本地 3716 KB 差 6 KB）** |

---

## 10. 验证脚本

本地/CI 跑完后做这几件事确认成功：

```bash
# 1. exe 大小对比
ls -la QCutter.exe
# 本地 ~3716 KB（mingw73 GCC 7.3），CI 也应该在 3716-3750 KB 范围

# 2. objdump 看依赖 DLL
/d/Qt/Qt5.12.12/Tools/mingw730_64/bin/objdump.exe -p QCutter.exe | grep "DLL Name" | sort -u
# 应该有 Qt5Core/Gui/Widgets/Network/Sql/PrintSupport/Concurrent
# + libsqlite3/libtiff/libwinpthread 等

# 3. CLI 模式启动
./QCutter.exe --cli
# 应该 exit 2（CLI 模式无输入的预期退出码）

# 4. GUI 模式（如果有条件）
./QCutter.exe
# 应该弹出主窗口，Qt5Core.dll 等 DLL 都能找到
```

---

## 11. 给未来 agent 的提示

1. **永远不要相信 CI runner 默认的 MinGW 编译器**。windows-2022 runner 自带的 MSYS2 mingw64 是 GCC 14，与 Qt 5.12 不匹配。先确认本地 Kit 用的哪个 GCC 版本（Qt 5.12 → 7.3，Qt 5.15 → 8.1，Qt 6.2+ → 11.2），CI 必须装**同一个版本**。

2. **`install-qt-action` 不装 toolchain**。看到 `arch: win64_mingw73` 不要以为 gcc.exe 就在 bin/ 里。

3. **`aqtinstall` 不是万能的**。它的 metadata 是手工快照，老版本（如 GCC 7.3）经常缺失。要装特定版本的 MinGW 时，先 `aqt list-tool windows desktop` 确认；要装 metadata 没有的，直接下 Qt SDK 仓库的 7z。

4. **vcpkg 需要 target-prefixed 编译器名**。Qt mingw bin/ 只有 `gcc.exe`，要 cp 出 `x86_64-w64-mingw32-gcc.exe` 让 vcpkg find_program 找到。

5. **pipefail 会吞 debug 输出**。`vcpkg install | tee log.log` 失败时后续 tar/find/zip 不会跑，必须 `|| true`。

6. **不要用 `mklink` / `ln -s`**。Windows 上 Git Bash 用 mklink 要 admin，ln -s 软链 Windows 应用不识别。用 `cp` 最稳。

7. **ABI=0 不是可选项**。Qt 5.x mingw 默认 ABI=0，vcpkg 依赖如果用 ABI=1 编译，QCutter.exe 启动时 `filesystem::path::parent_path()` 等符号找不到，整个项目崩。

8. **vcpkg 依赖必须和主程序同编译器**。跨 GCC 版本即使 ABI=0 仍有 std::list size 等差异，跨 DLL 边界传容器有 heap corruption 风险。

9. **debug artifact 要打包 buildtrees**。vcpkg detect_compiler 失败的真正 stderr 在 buildtrees 里的 .log 文件，stdout 只有引用路径，不打包 buildtrees 等于没调试信息。

10. **exe 大小是最直接的验证**。如果 CI exe 比本地大几百 KB，几乎肯定是编译器不对（GCC 14 vs 7.3 这种）。一个 `ls -la` 就能看出来。

---

## 12. 参考资料

- Qt SDK 在线仓库：[https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/tools_mingw/](https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/tools_mingw/)
- aqtinstall：[https://github.com/miurahr/aqtinstall](https://github.com/miurahr/aqtinstall)
- install-qt-action：[https://github.com/jurplel/install-qt-action](https://github.com/jurplel/install-qt-action)
- vcpkg mingw triplet 文档：[https://learn.microsoft.com/en-us/vcpkg/users/triplets](https://learn.microsoft.com/en-us/vcpkg/users/triplets)
- GCC ABI policy：[https://gcc.gnu.org/onlinedocs/libstdc++/manual/using_dual_abi.html](https://gcc.gnu.org/onlinedocs/libstdc++/manual/using_dual_abi.html)
- niXman mingw-builds-binaries（GCC 7.3.0 release 已被删，但 Qt SDK 仓库有同款 archive）：[https://github.com/niXman/mingw-builds-binaries](https://github.com/niXman/mingw-builds-binaries)

---

**维护者**：QCutter CI debugging session (2026-09-09)
**适用版本**：Qt 5.12.12 + MinGW 7.3 + vcpkg x64-mingw-dynamic-qt + GitHub Actions windows-2022
**复用条件**：本 playbook 适用于任何要在 GitHub Actions 复刻本地 Qt Creator MinGW Kit 的 Qt 项目。
