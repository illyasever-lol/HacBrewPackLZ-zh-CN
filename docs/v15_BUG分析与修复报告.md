# v15 BUG 分析与修复报告

日期：2026-11-08 · 版本：v15（基于 v14）· 涉及：**EXE（CLI + GUI）与 NRO 双端重编**

本轮处理实测反馈的 7 个问题。与零改动基线 `hacbrewpacklz/` 逐文件 diff 后的归属如下：

| # | 端 | 现象 | 根因（一句话） | 归属 |
|---|---|---|---|---|
| 1 | NRO | exe 显示「已发送」，NRO 报 `Couldn't rename 'sdmc:/switch/hacBrewPackLZ/journal/journal.new' (2001-0106)`，未正常安装 | `saveJournal` 的「写新→删旧→改名」忽略删除结果、不重试，且 libnx 的 rename **不覆盖**已存在目标；共享 FS 会话上一次 IPC 失败即抛异常中断安装 | **上游原版缺陷** |
| 2 | EXE | 两个游戏装完，列表仍留「正在发送 75% / 93%」 | 进度按「累计读取字节 / 文件长度」算，但 NRO 只读取它需要的 NSP 部分，本来就到不了 100%；发送服务常驻使状态永不清除 | **上游原版缺陷** |
| 3 | NRO | 插线后约 30 秒才刷新出发送的文件 | v13 加的 `FailedAgainSeconds = 30` 失败退避；线一直插着时 `usbToPc()` 恒真、没有新上升沿 | **本地 v13 回归** |
| 4 | EXE | 文件夹含多个 NSP 时交替刷「正在发送 A.nsp / B.nsp」 | `SendWatch.Read()` 的 `newFile = path != _file` 对交错读零容忍，每次翻转都播报 | **上游原版缺陷** |
| 5 | EXE | 传输后打开「详细信息」，拖拽改变窗口尺寸明显变慢 | 原生多行 EDIT 未加 `ES_AUTOHSCROLL` → 自动折行；每次 `WM_SIZE` 触发 4000 行全量重新折行 | **上游原版缺陷** |
| 6 | EXE | SDK 版本下方说明文字 | `OptionCatalog.cs:183` 文案冗长且混入 off-by-one 细节 | 文案需求 |
| 7 | EXE | 默认 920×620 文字显示不全 | `MainWindow.cs:33` 初始尺寸偏小 | 需求 |

> 前 5 项中 4 项为上游原版缺陷、1 项为我们在 v13 引入的回归；都不是本轮汉化字符串误伤。
> 判定依据：把工作副本与零改动基线同位置逐行比对，逻辑差异只有本版补丁与历史 v10～v14 的既有修复。

---

## 一、NRO journal 重命名失败（阻断级）

### 1.1 错误码先解码，别按字面猜

`codeOf()` 用的是 `format("%04u-%04u", 2000 + R_MODULE(rc), R_DESCRIPTION(rc))`，`%04u` 是**十进制**。
因此 `2001-0106` = module `1`、description `106`（十进制 `0x6A`），即 Result `0xD401`：

| 字段 | 值 | 含义 |
|---|---|---|
| `R_MODULE(rc)` | 1 | Svc / Kernel 层 |
| `R_DESCRIPTION(rc)` | 106 (`0x6A`) | `InvalidCurrentMemory` |
| Result | `0xD401` | 不是 FS 业务码 |

若为 FS 层错误，前缀应是 `2002`（例如 `2002-0001` = `ResultPathNotFound`、`2002-2001` = `PortSdCardNoDevice`）。
所以 **这不是「目标文件已存在」那类 FS 返回码**，而是共享 `FsFileSystem` 会话上单次 IPC 的偶发失败。
任天堂官方支持页里的 `2001-0106` 是系统级错误展示，与本项目无关，不能拿来套业务原因。

### 1.2 链路与三重缺陷

`switch/core/console.cpp::saveJournal()`（修复前）：

```cpp
sys::makeDirectories(folder);
std::string text = journal.text();
std::string temp = journalTempPath(folder);       // .../journal.new
{ sys::File file(temp, Create); file.write(...); file.flush(); }
sys::commit(temp);
sys::removeFile(journalPath(folder));             // 返回值被忽略，不重试
sys::renameFile(temp, journalPath(folder));       // 失败即抛 ToolError
sys::commit(journalPath(folder));
```

