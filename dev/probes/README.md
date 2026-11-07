# HID 探针脚本（调试用）

> **这批脚本不是产品的一部分。** 真正的程序在 `../../app/`（C++17 / Win32 /
> GDI+，产物 `yudian.exe`）。这里的 48 个 `.py` 是开发期用来**摸协议**的仪器：给设备发一帧
> 试探报文、看有没有回包、回包的哪个字节像电量。每个脚本对应一次具体的「这条路走不走得通」
> 的提问，结论已经固化进 C++ 的 provider；脚本本身不参与构建、不随程序分发，删掉不影响
> 程序运行，留着是为了让结论有原始证据可查。

摸新设备协议时写的 Python 小工具，Windows + Python 3、只用 ctypes 调 hid.dll / kernel32，
无需装 hidapi。**只读为主**：除电量查询外不发设置类命令。

运行方式：在本目录下 `python <脚本> <参数>`。

---

## 核心

| 文件 | 作用 |
|---|---|
`hidprobe.py` | 公共库：枚举 HID 接口、打开句柄、异步读（带超时）、异步写、读报告描述符。也当 CLI 用：`enum` / `listen <vid:pid> <秒> [usage]` / `desc <vid:pid> [usage]` / `send ...`
`probe3.py` | 最常用：对一个接口「写一帧 → 收全部回包」，参数 `<vid:pid> <usage> <长度> <hex...>`，`F:` 前缀表示走特性报告
`ask.py` | 命令字扫描：固定首字节，遍历第二字节，打印有回包的组合

## 迈从 MCHOSE

| 文件 | 作用 |
|---|---|
`v2_out.py` | V2 量产版写法：输出报告 `0x4D` + 同步句柄写 / 异步句柄读，已验证 100% 可读
`scan13.py` | 扫 `0x13` 报文的子命令空间（V1 接收器）
`namescan.py` | 扫 V1 命令空间，找可打印 ASCII（型号名）——结论：没有
`regscan.py` / `regscan2.py` | 用 `0x41/0x42/0x44` 读块命令扫寄存器地址，找型号字符串——结论：没有

## 飞智 Flydigi

| 文件 | 作用 |
|---|---|
`flydigi.py` | 首次试探：`5A A5 01 02 00` 三种投递方式
`flydigi2.py` | 干净重复测试，看清「隔一次才应答」
`flydigi3.py` / `flydigi4.py` | 超时 vs 重试的对比，最终确定「重试 3 次」

## 索尼 DualSense

| 文件 | 作用 |
|---|---|
`sony_dump.py` / `sony_watch.py` | 打印报文、统计报告 ID 分布（蓝牙下 500+ 帧全是 `0x01`）
`sony_usb.py` / `sony_usb2.py` | USB 模式抓 `0x01` 报文，逐字节找电量——定位到第 53 字节
`sony_enable.py` / `sony_mode.py` / `sony_mode2.py` | 尝试下发输出报告 `0x31` 切到完整报文（**失败**，留作证据）
`sony_diff.py` | 连续帧对比，看哪些字节在动
`sony_outlen.py` | 试输出报告的合法长度

## 通用工具

| 文件 | 作用 |
|---|---|
`lenscan.py` | 对某个 report id 逐长度试 `WriteFile`，找出该报告的真实输出长度
`ble_batt.py` | 走 BLE 的 GATT 电池服务（0x180F）读电量，不经过 HID；只读，不写
`battery_scan.py` | 扫所有 HID 接口，判断哪些设备暴露标准电量字段（0x06/0x20）；新设备能不能读，先跑它
`rapoo_probe.py` | 雷柏专用：探 0xFF00 私有通道（0xBA 帧）与状态特性报告，判断该型号能不能读电量
`zoom.py` | 把截图里的一小块按倍数放大，用来核对像素级细节（配合 `shot_panel.py`）
`scanrid.py` / `scanrid2.py` | 遍历 report id，看哪些被设备接受
`feat.py` / `featscan2.py` | 特性报告读写与 ID 扫描（`featscan2` 可指定区间，默认补扫 0x40–0x60，`0x4D` 就在这里）
`cmdscan.py` / `cmdread.py` | 0x4D 帧命令字扫描 / 发命令后监听
`matrix2.py` / `variants.py` / `variants_v2.py` | 多种帧布局的对照实验（历史记录）
`cs_try.py` | 校验字节位置/算法的对照实验（历史记录）
`loop_feat.py` / `loop_v2.py` / `loop_v2b.py` / `poll_bat.py` / `openprobe.py` / `sweep.py` / `v2_feat.py` / `probe_final.py` / `probe_matrix.py` / `crossif.py` / `classic.py` | 过程中各种轮询/跨接口/长跑实验，保留作参考

