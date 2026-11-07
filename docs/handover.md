# 余电 · 交接说明

> **这是一份历史记录，写于 2026-10-27。** 目录在 2026-10-29 整理过，文中提到的旧路径
> 对应关系如下；当前的完整结构请看根目录的 `README.md`。
>
> | 文中写的 | 现在的位置 |
> |---|---|
> | `peripheral-battery/` | `app/` |
> | `peripheral-battery/README.md` | `docs/technical-notes.md` |
> | `probe-tools/` | `dev/probes/` |
> | `HANDOVER.md` | `docs/handover.md`（本文件） |
> | `final-panel.png` | `docs/images/final-panel.png` |
>
> 另外：文中「面板的毛玻璃是怎么做的」一节讲的那套效果，已于 2026-10-29 整体移除（抓屏照片会过期，改成定时重拍又有停顿感），现在卡片是不透明的。

> 最后更新：2026-10-27 20:40
> 本目录是完整可续作的工作副本：源码、文档、已编译的 exe、以及调试用 HID 探针脚本。

---

## 1. 这是什么

Windows 托盘小工具，读无线键鼠 / 手柄的实时电量。原生 C++ / Win32 + GDI+，**单文件 563 KB**，
静态链接、不依赖运行时、不需要任何厂商驱动。

- 源码：`app/`
- 已编译：`app/build/yudian.exe`（重新构建过多次，字节数见构建日志）
- 设计文档：`docs/technical-notes.md`（支持范围、协议笔记、排查步骤都在里面）
- 调试脚本：`dev/probes/`（见该目录 README）
- 历史工作日志：`../.workbuddy/memory/2026-10-27.md`

---

## 2. 当前实测状态（本机四台设备）

```
DualSense 无线控制器   100%   USB · 索尼 · 充电中        ← 插数据线时
DualSense 无线控制器    --    蓝牙 · 索尼 · 蓝牙不提供电量  ← 拔线后（平台限制）
MCHOSE A5 V3 Pro        98%   2.4G 接收器 · 迈从 · 放电中
MCHOSE K99 V2           97%   2.4G 接收器 · 迈从 · 放电中
Flydigi Vader 5 Pro    100%   USB 有线 · 飞智 · 充电中    ← 插数据线时（21:05 实测）
Flydigi Vader 5 Pro    100%   2.4G 接收器 · 飞智 · 放电中  ← 拔线换接收器
```

四台设备全量扫描约 **157 ms**（设备休眠时厂商链路会拖到 1.8 s，属正常）。

蓝牙：本机连着华为 FreeBuds Pro 3 / FreeClip 2（经典蓝牙音频）与一只 BLE 鼠标
（DragonFly F1 SE）。`--ble` 能看到那只 BLE 鼠标，但它在休眠态读不出值；两只华为
耳机不走 GATT，详见 README「关于蓝牙」。

---

## 3. 怎么编译

```bat
cd app
build.bat
```

工具链查找顺序：`%MINGW_BIN%` → `D:\toolchains\mingw64\bin` →
`%LOCALAPPDATA%\Mingw64\mingw64\bin` → `..\.toolchain\mingw64\bin` → PATH。

**本机已装好的工具链在** `C:\Users\10424\AppData\Local\Mingw64\mingw64\bin`
（WinLibs GCC 16.2.0，winget 包 `BrechtSanders.WinLibs.POSIX.UCRT`）。
换机器重新装：

```powershell
winget install --id BrechtSanders.WinLibs.POSIX.UCRT -e
```

**坑：`ld.exe` 不能处理含中文的工具链路径**，MinGW 必须放纯英文目录；源码目录可以含中文。

编译脚本会校验 PE 子系统必须是 GUI（`00000002`），防止误编成控制台版。

---

## 4. 代码结构