1. `sys::removeFile()` 返回 `bool`，调用方**完全忽略**；`fsFsDeleteFile` 失败不重试。
2. `nx_sys.cpp::renameFile()` 在 SD 卡分支直接 `fsFsRenameFile`。**libnx 的 rename 不覆盖已存在目标**，
   而同一函数在 `!onCard()` 分支用的 POSIX `rename()` **会覆盖** —— 两端语义不一致。
3. `sdCard()` 返回 `fsdevGetDeviceFileSystem("sdmc")`，是**全进程共享**的 `FsFileSystem*`。
   安装线程与后台文件扫描 / USB 工作线程并发向同一会话发 IPC，单次失败即 `throw ToolError`，
   **整个安装被中断**。

一次安装每写入一个 NCA 就 `saveJournal` 一次，高频调用把小概率失败放大。
行为模拟（40 次调用、单次 IPC 失败率 15%）：修复前安装被中断的概率 **99.85%**，修复后 **0**。

### 1.3 决定性证据：同一作者已经在别处修过

`switch/app/app_saved.cpp::writeWholeFile()` 保存 settings 时用的是同样的原子替换，但**有兜底**：

```cpp
sys::removeFile(path);
try {
    sys::renameFile(temp, path);
} catch (const ToolError&) {
    // Whatever got in the way of the name, it's no reason to lose what's being saved.
    writeTo(path);
    sys::removeFile(temp);
}
```

journal 路径漏掉了这一层。与基线 diff 确认 `console.cpp` 相对上游**只有字符串汉化**，逻辑零改动
→ 上游原版缺陷。

### 1.4 修复

移植同一模式并加重试，保持「临时文件 + commit」的断电安全形状：

```cpp
writeTo(temp); sys::commit(temp);
bool swapped = false;
for (int attempt = 0; attempt < 3 && !swapped; attempt++) {
    sys::removeFile(target);
    try { sys::renameFile(temp, target); swapped = true; }
    catch (const ToolError&) { sys::sleepMs(10 * (attempt + 1)); }
}
if (!swapped) {
    try { writeTo(target); } catch (const ToolError&) { /* 保留旧 journal，不中断安装 */ }
    sys::removeFile(temp);
}
sys::commit(target);
```

设计取舍：journal 只用于断电后续做，写不进去最多让下次重做几个 NCA（放置是幂等的），
**绝不该让已经放好的内容被判定为安装失败**。协议、输出目录名 `extracted`、报错文案、
v13 事务锁、v14 目录兜底一律不动。

### 1.5 ELF 反汇编硬证据（纯逻辑改动没有新中文串可查）

`aarch64-none-elf-nm -C` 定位 `hbp::(anonymous namespace)::saveJournal` @ `0x76a60`，
`objdump -d -C` 展开函数体，调用序列实测：

```
makeDirectories → File::File → File::write → File::flush → ~File → commit
  → removeFile → renameFile → commit                       （主路径：删旧+改名）
  → __cxa_begin_catch → sys::sleepMs → __cxa_end_catch    （重试退避分支）
  → File::File → File::write → File::flush → ~File → removeFile(temp)  （覆写兜底分支）
  → __cxa_begin_catch → __cxa_end_catch                  （兜底也失败：吞掉，不再 _Unwind_Resume）
```

`File::write` 出现 **2 次**（临时文件 + 兜底覆写），证明新代码确实链接进 NRO，不是只改了源码。

---

## 二、EXE 安装完成后进度残留

`gui/TitleList.cs::Result()` 用 `sent.read >= sent.length` 判定「已发送」，否则显示
`正在发送 {read*100/length}%`。但 `SendWatch.Read()` 的 `read` 是**累计请求字节数**，
而 NRO 安装 NSP 时只读取它真正需要的段，本来就不会读满 100%；`Sender.Run()` 又常驻到用户
点「停止」，`Session._sendWatch` 与 `Activity == Sending` 一直不清除 → 75% / 93% 永久残留。

**修复**：`SendSnapshot` 增加 `Settled`（距最后一次读取 ≥ 8 s），`TitleList` 把
`read >= length || Settled` 都视为「已发送」。8 秒大于 `SpeedSeconds = 3`，避免解压/写卡时的
短暂停顿被误判为完成。行为模拟：读 750/1000 后静默 8 s，修复前 `正在发送 75%`、修复后 `已发送`。

---

## 三、插线后 30 秒才刷新（v13 回归）

