# Oink 源码分析 · 对余电的借鉴清单

> **这份是什么**：对 https://github.com/oinkvm/oink （软 KVM，Rust + Tauri）的架构拆解，
> 以及逐条落到「余电」上的可执行改法。
> **什么时候看**：准备动 `providers.cpp` 的结构、想给余电加测试、或者犹豫"要不要重写"的时候。
> **不看的理由**：里面没有余电的使用说明，那些在 `technical-notes.md`。

分析对象版本：`0.3.0`，commit 为 2026-10 拉取的 `main`。代码量实测 972 个 `.rs`、约 44 万行
（crate 40.8 万 + Tauri 后端 8.2k + 前端 25.9k TS/TSX）。

---

## 1. 一句话定位

Oink 是「一套键鼠控制多台电脑」的软 KVM：跨 Windows/macOS 的 P2P 网络、输入截获与注入、
剪贴板/拖放同步、音频转发、屏幕布局标定。README 明确写了**全部代码由 AI 编写**，人类只定方向。

这个背景很重要——它解释了 Oink 架构的两个特点，也决定了哪些该学、哪些别学：

- **该学的**：AI 愿意写"人类懒得写"的机械性工程设施（预算常量表、黄金向量、CI 门禁、
  不变量注释）。这些是纯收益，成本是一次性写对。
- **别学的**：AI 也倾向于"按教科书分层"而不是"按问题分层"，于是出现 15 个 crate 服务一个
  KVM、4621 行的 `router.rs`、8400+ 处 `.unwrap()`。这是过度设计的化石层。

---

## 2. 整体架构

### 2.1 四层结构

```
apps/            交付物：Tauri 桌面端(oink) / SYSTEM 服务+提权helper(winsvc)
                 / macOS File Provider / 音频驱动插件
      ↑ 只做"组装 + 平台外壳"，不含业务决策
crates/oink-agent        组合根：线程与资源所有者、本地 IPC、设备卡片投影、诊断
      ↑ 每个"资源"一个 owner，只有 owner 能改自己的状态
crates/<领域能力>         input / display / clipboard / audio / routing / network
                         / protocol / locator / update / telemetry / crash / autostart
      ↑ 平台实现在 trait 后面；跨模块只通过 foundation 里的共享类型说话
crates/oink-foundation   共享类型 + 路径 + 日志 + 原子存储（无业务）
```

依赖方向单向向下，`foundation` 不依赖任何业务 crate。`Cargo.toml` 里用
`[workspace.dependencies]` 统一版本，各 crate 只额外声明专用依赖——余电这种单体项目用不上，
但"依赖版本只有一个真源"的思路可以直接搬到 `build.bat` 的编译选项上。

### 2.2 平台差异被压成 trait，且只做"翻译"

三个后端 trait 形状一致（`oink-input/src/lib.rs`、`oink-display/src/lib.rs`、
`oink-clipboard/src/lib.rs`）：

```rust
pub trait InputBackend: Send + Sync {
    // 截获、注入、恢复光标、设截获模式
}
```

关键约束写在模块文档里，而且是**否定式**的：

> 对外只暴露 `InputBackend`；平台实现在 `src/macos`、`src/windows`，**只做「翻译」，不做路由决策**。

这句话是整个项目最值钱的一条纪律。它把"这段代码能不能测"变成了结构问题而不是态度问题：
决策在纯逻辑层，翻译层薄到可以靠真机冒烟覆盖。

### 2.3 路由是纯状态机，副作用被推迟成指令

`oink-routing` 不依赖 OS、不依赖网络、不碰 socket。输入是一个事件、一条对端消息或一个定时器，
输出是一个 `Output` 列表（`router.rs:212`）：

```rust
pub enum Output {
    Send { to: DeviceId, msg: Message, delivery: Delivery },
    Inject(InjectEvent),
    SetMode(CaptureMode),
    ConstrainCursor(Option<Rect>),
    RestoreLocal { at: Point },
    Dnd(DndCmd),
    CursorTrail { active: bool },
    Status(StatusEvent),
    // ...
}
```

