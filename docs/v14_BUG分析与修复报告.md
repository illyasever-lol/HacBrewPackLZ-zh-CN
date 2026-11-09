# v14 BUG 分析与修复报告

日期：2026-11-08 · 版本：v14（基于 v13）· 涉及：EXE（CLI + GUI）与 NRO 双端重编

本轮处理实测反馈的 3 个问题：

| # | 端 | 现象 | 性质 |
|---|---|---|---|
| 1 | EXE | USB 配合 NRO 传输到约 **987 MB** 时整个窗口「未响应」 | **死锁**（确定性根因，上游原版缺陷） |
| 2 | NRO | TF 卡已存在 `sdmc:/hacBrewPackLZ/extracted` 目录时选择解包，报「文件夹…无法访问。如果在 USB 可移动设备上，请确认已插好。」 | 目录判定失效（上游原版缺陷） |
| 3 | EXE | 游戏列表进度仍显示 `Sending 10%` | 漏译（v1 全量导入遗漏） |

---

## 一、EXE 987 MB「未响应」——锁序反转死锁

### 1.1 现象与排除

- Windows 报「未响应」= **UI 线程不再处理窗口消息**，不是 USB 传输变慢。
- 987 MB 不是 2 的整数边界，也不是 `int` 溢出点（2,147,483,647 B ≈ 2 GiB）：
  全链路 offset/size 均为 `i64`，单次 transfer ≤ 1 MiB，`SendWatch` 的 `read`/`length`/`count` 全是 `long`。
- 排除 v13 的 USB 端点并发问题：v13 已用 `CableGuard` 把整个 request/answer 事务与心跳探针串行化，
  且本轮未再出现 `2162-0002 (0x4a2)` 主机崩溃；本轮是 **PC 侧 UI 挂起**，两者现象与进程都不同。
- 排除 WinUSB 驱动、句柄泄漏、`LogView` 日志膨胀（每 5 秒一条，987 MB 期间不足 4000 行上限）。

### 1.2 根因（可对源码逐行复现）

两把锁：`SendWatch._gate`（进度状态）与 `Session._gate`（会话状态）。

**路径 A — USB/网络服务线程**，`src/Sender.cs` `SendWatch.Read()`（修复前）：

```csharp
lock (_gate)                       // 持有 SendWatch._gate
{
    ...
    if (newFile || 每 5 秒)
        Ui.Info($"正在发送 {…}");   // -> Session.Message() -> 申请 Session._gate
}
Changed?.Invoke();
```

`Ui.Info` → `Ui.Sink.Message` → `Session.Message` → `lock (Session._gate)`。
即 **持 SendWatch 锁去申请 Session 锁**。

**路径 B — UI 线程**，`MainWindow.Refresh()` → `src/Session.cs` `Snapshot()`（修复前）：

```csharp
lock (_gate)                       // 持有 Session._gate
{
    ...
    return new SessionSnapshot(..., _sendWatch?.Snapshot(), ...);  // -> 申请 SendWatch._gate
}
```

即 **持 Session 锁去申请 SendWatch 锁**。

两条线程以**相反顺序**申请同一对锁，一旦时序交叠即永久互等：

```
服务线程:  持 SendWatch._gate ──等──> Session._gate
UI 线程:   持 Session._gate   ──等──> SendWatch._gate    ⇄ 环路，UI 线程永挂
```

`PostFromWorker()` 是异步 `PostMessageW`，但它发生在 `Session.Message` **拿到 Session._gate 之后**，
所以异步投递并不能解开这个环。与零改动基线 `hacbrewpacklz/` 逐行比对确认：**两处写法原版就有，
属上游固有缺陷**，并非汉化引入。

### 1.3 为什么恰好在 987 MB

`Ui.Info` 只在「切换到新文件」或「距上次播报 ≥5 秒」时触发，因此死锁窗口很窄；
但每次读事件都可能与 UI 刷新交叠，属于小概率竞态。设单次撞上概率 `p`，
`n` 次读事件后累积命中率 `1-(1-p)^n` 单调上升：

| 读事件数 | 累积命中率（p=2e-5 示意） |
|---:|---:|
| 1,000 | 2.0% |
| 10,000 | 18.1% |
| 60,000 | 69.9% |

**传输越久、读事件越密，越容易撞上**，与累计字节数没有因果关系。此前 v12 实测常在 2～3 GB
时先遇到 v13 修掉的主机崩溃，PC 侧这个死锁没机会暴露；v13 让传输更稳定后，它反而先被观测到。

### 1.4 修复（两处都改，彻底断开双向依赖）

| # | 文件 | 改动 |
|---|---|---|
| 1 | `src/Sender.cs` | `Read()` 在锁内只计算要说的话到 `say`，**退出锁之后**才 `Ui.Info(say)`；`Changed?.Invoke()` 仍在锁外 |
| 2 | `src/Session.cs` | `Snapshot()` **进入 `_gate` 之前**先取 `SendSnapshot? send = _sendWatch?.Snapshot()`，锁内只使用已取好的 `send` |