`nx_remote.cpp::watchCable()` 的 `FailedAgainSeconds = 30` 是 v13 为「服务停止期间反复 Cancel URB」
加的退避。用户先点 exe「发送」再插线时，`usbToPc()` 只看 USB 物理配置状态（线插着恒真），
插线后首次 Hello 探针因服务/枚举尚未就绪超时 → `lastFailed = true` → 之后整整 30 秒不再探测。

v13 同时引入了 `CableGuard` 事务级互斥 + 探针 try-guard：探针抢不到锁就**整次跳过**，
根本不会 Cancel URB，长退避已失去原有理由，只剩副作用。

**修复**：`30 → 2` 秒，并保留 `CableGuard` / `QuietSeconds = 2` / `AliveCheckSeconds = 3` 不变。
行为模拟（0.5 s 时间步、服务在 t=2.0 s 启动）：修复前首次成功探测 t=31.5 s
（服务启动后 **29.5 s**，与用户「大约 30 秒」吻合），修复后 t=3.5 s（**1.5 s**）。

**ELF 反汇编证据**：`watchCable` lambda 函数体内可见 `#2.0` / `#10.0` / `#3.0` 浮点常量，
**`#30.0` 已完全消失**，`sleepMs` 循环仍在。

---

## 四、多 NSP 交替刷屏

`SendWatch.Read()` 修复前每次都 `bool newFile = path != _file; _file = path;`，
NRO 交替请求多个输出 NSP 时 `_file` 来回跳，每次翻转都 `Ui.Info($"正在发送 …")`，
既刷日志也让状态栏在两个文件名之间抖动。

**修复（两处）**：
1. **稳定显示**：新 path 与 `_file` 不同时，只有当前文件**已读完**或**已静默 ≥ 2 s** 才切换；
   否则保持 `_file` 不变（交错读只是同一批安装的正常行为）。
2. **播报去重**：`_announced` 集合保证每个输出文件在本次发送中最多播报一次「正在发送」。

行为模拟（A/B 交错 100 次读、每 0.2 s 一次）：修复前 **100 条**「正在发送」，
修复后 **1 条**（另 3 条是每 5 秒的正常进度播报）。日志量下降也直接缓解了问题 5。

---

## 五、打开「详细信息」后拖拽窗口变慢

`gui/Widgets.cs::LogView` 是原生 Win32 多行 EDIT：

```csharp
CreateWindowExW(0, "EDIT", "", WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, ...)
```

缺 `ES_AUTOHSCROLL` → **自动折行**。`AppWindow` 的 `WM_SIZE` 每次都 `Layout()` →
`LogView.Place()` → `SetWindowPos(EDIT)` → 控件按新宽度**重新折行全部行**。
日志上限 `MaxLogLines = 4000`，拖拽一次窗口会有几十次 `WM_SIZE`；
问题 4 的高频播报又让日志更快膨胀 → 卡顿明显，关掉详细信息（EDIT 隐藏）即恢复。

**修复**：
1. EDIT 加 `ES_AUTOHSCROLL | WS_HSCROLL`，关闭自动折行，长行改用横向滚动条查看；
   `Win32.cs` 补 `WS_HSCROLL = 0x00100000`。
2. `MainWindow.Refresh()` 在抽屉隐藏时不再 `_log.SetLines()`；`ToggleDetails()` 打开时补齐。

行为模型：4000 行 × 40 次宽度变化 = 修复前 **160,000 次整行重折**，修复后 **0**。

---

## 六 / 七、文案与默认尺寸

- **SDK 版本说明**（`src/OptionCatalog.cs:183`）整条替换为用户指定文案：
  > 选择沿用源文件则不修改密钥版本，可在下拉框选择需要修改的SDK版本，例：11.0.0、0xA (9.1.0-12.0.3)或000C1100。

  只改这一条 help 文案；`Placeholder`、`CommandHelp`、`FirmwareVersions` 的 off-by-one 逻辑
  （主密钥版本 0 起、NCA 头 1 起）一律不动。CLI 与 GUI 两个程序集的 `#US` 堆都核验了新串编入、旧串清零。
- **默认窗口尺寸**（`gui/MainWindow.cs:33`）：`920, 620` → `1024, 768`；`MinimumSize` 仍是
  `660×460`，小于默认值，无冲突。

---

## 补丁与构建