`Router` 只**决定**要发生什么，执行方按产生顺序执行。带来三个直接好处：

1. 单元测试不需要任何硬件——断言 `Vec<Output>` 就行；
2. 顺序语义显式化（"执行方按产生顺序执行两者"写进了注释），不靠调用栈隐式保证；
3. 同一份逻辑可以喂给仿真器（`SimpleLayout` + `FakeInput`），两个完整 `Node` 在回环上跑真 QUIC。

### 2.4 UI 不写状态，只渲染投影

`oink-agent/src/device_cards/` 是这条原则的样本：

- `projector.rs`：把"成员关系 + 在线状态 + RTT + 焦点 + 能力"等一堆观察，投影成**一份自洽的卡片快照**；
- `tokens.rs`：UI 的每次操作带一个不透明 token，绑定 `OwnerVersion { group, device, boot_nonce, revision }`。
  UI 只能说"对第 N 版执行这个操作"，不能说"把值改成 X"；
- 结果：UI 永远不会自己编一个状态出来，也不会和后台打架（revision 不匹配就拒绝）。

对余电的启发很直接：现在 `g_devices` 是 `extern` 的全局 `vector`，UI 想读就读。改成
"扫描线程产快照 → PostMessage 移交所有权 → UI 只持有一份只读快照"，代码量不变，语义变干净。

### 2.5 每个边界都有预算

`oink-protocol/src/v1/limits.rs` 是一张纯常量表：帧 16KB、配置帧 128KB、CBOR 嵌套 32 层、
地图键 65535、配对对象 1MB、诊断包 64MB、错误率 8/s……**所有**外部输入在入口按这些常量校验，
而且是在分配内存**之前**先 `preflight` 走一遍结构（`schema.rs` 的 `WireDto::preflight`）。

这是"拒绝服务"防御的标准做法，也是把"性能事故"变成"编译期常量"的手法。

### 2.6 协议是契约，不是结构体

`oink-protocol` 没有用 serde derive 直接序列化业务类型，而是：

- 手写 DTO，字段号显式分配（`schema.rs` 的 `Field { key, name, shape, absence }`）；
- 业务模型与 wire DTO **在边界显式转换**，DTO 保留原始树（"retaining"），未知字段不丢；
- `tests/fixtures/v1/*.hex` 是黄金向量，`v1_golden.rs` 要求"签入的每个向量都必须被消费"，
  并且校验 `manifest.json` 里的 `schema_sha256`——**改字段必然让测试红**；
- `tests/compatibility.py` 拉一个"独立构建、哈希锁定"的旧版 codec 做 HEAD↔旧版 双向兼容矩阵。

### 2.7 工程门禁

`Cargo.toml` 的 `[workspace.lints]` 全 crate 继承：`unsafe_op_in_unsafe_fn = deny`、
`undocumented_unsafe_blocks = warn`、`clippy::todo/unimplemented/dbg_macro` 全部 warn。
CI（`.github/workflows/ci.yml`）固定工具链 1.99.0、固定 action SHA、跑
`cargo fmt --check` → `clippy -D warnings` → 单测 → release 模式再跑一遍 → `cargo deny check`
（许可证白名单 + 禁止 yanked + 禁止未知源）→ `cargo about` 校验第三方归属。

`build-support/ci.py` 还会按改动文件算出"受影响的包 + 反向依赖闭包"，只测这一坨。

### 2.8 把产品指标写成测试

三个我认为最值得抄的例子：

- `oink-agent/tests/idle_cpu.rs`：起真实子进程，空闲 3 秒，读 `ps -o time=`，断言 CPU 时间只有几十毫秒。
  **"托盘程序空闲不能烧 CPU"变成了一条会红的测试**。
- `oink-audio/src/allocation_audit.rs`：线程局部 `GlobalAlloc` 计数器，在实时音频路径上断言零分配。
- `oink-display` 里的 `proptest-regressions/`：属性测试的失败用例自动入库。

