# QCutter

QCutter 是 [swCutter](https://github.com/sw cutter) 的 Qt5 重构版本，1:1 复刻全部核心功能。

## 功能特性

- ✅ GeoTIFF 切片为 Web 瓦片（PNG / XYZ 或 TMS）
- ✅ 相对模式金字塔（Google 风格）
- ✅ Mercator 绝对级别模式（gdal2tiles 兼容）
- ✅ GeoTIFF 地理参考自动检测（EPSG:3857 / 4326 / UTM / 中国 GK）
- ✅ 精确反算（每像素反算到源投影坐标）
- ✅ 透明处理三模式（Keep / Threshold / ColorKey）
- ✅ 双线性 / 最近邻重采样
- ✅ 瓦片估算、并发调度、断点续切
- ✅ 任务队列：启动 / 暂停 / 恢复 / 取消 / 删除
- ✅ 进度实时上报（Qt 信号 + 速度计算）
- ✅ SQLite 历史持久化（含旧版 history.json 迁移）
- ✅ 浏览器预览（本地 MapLibre 静态服务）
- ✅ 暗色 / 亮色主题
- ✅ 全局设置（默认输出 / 瓦片尺寸 / 重采样 / 并发度 / 底图）

## 架构

```
QCutter (Qt5 GUI 应用, 桌面)
├── main.cpp / main_window / home_shell
├── pages/      (新建任务 / 任务中心 / 设置)
├── widgets/    (TaskCard 等)
├── state/      (全局 AppState: 任务 + 设置 + 速度 tick)
├── api/        (TaskManager / HistoryStore / PreviewServer)
├── engine/     (alpha / planner / mercator / proj_engine / meta / source / cutter / writer)
└── util/       (paths / logger)
```

`QCutterCore` 是无 GUI 依赖的静态库，便于嵌入其他项目或单元测试。

## 依赖

- Qt5 (5.12+) — Core / Gui / Widgets / Network / Concurrent / Sql
- libtiff (TIFF 文件读写)
- Sqlite3 (任务历史持久化)
- nlohmann/json (JSON 序列化)
- CMake 3.21+ (CMakePresets)
- MinGW 7.3.0+ / MSVC 2017+ 编译器

## 快速开始（推荐方式：Qt Creator）

1. 双击桌面 **Qt Creator 5.12.12** 快捷方式（已由 `install_qtcreator_shortcut.bat` 生成）
2. **File → Open File or Project...** → 选择 `CMakeLists.txt`
3. 选择 **Desktop Qt 5.12.12 MinGW 64-bit** Kit
4. Qt Creator 提示安装依赖时，运行 `setup_env.bat`（vcpkg 自动安装）
5. **Build → Run Project** (Ctrl+B, Ctrl+R)

详细 Kit 配置请见 [KITS_SETUP.md](KITS_SETUP.md)。

## 手动编译 (Windows / MinGW)

```powershell
# 1) 安装依赖 (任选其一)
setup_env.bat                    # 通过 vcpkg 安装
install_libtiff_msys2.bat        # 通过 MSYS2 pacman 安装

# 2) 编译
mkdir build; cd build
cmake -G "MinGW Makefiles" ^
    -DCMAKE_PREFIX_PATH="C:/Qt/Qt5.12.12/5.12.12/mingw73_64" ^
    -DCMAKE_C_COMPILER="C:/Qt/Qt5.12.12/Tools/mingw730_64/bin/gcc.exe" ^
    -DCMAKE_CXX_COMPILER="C:/Qt/Qt5.12.12/Tools/mingw730_64/bin/g++.exe" ^
    -DCMAKE_MAKE_PROGRAM="C:/Qt/Qt5.12.12/Tools/mingw730_64/bin/mingw32-make.exe" ^
    ..
cmake --build .
.\QCutter.exe
```

## 手动编译 (MSVC)

```powershell
# 在 VS 2017/2019/2022 Developer Command Prompt 中
setup_env.bat
mkdir build; cd build
cmake -G "Visual Studio 17 2022" -A x64 ..
cmake --build . --config Release
.\Release\QCutter.exe
```

## 用法

### GUI
启动应用后：
- 选择左侧「新建任务」 → 「选择 GeoTIFF…」 → 配置参数 → 「开始切片」
- 「任务中心」查看进度、暂停/取消、点击「预览」在浏览器打开切片

### 输出目录结构
```
output/
├── manifest.json          # 瓦片元信息
├── preview.html           # MapLibre 预览页
├── maplibre-gl.{js,css}   # 离线 MapLibre 资源
└── {z}/{x}/{y}.png        # 瓦片
```

## 与 swCutter (Rust/Flutter) 的差异

- UI 由 Flutter 改为 Qt5 Widgets
- 投影引擎由 proj4rs 改为纯 C++ 自实现 (无 libproj 依赖)
- 颜色转换 / 重采样由 image crate 改为 Qt5 QImage
- PNG 编码由 image crate 改为 Qt5 QImage::save
- 并发由 rayon 改为 std::async + std::thread

## License

MIT