| 文件 | 行数 | 职责 |
|---|---|---|
| `src/main.cpp` | 649 | 入口、单实例、配置读写、扫描线程、托盘消息 |
| `src/ui.cpp` | 2564 | 面板绘制、**毛玻璃底图**、动画、托盘图标、toast、菜单 |
| `src/providers.cpp` | 2208 | **所有电量协议**（罗技 / 索尼 / Compx / Nordic / 迈从 / 飞智 / **BLE GATT** / 通用 HID） |
| `src/hid.cpp` | 375 | HID 枚举与读写封装（异步句柄、`GetFeature`） |
| `src/app.h` | 156 | 公共类型（`Reading`、`Config`）、全局声明 |

调试用命令行（都在 `main.cpp` 里分派）：

```
yudian.exe --list out.txt       扫描一次，写出设备与电量
yudian.exe --dump hid.txt       列出所有 HID 接口（VID/PID/Usage/长度/识别标签）
yudian.exe --ble ble.txt        BLE 电量诊断（连接态、0x180F、失败 HRESULT）
yudian.exe --vendor v.txt       Compx 族原始帧诊断
yudian.exe --raw raw.txt        罗技 HID++ 原始帧
yudian.exe --shot a.png         把面板渲染成图（不开窗口）
yudian.exe --bench b.txt        连续扫描 5 次，测耗时
```

### 面板的毛玻璃是怎么做的

分层窗口（`WS_EX_LAYERED` + `UpdateLayeredWindow`）拿不到 DWM 的背景模糊，所以是自己
做的：弹出前按 `ComputePanelPos()` 算好坐标，趁窗口还不可见用 `BitBlt(CAPTUREBLT)` 把
卡片背后那块桌面抓下来 → 降采样到 1/4 → 三遍盒式模糊 + 去饱和 → 放大回去当底图，
再压一层 74%~79% 不透明度的主题底色。只在弹出那一次付成本（几毫秒），动画帧复用。

**顺序不能反**：`PlacePanel()` 里带了 `SWP_SHOWWINDOW`，先定位后抓屏会把自己抓进去，
所以定位逻辑拆成了 `ComputePanelPos()` + `PlacePanel()` 两个函数。

---

## 5. 已实现的协议（按验证程度）

| 设备 | 通道 | 偏移 / 命令 | 状态 |
|---|---|---|---|
| 罗技 Lightspeed/Unifying/Bolt | HID++ 2.0 特性 | `0x1000` / `0x1001` | 已实测（G304） |
| 罗技 C542 Nano | 无 HID++ | — | 标注「硬件不支持」 |
| DualSense **USB** | 输入报告 `0x01`（64B） | 电量第 **53** 字节 | **已实测** |
| DualSense **蓝牙** | 只有简化报文 `0x01`（78B） | 无电量字段 | 平台限制，见 §6 |
| DualShock 4 | `0x01`(USB,30) / `0x11`(BT,32) | — | 未真机验证 |
| Compx 族（VGN/ATK/VXE…） | `0xFF02/0x0002`，17B | `08 04` / `08 10` | 未真机验证 |
| ATK Zero（Nordic 54） | 64B `08`/`72` | 第 7 字节 | 未真机验证 |
| **迈从 V2**（A5 V3，`3837`） | **输出报告 `0x4D`**，64B | 命令 `0x0900`，应答 `data[12]` | **已实测** |
| **迈从 V1**（K99 V2 接收器，`41E4`） | 20B 报告 `0x13` | `13 4A 01 00`，应答第 6 字节 | **已实测** |
| 迈从旧 L7 Pro（`5253`） | 被动 `0x13/0x1D` 推送 | XOR 0xFF | 未真机验证 |
| **飞智 Vader 5 Pro**（`37D7`） | `0xFFA0`，33B，`5A A5` 帧 | `01` 取信息，`data[11]` | **已实测**（有线 + 接收器） |
| **BLE 外设**（`0x180F`） | GATT Battery Service | `0x2A19` | 已实现，等连接态 BLE 设备复测 |

### 三个反复踩到的通用坑（写在 README 协议笔记 26–35）