---

## 3. 哪些别学（重要）

| 现象 | 为什么不值得抄 | 余电的对应结论 |
|---|---|---|
| 15 个 crate、972 个文件服务一个 KVM | 概念数远小于模块数；`oink-agent` 一个 crate 285 文件 14.8 万行 | **不拆 crate**。余电 6 个文件是合适的粒度 |
| `router.rs` 4621 行 + 30 个文件里散落 `impl Router` | 这是 Rust 孤儿规则逼出来的"文件切分"，类型仍是上帝对象；改动要跨 30 个文件找 | 要学的是"纯状态机"，不是"一个类型切 30 个文件" |
| 8400+ 处 `.unwrap()` | 常驻后台进程里 panic = 托盘图标消失，属产品缺陷 | 余电现有 `if (!dev.Open()) continue` 的风格反而更对 |
| 手写 CBOR + 手数字段号 + 兼容脚本 | 只在"两端独立升级、无法同时发布"时回本；单机工具是纯负债 | **绝不引入**。余电的"协议"是 HID 报文，方向相反：要适配别人的字节，不是设计自己的字节 |
| `contract.ts` 手工镜像 Rust 结构体 | 没有自动一致性校验，靠人肉同步，迟早漂 | 对应余电的"文档描述协议"——同样会漂，但成本低得多，可接受 |
| 全量 CI（打包、签名、更新源） | 单人项目维护不起 | 只取"编译即校验"这一条 |
| `oink-winsvc` 的 SYSTEM 服务 + 提权 helper | 余电不需要管理员权限，这是它最大的优点之一 | **不要**为了"架构完整"引入服务 |

判断标准可以浓缩成一句：**Oink 的复杂度是在为"跨两台机器、两个 OS、两个独立版本"付账；
余电只有一台机器、一个 OS、一个进程，这笔账不该付。**

---

## 4. 余电现状（基于当前源码）

```
app/src/
  main.cpp       698 行  入口、配置、自启、扫描线程、消息循环、低电量判定
  ui.cpp        2525 行  托盘、分层窗口自绘、动画、主题、toast、菜单、离屏截图
  providers.cpp 2413 行  8 条设备链路：罗技 / 索尼 / Compx / Nordic54 / 迈从 / 飞智 / BLE / 通用 HID
  hid.cpp        475 行  SetupAPI 枚举、描述符解析、异步读写
  app.h          168 行  Reading / Config / Kind
  hid.h           86 行  HidInfo / ReportField / HidDevice
```

### 4.1 已经做对的

1. **线程模型干净**。扫描线程独占 `g_monitor`，产出 `new std::vector<Reading>`，
   经 `PostMessageW` 把所有权交给 UI（`main.cpp:192`）。这是标准的单生产者单消费者移交，
   没有锁、没有共享可变状态。比很多商用托盘程序都规矩。
2. **状态机雏形已经在**。`MonitorState::Scan` 里有 misses 宽限（自动轮询给两次机会）、
   在线优先 + 类型排序、合并逻辑——本质上就是"设备生命周期路由"。
3. **纯函数接缝已经有意识**。`MergeReading(incoming, old, now)` 把时间当参数传入，
   注释直接写明"单独暴露是为了能脱离硬件做纯函数测试"。这个直觉是对的。
4. **诊断能力超前**。`--list` / `--vendor` / `--ble` / `--shot` / `--mock` 这套开发命令，
   思路和 Oink 的 dev-only 环境变量、诊断包一致。

### 4.2 结构性问题（按影响排序）

**S1. Provider 没有接缝，加一个品牌要改三处。**
`MonitorScan`（`providers.cpp:2250`）手写调用链：先 `ScanLogitech` + `ScanLogitechNoHidpp`，
再 `ScanSony`，再 `ScanCompx`/`ScanNordic54`/`ScanMchose`/`ScanFlydigi`/`ScanBle`，最后 `ScanGeneric`。
每段后面跟一句 `if (ScanCancelRequested()) return;`，计时靠 `t1..t4` + 一条 `swprintf` 手工拼。
加一条链路 = 改调用点 + 改计时字符串 + 改文档，三处都可能漏。

