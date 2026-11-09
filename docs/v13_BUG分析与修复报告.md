# v13 BUG 分析与修复报告

**问题**：USB 数据线配合 PC 端 exe 连续传输多个游戏安装文件，累计约 2～3 GB 时 Switch 整机崩溃，报错 `2162-0002 (0x4a2)`。
**结论**：v11 新增的「USB 发送服务心跳」与安装事务**并发使用同一 USB 端点与同一块 1 MiB 缓冲**，是本次崩溃的根因；不是 32 位字节溢出，也不是固件/大气层版本问题。
**处置**：新增 USB 端口事务级互斥（v13），只重编 NRO；EXE 未改动。

---

## 一、崩溃现场解码

| 现场字段 | 值 | 判读 |
|---|---|---|
| Error Code | `2162-0002 (0x4a2)` | `0x4A2 = 1186`；按 Switch 错误码显示规则 `xxxx = 2000 + R_DESCRIPTION`、`yyyy = R_MODULE`，即 `2000+162` / module `2`。该码在 `nim`、`npns`、`qlaunch`、`usb` 等**多个不同系统模块**的 fatal 报告中重复出现，属于系统进程主动 abort 的通用结果码，**不能**据此反推业务原因 |
| Program | `0100000000000006` | 查 SwitchBrew sysmodule 表 = **`usb` 系统模块**。崩溃进程是系统 USB 服务，**不是 NRO 自身**（homebrew 崩溃会是 `0100000000001000` 一类 title，且多为 `2168-0002`） |
| Firmware | `22.5.0 (Atmosphère-1.11.2-master-538824be)` | 版本与大气层匹配，开机/WiFi 均正常，仅长时间 USB 安装后必现 → 排除版本错配 |
| PC | `00000050F4A19254` | 落在 `0x50F4A1xxxx` 段 |
| Backtrace | Start `00000050F4A00000`；BT100–BT123 几乎全部为 `0x00000050F4A1xxxx`，末尾归零 | 崩溃调用栈集中在同一个系统模块地址段，符合 USB 服务内部状态断言失败后主动 fatal，而非用户态越界访问 |

> 一句话：**主机 `usb` 系统服务的端点/URB 状态被破坏后主动 fatal**，NRO 只是触发者。

---

## 二、排查路径与排除的假设

| # | 假设 | 取证 | 结论 |
|---|---|---|---|
| 1 | **32 位累计字节溢出**（`2^31 = 2 GiB` 与「2～3 G」太接近，第一嫌疑） | 全链路核对：`RemoteFile::_size` / `read(offset)` / `sendRequest(a,b)` 全为 `i64`；`LargestRead = 8 MiB`；`UsbPort::transfer()` 每次 ≤ 1 MiB；`UsbDsReportData` 的 `transferred` 是**单次** URB 字节数；`urbId` 为 `u32` 但 2～3 GB 只需约 2500 次 transfer，离 `2^32` 极远；PC 端 `answer = new byte[b]` 且 `b ≤ 16 MiB` | ❌ 排除，不存在累计 2 GB 的 32 位计数 |
| 2 | PC 端 WinUSB 长度/句柄泄漏 | `Sender.Serve()` 单线程顺序处理；`FileStream` 按路径复用并在 `finally` Dispose；`SwitchUsb.Write` 的 `uint` 长度每次 ≤ 8 MiB。且崩溃进程在 **Switch 侧** | ❌ 排除 |
| 3 | NRO 内存泄漏（多个游戏文件累积） | `link->streams = id != "USB"` —— USB 链路**不启用**预取流，`RemoteFile` 只保留固定 1 MiB `_block`；`g_links` 中每 PC 一个常驻 Link | ❌ 排除 |
| 4 | 固件 / 大气层 / prodinfo 版本错配 | 该组合的典型表现是开机或连 WiFi 立即 fatal，且重启后仍崩；本次重启正常、只有跑满 2～3 GB USB 传输才崩 | ❌ 排除 |
| 5 | 上游原版就有此缺陷 | 与零改动基线 `hacbrewpacklz/` 逐行 diff：`nx_remote.cpp` 除 UI 字符串汉化外，**唯一逻辑差异就是 v11 新增的 3 s 心跳与 `g_lastUsbTraffic` 软时间戳**；原版 `watchCable()` 只在插线时问一次名字 | ⚠️ 指向 v11 回归 |
| 6 | **v11 心跳与安装并发抢端点/缓冲** | `UsbPort` 是单例，只有一对端点 `_in/_out` 和一块 1 MiB `_buffer`；`askUsbName()` 直接对同一单例 `write/read`，**与主事务没有任何互斥**；唯一保护是 `now - g_lastUsbTraffic >= 2` 的软时间戳，且该时间戳**只在 `write()/read()` 入口刷新** | ✅ **命中** |