---

## 脚本分级（先看这个再运行）

不是所有脚本都"只读"。按副作用分三档，默认只跑第一档：

| 档位 | 行为 | 脚本 |
|---|---|---|
| **只读** | 只 GetFeature / ReadFile，或只发协议规定的读命令 | `hidprobe.py enum/listen/desc`、`caps.py`、`sony_dump.py`、`sony_watch.py`、`sony_diff.py`、`sony_usb.py`、`sony_usb2.py`、`ask.py`、`feat.py`、`cmdread.py`、`probe3.py`、`probe_final.py`、`poll_bat.py`、`loop_feat.py`、`openprobe.py` |
| **会写设备** | 向设备发写命令；可能改配置、切模式或让设备短暂无响应 | `scan13.py`、`scanrid.py`、`scanrid2.py`、`lenscan.py`、`cmdscan.py`、`sweep.py`、`matrix2.py`、`variants.py`、`variants_v2.py`、`cs_try.py`、`v2_out.py`、`v2_feat.py`、`loop_v2.py`、`loop_v2b.py`、`probe_matrix.py`、`crossif.py`、`classic.py`、`flydigi*.py`、`sony_enable.py`、`sony_mode.py`、`sony_mode2.py`、`sony_outlen.py`、`namescan.py`、`regscan*.py` |
| **会杀进程** | 结束正在运行的程序 | `shot_panel.py`、`frost_check.py`（先发 WM_CLOSE，超时才 taskkill） |

`featscan2.py` 默认**只读**；要真的往每个特性 ID 写全零必须显式加 `--yes`
（会对 LED、灵敏度档、模式开关这类配置产生副作用）。

## 结论先看这里

- 迈从 V2：`0x4D` 是**输出报告**，不是特性报告；写用同步句柄、读用异步句柄。
- 迈从 V1：`13 4A 01 00` 取电量，应答末字节是**前 19 字节求和**（不是 `0x55 - sum`）。
- 飞智：`5A A5 01 02 00`，33 字节报文，首字节是 report id `0`；隔一次才应答，要重试。
- 索尼：USB 报文电量在**第 53 字节**，蓝牙完整报文在第 54 字节；蓝牙简化报文没有电量。
- 通用：**写和读不要共用一个异步句柄**，否则输入管道静默，表现像「设备休眠」。
  这条在 2026-10-29 的修复里已经落实到所有脚本：写用同步句柄、读用独立异步句柄。
  此前用单句柄扫出来的"无回包"结论（`scan13` / `scanrid*` / `matrix2` 等）需要用
  当前版本重跑一遍才算数。

## 公共库约定

`hidprobe.py` 提供下面几个东西，脚本一律走它们，不要再各自造轮子：

| 名字 | 用途 |
|---|---|
| `require_one(vidpid, usage)` | 取唯一接口；找不到时给出可执行的提示，而不是 `IndexError` |
| `target_args(默认VIDPID, 默认usage)` | 允许命令行覆盖脚本里写死的设备标识 |
| `drain(handle, len, timeout, limit)` | 排空积压，有次数上限（避免设备持续推送时死循环） |
| `_read_timeout` / `_write_timeout` | 异步读写；超时后 `CancelIoEx` + `GetOverlappedResult(wait=True)` 再释放，不会留下悬空指针 |

> 时间统计口径：`_read_timeout` 返回的是**该接口真实报告长度**，可能小于请求长度。
> 解析前一律先判 `len(d)`，不要假设 `d[LEN-1]` 一定存在。