**S2. 设备归属靠"后来者手工排除前者"，顺序敏感。**
`ScanGeneric` 开头有 6 行 `continue`：

```cpp
if (d.vid == LOGITECH_VID || d.vid == SONY_VID) continue;
if (IsCompxVendor(d.vid) || IsCompxCandidate(d)) continue;
if (IsNordic54(d.vid, d.pid) || LooksLikeMchose(d)) continue;
if (LooksLikeFlydigi(d)) continue;
```

这是把**所有其它 provider 的匹配谓词复制了一遍**。任何一条链路的判定条件改了，这里不会跟着变；
判定放宽 → 同一台设备出两行；判定收紧 → 设备掉进通用兜底读出不存在的电量。
`ScanCompx` 里那句 `if (IsNordic54(d.vid, d.pid)) continue;` 是同一个病的第二个病灶。

**S3. 协议细节直接长成中文字符串。**
`Reading::status` / `detail` 由 provider 自己拼：

```cpp
swprintf(detail, 96, L"标准 HID 电量字段 · 原始值 %d", raw);
```

后果：无法断言（测试只能比字符串）、无法统计（"哪类失败最多"要正则）、文案分散在 8 个函数里、
UI 层想统一措辞得回去改 provider。Oink 的对应做法是 `FileProviderReason` 那种
**固定、无路径、可序列化的枚举**，字符串只在 UI 最后一跳生成。

**S4. 配置写入非原子，且没有 schema 版本。**
`Config::Save`（`main.cpp:397`）直接 `_wfopen(path, L"wb")` —— 截断后就地写。
写到一半断电/崩溃 → 配置归零，用户的自启、别名、面板位置全丢。
`Config::Load`（`main.cpp:349`）尾部有一段手写兼容处理（30 秒档 → 120 秒），
没有 `schema=` 键，下次再改语义就得在同一个函数里叠第二段 if。

**S5. 验证资产在仓库外，P0 因此卡住。**
`docs/reviews/2026-10-29-fixes.md` 里 C1/C2/C8/C12 的验证程序是 `/tmp/verify/v.cpp`，
现在多半已经没了。同一份文档的 N17（P0，"通用 HID 整条链路是死的"）明确写着
**"本机没有设备，改了无法验证，盲改等于换一个未经验证的实现"** —— 于是搁置。

这就是缺回归夹具的直接代价：不是"没时间写测试"，而是**一个已经定位的 P0 修不了**。
Oink 的黄金向量机制正好治这个病：帧一旦入库，改动就有对照，不需要设备在场。

**S6. 全局变量直连 UI。**
`extern std::vector<Reading> g_devices; extern Config g_config;`（`app.h:98`）。
目前靠"约定"保证只有 UI 线程读写，编译器不帮忙，新人（包括三个月后的自己）也看不出来。

---

## 5. 逐条映射：抄什么、怎么抄

原则：**只搬"降低改动成本"的东西，不搬"看起来专业"的东西**。每条都给最小可行改法。

### 5.1 【学 Oink 的 Output/纯函数】→ 把 Provider 变成表，而不是继承体系

不要引入虚函数基类（余电是 C++17 + 静态链接，虚表能省则省，更重要的是别为 8 个实现造一个类层次）。
用一个 `constexpr` 描述表：