---

## 三、根因时序（为什么长应答窗口是致命的）

一次 `ask()` 事务 = `sendRequest`（2 次 `write`）+ `receiveAnswer`（`read` 16 字节头 + `read` 最多 8 MiB 正文），正文被 `UsbPort::read()` 拆成 8 次 `transfer(1 MiB)`，每次都向 `_out` 端点 `PostBufferAsync(_buffer, 1 MiB, &urb)`。

| 时刻 | 主事务（安装/列表） | 心跳线程 `watchCable()` |
|---|---|---|
| T0 | `UsbPort::write()` 入口刷新 `g_lastUsbTraffic = T0` | — |
| T2 | `UsbPort::read(8 MiB)` **入口**刷新 `g_lastUsbTraffic = T2`，随后开始 8 次 `transfer` | — |
| T3 | PC 端读盘 / 压缩准备 / 切换游戏文件，某次 `transfer` 耗时 > 2 s。**循环内部不刷新时间戳** | 误判：`now - T2 ≥ 2 s` 且 `now - askedAt ≥ 3 s` → 判定「通道空闲」 |
| T4 | 仍在第 8 次 `transfer`，`_out` 端点上有未完成 URB，`_buffer` 正在被 `memcpy` 读走 | 发出 Hello 探针 |
| T5 | — | 探针 `port.write(28)` 先 `memcpy(_buffer, …, 28)` **覆盖主事务正在读的缓冲**；`port.read(16)` 再对**同一个 `_out` 端点** `PostBufferAsync` **同一块 `_buffer`** |
| T6 | — | usb:ds 发现同一端点两个未完成 URB 共用缓冲 → **`usb` sysmodule 内部状态破坏 → fatal `0x4a2`** |

此外，错位后主流程的魔数校验失败 → 抛错 → `link->transport.reset()` → 反复 `usbDsEndpoint_Cancel()` / `catchUp()`，进一步污染端点与 report 队列，使崩溃从「可能」变成「必然」。

### 为什么是「大概 2～3 GB」而不是固定字节数

**不是 32 位溢出，是概率/时间累积**：触发条件是「出现一次 > 2 秒的 8 MiB 长应答」。单个游戏文件通常不到 2 GB，但连续安装多个文件时，PC 端文件切换、机械盘/杀软扫描、压缩准备会反复制造这种长应答；传输量越大、总时长越长，撞上该窗口的概率越接近 1。用户观察到的 2～3 GB 是这个概率累积后的经验阈值，会随 PC 负载与文件数量浮动。

---

## 四、修复（v13）：5 条不变量

只改 NRO 三个文件，**不动协议、不动 EXE、不动任何 UI 文案**。

| # | 文件 | 改动 | 作用 |
|---|---|---|---|
| 1 | `core/remote.h` / `core/remote.cpp` | 新增 `std::atomic<bool> cableBusy` 与 `CableGuard`；`ask()` 对 USB 链路在**整个 request/answer 事务**期间持有它（CAS 抢占 + 阻塞等待），网络链路不启用 | 把互斥粒度从「单次 write/read」提升到「整个事务」，探针不再可能插在头与正文之间 |
| 2 | `nx/nx_remote.cpp` `askUsbName()` | 改为 `bool askUsbName(std::string&)`，用 `CableGuard(true, false)` **try 抢占**；抢不到就**整次跳过**并保留上次名字，绝不阻塞安装；探针全程持锁 | 硬互斥替代软时间戳；安装期间心跳自动让路 |
| 3 | `nx/nx_remote.cpp` `UsbPort::transfer()` | `g_lastUsbTraffic` 改在**每次 transfer 成功后**刷新 | 8 MiB 长应答期间持续刷新，不再被误判为「通道空闲」 |
| 4 | `nx/nx_remote.cpp` `transfer()` 超时路径 | 循环等待 `CompletionEvent`（原代码只 `waitSingle` 一次且**不检查返回值**），并在 Cancel 后 `GetReportData` 排空被取消 URB 的 report | URB 未完成时不再复用 `_buffer`；消除陈旧 report 累积 |
| 5 | `nx/nx_remote.cpp` `watchCable()` | 探针无应答后间隔由 10 s 退避到 **30 s**；只有真正发出探针才推进 `askedAt` | 服务停止期间不再每 3 s 制造一次 Cancel |

