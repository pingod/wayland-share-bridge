# wayland-share-bridge

[![build](https://github.com/pingod/wayland-share-bridge/actions/workflows/build.yml/badge.svg)](https://github.com/pingod/wayland-share-bridge/actions/workflows/build.yml)
[![release](https://img.shields.io/github/v/release/pingod/wayland-share-bridge)](https://github.com/pingod/wayland-share-bridge/releases)

把 **Wayland** 的屏幕和窗口共享给 **X11 应用**。

很多应用仍然跑在 XWayland（或本身就是纯 X11 客户端）下，完全看不到 Wayland 原生窗口：
它们的共享对话框只列出其他 XWayland 窗口，共享整屏则是黑屏。`wayland-share-bridge`
把选中的屏幕或窗口镜像到一个普通 X11 窗口里，任何 X11 应用都能看到并共享它。

```
                 Wayland 合成器
   ext-image-copy-capture-v1 | ext-foreign-toplevel-list-v1
                             v
              wayland-share-bridge --- 每帧绘制 ---> 隐形的 X11 窗口
                             ^                        ("Wayland Share Bridge")
                             |                             |
      （兜底）桌面门户 ScreenCast + PipeWire                v
                         X11 / XWayland 应用（WPS、Zoom、飞书、
                                       Electron、浏览器、OBS ...）
```

共享源在托盘菜单里选：点一下选整屏、点一下选窗口，选过的源下次启动自动恢复。
对于不支持直接抓屏协议的合成器，还可以用 **Share via system portal...**（等价于
`--portal`）走桌面门户的系统选择器兜底。

这个 X11 窗口是隐形的、点击穿透的，不会挡住你的操作。

## 为什么需要它

- WPS Office、WPS 协作/会议、Zoom、飞书以及大量 Electron 应用都是 X11/XWayland
  客户端。在 Wayland 下，它们的共享选择器要么列不出 Wayland 原生窗口，要么
  抓到的是黑屏。
- 这些应用的 Wayland 原生实现走 `org.freedesktop.portal.ScreenCast` 门户，
  而 X11 应用不走。
- 这个桥给 X11 应用一个"能抓的目标"：一个装着 Wayland 画面实时副本的 X11 窗口。

## 工作原理

两套抓屏后端：

1. **直接抓屏（默认）**：直接通过 `ext-image-copy-capture-v1` 跟合成器打交道，
   用 `wl_output` 和 `ext-foreign-toplevel-list-v1` 枚举屏幕与窗口。不走门户、
   不弹系统选择器，托盘菜单就是源列表，切换只需要点一下。
2. **门户兜底（`--portal`）**：通过 `xdg-desktop-portal` 建 ScreenCast 会话，
   收 PipeWire 帧并画进同一个 X11 窗口。合成器不支持直接抓屏协议时使用；
   此时会用系统选择器代替内置菜单。

已通过直接抓屏选择的源会记在 `$XDG_STATE_HOME/wayland-share-bridge/state`
（形如 `source=screen:DP-2`、`source=window:firefox|标题`），下次启动自动恢复。

## 运行要求

- Wayland 合成器支持 **XDG Desktop Portal ScreenCast**（PipeWire + 会话总线）；
  或者支持直接抓屏协议 `ext-image-copy-capture-v1`、
  `ext-image-capture-source-v1`、`ext-foreign-toplevel-list-v1`。
  Hyprland 0.56+ 全部支持。
- **XWayland**（X11 那一侧）。
- 合成器能把 X11 窗口隐藏或忽略，见下文"让桥接窗口隐形"。

编译依赖（括号内为 Arch 包名）：

- C 编译器、`meson`、`ninja`
- `dbus-1`（`dbus`）
- `libpipewire-0.3`（`pipewire`）
- `wayland-client`（`wayland`）、`wayland-scanner`（`wayland`）
- `x11`（`libx11`）、`xext`（`libxext`）、`xfixes`（`libxfixes`）

三个 Wayland 协议 XML 已内置在 `protocols/` 目录，不依赖特定的
`wayland-protocols` 版本。

## 编译与安装

```sh
meson setup build
ninja -C build
sudo ninja -C build install
```

安装内容：

- `/usr/local/bin/wayland-share-bridge` – 主程序
- `/usr/local/bin/wayland-share-bridge-launch` – 启动脚本，额外配置 Hyprland 规则
- `/usr/local/share/applications/wayland-share-bridge.desktop` – 桌面菜单项
- `/usr/local/share/man/man1/wayland-share-bridge.1` – 手册页（`man wayland-share-bridge`）

## 快速开始

1. 启动桥（应用菜单 **Wayland Share Bridge**，或在终端运行）：

   ```sh
   wayland-share-bridge-launch
   ```

2. 会出现一个名为 **Wayland Share Bridge** 的隐形窗口和一个托盘图标。
   右键托盘图标选共享源：
   - `Screen: DP-2 (2560x1440)` → 镜像该显示器
   - `Window: firefox — ...` → 镜像该 Wayland 原生窗口
   - **Show shared window** → 临时显示镜像窗口，本机确认正在共享什么
   - **Stop sharing** → 停止共享但不退出程序
   - **Share via system portal...** → 系统选择器（给不支持直接抓屏的合成器兜底）
3. 在要共享的应用里打开共享对话框，选择 **Wayland Share Bridge** 窗口：

   - WPS 会议/协作：**共享 → 应用窗口 → Wayland Share Bridge**
   - Zoom：**Share Screen → Window → Wayland Share Bridge**
   - OBS：添加 **窗口采集** 源并选择 **Wayland Share Bridge**
   - 浏览器：共享窗口里选 **Wayland Share Bridge**

这些应用里的"整屏"选项仍然是黑的：它抓的是 X11 root 窗口，在 Wayland 下没有内容。
始终共享桥接窗口即可。

之后换源只需托盘菜单点一下，不用重启、不再弹选择器。

## 用法

```
用法：wayland-share-bridge [选项]
      wayland-share-bridge --list-sources
      wayland-share-bridge --reselect[=KIND]
      wayland-share-bridge --status
      wayland-share-bridge --quit

选项：
  -c, --cursor=MODE     门户后端的鼠标模式：hidden、embedded、metadata
                        （默认 embedded）
  -k, --source-kind=KIND
                        门户后端的源类型：auto、any、screen、window
                        （默认 auto）
  -S, --source=SPEC     立刻共享指定源：screen:NAME 或
                        window:APP_ID 或 window:APP_ID|TITLE
  -L, --list-sources    列出可共享的屏幕和窗口
      --portal          强制走桌面门户（系统选择器）
  -r, --remember        记住上次的源（默认）
  -n, --no-remember     不记住源
  -s, --show            显示桥接窗口（调试用）
  -t, --title=TEXT      窗口标题（默认 Wayland Share Bridge）
  -f, --token-file=PATH 门户后端的 restore token 文件路径
      --no-tray         不创建托盘图标
      --allow-multiple  允许同时运行多个实例
  -h, --help            显示帮助
  -V, --version         显示版本
```

示例：

```sh
wayland-share-bridge --list-sources
wayland-share-bridge --source=screen:DP-2
wayland-share-bridge --source='window:firefox|DeepSeek' --no-tray
wayland-share-bridge --portal --cursor=hidden
```

## 托盘菜单

| 菜单项 | 作用 |
| --- | --- |
| **Stop sharing** | 停止共享，程序继续运行。 |
| **Show/Hide shared window** | 切换镜像窗口的可见性（本机确认共享内容）。 |
| `Screen: NAME (WxH)` | 点一下就镜像该显示器（勾选=正在共享）。 |
| `Window: APP — TITLE` | 点一下就镜像该 Wayland 窗口（勾选=正在共享）。 |
| **Share via system portal...** | 系统选择器（等价 `--reselect`）。 |
| **Quit** | 停止桥接。 |

菜单是扁平的（没有子菜单），因为不同托盘宿主对嵌套子菜单的 hydrate 行为不一致；
当前共享的源用勾选标记，托盘 tooltip 里显示共享状态。
每次打开菜单都会重新枚举合成器当前的输出和窗口，新开的窗口下次打开即可看到。

## 让桥接窗口隐形

桥接窗口带有 `_NET_WM_WINDOW_OPACITY=0`，遵循该属性的合成器（KDE Plasma、GNOME）
会自动隐藏它。Hyprland 不认这个属性，所以安装的启动脚本
（`wayland-share-bridge-launch`）会加一条窗口规则：

```sh
hyprctl eval 'hl.window_rule({ name = "wayland-share-bridge-hidden",
  match = { class = "^wayland-share-bridge$" },
  opacity = 0.0, float = true, no_blur = true, no_focus = true })'
```

老版解析器的 Hyprland 上会回退到 `hyprctl keyword windowrulev2 ...`。
详见 [docs/hyprland.md](docs/hyprland.md)。

## 常见问题

| 症状 | 处理 |
| --- | --- |
| 提示 `Direct capture is not available` | 合成器不支持 `ext-image-copy-capture-v1`，改用 `--portal`。 |
| 托盘菜单是空的 | 没有托盘宿主（StatusNotifierWatcher）；改用 `--list-sources` + `--source=`。 |
| 菜单项过几秒才出现 | 菜单在打开时刷新，重新点开即可。 |
| WPS 里缩略图还是黑的 | 你共享的是 X11 整屏，不是桥接窗口；改选 **Wayland Share Bridge**。 |
| 窗口显示在屏幕上 | 用启动脚本启动，或手动加 Hyprland 规则。 |
| 镜像里没有鼠标指针 | 只有整屏抓取会画指针，窗口抓取不画。 |
| 颜色红蓝互换了 | 请反馈合成器/shm 格式；只用了 XRGB8888/ARGB8888。 |
| 门户每次都问选源 | Hyprland 不返回 restore token；直接抓屏不需要它。 |

更多见 [docs/troubleshooting.md](docs/troubleshooting.md) 与
[docs/hyprland.md](docs/hyprland.md)。

## 限制

- 不支持区域抓屏（屏幕上的一块矩形）。
- 不处理音频；请用应用自身的音频共享选项。
- 窗口抓屏镜像的是窗口内容，不含窗口装饰。
- 帧循环约 30 Hz，开销随分辨率变化。
- 记住的“窗口”源绑定具体窗口：应用重启后该源就不存在了，桥接会以未共享状态
  启动；记住的“整屏”源不受影响。
- 系统门户兜底时，选择器打开期间桥接会阻塞；直接抓屏后端不会。

## 项目结构

```
src/            源码（抓屏后端、X11 窗口、托盘、控制总线）
protocols/      内置的 Wayland 协议 XML
data/           启动脚本与 .desktop
docs/           合成器说明、场景、排错
man/            手册页
CHANGELOG.md    版本历史
```

## 许可与致谢

EUPL-1.2。最初的思路与实现来自 [video_bridge](https://github.com/lmarz/video_bridge)
（作者 lmarz），改动列表见 [NOTICE](NOTICE)。
