# 汉化版更新日志

上游基线：hacBrewPackLZ 1.4.3（<https://gitlab.com/ismaellozanoperezserrano/hacbrewpacklz>），GPL-2.0。
当前版本：**v16**（2026-11-09）。

| 版本 | 内容 | 验证结论 |
| --- | --- | --- |
| v1 | 按定稿译文全量导入并编译 4 个 EXE + 2 个 NRO，出首个干净发布包 | 4/4 编译 0 错 0 警 |
| v2 | 19 项截图驱动的漏译与措辞修订（43 处替换 / 10 文件） | 旧英文串 0 残留 |
| v3 | 本体+UPD+DLC 合并单一 NSP、SDK 版本下拉、所需系统版本下拉、关于窗口加 B 站链接；新增 `gui/AboutWindow.cs`（26 处 / 10 文件） | 26 处补丁重放通过 |
| v3.1 | 修复点击「关于」时空引用崩溃并自动退出（`Layout` 增加控件与尺寸空值守卫） | A/B 对照验证通过 |
| v4 | 漏译补齐、SDK `0xFF (Unknown)` 映射校正（新增 `src/FirmwareVersions.cs`）、DLC 文件名标注数量与版本（35 处 / 12 文件） | 参数链路 6/6 |
| v5 | 孤立更新包提示汉化、DLC 合并摘要行、SDK/系统版本自定义填入、X 按钮清除整个项目（11 处 / 4 文件） | GUI 端到端输入实测通过 |
| v6 | 两处运行时提示漏译；修复选 `0xD` 生成 `0xC` 的密钥世代 off-by-one；改为**免安装 .NET 8** 的自包含单文件 EXE | A/B 探针复现并修复；12/12 断言；无运行时实测通过 |
| v7 | 修复游戏文件夹行点 X 删不掉；解包「控制」目录改回英文 `control` 并同步打包读取，兼容旧中文目录（14 处 / 8 文件，C# 10 + Switch C++ 4） | 反射调用真实 `Session.Remove` 行为级 A/B 通过；13/13 断言 |
| v8 | NRO（Switch 端）30 处截图驱动的全量汉化；`USB 盘`→`USB 可移动设备`；关于框新增作者行；`wrapText()` 按 UTF-8 码点断行修复中文溢出；**首次重编 NRO**（115+9 处 / 11 文件） | 32/32 新中文、30 条旧英文 0 残留、USB 8/0、断行 697px→595px 两行；40 对象 0 错 0 警 |
| v9 | NRO 四处漏译（`Compress` / `Compressing the game data` / 压缩用时 / 压缩后）；修复 USB 发送报 `Entry point was not found.`（导出名 `Free` 被误译）；进度文字产出源与中文前缀表双端同步；补齐 `Downloads` 路径与 5 处漏译（21 处 / 7 文件，EXE 与 NRO 均重编） | 60/60 断言；系统 `winusb.dll` 导出表 6/6 存在、误译名不存在；7 条旧英文 C 串 0 残留 |
| v10 | Switch 不认中文路径：默认解包目录复原 `extracted`；`asciiPathOnly()` 对旧 `settings.txt` 残留中文目录回退；`forOutputName()` 把 UTF-8 非 ASCII 折叠为 `_`（仅 NRO 端） | 34/34（源码 14 + 3010 纯 ASCII 零回归用例 + 二进制 13 + 编译）；含中文且含 `/` 的 C 串 0 条 |
| v11 | EXE 发送流程 11 处汉化（共享数量、局域网/防火墙、Zadig/DBI 驱动说明）；NRO 新增 USB 发送服务检测：3 s Hello 心跳 + 通道空闲 2 s 门槛 + 服务变化即自动刷新列表（18 处 / 3 文件，EXE 与 NRO 同步重编） | 94/94（源码 39 + 行为模拟 6 + NRO 二进制 17 + EXE/#US 堆 26 + 冒烟 6）；状态机模拟 6/6；停止→清除 3～5 s、恢复→新内容 3～10 s |
| v12 | 两处文案修订：EXE 估算行 `game data compresses to` → `游戏数据可压缩至`；NRO 解包收尾「位于 3 s」→「用时 3 s」（`in %s` 表耗时非位置）（2 处 / 2 文件，EXE 与 NRO 同步重编） | 40/40（源码 10 + 补丁重放 4 + NRO 二进制 10 + EXE/#US 堆 12 + 构建/冒烟 4）；自包含 EXE 2/2、NRO 40 对象 0 错 0 警 |
| v13 | **USB 长传输崩溃修复**：`2162-0002 (0x4a2)` / `usb` sysmodule。根因是 v11 心跳探针与安装事务并发 `PostBufferAsync` 同一端点与同一块 1 MiB 缓冲；新增 `cableBusy` + `CableGuard` 事务级互斥、探针 try-guard、URB 超时排空、失败退避（9 处 / 3 文件，**仅 NRO 重编**） | 51/51；40 对象全量重编 0 错 0 警；行为模拟 4 GB 并发 post 3→0 |
| v14 | **EXE 发送死锁 + NRO 输出目录误判 + 漏译**：`SendWatch.Read()` 跨对象回调不得持自身锁发出（锁序反转）；`isDirectory()` 单次 FS 调用失败重试并回落 open；`makeDirectories()` 先判存在；补齐 `Choose:`/`Add N files`/`On-Off`/`N of M` 等漏译 | 77/77（源码 30 + 基线 15 + ELF/二进制 12 + 构建 8 + 模拟 8 + 冒烟 4）；EXE + NRO 双端重编 |
| v15 | **实机反馈 7 项**：`saveJournal` 删+改名重试 3 次并覆写兜底（journal 失败不再中断安装）；部分读取即视为已发送；服务发现退避 30 s → 2 s；多 NSP 每文件每次发送只播报一次；多行 EDIT 加 `ES_AUTOHSCROLL` 消除拖窗重折行；SDK 帮助文案按指定文本；默认窗口 1024×768 | 105/105；EXE 2/2 + NRO 40 对象 0 错 0 警；行为模拟 5/5（journal 中断 99.85%→0） |
| v16 | **装完后进度残留与状态栏冻结**：`Settled` 是取快照瞬间的时间函数，而 `Refresh()` 纯事件驱动，Switch 停止读取后快照被永久冻结；新增仅发送期运行的 1 s 心跳推进时间，`IsSettled(paths)` 按本行自己的文件判定，交错安装不回退 | 51/51；仅 EXE 2/2 重编 0 错 0 警；行为模拟 5/5（8 s 清进度、3 s 清状态栏、回退 46→0）；NRO 未改动 |