```cpp
// providers.cpp
struct ScanCtx {
    const std::vector<HidInfo>& all;
    ULONGLONG now;
    bool fullSweep;          // 原 g_fullSweep
    bool (*cancel)();        // 原 ScanCancelRequested
};

struct ProviderDesc {
    const wchar_t* id;                         // L"logitech"
    const wchar_t* label;                      // L"罗技"（计时/诊断显示用）
    bool (*claims)(const HidInfo&);            // 这台设备归我吗（纯谓词）
    void (*scan)(const ScanCtx&, std::vector<Reading>*);
    bool hidBased;                             // false = 走 BLE/GATT，不参与枚举分派
};

constexpr ProviderDesc kProviders[] = {
    { L"logitech",  L"罗技",   ClaimsLogitech,  ScanLogitech,  true  },
    { L"sony",      L"索尼",   ClaimsSony,      ScanSony,      true  },
    { L"compx",     L"Compx",  ClaimsCompx,     ScanCompx,     true  },
    { L"nordic54",  L"Nordic", ClaimsNordic54,  ScanNordic54,  true  },
    { L"mchose",    L"迈从",   ClaimsMchose,    ScanMchose,    true  },
    { L"flydigi",   L"飞智",   ClaimsFlydigi,   ScanFlydigi,   true  },
    { L"ble",       L"蓝牙",   nullptr,         ScanBle,       false },
    { L"generic",   L"通用HID",nullptr,         ScanGeneric,   true  },  // 兜底，必须最后
};
```

`MonitorScan` 退化成 30 行循环：先 `EnumHidDevices()`，再对每条链路 `scan`，逐条记耗时。

**顺带解决 S2**：加一个"认领表"。扫描前先让所有 `claims()` 对每个 `HidInfo` 投票，
按表顺序先到先得，结果是一张 `path → providerId` 的映射；`ScanGeneric` 只处理没被认领的。

```cpp
std::map<std::wstring, const ProviderDesc*> owner;   // 每轮重建，O(设备数 × 链路数)
for (const auto& d : all)
    for (const auto& p : kProviders)
        if (p.claims && p.claims(d)) { owner[d.path] = &p; break; }
```

于是那 6 行手工排除可以删掉，`ScanGeneric` 变成"没人要的才归我"，
**加链路不再需要回头改别人的排除列表**。顺序敏感性只剩一条规则：越具体的越靠前。

> 注意：不要一步到位。先加表 + 认领表，保留现有 `ScanXxx` 函数体不动，
> 只把"谁调谁"从硬编码换成循环。函数内部逻辑一行不改，风险最低。

### 5.2 【学 Oink 的 reason 枚举】→ 用 Outcome 替代 status 字符串

```cpp
enum class Outcome : uint8_t {
    Ok, NoReply, Sleeping, Busy, BadChecksum, NoBatteryField,
    Unsupported, OpenFailed, Timeout,
};
struct Reading {
    // ...
    Outcome outcome = Outcome::Ok;   // 替代 status 的一部分
    int rawValue = INT_MIN;          // 原始值留作诊断，不进 UI 文案
};
```

字符串只在 UI 一处生成（`OutcomeText(Outcome, const Reading&)`）。收益是立刻可测的：

- 诊断输出可以按 outcome 计数："本轮 8 台：6 Ok / 1 Sleeping / 1 NoReply"；
- 回归测试可以断言 `outcome == Outcome::BadChecksum`，而不是比对中文；
- 文案改一次全站生效，不用在 8 个 `ScanXxx` 里搜。

`detail` 保留为"给人看的补充"，但只允许放**数值和原始帧摘要**，不允许放结论性措辞。

### 5.3 【学 Oink 的黄金向量】→ `--selftest` + 帧夹具，先解决 N17

这是投入产出比最高的一条，也是唯一能解开"P0 因为无法验证而搁置"死结的办法。

设计要点：

1. **不引入测试框架**。余电的卖点是"不装运行库、单文件 exe"，加 gtest 会破坏这个卖点。
   用 `yudian.exe --selftest` 子命令，输出 `ok 1 - C1 reportId=0` / `not ok 2 - ...`，
   有任何 `not ok` 就返回非 0 退出码。TAP 格式够用，`build.bat` 末尾追加一行调用即可。
2. **夹具用文本十六进制**，和 Oink 的 `.hex` 一致，人眼能读、git diff 能看：

