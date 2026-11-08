# 开发交接记录

> 最后更新：2026-11-06 @ 公司笔记本
> 当前分支：main（本地 Git 仓库，尚未绑定远端）

## 1. 本次已完成

- 项目从 `D:\WorkBuddy\个人开发` 拆出，迁入 `D:\dev\myproj\余电`
- 建立本地 Git 仓库，补 `.gitignore` / `.gitattributes`，README 改写为余电专用
- 保留 `app/`、`dev/`、`docs/`、`.workbuddy/memory/`（7 份日志）；排除 backup / trash / 编译产物
- `app/build/yudian.exe` 已拷到本地、程序可直接运行，但被忽略、不进仓库

## 2. 未完成 / 进行中

- 尚未执行首次 commit
- GitHub 上尚未创建本项目的仓库，未 push
- 家里台式机尚未做任何操作
- 新路径下的「开机自启」尚未重设
- 源目录 `D:\WorkBuddy\个人开发` 仍保留（回滚点，暂不删）

## 3. 下一步建议

1. `git add -A` → `git commit -m "chore: 迁入余电项目"`
2. 按手册 2.4 在 GitHub 网页建空的仓库（如 `yudian`，三个初始化选项都别勾）
   → `git remote add origin git@github.com:1042407063/yudian.git`
   → `git push -u origin main`
3. 双击 `app/build/yudian.exe` 验证新路径能跑；托盘里取消再勾一次「开机自启」
4. 确认无误后把源目录**改名归档**（不要直接删），观察几天
5. 家里电脑按手册 2.5 配置并 `clone ... 余电`（末尾目录名写「余电」）

## 4. 需要注意的坑

- 两台电脑路径必须完全一致：`D:\dev\myproj\余电`
- GitHub 地址用用户名 `1042407063`，不是显示名 `yyf`
- 家里台式机也要建一份 `~/.ssh/config`（SSH over 443）
- `app/build/` 是编译产物、已忽略；另一台 clone 后需双击 `app/build.bat` 重新生成 exe
- 开机自启存的是旧完整路径，需手动取消再勾选一次
- 不要在父目录 `D:\dev\myproj` 里 `git init`，它只是容器

## 5. 最近提交

- （首次提交后补上）

## 6. 2026-11-08 追加：G913 TKL 电量适配

- 罗技 G913 TKL（`046D:C547`）已能读数：面板显示 `≈73%`（原始电压 3948 mV）。
- 根因是这台键盘只有 `0x1001` 电压特性、没有 `0x1000` 百分比，而旧代码把电压按小端解析后被丢弃。
- 改动在 `app/src/providers.cpp`（电压解析 + 罗技电压曲线估算）、`decode.h`（两个纯函数）、
  `app.h`/`ui.cpp`/`main.cpp`（`≈` 估算标记）、`selftest.cpp`（16 条新用例）。
- 证据：`dev/probes/logitech_g913.py`（协议实测脚本）、`docs/technical-notes.md` 协议笔记 38。
- 已重新编译，`app/build/yudian.exe` 为最新；自检 38/38 通过，`--list` 真机验证通过。
- 估算值与 G HUB 可能差几个百分点，属硬件只给电压的限制，不是 bug。

## 7. 2026-11-08 追加：充电跳变、绿色图标、双模去重

- 用户反馈插线后电量瞬间 +20%、充电图标要变绿、鼠标插线出现两行。
- 实测：G913 TKL 插线会**换 USB 身份**（C547 接收器 → C343 有线），电压 3948→4133→4174 mV，
  flags `0x00`→`0x80`。根因是拿充电端电压套放电 OCV 曲线。
- 修复：`LogitechBatteryPercent()` 充电扣 185 mV IR 抬升 + 充满校准；
  `SmoothEstimatedLevel()` 每轮 ±6 点限速；按型号名归一化 key（插线前后同一行）；
  transport 改「USB 有线」；面板充电中数字与电池胶囊用 `g_colors.ok` 绿色；
  `MergeWiredWirelessDuplicates()` 合并同名同品类的一条有线+一条无线。
