# v16 BUG 分析与修复报告

**日期**：2026-11-08　**范围**：仅 EXE（CLI + GUI）重编；NRO 与 v15 二进制完全一致（未改动）
**结论**：v15 对「安装完毕仍留进度」的修复**只做了一半**——判定逻辑正确，但缺少让时间推进的触发源。本次补齐。

---

## 1. 用户实测现象

| 项 | 现象 |
|---|---|
| 截图 | 两行游戏「Teenage Mutant Ninja Turtles: Shredder's Revenge」1.62 GB 显示 **正在发送 66%**、「疯狂兔子：奇遇派对」3.28 GB 显示 **正在发送 93%** |
| 对比 | **NRO 上所有游戏均已安装完毕**，PC 端却仍有两行传输进度未完成 |
| 附带 | 状态栏/详细信息提示 **正在发送 Concealed [B+U65536].nsp**，且不再消失 |

---

## 2. 归因

与 v15 备份源码（`patch_backup_v16/`）逐行比对：**v15 本地缺陷（修复不完整）**，不是上游原版缺陷、不是汉化误伤、也不是 v13/v14 资产退化。

| 检查 | 结果 |
|---|---|
| v15 是否已有 `Settled` 判定 | 有：`SendWatch.Snapshot()` 里 `settled = (now - _lastRead) >= 8s` |
| v15 `TitleList.Result()` | `sent.read >= sent.length \|\| send.Settled` → 显示「已发送」 |
| 那为什么没生效 | **这个 `bool` 是「取快照那一刻」算出来的瞬时值，而 GUI 只在有事件时才重新取快照** |
| 其它 7 项 v15 修复 | 全部仍在（`ES_AUTOHSCROLL`、`WS_HSCROLL`、1024×768、SDK 文案、`CableGuard`、锁序、journal 兜底） |

---

## 3. 根因链路（决定性证据）

```
Switch 停止读取
   └─ Sender.Serve() 不再调用 SendWatch.Read()
        └─ SendWatch.Changed 不再触发                    ← 唯一的刷新事件源
             └─ Session.Changed 不再触发
                  └─ MainWindow.OnMessageFromWorker() 不再被调用
                       └─ Refresh() 不再执行  →  _state.Send 被永久冻结
```

`MainWindow.cs` 的三条代码路径共同造成冻结：

| # | 位置 | 事实 |
|---|---|---|
| 1 | `session.Changed += ...` | 只有 `session.Changed` 会置 `_refreshPending = 1` 并 `PostFromWorker()`；**没有任何定时器会置位** |
| 2 | `OnMessageFromWorker()` | `if (Exchange(ref _refreshPending, 0) == 1) Refresh(); else Invalidate();` —— 定时器消息只走 `Invalidate()`，**不重取快照** |
| 3 | `Refresh()` 内 `Animate(sliding)` | `sliding = IsBusy && ProgressFraction(state) is null`。冻结帧里 `send.Reading` 仍非 null，`ProgressFraction` 返回 0.66 → **`sliding = false`，33 ms 动画定时器根本没启动** |

结果：`_state.Send`（含 `Settled = false`、`Reading = "Concealed [B+U65536].nsp"`）永远是最后一次 `Read()` 时的那一帧快照：

- `TitleList.Result()` 反复渲染同一帧 → **正在发送 66% / 93%** 永不变化；
- `MainWindow.SendingText()` 反复渲染同一帧 → **正在发送 Concealed [B+U65536].nsp** 永不变化。

**为什么只有两行残留**：其余游戏在最后一次 `Changed` 触发时恰好读到 100%（`read >= length`），当帧即显示「已发送」；这两个合并 NSP 只读取了它需要的 66% / 93%，只能依赖 `Settled`，而 `Settled` 需要**时间推进**才会翻转。

---

## 4. 第二处缺陷（同一根因放大）

v15 的 `Settled` 是**全局单一 bool**，基于 `_lastRead`（全进程最后一次读取）：

| 场景 | v15 行为 | 期望 |
|---|---|---|
| 装完 A 后 5 s 开始装 B | A 从「已发送」**回退**成「正在发送 66%」（B 的读事件不断刷新全局时钟） | A 保持「已发送」 |
| A、B、C 交错安装，间隔 < 8 s | 全部行一直显示百分比，直到**所有**游戏装完 8 s 后才一起变「已发送」 | 各自装完各自清 |

行为模拟量化：**v15 在 22 s 观察窗内回退 46 次，v16 为 0 次**。

---

## 5. 修复（`apply_patch_v16.py`，10 处替换 / 3 文件）

| # | 文件 | 改动 |
|---|---|---|
| 1 | `src/Sender.cs` | `SendWatch` 新增 `_lastReadAt: Dictionary<string, DateTime>`，`Read()` 写入每文件最后读取时刻 |
| 2 | `src/Sender.cs` | `Snapshot()` 把 `LastReadAt` 副本与快照时刻 `At` 传出；`SettleSeconds` 常量移到 `SendSnapshot` 作单一真源 |
| 3 | `src/Sender.cs` | `SendSnapshot` 新增 `IsSettled(paths)`：**按本行自己的文件**判定静默；抽出 `Matching()` 让 `Of()` 与 `IsSettled()` 共用同一套路径匹配，避免判定不一致 |
| 4 | `gui/TitleList.cs` | `Result()` 由 `send.Settled` 改为 `send.IsSettled(row.Files)` |
| 5 | `gui/MainWindow.cs` | 新增 **1 s 心跳定时器** `TickSending()`：仅 `Activity == Sending` 时运行，回调置 `_refreshPending = 1` 后 `PostFromWorker()`，让 `Refresh()` 真正重取快照；`OnClosed()` 释放 |
| 6 | `gui/MainWindow.cs` | `Refresh()` 按发送状态启停心跳 |