```
app/tests/frames/
  generic_reportid0.txt      # 描述符 + 报文 + 期望 level，把 C1 的用例固化
  generic_logical_0_1.txt    # C2
  generic_multi_usage.txt    # C8
  compx_battery_real.txt     # 从 dev/probes 跑出来的真实帧
  mchose_v2_real.txt
  sony_dualsense_usb.txt
  bad_checksum.txt           # 负例：必须 BadChecksum
  truncated_frame.txt        # 负例：必须拒绝而非崩溃
```

每个文件三段：`# descriptor <hex>` / `# report <hex>` / `# expect level=100 outcome=ok`。
解析器 20 行，断言 5 行。
3. **夹具来源现成**：`dev/probes/` 里 48 个脚本这两年攒下来的原始帧，直接转成文本。
   这是余电独有的资产，Oink 没有——它只能自己造夹具。
4. **N17 的正确做法**：先写一个"期望通用 HID 能读出电量"的夹具（用 `caps.py` 证明过的
   真实设备的 value caps 数据），它会红；再用 `HidP_GetValueCaps/GetUsageValue` 重写
   `ScanGeneric`，让它变绿。**先把"无法验证"变成"有断言"，再动手**——这正是 Oink 的顺序。

### 5.4 【学 Oink 的原子存储 + 配置版本】→ 20 行改动

```cpp
void Config::Save() {
    std::wstring tmp = ConfigPath() + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return;
    fprintf(f, "schema=1\n");          // 新增：以后迁移有依据
    // ... 原有键
    fflush(f);
    fclose(f);                          // 先落盘再换名，不留半截文件
    MoveFileExW(tmp.c_str(), ConfigPath().c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
```

`Load` 里把末尾那段兼容处理抽成 `Migrate()`，读 `schema` 决定走哪条路径。
现在只有 1 条规则，抽出来显得小题大做——但它是第 2、3 条规则的落点，
而且 Oink 的 `config_module` 证明了这个位置迟早会被用到。

### 5.5 【学 Oink 的不变量注释】→ 每个文件顶部三行

Oink 的 crate 文档全是"我负责什么 + 我**不**负责什么 + 不变量"：

> Capture never changes endpoint volume or default devices; source mute uses a durable restoration journal.

余电的 `README` 已经有"一个文件管一件事"的表格，但代码里没有。建议在每个 `.cpp` 顶部补三行：

```cpp
// providers.cpp —— 设备链路
// 负责：把 HidInfo 变成 Reading；解析厂商报文；决定"这台设备归我"
// 不负责：线程、定时、配置、文案排版（这些在 main.cpp / ui.cpp）
// 不变量：不写设备（只读 Feature/Input 报文）；同一 path 同一轮只产出一条 Reading
```

成本 5 分钟，收益是改代码时不用重新推导边界。特别是"不写设备"这条——
它是余电敢说自己"不装驱动、不干扰设备"的技术依据，值得写在最显眼的地方。

### 5.6 【学 Oink 的空闲预算测试】→ 一条手工基线，不要自动化

Oink 的 `idle_cpu.rs` 是自动化测试，因为它是跨平台后台服务，回归一次就是用户投诉。
余电只有一台机器，自动化收益低。但**手动基线值得固化进文档**：
`technical-notes.md` 已有「资源占用（本机实测）」一节，建议补两列
「空闲 10 分钟 CPU 时间」「一次扫描的唤醒次数」，每次改 provider 后手工对一遍。
这比写一个测不出波动的测试实在。

---

## 6. 可落地改进清单

按"投入 / 收益"排序。P0 是本周能做完的，P1 是两周内，P2 是有空再说。

### P0（每条 < 2 小时，互不依赖）

- [ ] **配置原子写 + `schema=1`**：`Config::Save` 改成 tmp + `MoveFileEx`；`Load` 抽 `Migrate()`。
      风险极低，防的是"崩溃后配置全丢"这类不可逆事故。