1. **写用同步句柄、读用异步句柄**。同一个异步句柄既写又读，输入管道会静默——连每秒一次的心跳都收不到，看起来像设备休眠。
2. **先判报文类型，再谈字段偏移**。DualSense 的简化报文和完整报文都是 78 字节，长度对得上不代表字段在。
3. **有些设备隔一次才应答**（飞智），加长超时没用，直接重试更快。

---

## 6. 已知边界

- **PS5 手柄蓝牙读不到电量**。蓝牙下 Windows 只给简化报文（前 8 字节是摇杆按键，之后全补零），
  完整的 `0x31` 报文需要 PS5 式握手。已实测：每秒 500+ 帧全是 `0x01`；主动下发 78 字节输出报告 `0x31`
  （3 种 CRC 种子 × 3 种标志位）无法切换；系统无 Battery 设备、无电池属性。属平台限制。
  **插能传数据的 USB 线即可读**（充电线只供电、不出接口的那种不行）。
- **迈从 V2 休眠时不回包**，与罗技一致；显示「设备休眠」，保留 90 秒内上一次读数。
- **飞智电量只有 6 档**（0/20/…/100），协议就是 4 bit 档位，不是解析误差。
- **蓝牙音频设备的电量拿不到**。华为 FreeBuds Pro 3 / FreeClip 2 在本机只有经典蓝牙
  节点（A2DP/HFP/AVRCP），没有 BTHLEDEVICE，设备属性里也没有任何 Battery 字段——
  已逐条枚举确认。它们的电量只在 HFP 的 AT 指令里，而 PC 侧 HFP 链路被 Windows
  自己的免手操驱动占着，再连一条会影响通话/播放，所以不做。BLE 设备（有 `0x180F`）
  是另一条路，已实现，`--ble` 可查状态。
- **迈从 K99 接收器不报型号**（product 固定 `MCHOSE 2.4G Wireless`，manufacturer 是芯片商 `Compx`，
  `0x13` 命令空间 + `0x41/0x42/0x44` 寄存器全扫都没有 ASCII 型号）→ 用配置别名，见 §7。

---

## 7. 配置

`%APPDATA%\PeripheralBattery\config.ini`，纯 `key=value` 文本，UTF-8：

```ini
interval=120
low_threshold=20
notify_low=1
autostart=0
theme=auto
panel_x=-1
panel_y=-1
name.41E4:2001=MCHOSE K99 V2      ← 设备别名，键是 VID:PID（四位大写十六进制）
```

别名对所有界面生效。给其它「只报通用名」的接收器起名，照这个格式加一行即可，
VID:PID 从 `--dump` 里查。

---

## 8. 接下来可以做的（按价值排序）

1. **索尼蓝牙**：若愿意引入外部依赖，可参考 DS4Windows 的 ViGEm 路径；纯 Win32 做不到。
   另一条路是识别到蓝牙简化报文时，用上一次 USB 读数 + 「N 分钟前」标注，而不是直接显示 `--`。
2. **Compx / ATK / DS4 真机验证**：代码照公开协议写好但没设备。拿到设备后先跑 `--dump` 和 `--vendor`，
   再对照 `README.md` 里的协议笔记改偏移。
3. **迈从 V2 轮询率/DPI 只读扩展**：协议已摸清（`0x0102` / `0x0103` / `0x0109`），现在只发了 `0x0900`。
   若要做设置面板，注意只发读命令，不写配置。
4. **低电量提醒**已有，但四台设备同时低电时的合并提示还没做。
5. **界面**：面板固定 5 行，超过要滚动；设备多了可以按类型分组。

---

## 9. 目录清单

见根目录 `README.md` 的「目录地图」一节——整理后只在这一处维护，避免两份对不上。

---

## 10. 换账号后第一步

```bat
cd 余电\app
build.bat                :: 先确认工具链还在、能编过
build\yudian.exe --list r.txt
type r.txt               :: 看四台设备是否还能读到
```

读不到就先看 `--dump hid.txt`：`识别` 一列会直接告诉你接口被归到哪条协议、
是「USB」还是「蓝牙·无电量」、是「迈从 V2」还是「V1」。