- 官方机制：G HUB 用型号专属 PowerModel 查表 + 充满校准，本身也是估算（官方文档承认），
  G913 这代没有 `0x1004 unifiedBattery`，硬件不给精确百分比。
- 验证：`build.bat` 通过、自检 **53/53**；真机插线 `--list` = `≈80% | USB 有线 | 充电中`；
  `--shot` 截图确认绿色生效。
- 注意：编译时结束了用户正在运行的 `yudian.exe` 实例（否则 exe 被占用无法链接），需重新双击启动。

## 8. 2026-11-08 追加：拔线跳 99% / 追平 G HUB 行为

- 用户反馈「充电 75%，拔线立刻 99%」，并希望像迈从那样走设备自报、不要估算。
- **证伪「硬件有自报电量」**：`C343` 完整 36 个 HID++ 特性里没有 `0x1000`/`0x1004`/`0x1F20`；
  `0x1001` 只有电压+flags；34 个 HID 接口无标准电量字段；Windows 无 Battery 节点；
  隐藏特性需 `0x1E00` 写设备解锁（违反只读铁律）。本机也未安装 G HUB，拿不到 PowerModel。
- **根因**：上一版「充电一律扣 185 mV IR 抬升」是错的。充电末期电流变小、IR 抬升趋近 0，
  扣 185 把 99% 压成 75%；拔线不扣又弹回 99%，19 点断层纯属人为制造。
- **最终方案（对齐 G HUB 行为，用户指定目标）**：充放电共用同一条曲线，绝不按状态扣偏移；
  充电期间不用电压，改按充电时长（180 分钟充满）从插线锚点推进；充满或电压 ≥4186 mV
  对齐 100%；两端过 `SmoothEstimatedLevel()` ±6 点限速。
  删掉 `kLogiChargeIRDropMv`，新增 `ChargingProgressLevel()`，`LevelState` 扩展充电锚点字段。
- 验证：`build.bat` 通过、自检 **57/57**；真机 `≈99% | USB 有线 | 充电中`（4179 mV）；
  时间线模拟：插线 73%→73%、充电 48 分钟到 98%、拔线 99%、之后缓降，两端都不瞬跳。
- 若要逐点追平 G HUB，只能装 G HUB 并提取该型号 PowerModel XML（或走 `lghub_agent`
  的本地 WebSocket），当前纯本地无依赖方案不做。

## 9. 2026-11-08 追加：接入 G HUB 官方 PowerModel（电量准确）

- 用户装好 G HUB，要求 G913-TKL 电量准确显示。
- 在 `C:\ProgramData\LGHUB\depots\887386\g915_tkl_us\battery.xml` 找到官方 PowerModel
  （`pid="0xc343"`，Harpy Wireless）。关键是 `<discharge>` 与 `<charging>` 是**两条独立曲线**：
  4133mV 充电表 83%、放电表 99%；3948mV 放电表 76%。之前所有偏差都来自只用一个电压公式。
- 新增 `app/src/logitech_powermodel.h`（151 点放电 + 120 点充电，从 XML 固化）与
  `dev/probes/extract_ghub_powermodel.py`（可复现提取，换固件/电池后重跑）。
- `providers.cpp`：`LogitechDischargePercent` / `LogitechChargingPercent` 查表插值，
  `LogitechTrickleLevel` 按官方 `equation_coefficiency=150` 做涓流（≥4100mV 启动），
  充满标志对齐 100%，两端仍过 `SmoothEstimatedLevel` 限速。名字读取移到电量之前，
  按型号（g913/g915）选择官方曲线，其他罗技设备退回内核通用表。
- 删除自我发明的 `kLogiChargeIRDropMv`、`ChargingProgressLevel`、180 分钟时间模型。
- 验证：自检 **64/64**；真机放电 4169mV → `≈100% | G HUB 官方充放电曲线`，与 G HUB 一致。
- 注意：首次冷启动若键盘在休眠且从未应答过，`live` 为空不会出占位行（既有设计），
  重扫一次即可；G HUB 常驻会与我们在 HID++ 通道上竞争，偶发首轮超时，稳态无影响。