**为什么是 1 s 而不是复用 33 ms 动画定时器**：33 ms 全量 `Refresh()` 会拖慢 UI；1 s 足够覆盖 8 s 稳定窗口，且 `LogView.SetLines()` 有 `if (lines.Length == _shown) return;` 增量守卫，日志行数不变时**完全不触碰原生 EDIT**，v15 问题 5（详细信息拖窗卡顿）不回归——模拟验证 120 次心跳刷新，**全量文本重设 0 次**。

**锁序**：心跳只在 UI 消息循环里触发 `Refresh()`，不在任何锁内发出跨对象回调，v14 的锁序铁律保持。

---

## 6. 验证

| 层 | 结论 |
|---|---|
| 补丁 | 10/10 锚点命中，落地校验通过，3 文件 CRLF 保持 |
| 编译 | EXE **2/2** 自包含单文件，**0 错 0 警** |
| 综合验证 | `verify_v16.py` **51/51 ALL_OK**（源码 14 · 基线 9 · 补丁/构建 6 · EXE 元数据与 #US 堆 15 · NRO 1 · 模拟 6 · 冒烟 1） |
| 行为模拟 | `simulate_v16.py` **5/5** |
| 二进制取证 | 中间 DLL 元数据含 `IsSettled` / `LastReadAt` / `_lastReadAt` / `SettleSeconds` / `_sendTick` / `TickSending`；#US 堆中文译文未回退（CLI 1237 / GUI 1291 串，较 v15 各 +2 编译噪声，注释未编入） |
| NRO | SHA 与 v15 完全一致，**未改动、未重编** |

### 行为模拟 A/B（`v16_行为模拟.json`）

| 场景 | v15（修复前） | v16（修复后） |
|---|---|---|
| Switch 停止读取后 40 s | `正在发送 66%`、`正在发送 93%`、`正在发送 B_Rabbids[B+U].nsp` | `已发送`、`已发送`、状态栏转为「正在共享 N 个游戏」 |
| 进度清除耗时 | **永不清除** | **8.0 s**（`SettleSeconds`） |
| 状态栏「正在发送 XX.nsp」消失 | **永不消失** | **3.0 s**（`SpeedSeconds`） |
| 交错安装时已装完行回退 | **46 次** | **0 次** |
| 心跳刷新 120 次的全量日志重设 | — | **0 次** |

---

## 7. 产物指纹

| 文件 | 大小 | SHA256 |
|---|---|---|
| `artifacts/dist/hacbrewpacklz-gui_zh.exe` | 35,400,951 B | `03ba3924af9638b9ed91269805b511fec1b69f4bad6775782bf3cefc6fcbf0b4` |
| `artifacts/dist/hacbrewpacklz_zh.exe`（CLI） | 35,316,489 B | `cc19d9468eabe60387b734f34e1f21357d43aafac85373fcc5635d703e4e194c` |
| `artifacts/dist/hacBrewPackLZ_zh.nro`（= v15） | 1,633,335 B | `19b50c5d2e48139d5f47fc10aff4d3b6576c3d949fefb3d03e950cd7bf238a1b` |
| `artifacts/hacBrewPackLZ_zh_v16_20261108.zip` | 整包 14 项 | 见 `SHA256SUMS.txt` |

---

## 8. 实机复测清单

1. 复现原场景：一次发送含**合并 NSP** 的多个游戏，在 NRO 上全部安装完毕 → PC 端两行应在 **约 8 秒内**由 66% / 93% 变为「已发送」；
2. 状态栏的「正在发送 XX.nsp」应在 **约 3 秒内**消失（转为共享/等待提示）；
3. **连续安装**：装完第 1 个后立刻装第 2 个，第 1 个应保持「已发送」，不得回退到百分比；
4. 打开「详细信息」后左右拖动窗口改宽度，应仍然流畅（v15 问题 5 不回归）；
5. 点「停止」后心跳定时器关闭，界面无残留刷新。

---

## 9. 未验证 / 已知限制

- 工作区无真实 Switch / WinUSB / `prod.keys`：USB 传输链路只做到源码 + 中间 DLL 元数据 / #US 堆 + 编译 + 冒烟 + 行为级模拟；**实机长稳仍需上面 5 项复测**。
- NRO 仍为 ISC + curl stub 验证构建，在线更新不可用。
- **线上 GitHub 未改动**：沿用「暂不同步，只要本地」的选择，线上仍是 v12 源码与 v12 Release。
- `SettleSeconds = 8` 与心跳 1 s 为经验值；若实机发现安装中途有 > 8 s 的正常停顿（例如 NRO 在处理大 NCA），可上调该常量，逻辑无需改动。
