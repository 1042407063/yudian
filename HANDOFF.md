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