- [ ] **`--selftest` 骨架 + 5 条夹具**：先只做 `ExtractBits` / `NormalizeLevel` /
      `DecodeBatteryReport` 三个纯函数，把 C1/C2/C8/C12 的 14 条断言从 `/tmp/verify/v.cpp`
      搬进仓库。**这是还债，不是新增功能**——那 14 条断言现在处于"写过了但没了"的状态。
- [ ] **`build.bat` 末尾跑 `--selftest`**，失败时打印醒目的一行并不删除旧 exe。
      没有门禁的测试等于没有测试。
- [ ] **诊断输出加一行汇总**：`本轮 8 台 / 6 应答 / 1 休眠 / 1 无应答 / 耗时 812ms`。
      用户反馈"读不到电量"时，这一行能省掉三轮问答。

### P1（结构性，但一次只动一层）

- [ ] **认领表**（§5.1 的后半）：先只加 `claims()` 谓词和 `owner` 映射，
      `ScanGeneric` 改用 `owner` 判断，删掉那 6 行手工排除。函数体不动。
      验收标准：`--list` 输出与改动前逐字节一致（这是现成的回归基线）。
- [ ] **Provider 描述表 + `MonitorScan` 循环**：把硬编码调用链换成 `kProviders` 遍历，
      耗时统计改为按表聚合，删掉手写 `swprintf`。
      验收标准同上：`--list` 一致 + 耗时数字仍在。
- [ ] **Outcome 枚举**：`Reading.status` 拆成 `outcome` + `detail`；`OutcomeText()` 落在 UI 层。
      这一步会碰到 `ui.cpp` 的文案，建议和下一次改 UI 一起做。
- [ ] **`hid.cpp` 的 `ParseDescriptor` 加前置不变量**：`bitSize ∈ (0,32]`、
      `bitOffset ≥ 0`、`bitOffset + bitSize ≤ len*8` 在解析层就拒绝，而不是等 `ExtractBits` 兜。
      C12 修在 `ExtractBits` 是"最后一道闸"，解析层才是"源头"。

### P2（收益递减，但便宜）

- [ ] 每个 `.cpp` 顶部三行契约（§5.5），顺手把 `providers.cpp` 的 8 条链路各写一句"认哪些设备"。
- [ ] `g_devices` 更名 `g_snapshot` 并加 `// 只读：由扫描线程经 PostMessage 移交，UI 不得修改`。
      改名是最便宜的"防误用"手段。
- [ ] `build.bat` 加 `--check` 开关：`-Wall -Wextra` 只报告不阻断，先看有多少噪音再决定是否收紧。
- [ ] N17（`HidP_GetValueCaps` 重写通用 HID）：**等 P0 的夹具就位后再做**。顺序不能反。

### 明确不做

| 想法 | 为什么不做 |
|---|---|
| 把 providers 拆成多个 .cpp | 2413 行确实长，但拆开会让"认领顺序"这种全局约束更难看清。先做 §5.1，拆不拆到时候再说 |
| 引入虚函数 Provider 基类 | 8 个实现、无运行时插件需求，虚表换来的只有间接层 |
| 引入 gtest / Catch2 | 破坏"单文件、无依赖"的卖点。`--selftest` 足够 |
| 手写二进制协议 / 版本协商 | 余电的"协议"是别人的 HID 报文，方向相反 |
| 加 CI | 单人项目，`build.bat` + `--selftest` 就是 CI。真要加，等有多台机器要验证时再说 |
| 拆成多个进程 / 加服务 | 权限最小化是余电的优点，别为了"架构完整"丢掉 |

---

## 7. 一句话总结

Oink 值得抄的不是它的 15 个 crate，而是它把**"决策"和"执行"、"归属"和"呈现"分开**的那几条线——
以及在每个边界上写下预算、把每次踩坑固化成夹具的习惯。

余电已经有正确的直觉（线程移交、纯函数合并、诊断命令），缺的只是把这些直觉变成结构：
**provider 表 + 认领表让"加设备"变成加一行；`--selftest` + 帧夹具让"改了有没有坏"不用等真机。**
这两件事做完，2413 行的 `providers.cpp` 会从"不敢动"变成"可以动"。