每版均执行：补丁逐字节重放（`--check` 预检）→ 编译（2 个自包含 exe；改了 Switch 端再加 NRO）
→ `#US` / `#Strings` 堆或 NRO 二进制 C 串核验 → 运行与参数链路 / 行为级模拟验证
→ 发布包 + SHA256SUMS → 交付页 / overview / 对照表 / 验证报告同步。

**验证口径说明**：源码为 CRLF，多行断言需归一化；NRO 二进制中 `"已解包\0"` 的唯一命中
是 UI 结果行 `"%s 已解包"` 的尾部子串，不是路径字面量；`folderFor()` 用三元表达式赋值，
断言以「与基线逐字节一致且无中文」为准。

**明确未验证**：无 `prod.keys` / 真实 NSP / 真实 Switch + WinUSB 设备；USB 端点真机时序、
NRO 界面观感、NSP 传输安装需实机确认。NRO 为 ISC + curl stub 构建，在线更新不可用。

**发布记录**：公开仓库 `illyasever-lol/HacBrewPackLZ-zh-CN`；源码 + 原版上游 ZIP + 界面截图
随版本同步；`hacBrewPackLZ_zh.nro` 与 `hacbrewpacklz-gui_zh.exe` 等二进制走 Releases。
v13～v16 四版累积改动于 v16 一次性同步，v11 / v12 Release 原样保留。
