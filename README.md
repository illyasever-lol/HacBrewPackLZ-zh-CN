# HacBrewPackLZ 汉化版

> 简体中文界面 · 功能增强 · 免安装 .NET 8 的 Windows 单文件版 · Switch NRO 全量汉化
>
> 上游项目：[hacBrewPackLZ 1.4.3](https://gitlab.com/ismaellozanoperezserrano/hacbrewpacklz)（GNU GPL-2.0）
> 汉化与增强：[illyasever](https://space.bilibili.com/36010)
> 当前版本：**v11**（2026-11-08）

把 Nintendo Switch 游戏**变小**的工具：把 NSP / NSZ / XCI / XCZ 重建为数据经过 LZ4 压缩的 NSP。
LZ4 是主机固件原生支持的 NCA 压缩层，压缩后照常运行、无需解包，也绝不改动原始文件。

本仓库是上游 1.4.3 的**简体中文汉化与功能增强版**，包含完整可编译的修改后源码、
原始上游源码快照（ZIP）、界面截图与逐版更新日志。上游仓库保持零改动，
所有修改均以精确字节补丁方式应用在本仓库源码上，编译产物与密钥一律不入库。

---

## 下载（Releases）

编译好的二进制在仓库 **Releases** 页发布，不放在 git 仓库里：

| 文件 | 说明 |
| --- | --- |
| `hacBrewPackLZ_zh.nro` | Switch 端程序（放到 SD 卡 `/switch/` 下用 Homebrew Menu 启动） |
| `hacbrewpacklz-gui_zh.exe` | Windows 图形界面，免安装 .NET 8 |
| `SHA256SUMS.txt` | 上述文件的校验和 |

图形界面版双击即用；`prod.keys` 请自行准备，放在 exe 同目录或用 `-k` 指定。
**本仓库与 Release 均不包含任何 Nintendo 密钥或游戏文件。**

---

## 界面截图

### 主界面

![主界面](screenshots/main-window.png)

拖入文件即可开始；支持 NSP、NSZ、XCI、XCZ，以及含 `exefs` / `romfs` / `control` 的游戏文件夹。

### 设置 · 输出

![输出设置](screenshots/settings-output.png)

「是否整合 UPD+DLC+本体 为单一文件」是本汉化版新增的合并开关。

### 设置 · 版本

![版本设置](screenshots/settings-version.png)

游戏版本（SDK / 密钥世代）与所需系统版本支持下拉选择，也可自定义填入可读版本号或十六进制值。

### 设置 · 压缩

![压缩设置](screenshots/settings-window.png)

压缩级别、块大小、线程数等原有选项也已完整汉化。

### 关于

![关于窗口](screenshots/about-window.png)

关于窗口完整汉化，并附汉化作者与 B 站空间链接。

> 以上为 Windows 图形界面截图。Switch 端 NRO 的界面同样已全量汉化，实机画面请用 Release 中的
> `hacBrewPackLZ_zh.nro` 在主机上查看。

---

## 软件功能

| 功能 | 说明 |
| --- | --- |
| LZ4 重打包 | 把 NSP / NSZ / XCI / XCZ 重建为 LZ4 压缩的 NSP，主机原生读取，无需解包 |
| 体积预估 | 「分析」逐个列出压缩前 / 后体积与节省百分比，只勾选值得处理的游戏 |
| 合并本体与更新 | 同一 Title ID 的本体 + 更新包（+ DLC）合成同一个文件 |
| 解包 | 把游戏导出为文件夹（`exefs` / `romfs` / `logo` / `control` / `htmldoc` / `legalinfo`） |
| 回读校验 | 生成后回读 NSP 并校验哈希、块表，可用 `--verify` 复核 |
| 发送到 Switch | 通过局域网或 USB 把文件共享给 Switch 端（需配合本仓库 NRO） |
| 原文件安全 | 原始文件永不修改，结果另存为 `[LZ4].nsp` |
| 多端可用 | Windows 图形界面 / 命令行 / TUI，以及 Switch 本体运行的 NRO |

命令行示例：

```bat
hacbrewpacklz_zh.exe analyze -k prod.keys "D:\My Games"
hacbrewpacklz_zh.exe convert -k prod.keys "D:\My Games" --nspdir "D:\Compressed"
hacbrewpacklz_zh.exe extract -k prod.keys "Game.nsp" --outdir "D:\Extracted"
hacbrewpacklz_zh.exe --help
```

---

## 本汉化版新增功能

汉化覆盖 GUI、CLI、TUI、运行时提示与 Switch NRO（约 1200 条词条）。在完整汉化之外，
还做了以下功能增强与缺陷修复：

### 功能增强

1. **本体 + 更新包 + DLC 合并为单一 NSP**
   参照 NSC_BUILDER，放开 `NspWriter` 只预留一个 meta NCA 的限制，按 Application ID 把 DLC
   折叠进本体任务，一个 NSP 内写入多份 CNMT。开关位于「设置 → 输出」。
2. **DLC 信息进入文件名与列表摘要**
   输出名标注 DLC 数量与版本，例如
   `Pokémon Sword [0100ABF008968000][v458752][v1.3.2][2DLC][LZ4].nsp`；
   列表行显示「本体 vX + 更新包 vY + N 个 DLC，重新打包为一个文件」。
3. **SDK / 密钥世代与所需系统版本可选**
   下拉可选「沿用源文件」或 `0x3` ~ `0x11` 各代，并同步写入相符的密钥世代；
   系统版本支持 `12.0.0` / `20.0.0` 等可读版本号或 `000C1100` 十六进制值，也可自定义填入。
4. **X 按钮清除整个项目**
   按 Title ID 从 `TitleSet` 取全部输入一次清理（本体、UPD、DLC、游戏文件夹行），
   不再留下孤立输入导致重读后「复活」。
5. **关于窗口汉化 + 可点击链接**
   加入「汉化作者: illyasever」与 B 站空间链接，并修复构造期 `WM_SIZE` 触发的空引用崩溃。
6. **NRO 端全量汉化与中文换行**
   Switch 端欢迎页、设置说明、弹窗、按钮、菜单、关于框全部汉化；
   `wrapText()` 改为按 UTF-8 码点逐字符断行，中文长文本不再横向溢出屏幕。
7. **USB 发送服务检测（v11 新增）**
   NRO 每 3 秒探测一次 PC 发送服务，PC 点「停止发送」后约 3～5 秒自动刷新列表、
   文件图标消失；重新发送约 3～10 秒刷新出新内容，**无需拔插 USB 数据线**。
   心跳仅在 USB 通道空闲 2 秒后发出，避免干扰正在进行的 NSP 安装。

### 缺陷修复

- **SDK 世代 off-by-one**：下拉选 `0xD` 却生成 `0xC`（加密密钥错代）。NCA 头部是 1 起值，
  读取方一律减 1 再查密钥，现已正确写入 `revision + 1`，并用 A/B 探针验证。
- **WinUSB 导出名误伤**：导出名 `Free` 曾被误译导致「Entry point was not found.」、
  USB 完全无法连接；已复原并加入 6 个驱动导出的预检与中文诊断。
- **解包目录名**：`control` / `data` 改回英文以与生态工具一致，并兼容识别旧版中文「控制」目录。
- **解包输出路径 ASCII 化**：默认目录 `extracted`，中文 / 日文游戏标题折叠为 `_`，
  旧设置文件里残留的中文目录自动回退 —— 避免 Switch 读不了中文路径。
- **进度显示双端同步**：进度文字的产出源与中文前缀查找表保持一致，
  阶段统计、耗时估算与步骤高亮不再停滞。

### 交付形态

- **免安装 .NET 8 的自包含单文件 EXE**（Windows x64，每个约 35 MB），无需安装任何运行时；
- **完整中文的 Switch NRO**，与 PC 端同一版本迭代；
- 每版附 ZIP、SHA256 校验和、逐项对照表与验证报告。

---

## 相对原始版本的优化

上游基线为 GitLab 1.4.3。下表列出本汉化版相对原版的实际改动与验证依据：

| 维度 | 上游 1.4.3 原版 | 本汉化版 v11 | 依据 |
| --- | --- | --- | --- |
| 界面语言 | 全英文（GUI / CLI / TUI / NRO） | 简体中文全端覆盖，约 1200 条词条 | v1、v2、v8、v11 |
| 运行依赖 | 需自行安装 .NET 8 运行时 | 自包含单文件 exe，免安装运行时（win-x64，约 35 MB/个） | v6 |
| Switch NRO | 英文界面，中文长文本会横向溢出 | 全量汉化 + 按 UTF-8 码点断行（697px 单行 → 595px 两行） | v8 |
| SDK / 密钥世代 | 下拉选 `0xD` 实际写入 `0xC`，密钥错代 | 写入 `revision + 1`，A/B 探针验证头部与外部显示一致 | v6 |
| 合并本体 / 更新 / DLC | 分别处理 | 可合并为单一 NSP，内含多份 CNMT | v3 |
| X 按钮删除 | 游戏文件夹行删不掉、孤立输入会「复活」 | 按 Title ID 一次清理全部输入与缓存 | v5、v7 |
| 解包目录名 | `control` / `data` 英文 | 保持英文，另兼容旧中文「控制」目录 | v7 |
| 解包输出路径 | 英文默认目录 | 强制全 ASCII；中文标题折叠为 `_`，旧设置自动回退 | v10 |
| USB 驱动连接 | 依赖 WinUSB / libusbK | 6 个驱动导出预检 + 中文错误诊断 | v9 |
| PC 停止发送后的列表 | 上游原版同样不刷新，必须拔插 USB 线 | 3 秒服务心跳 + 服务变化即自动刷新（3～10 秒） | v11 |
| 压缩进度文字 | 英文阶段文案 | 中文，且产出源与查找表双端同步，进度/高亮正常 | v9 |
| 关于窗口 | 无作者信息 | 汉化作者 + B 站链接，修构造期空引用崩溃 | v3、v3.1 |
| 发送流程提示 | 英文 | 共享数量、局域网/防火墙、Zadig/DBI 驱动说明全中文 | v11 |
| 版本参数输入 | 手写命令行参数 | 下拉可选 + 自定义可读版本号 / 十六进制值 | v3、v4、v5 |
| 发布形态 | 仅源码 | ZIP + 2 个免运行时 EXE + 中文 NRO + SHA256 校验 + Release | v6～v11 |

> 说明：v11 的「PC 停止发送后列表不刷新」经与零改动基线逐文件 diff 确认是**上游原版就有的行为**，
> 不是汉化引入的缺陷；本汉化版额外增加了发送服务检测，属于主动增强。
> 逐版改动与验证结论见 [CHANGELOG.md](CHANGELOG.md)。

---

## 版本记录

| 版本 | 主要内容 | 验证 |
| --- | --- | --- |
| v1～v5 | 全量汉化、合并 NSP、SDK/系统版本下拉、DLC 文件名、X 按钮、关于窗口 | 编译 + 参数链路 + GUI 实测 |
| v6 | 两处漏译、SDK 世代 off-by-one 修复、**免安装 .NET 8** | A/B 探针，12/12 断言 |
| v7 | 游戏文件夹行删除、解包目录名回英文并兼容旧目录 | 行为级反射 A-B，13/13 断言 |
| v8 | NRO 全量汉化、中文换行、作者行、首次重编 NRO | 32/32 新中文、30 条旧英文 0 残留 |
| v9 | NRO 四处漏译、`Free` 导出名复原、进度映射双端同步 | 60/60 断言 + 系统 DLL 导出表 6/6 |
| v10 | 解包输出路径 ASCII 化、旧设置兜底、输出名折叠 | 34/34（含 3010 纯 ASCII 零回归用例） |
| v11 | EXE 发送流程汉化、USB 服务心跳、服务变化自动刷新 | 94/94 + 状态机行为模拟 6/6 |

完整明细见 [CHANGELOG.md](CHANGELOG.md)。

---

## 免安装运行

1. 从 Releases 下载 `hacbrewpacklz-gui_zh.exe`（图形界面）与 `hacBrewPackLZ_zh.nro`（Switch 端）；
2. 把 `prod.keys` 放在 exe 同目录，或用 `-k` 指定；
3. 拖入游戏文件或文件夹，点「分析」查看预估，再点「压缩」；
4. 输出文件写在源文件旁，文件名以 `[LZ4].nsp` 结尾；
5. 需要传到主机时，用「发送到 Switch」，并配合 Switch 端 NRO（PC 需 WinUSB / libusbK 驱动，
   可用 Zadig 安装，与 DBI 使用同一个驱动）。

历史 v5 及更早版本是 framework-dependent 构建，需要自行安装 .NET 8，且含 SDK 版本缺陷，
**请勿用于正式打包**。

---

## 从源码编译

环境：.NET 8 SDK（见 `global.json`），Windows x64。

```bat
:: 需 .NET 8 运行时的常规构建
dotnet build hacbrewpacklz.csproj -c Release
dotnet build gui/hacbrewpacklz-gui.csproj -c Release

:: 免运行时自包含单文件
dotnet publish hacbrewpacklz.csproj -c Release -r win-x64 --self-contained true ^
  -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true
dotnet publish gui/hacbrewpacklz-gui.csproj -c Release -r win-x64 --self-contained true ^
  -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true
```

若构建环境无法访问 NuGet（例如受限沙箱），可离线装配官方
`Microsoft.NETCore.App.Runtime.win-x64` 运行时包后用 `--no-restore` 发布。
Switch 端 NRO 需要 devkitPro / libnx，编译方式见 `switch/README.md`。
上游源码逐行对照见 `upstream/hacbrewpacklz-upstream-source.zip`。

---

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `src/` | C# 核心：NCA / NSP / CNMT / LZ4 / 选项 / 会话 / 校验等（已汉化并增强） |
| `gui/` | Win32 自绘图形界面（含新增 `AboutWindow.cs`） |
| `switch/` | Switch 端 NRO 源码（C++，全量汉化 + 路径 ASCII 化 + USB 服务心跳） |
| `docs/`、`tests/`、`selftest/`、`tools/`、`assets/` | 上游文档、测试、工具与资源 |
| `screenshots/` | 本汉化版界面截图 |
| `upstream/hacbrewpacklz-upstream-source.zip` | **原始上游源码快照**（打包为 ZIP，已排除 `.git` 与二进制） |
| `upstream/README-upstream.md` | 上游英文 README 原文 |
| `CHANGELOG.md` | v1 → v11 逐版改动记录 |
| `LICENSE` | GNU GPL-2.0 原文 |

---

## 与上游的关系

- **上游仓库**：<https://gitlab.com/ismaellozanoperezserrano/hacbrewpacklz>，版本 1.4.3，许可证 GPL-2.0。
- 本仓库只做**本地化与功能增强**，不改动上游任何文件；`upstream/` 内的 ZIP 是原版源码原样快照，便于逐行对照。
- 汉化采用「基线副本 + 精确字节补丁」流程：每版一份独立补丁脚本，编译前逐字节重放校验，
  编译后做 `#US` / `#Strings` 堆二进制核验、A-B 行为级反射验证、状态机行为模拟与无运行时实测。
- 依据 GPL-2.0，本仓库同样以 GPL-2.0 发布，修改后源码一并公开。

---

## 已知限制

- 免运行时 EXE 仅提供 **Windows x64**；Linux / macOS 需按上游方式自行编译。
- 开发环境没有 `prod.keys`、真实 NSP 样本与真实 Switch + WinUSB 设备：
  合并打包、密钥世代写入、USB 传输安装、USB 端点真机时序（尤其 v11「通道空闲 2 秒」门槛）
  仅完成编译 / 源码断言 / 二进制核验 / 行为模拟验证，**建议实机打包后运行 `--verify` 复核**。
- Switch 端 NRO 为 ISC + curl stub 验证构建（缺 ntfs-3g / lwext4 / curl 三个 portlibs），
  在线更新不可用，NTFS / ext4 移动硬盘可能无法读取。
- v10 之前版本生成的中文输出目录 `sdmc:/hacBrewPackLZ/已解包/` 不会自动迁移，
  请手动删除或把内容移入 `extracted/` 后重新解包。
- 本项目不包含、也不提供任何 Nintendo 密钥或游戏文件。

---

## 致谢

- 上游作者与 [hacBrewPackLZ](https://gitlab.com/ismaellozanoperezserrano/hacbrewpacklz) 项目，GPL-2.0。
- 汉化作者：**illyasever** · <https://space.bilibili.com/36010>
