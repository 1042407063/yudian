# 余电

常驻任务栏右下角，随时显示无线鼠标、键盘、手柄的剩余电量，电量低时弹卡片提醒。
纯本地程序：不联网、不装驱动、不弹广告。C++ / Win32，源码在 `app/src/`。

## 两台电脑统一路径

`D:\dev\myproj\余电`（公司笔记本与家里台式机必须完全一致，含中文目录名）

> 本目录目前是**本地 Git 仓库**，尚未连接 GitHub。按《Git双机开发交接使用手册》2.4
> 在 GitHub 建空仓库并 push 后，另一台电脑用 2.5 的 `git clone ... 余电` 即可双机接力。

## 一分钟看懂目录

| 目录 | 里面是什么 |
| --- | --- |
| `app/src/` | C++ 源码：`main.cpp` / `providers.cpp` / `ui.cpp` / `hid.cpp` 等 |
| `app/build.bat` | 双击重新编译（约 20 秒，含自检） |
| `app/build/` | 编译产物 `yudian.exe`，已忽略、不进仓库（删了重编就有） |
| `app/README.md` | 程序怎么用、代码怎么分工 |
| `dev/probes/` | 当初试各品牌设备电量协议的 Python 小脚本 |
| `dev/ui-labs/` | 界面实验程序 |
| `docs/` | 技术笔记、架构拆解、交接说明、审查记录、界面截图 |
| `.workbuddy/memory/` | 项目工作日志（AI 上下文资产，跟着仓库走） |
| `HANDOFF.md` | 双机交接的进度断点 |

## 怎么用

- 只想用：双击 `app/build/yudian.exe`
- 改界面 / 配色：`app/src/ui.cpp`
- 让程序认识一款新设备：`app/src/providers.cpp`
- 改完代码重新编译：双击 `app/build.bat`，看到 `[3/3] done. subsystem=00000002 (GUI)` 即成功
- 查某设备为什么读不到电量：`docs/technical-notes.md` 的「排查」一节

## 搬迁说明（2026-11-06）

- 本项目从 `D:\WorkBuddy\个人开发` **拆出**。原目录是「余电 + token-panel」的总目录，
  token-panel 已迁到 `D:\dev\myproj\Token看板`，所以那份讲两个项目的总览 README 没有搬过来，
  本文件是余电专用版。`docs/` 下的历史笔记均属余电，原样保留。
- 排除了 `.workbuddy/backup/`、`trash/` 与编译产物；源目录一个文件没动、没删，留作回滚点。
- **开机自启存着旧路径**：右键托盘图标 →「开机自启」取消 → 再勾一次，路径才会更新；
  不做这步的唯一后果是下次开机不会自动启动，手动双击 exe 一切正常。

## 关联

- 双机开发规则：桌面《Git双机开发交接使用手册》
- 进度断点：`HANDOFF.md`（收工前让 WorkBuddy 更新，开工时先读它）