只挪一处仍可能留下「持锁回调」的隐患，故两端同时消除。锁序铁律：**任何跨对象回调都不得在持有自身锁时发出**。

### 1.5 验证

- `simulate_v14_deadlock.py`：用 `Barrier` 强制交错，修复前 **5/5 复现双向等锁（UI 线程挂起）**；
  修复后 **20/20 无死锁**，服务线程与 UI 线程 **20/20 全部完成**。
- 77/77 综合验证含源码断言、补丁重放、#US 堆、反汇编、构建与无 .NET 冒烟。

---

## 二、NRO 已存在 `extracted` 目录时解包报「无法访问」

### 2.1 报错定位

截图文案「文件夹 `sdmc:/hacBrewPackLZ/extracted` 无法访问。如果在 USB 可移动设备上，请确认已插好。」
精确对应 `switch/app/app_work.cpp::App::extract()` 的 `ToolError`；同文案也用于 `needRoomIn()`。
`extractFolder()` 返回 `joinPath(_platform.outputFolder(), "extracted")`，
而 `nx_main.cpp` 的 `OutputFolder = "sdmc:/hacBrewPackLZ"`，与用户 TF 卡上的
`TF:\hacBrewPackLZ\extracted` 是同一目录。

### 2.2 与上游基线比对

`app_work.cpp` 的 `extract()` / `needRoomIn()` 与 `hacbrewpacklz/` 零改动基线 **逐行 diff 只有汉化差异**，
逻辑一致 → **属原版固有缺陷**，不是 v10 ASCII 路径改造引入（v10 只改了 `app_saved.cpp::extractFolder()` 的
`asciiPathOnly` 兜底与 `core/nca.cpp` 的输出名折叠）。

修复前的写法：

```cpp
sys::makeDirectories(output);
if (!sys::isDirectory(output))
    throw ToolError("文件夹" + output + "无法访问。…");
```

### 2.3 失效模式

核对 libnx `nx/source/services/fs.c`：

| 事实 | 后果 |
|---|---|
| `fsFsCreateDirectory` 对**已存在**目录返回 `PathAlreadyExists`（`makeDirectories` 逐段调用且忽略返回值） | 目录本来就在，仍会反复向共享 FS 会话提交无效 CreateDirectory |
| `fsFsGetEntryType` 只是 IPC 转发；同一 `FsFileSystem` 会话被文件扫描线程并发使用时单次调用可能失败 | `isDirectory()` 原来**只信这一次调用**，失败即 `false` → 把「目录已存在」误报成「无法访问」 |
| `loadSettings()` 用 `line.substr(eq+1)` 读 `extractfolder`，未去行尾 `\r`；`asciiPathOnly()` 只拒绝 `>=0x80` | Windows 编辑过的 `settings.txt` 会让路径带不可见控制字符，`\r` 属于「技术上 ASCII」却被 FS 服务拒收 |
| `makeDirectories` 从不先判断目标是否已存在 | 已存在路径无法走快速成功路径，每次解包都重复上述风险 |

`TF:\hacBrewPackLZ\extracted` 已存在时，文件扫描会把它当作「解包游戏」来源遍历，
与工作线程的目录检查并发，正是最容易触发上面第 2 条的场景；目录不存在时没有这次扫描，
因此表现为「有目录才报错」。

### 2.4 修复（5 处，不改协议 / 不改输出目录名 / 不动 v13 事务锁）

| # | 文件 | 改动 |
|---|---|---|
| 1 | `switch/nx/nx_sys.cpp` `isDirectory()` | `fsFsGetEntryType` 失败重试 3 次（10/20/30 ms）；仍失败则用 `fsFsOpenDirectory` 作权威判定并**配对 `fsDirClose`**；调用明确返回「文件」时仍立即 `false` |
| 2 | `switch/nx/nx_sys.cpp` `makeDirectories()` | SD 卡分支开头 `if (isDirectory(path)) return;`，**已存在目录整体短路**，不再提交无效 CreateDirectory |
| 3 | `switch/app/app_work.cpp` | `needRoomIn()` 与 `extract()` 改为「先查 → 必要时建 → 再查」，报错文案保持不变 |
| 4 | `switch/app/app_saved.cpp` `asciiPathOnly()` | 除 `>=0x80` 外，拒绝 `<0x20` 控制字符 |
| 5 | `switch/app/app_saved.cpp` `loadSettings()` | 对读出的值 `while (… <= ' ') pop_back()`，消除行尾 CR/空白 |

### 2.5 二进制级硬证据

libnx 静态库本来就含 `fsFsOpenDirectory` / `fsDirClose` 符号，**只看符号表不能证明新代码编入**。
故对 `hacBrewPackLZ.elf` 反汇编 `hbp::sys::isDirectory`（`0xcded0`）函数体，实际调用序列：

```
bl  fsFsGetEntryType      @ cdf44
bl  svcSleepThread        @ cdf54   ← 重试退让
bl  fsFsOpenDirectory     @ cdf98   ← 权威兜底
bl  fsDirClose            @ cdfe8   ← 句柄配对关闭
```

