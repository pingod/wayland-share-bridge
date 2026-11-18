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

- PipeWire 0.3.44 或更新（流按 object serial 指定节点）。
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
meson test -C build      # 纯函数的单元测试
sudo ninja -C build install
```

安装内容：

- `/usr/local/bin/wayland-share-bridge` – 主程序
- `/usr/local/bin/wayland-share-bridge-launch` – 启动脚本，额外配置 Hyprland 规则
- `/usr/local/share/applications/wayland-share-bridge.desktop` – 桌面菜单项
- `/usr/local/share/man/man1/wayland-share-bridge.1` – 手册页（`man wayland-share-bridge`）
- `/usr/local/share/systemd/user/wayland-share-bridge.service` – systemd 用户单元
  （`systemctl --user enable --now wayland-share-bridge`）
- `/usr/local/share/metainfo/wayland-share-bridge.metainfo.xml` – AppStream 元数据

## 快速开始

1. 启动桥（应用菜单 **Wayland Share Bridge**，或在终端运行）：

   ```sh
   wayland-share-bridge-launch
   ```

2. 会出现一个名为 **Wayland Share Bridge** 的隐形窗口和一个托盘图标。
   右键托盘图标，在子菜单里选共享源：
   - **屏幕 ▸** `DP-2 (2560x1440)` → 镜像该显示器
   - **窗口 ▸** `firefox (3) ▸ 窗口标题` → 镜像该 Wayland 原生窗口
   - **显示共享窗口** → 临时显示镜像窗口，本机确认正在共享什么
     （显示时仍保持点击穿透，用合成器的拖动绑定移动它）
   - **停止共享** → 停止共享但不退出程序
   - **使用系统门户共享…** → 系统选择器（给不支持直接抓屏的合成器兜底）
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
  -S, --source=SPEC     立刻共享指定源：screen:NAME、window:APP_ID、
                        window:APP_ID|TITLE 或 region:NAME,X,Y,W,H
  -L, --list-sources    列出可共享的屏幕和窗口
      --portal          强制走桌面门户（系统选择器）
  -r, --remember        记住上次的源（默认）
  -n, --no-remember     不记住源
  -s, --show            显示桥接窗口（调试用）
  -t, --title=TEXT      窗口标题（默认 Wayland Share Bridge）
  -f, --token-file=PATH 门户后端的 restore token 文件路径
      --no-tray         不创建托盘图标
      --allow-multiple  允许同时运行多个实例
      --reselect[=KIND] 让运行中的实例通过门户重新选源
      --status          打印运行中实例正在共享什么
      --quit            停止运行中的实例
      --search=QUERY    过滤运行中实例的“窗口”子菜单（空字符串清除过滤）
  -h, --help            显示帮助
  -V, --version         显示版本
```

示例：

```sh
wayland-share-bridge --list-sources
wayland-share-bridge --source=screen:DP-2
wayland-share-bridge --source='window:firefox|DeepSeek' --no-tray
wayland-share-bridge --source=region:DP-2,100,200,1280,720
wayland-share-bridge --search firefox
wayland-share-bridge --portal --cursor=hidden
```

## 托盘菜单

| 菜单项 | 作用 |
| --- | --- |
| **停止共享** | 停止共享，程序继续运行。 |
| **显示/隐藏共享窗口** | 切换镜像窗口的可见性；两种状态下都保持点击穿透。 |
| **屏幕 ▸** | 每个显示器一项，`DP-2 (2560x1440)`；当前源带勾选。 |
| **窗口 ▸** | 按应用分组：`firefox (3) ▸ 窗口标题`；当前源带勾选。 |
| **使用系统门户共享…** | 系统选择器（等价 `--reselect`）。 |
| **退出** | 停止桥接。 |

菜单按类别分成子菜单，窗口列表按应用分组、长标题会省略；存在搜索过滤时，
**窗口 ▸** 的第一项是 `搜索: QUERY（清除）`。当前共享的源用勾选标记，
托盘 tooltip 里显示共享状态和版本。
每次打开菜单都会重新枚举合成器当前的输出和窗口（50 ms 内的连续查询复用同一次
合成器往返），新开的窗口下次打开即可看到。

界面文案跟随 `LC_ALL`/`LC_MESSAGES`/`LANG`：中文环境显示中文菜单。

## 日志

从桌面菜单启动时没有终端，`wayland-share-bridge-launch` 会把桥接的 stderr
追加到 `$XDG_STATE_HOME/wayland-share-bridge/bridge.log`（超过 1 MiB 时清空）。
在终端直接运行二进制即可看到同样的输出。

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

- 区域抓屏只在直连后端可用：`--source=region:DP-2,X,Y,W,H`。
- 不处理音频；请用应用自身的音频共享选项。
- 窗口抓屏镜像的是窗口内容，不含窗口装饰。
- 帧循环约 30 Hz；只把合成器上报的脏矩形推给 X server，服务器支持
  MIT-SHM 时不再经 X11 socket 拷贝整帧。
- 旋转屏按合成器上报的 transform 旋转；合成器升级后建议再看一眼画面方向。
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
