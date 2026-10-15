# DimMon — brightnessctl

Windows 显示器亮度控制工具：一个**零依赖的单文件 exe**（约 350 KB），不装运行时、不写注册表、不留后台服务。

两种用法：

- **双击 exe** → 后台常驻 + 自动打开一个本地网页做滑杆调节（推荐）
- **命令行** → `brightnessctl set 70`，适合写进脚本/快捷键

亮度核心改写自 [Monitorian](https://github.com/emoacht/Monitorian)（C#/WPF），本项目把它重写成纯 C++17，界面从 WPF 换成内嵌的本地网页。

---

## 特性

- **双通路**：外接显示器走 **DDC/CI**（dxva2 高层 `GetMonitorBrightness` 优先，失败自动降级到低层 VCP `0x10`）；笔记本内置屏走 **WMI**（`ROOT\WMI` 的 `WmiSetBrightness`）。
- **自动归一化**：不少显示器 raw 范围是 0–255 而非 0–100，程序按实际能力位换算，不会「调到 100% 其实只有 39%」。
- **抗抖动**：DDC 总线的 5 类瞬时错误自动重试一次。
- **写后回读校验**：`SetMonitorBrightness` 失败时也可能返回 TRUE，所以每次写完强制读回确认。
- **本地 Web UI**：纯 Winsock2 手写的最小 HTTP/1.1 服务，只绑 `127.0.0.1`，端口随机。
- **体验细节**：双击自动隐藏黑框；浏览器起不来就把网址塞进剪贴板并弹窗；UI 页面开发期读 `ui\index.html`，发布时以 RCDATA 内嵌进 exe。

---

## 编译

用 **CMake + MinGW-w64**（GCC 需支持 C++17；Qt 6.8.3 附带的 MinGW 13.1.0 即可）。
不需要 MSVC、不需要 Windows SDK、不需要 Qt 库本体 —— 项目只借 Qt 自带的 GCC 编译器，不链接任何 Qt 库。

```bat
set PATH=D:\git\tools\Qt\Qt6.8.3\Tools\mingw1310_64\bin;%PATH%

cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
```

产物：`build\brightnessctl.exe`（约 350 KB，静态链接的自包含单文件）。

> 第一行不是可选的装饰 —— **MinGW 的 `bin` 必须在 PATH 里**。
> `cc1.exe` / `cc1plus.exe` 位于 `libexec\gcc\...\`，但它们依赖的 `libwinpthread-1.dll` 只在 `bin\` 下；
> 不在 PATH 时 gcc 会**静默失败**（只返回退出码，无任何报错）。
> `CMakeLists.txt` 会提前检查这一点，缺了就直接给出上面的 `set PATH=...` 提示，不会让你去猜 CMake 那句 "compiler is broken"。
> 另外，PATH 里若混进其他工具链（如 `C:\cygwin64\bin` 自带的 gcc/g++）也会干扰构建，遇到怪问题先把 PATH 清干净。

### 指定工具链

`CMakeLists.txt` 按以下顺序找 MinGW，找不到会在 configure 阶段直接报 fatal error：

1. `-DMINGW_BIN=<mingw 的 bin 目录>`
2. 环境变量 `MINGW`
3. 默认 `D:\git\tools\Qt\Qt6.8.3\Tools\mingw1310_64\bin`
4. 回退 `C:\Qt\Qt6.8.3\Tools\mingw1310_64\bin`

```bat
cmake -S . -B build -G "MinGW Makefiles" -DMINGW_BIN=D:\git\tools\Qt\Qt6.8.3\Tools\mingw1310_64\bin
```

### 依赖什么

- **CMake** ≥ 3.16（本机为 4.1.0，位于 `C:\Program Files\CMake\bin`）
- **MinGW-w64** 工具链：`g++`、`windres`、`mingw32-make`（都在 Qt 的 `mingw1310_64\bin` 里）

### 获取 MinGW

若不想装整套 Qt，可直接下载官方 7z（约 107 MB，解压 619 MB），解压后把里面的 `Tools\mingw1310_64` 放到任意目录即可：

```
https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/tools_mingw1310/qt.tools.win64_mingw1310/13.1.0-202407240918mingw1310.7z
```

---

## 用法

### 命令行

| 命令 | 说明 |
|---|---|
| `brightnessctl`（无参数） | 后台运行并打开网页配置界面（双击时的行为） |
| `brightnessctl ui [port]` | 同上，可指定端口（默认随机端口） |
| `brightnessctl stop` | 通知后台实例退出 |
| `brightnessctl list` / `get` | 列出所有显示器及其当前亮度、通路、raw 范围 |
| `brightnessctl set <0-100>` | 设置所有显示器亮度 |
| `brightnessctl max` / `min` | 调到最高 / 最低 |
| `brightnessctl vcp <code> <value>` | 写任意 VCP（如 `vcp 0x10 100`） |
| `brightnessctl help` | 打印用法 |

`list` / `get` / `set` / `max` / `min` / `vcp` / `stop` 都是「干完就退出」的一次性命令，**可以多个同时跑**，也不会和后台 UI 实例冲突。

### 网页界面

启动后浏览器会打开 `http://127.0.0.1:<随机端口>/?token=<32位随机串>`。页面提供每个显示器一条滑杆、快捷百分比按钮、刷新和「退出程序」。

**关掉网页后怎么再打开？** 再运行一次 `brightnessctl.exe` 即可 —— 它会检测到已有实例在跑，通知那个实例**重新生成带 token 的地址并打开页面**，然后自己立刻退出。整个过程只有一个进程在跑。

### HTTP API

| 接口 | 方法 | 说明 |
|---|---|---|
| `/api/state` | GET | 返回所有显示器状态（JSON） |
| `/api/set` | POST | 设置亮度（JSON body） |
| `/api/quit` | POST | 退出后台程序 |

安全设计：

- 只监听 `127.0.0.1`，外部网络不可达；
- 每个请求必须带 token（`?token=` 或 `X-Token` 头），token 由 `CryptGenRandom` 生成、仅存在于进程内存，**从不落盘**；
- 校验 `Host` / `Origin` 头，防 CSRF；
- 单实例互斥体 + 禁用地址复用。

---

## 运行机制

| 项 | 位置 / 名称 |
|---|---|
| 单实例互斥体 | `Local\brightnessctl_ui_singleton` |
| 「请重新打开页面」信号 | `Local\brightnessctl_ui_open` |
| 退出信号 | `Local\brightnessctl_ui_stop` |
| 端口记录文件 | `%TEMP%\brightnessctl_ui.port`（只记端口，不含 token） |
| 日志 | `%TEMP%\brightnessctl.log` |

后台模式只允许一个实例；第二个进程发信号后即退出。日志文件默认追加，只有确认自己是唯一的常驻实例时才清空重开。

---

## 目录结构

| 文件 | 说明 |
|---|---|
| `brightnessctl.cpp` | 主程序：显示器枚举、DDC/CI + WMI 读写、命令行、单实例与后台模式 |
| `httpd.h` / `httpd.cpp` | 极简 HTTP/1.1 服务器（Winsock2，零第三方依赖） |
| `ui\index.html` | 网页界面（开发时直接读此文件，发布时编进 exe 资源） |
| `brightnessctl.rc` | 资源脚本：版本信息、图标、内嵌 UI 页面 |
| `brightnessctl.manifest` | DPI 感知与公共控件 v6 清单 |
| `brightnessctl.ico` | 程序图标 |
| `CMakeLists.txt` | 构建脚本：工具链选择、资源编译、静态链接与安全加固选项 |

---

## 与 Monitorian 的关系

亮度控制部分逐文件重写自 Monitorian（MIT 许可，Copyright (c) 2017 emoacht）：

| Monitorian (C#) | 本项目 (C++) |
|---|---|
| `DeviceContext.cs` | `EnumDisplayMonitors` / `GetMonitorInfo` 枚举显示器 |
| `MonitorConfiguration.cs` | dxva2 高层 API + 低层 DDC/CI |
| `DdcMonitorItem.cs` | raw ↔ percent 换算、瞬时错误重试 |
| `MSMonitor.cs` | WMI `ROOT\WMI` `WmiMonitorBrightnessMethods` |

主要差异：去掉 .NET/WPF 运行时（内存占用从数十 MB 降到私有字节 ~3.6 MB），界面改为内嵌网页，最终是零依赖单文件。

---

## 常见问题

**`list` 显示「没有可控亮度的显示器」**
虚拟机、远程桌面会话，以及部分经 USB-C/扩展坞的连接不支持 DDC/CI，属正常现象；物理机直连（DP/HDMI）才可用。

**浏览器打开是 `forbidden: invalid token`**
地址必须带 token。不要手动拼 `http://127.0.0.1:<port>/`，用程序给出的完整地址，或重新运行一次 exe 让它自己打开。

**改了 `ui\index.html` 没生效**
网页是编译期内嵌进 exe 的（RCDATA）。程序只认**和 exe 同目录**的 `ui\index.html`，而构建产物在 `build\` 下，所以它用的是内嵌版本。
调试页面时把 exe 复制（或把 `ui\` 复制）到同一层目录即可改完重启生效；要打进发布版则重新执行 `cmake --build build`。

---

## 许可

代码基于 Monitorian（MIT，Copyright (c) 2017 emoacht）重写，仓库应保留该署名。本仓库尚未添加 LICENSE 文件，建议按需补一份（沿用 MIT 即可与上游保持一致）。