死锁分析：主事务先拿 `link->mutex` 再等 `cableBusy`；探针只拿 `cableBusy`、**从不拿** `link->mutex` → 无环。探针是 try-guard，不会阻塞安装；主事务的阻塞等待上限即一次探针（1.5 s），且 `QuietSeconds=2` 使其几乎不会触发。

**功能保留**：v11 的「停止发送后 3～5 s 自动刷新、重新发送 3～10 s 恢复、无需拔线」在**通道空闲时**行为不变；仅在正在传数据时把探针顺延。

---

## 五、验证证据

| 项 | 结果 |
|---|---|
| 补丁预检 / 落地 | `apply_patch_v13.py` **9/9 命中**，3 文件 9 处精确替换，重放校验通过，三份原文件已备份至 `artifacts/patch_backup_v13/` |
| 重编 | **全量**重编 26 个 `main_*.o`（`remote.h` 变更），`objs=40`、`errors=0`、build.log **0 error / 0 warning** |
| ELF 符号表 | `aarch64-none-elf-nm -C` 确认 `hbp::remote::cableBusy`、`CableGuard::CableGuard(bool, bool)`、`CableGuard::~CableGuard()` 均已链接进 NRO |
| 综合验证 | `verify_v13.py` **51/51 ALL_OK**（源码 22 · 基线 6 · 二进制 6 · 符号 4 · 构建 3 · 行为模拟 5 · 补丁重放 5） |
| 行为级模拟 | `simulate_v13_usb_guard.py` **7/7**：确定性慢读场景修复前出现并发 post、修复后 0；4 GB 多文件场景修复前 **3 次**并发 post、修复后 **0 次**；修复后主事务被推迟 **0 次**；空闲 40 s 探针仍可发出 ≥10 次（v11 功能未退化） |
| 既有功能 | v12「用时 3 s」、v11 服务停止提示、v10 `extracted` ASCII 路径、NRO0 魔数、中文 C 串全部仍在 |

模拟是**离散事件模型**（参数见脚本头部），用于验证「互斥不变量」与机制，不是真机测速；其中首次冲突的 MB 数由模型里的慢读间隔决定，**不用于预测实测的 2～3 GB 阈值**。

---

## 六、产物与未验证项

- 新 NRO：`artifacts/dist/hacBrewPackLZ_zh.nro`（1,633,335 B，SHA256 见 `v13_验证报告.md`）
- 单独 NRO：`artifacts/hacBrewPackLZ_zh_NRO_v13_20261108.nro`；发布包 `hacBrewPackLZ_zh_v13_20261108.zip`
- **EXE 未改动**：本次是纯 NRO 端逻辑修复，`hacbrewpacklz-gui_zh.exe` 沿用 v12 构建。

仍需实机确认（工作区无真实 Switch / WinUSB 设备 / `prod.keys`）：

1. 连续 USB 安装累计 5 GB 以上不崩溃（重点验证 2～3 GB 之后仍能继续）；
2. 传输中不出现卡顿或明显降速（探针被跳过、主事务未被推迟的实测体现）；
3. PC 点「停止发送」后不拔线，列表仍在 3～5 s 内刷新；重新发送后 3～10 s 恢复；
4. 拔线/插线、服务超时退避 30 s 等边界行为。

`0x4a2` 是 `usb` sysmodule 主动 fatal 的结果码，Atmosphère 未公开其内部分支；本报告的定位依据是「崩溃进程 ID + 代码中可证明的 usb:ds 非法用法」两边夹逼，**未做固件级反汇编**。