| 项 | 结果 |
|---|---|
| `apply_patch_v15.py` | **14 处替换 / 8 文件**（先算后写 · `--check` 预检 · `--verify-only` 补跑 · 重放校验） |
| 备份 | `artifacts/patch_backup_v15/` 8 文件，均为修复前版本，可逐字节重放 |
| NRO | 改了 `core/console.cpp` 与 `nx/nx_remote.cpp`；按铁律删全部 **26 个 `main_*.o`** 全量重编，40 对象 **0 错 0 警** |
| EXE | 2/2 自包含单文件（CLI + GUI），**0 错 0 警**，每个输出目录仅 1 个 exe |
| 误伤检查 | v13 `CableGuard`、v14 死锁修复与 `isDirectory` 兜底、v12 措辞、v10 ASCII 路径均通过断言 |

## 验证结论（105/105 ALL_OK）

| 组 | 通过 | 关键证据 |
|---|---|---|
| 源码 | 26/26 | 14 处补丁逐条落地断言 |
| 基线 | 15/15 | 上游 `saveJournal` 无 try/catch、上游 `writeWholeFile` 有兜底、上游 EDIT 无 `ES_AUTOHSCROLL` |
| 补丁 | 5/5 | 8 份修复前备份 + 机读修复资产（7 问题 / 8 条 rebase 不变量） |
| NRO | 7/7 | `NRO0` 头、既有中文 C 串、相对 v14 SHA 已变化 |
| 符号/反汇编 | 18/18 | `saveJournal` 全调用链、`File::write`×2、`watchCable` 的 `#30.0` 消失 |
| EXE | 27/27 | 2/2 发布、CLI+GUI 双 `#US` 堆新 SDK 文案、元数据含 `Settled` / `_announced` |
| 构建 | 6/6 | NRO `build.log` 0 错 0 警、两个改动 .cpp 确实重编 |
| 行为模拟 | 8/8 | 5/5 场景 + v13 / v14 历史模拟未回退 |
| 冒烟 | 2/2 | 屏蔽系统 .NET 后 CLI `--help` 正常且含中文 |

`simulate_v15.py` 的 5 个场景：journal 中断概率 99.85%→0、进度 75%→已发送、
探测延迟 29.5 s→1.5 s、播报 100→1 条、全量重折 160000→0。

## 产物指纹

| 文件 | 大小 | SHA256 |
|---|---|---|
| `hacBrewPackLZ_zh.nro` | 1,633,335 B | `19b50c5d2e48139d5f47fc10aff4d3b6576c3d949fefb3d03e950cd7bf238a1b` |
| `hacbrewpacklz-gui_zh.exe` | 35,399,533 B | `607b7253e0c58ee025030acaa2f3369528524c4900cee177bdb0590d12ccf950` |
| `hacbrewpacklz_zh.exe`（CLI，本地保留不发布） | 35,315,501 B | `d3b0b944ec7786f5333fbc764aad19d5d49fa1d68c5c09ae8be9a7a1f70483d3` |

## 请实机复测（对应 7 个问题）

1. 重新发送并完整安装 `Concealed`（本体 + 更新包合并），确认 NRO **不再报 journal 重命名错误**、
   游戏可正常启动；建议连续装多个游戏以覆盖共享 FS 会话并发。
2. 两个游戏装完后，exe 列表应从「正在发送 NN%」变为「**已发送**」，不再残留百分比。
3. 点「发送」后插 USB 线，NRO 应在 **数秒内**刷新出文件（原来约 30 秒）。
4. 拖入含多个 NSP 的文件夹，状态栏稳定显示当前文件，「正在发送 …」不再交替刷屏。
5. 传输后打开「详细信息」，反复左右拖动改变窗口宽度，确认**不再卡顿**；长日志行可横向滚动查看。
6. 设置页 SDK 版本下方显示新说明文案。
7. 冷启动主界面为 **1024×768**，文字不再被截断。

## 明确未验证

- 工作区无真实 Switch / WinUSB / `prod.keys`：journal 重命名在共享 FS 会话下的真机失败率、
  USB 服务发现实际延迟、安装完成后的进度清除时机、EDIT 拖拽手感，均只做到源码 + ELF 反汇编 +
  `#US` 堆 + 行为模拟 + 编译级，需按上表实机确认。
- NRO 仍为 ISC + curl stub 验证构建，**在线更新不可用**、NTFS/ext4 可能无法读取。
- 行为模拟中的 IPC 失败率 15%、日志 4000 行、40 次宽度变化是**说明机制用的模型参数**，
  不是现场实测值。
- 线上 GitHub 沿用此前「暂不同步，只要本地」的选择，本轮未改动；`latest` 仍为 v12。