`makeDirectories` 内 `bl _ZN3hbp3sys11isDirectory…` 出现在 `bl fsFsCreateDirectory` **之前**，
证明幂等短路已链接。`build.log` 确认 `nx_sys.cpp` / `app_work.cpp` / `app_saved.cpp` 三文件均重编。

---

## 三、EXE 漏译与同类 GUI 英文残留

| 位置 | 修订前 | 修订后 |
|---|---|---|
| `gui/TitleList.cs:341` | `Sending 10%` | `正在发送 10%` ← **用户报告** |
| `gui/Widgets.cs:144` | `On` / `Off` | `开` / `关` |
| `gui/FormWindow.cs:245,254` | `Choose: X` | `选择：X` |
| `gui/FormWindow.cs:589` | `Add 1 file` / `Add N files` | `添加 1 个文件` / `添加 N 个文件` |
| `gui/FormWindow.cs:591-593` | `N found` / `, … chosen` | `共找到 N 个文件` / `，已选 …` |
| `gui/MainWindow.cs:114-116` | 整段英文重新打包提示 | 中文（含「另有 N 个」「是否取消勾选它们？」） |
| `src/Session.cs:103` | `…: 3 of 12` | `…: 3/12` |

`Sending 10%` 出现在 `TitleList.Result()` 的发送进度分支，与相邻的 `已发送` 同属 v1 全量导入时
遗漏的拼接片段。其余为本轮主动清理的同类 GUI 可见英文，均为纯字符串替换。

GUI 程序集 `#US` 堆（UTF-16LE）逐串核验：上述新译文全部命中，
`Sending ` / `With compression off` / `Choose:` / `Add 1 file` / `{visible.Count} found` 全部清零。

---

## 四、明确未改动 / 保留

- 协议层 `HBPQ`/`HBPR` 魔数、请求结构、EXE↔NRO 通讯格式。
- v13 的 `cableBusy` + `CableGuard` 事务锁、探针 try-guard、CompletionEvent 排空、30 s 退避。
- v12 两处文案（`游戏数据可压缩至`、`已解包到 …，用时 …`）。
- v11 USB 服务心跳检测、v10 输出路径 ASCII 化（`extracted` 目录名与 `forOutputName()` 折叠）。
- 解包报错文案本身（只改判定流程，不改措辞）。
- Release 资产范围仍只有 `hacbrewpacklz-gui_zh.exe` + `hacBrewPackLZ_zh.nro` + 两行 SHA256SUMS，
  **不含 CLI exe**。

---

## 五、验证汇总（77/77 ALL_OK）

| 组 | 内容 |
|---|---|
| 源码 18 | 17 处补丁全部落地、双向锁序断开、5 处 NRO 目录修复、7 处译文 |
| 基线 8 | 上游原版同样存在两处缺陷写法（确认非汉化引入）；v10～v13 功能未回退 |
| 补丁 3 | 9 个改动文件均有修复前备份且可逐字节重放；`v14_修复资产.json` 记录 3 个问题 |
| NRO 6 | `NRO0` @0x10、size 字段、v10/v11/v12 中文 C 串、报错文案、SHA 相对 v13 已变 |
| 符号/反汇编 8 | `cableBusy`/`CableGuard` 仍在；`isDirectory` 4 个调用 + `makeDirectories` 调用顺序 |
| EXE 27 | 2/2 自包含 0 错 0 警；CLI+GUI `#US` 堆新译文 8 组、旧英文 4 组清零 |
| 构建 6 | NRO 40 对象 0 错 0 警，3 个改动 C++ 确实重编 |
| 模拟 5 | 死锁 5/5→0/20、概率模型、v13 USB 并发模拟未回退 |
| 冒烟 2 | 屏蔽系统 .NET 后 `--help` 正常且输出含中文 |

NRO 全量重编 40 对象（删 26 个 `main_*.o`），EXE 2/2 自包含单文件重编成功。

---

## 六、未验证 / 实机复测建议

无真实 Switch / WinUSB / `prod.keys`，本轮为源码 + ELF 反汇编 + 二进制 + 行为模拟级验证。
建议实机按以下顺序复测：

1. **死锁**：USB 连续安装 **5 GB 以上**（跨多个 NSP），观察 EXE 窗口是否始终响应、进度是否持续推进；
   重点看切换游戏文件与 5 秒播报时刻。
2. **目录**：在 TF 卡上**预先手工创建** `hacBrewPackLZ/extracted`（空目录与含上次解包内容的两种情况都测），
   再选择解包游戏；应正常进入解包而不是报「无法访问」。
3. **回归**：v11 服务检测（PC 停止发送后 3～5 s 图标消失、恢复后 3～10 s 出现）仍正常；
   解包收尾显示「已解包到 …，用时 … s。」；游戏列表进度显示「正在发送 NN%」。

若第 2 项在实机仍复现，请抓取 NRO 日志中 `fsFsGetEntryType` 的实际返回码，以便把兜底策略进一步收敛。
