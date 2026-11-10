# WinTools

Windows 多显示器工具：**跨屏拼接壁纸** + **显示器断电/重连后把窗口摆回原来的位置**。

单文件 exe，静态 CRT，零运行时依赖，托盘常驻。双击打开设置面板，不需要安装、不写 Program Files、不联网。

| | |
|---|---|
| 适用系统 | Windows 10 1809+ / Windows 11，x64 |
| 当前版本 | v0.1.0 |
| 产物 | 884,224 字节，只导入 7 个系统 DLL |
| 实现 | C++23 / Win32 / WIC / 自绘 GDI，无第三方库 |
| 测试 | 102 个单测，debug / release 双绿 |

![WinTools 设置面板](docs/panel.png)

> 实机面板（1025×934）。截图使用中性示例数据：3 台显示器、2 个组合、7 个示例窗口；
> 壁纸是本项目自制的示意图，不含真实窗口标题、文件路径或个人内容。

---

## 一、功能

### 1.1 功能一览

| 功能 | 说明 |
|---|---|
| 跨屏拼接壁纸 | 一张图铺满所有显示器，接缝按各显示器真实矩形对齐 |
| **组合** | 每台屏属于一个组合，每组一张图、一套取景、一组成员屏；两两拼接与四屏独立是同一个模型 |
| 取景调整 | 布置图上拖动平移、滚轮以光标为锚点缩放（1.0~8.0）、双击或 `0` 复位 |
| 壁纸预览 | 预览走与真正贴图同一套 cover 裁切代码，每屏每组的画面就是贴出来的画面 |
| 记住窗口布局 | 按显示器记录位置，含最大化、贴靠、分割比例；显示器原点变了窗口跟着平移 |
| 自动恢复 | 显示器回来/拔掉后，2 秒去抖把窗口摆回去；只动需要动的窗口 |
| 逐窗口排除 | 行首勾选框决定「记不记得住」，取消勾选不删行 |
| 按屏筛选 | 布局区的纯数字 chip 可多选，一个都不选 = 全部 |
| 主题 | 浅色 / 深色 / 跟随系统，标题栏 `◐` 循环，选择落盘 |
| 状态记录 | 页脚保留最近三条结果，失败不会被下一次成功直接抹掉 |
| 全键盘操作 | Tab 焦点环、1~9 切组、`+ - 0` 缩放复位、方向键平移、Ctrl+Z 撤销勾选、Esc 关弹层 |
| 托盘与自启 | 托盘菜单含「更换壁纸 / 还原壁纸 / 刷新布局 / 恢复布局 / 退出」，可设开机自启 |

### 1.2 组合：这台屏和谁共用一张图

组合是壁纸配置的唯一分组粒度。一台屏属于且仅属于一个组合，每个组合有**一张图、一套取景、一组成员屏**。

- 一张图铺满四台屏 = 一个组含四台屏；两两拼接 = 两个组各两台；四屏独立 = 四个单屏组。
- **组合名由成员屏号派生**（`1+2`、`3+4`），不存编号。
- **改组只有一个入口**：屏内左上角那个蓝底白字的编号标签。点它选「加入 X」或「单独成组」；
  反向可以点当前组合 chip 上的 ＋ 把别的屏拉进来。两个方向是同一个原子操作。
- 点屏身是拖壁纸，点编号才出菜单。移动光标只在有壁纸的屏内出现。
- **分组只由屏外侧的一条轮廓线表达**，每组一条。轮廓按成员屏的边描出真实并集形状，
  L 形排列不会框进缺角；线的内侧固定离屏 2px，选中时变粗只向外长，
  两个相邻组之间始终留出图片，不会拼成一条粗带。
- 组合被拆空自动消失，其余重排序号；成员变了的组取景复位并标「● 未应用」。

把屏 3 并入 `1+2`：轮廓随成员自动融合，三块屏改用同一张壁纸。

![组合改组演示](docs/panel-demo.gif)

### 1.3 布置图与取景

顶部那张图按当前在线显示器的**真实相对位置**绘制：负坐标、非对齐边、中间的空洞都照原样，
比例尺由外接矩形等比缩到卡片里。网格底 + 白色空隙 = 桌面没覆盖到的地方。

- 图上不叠任何半透明色块、色条或内描边，壁纸颜色即原色。
- **拖动**：图跟着光标走，作用范围是当前组合；拖别组的屏会先切到那一组。
- **滚轮**：以光标下的像素为锚点缩放，1.0~8.0 倍，一格 10%，精密触控板的小 delta 也按小数格算。
- **双击屏身 / `0`**：取景复位。拖动过程中不解码、不落盘、不重贴。
- 标题行右侧的「● 未应用」表示取景改过但还没贴；点「应用壁纸」才生成切片。

### 1.4 窗口布局

用户要做的只有三件事：**换壁纸** → **点「获取布局」** → **让「自动恢复」一直开着**。
面板上对应两个主按钮、一个开关和一个非主按钮「恢复布局」。

- 列表每行：勾选框 / 窗口标题 / 进程名 / `屏 1 · 最大化` 这样的位置标签。
  标题是主信息，进程名只在重名时用于区分；点标题不会改勾选。
- 位置标签**只读**，显示捕获到的结果：`最大化` / `左半屏` / `右侧 33%` / `铺满`，
  悬浮窗口显示尺寸。窗口当时怎么摆就按那个摆回去。
- 再点「刷新」重新捕获位置，同时继承已取消的勾选。
- `apply` 只移动已经在运行的窗口，不启动任何程序。
- 托盘菜单、面板按钮、命令行走同一批动作函数。

### 1.5 面板与交互

四段式单栏：**标题栏**（齿轮 / 主题 / 帮助 / 隐藏到托盘）→ **壁纸区**（组合 chip、提示行、
布置图）→ **布局区**（屏号筛选、开关、两个按钮、窗口列表）→ **页脚**（状态与记录、退出）。

两个区块的标题行结构完全一致：标题 + 对象选择器 + 弹性空白 + 一个主按钮 + 一个次按钮；
全面板共用一套按钮尺寸、一套行高。宽 820 设计单位，高度随内容和屏幕在 557~907 之间。

| 键 | 作用 |
|---|---|
| `Tab` / `Shift+Tab` | 在控件之间移动，双圈焦点环 |
| `Enter` / `Space` | 打开分组菜单 / 切换勾选 |
| `1` ~ `9` | 切换当前组合 |
| `+` `−` `0` | 缩放 / 复位当前组合 |
| 方向键（Shift 加速） | 平移当前组合 |
| `Ctrl+Z` | 撤销上一次勾选 |
| `Esc` | 关弹层；再按隐藏到托盘 |

### 1.6 命令行

不带参数 = 常驻托盘并打开面板；`--tray` = 只进托盘（开机启动用）。

```bat
WinTools.exe monitors                  :: 列显示器和虚拟桌面矩形
WinTools.exe windows                   :: 列可管理的窗口

WinTools.exe span "D:\pics\wide.png"   :: 一张图拼接铺满
WinTools.exe span "D:\pics\wide.png" --dry-run
WinTools.exe dump "D:\pics\wide.png"   :: 只算切片参数，不写文件不改壁纸
WinTools.exe restore                   :: 还原成换之前的壁纸

WinTools.exe capture                   :: 记住当前窗口布局
WinTools.exe show                      :: 看已保存的布局
WinTools.exe apply                     :: 把窗口摆回去
```

---

## 二、架构

### 2.1 分层

```
main.cpp                      wWinMain，借用父控制台后分发子命令
    │
    ├── app/                  进程边界层
    │   ├── cli / commands    参数解析与子命令
    │   ├── actions           用户动作（拼壁纸 / 还原 / 存布局 / 摆布局 / 改组）
    │   ├── service           常驻服务：托盘、显示变化监听、状态机、持久化
    │   ├── panel             自绘设置面板（无子控件）
    │   └── tray / settings / startup
    │
    ├── core/                 纯业务逻辑，不依赖 UI
    │   ├── group             组合模型与 groups.tsv
    │   ├── layout_math / composer / desktop_wallpaper / state / slice
    │   ├── window_enum / window_layout / layout_store
    │   └── display_watcher
    │
    ├── platform/win/         COM RAII、WIC 工厂
    └── util/                 path / text / log
```

三条约束贯穿全项目：

- **一个动作只有一份实现**：托盘菜单、面板按钮、命令行都调 `actions.h`，`Service` 是唯一
  知道「这个动作意味着什么」的地方。
- **`Panel` 不做 IO**：把所有点击翻译成 `PanelAction` 交给 `Service`；`PanelState` 是纯函数输入，
  一次 `ReadState()` 灌进来。面板不读注册表、不读文件。
- **绘制与命中同源**：`ComputeGeometry()` 同时供两者使用，点击不可能落在没画出来的控件上。

### 2.2 架构特点

| 特点 | 做法 |
|---|---|
| 零依赖单文件 | 静态 CRT；只导入 `KERNEL32 / USER32 / GDI32 / ole32 / SHELL32 / ADVAPI32 / dwmapi` |
| 纯 COM 图形栈 | `IDesktopWallpaper` + `IWICImagingFactory`，系统自带；不用 WinRT / .NET / Windows App SDK |
| 几何可单测 | cover 裁切、取景、摆放计划、面板几何都是静态纯函数，测试二进制直接编 `panel.cpp` |
| 按组渲染 | 每组按自己的成员屏画布调一次 `RenderSpan`；全轮 `pieces` 攒齐后统一清理一次 |
| 预览与渲染等价 | 两侧共用同一套 cover 几何；`SampleFor` 与 `ComputeSlice` 的输出由单测逐组对齐 |
| 高 DPI | 进程 Per-Monitor V2，面板按 96 dpi 设计单位排版，`MM_ANISOTROPIC` 一次映射到设备像素 |
| 自绘渲染 | borderless `WS_POPUP`，整块客户区画进 32bpp DIB；圆角按距离场逐像素抗锯齿 |
| 异步反馈 | 长任务先绘制 busy 帧，再交给后台线程，完成后 `PostMessage(WM_APP+4)` 回 UI 线程 |
| 容错 | COM 失败只记日志、返回 false，不抛异常；日志带毫秒时间戳，1 MB 轮转 |

### 2.3 组合模型

`core::Group = { slot, image, view, members, dirty }`，一组一行存在 `groups.tsv`：

```
group  zoom  focusX  focusY  dirty  image  m1;m2;...
```

- `NormaliseGroups()` 幂等，每次 `ReadState()` 都跑：一台屏只归一组、新插入的屏并入首组、
  拆空的组消失、`slot` 重排、暂时拔掉的成员留在原位。
- `MoveMonitorToGroup()` 是唯一原子改组操作，`targetSlot == -1` 表示单独成组。
- 组合名是派生值：`GroupLabel()` 取成员屏号升序拼接；屏号本身按「先分行、再按 left 排序」读出，
  2×2 排列不会按列编号。
- 旧格式的 `wallpaper.source` + `wallpapers.txt` 由 `GroupsFromLegacy()` 在首次启动时迁移：
  未被覆盖的屏进一个主组，每个逐屏覆盖屏进自己的单屏组。旧文件保留。

### 2.4 壁纸渲染管线

```
源图 ── WIC 解码 ── cover 缩放 ── 按显示器矩形裁切 ── PNG 切片 ── IDesktopWallpaper
                        ▲                                    │
              WallpaperView{zoom,focusX,focusY}       position = Center
```

- 不用系统的 Span：Span 把图映射到虚拟桌面外接矩形，只有源图尺寸恰好等于该矩形时才正确。
  这里逐显示器算切片，外接矩形里的空洞不会进入任何一张图。
- 源图尺寸与画布相等时走 1:1 快路径，直接取像素。
- `WallpaperView` 一路穿到渲染器，默认 cover 居中；面板上的拖动与滚轮改的就是它。
- `ApplyGroups()` 按组调用渲染，最后统一 `RemoveStalePieces(folder, keep)`：
  清理依据是「本轮生成了什么」，只认切片文件名形状。
- 布置图的预览按组用自己的画布 context 调 `RenderPreview`，与真实贴图同一套裁切几何。

### 2.5 窗口布局恢复

- **按显示器归属记录，不记绝对坐标**：显示器原点变了，窗口跟着平移同样的量；
  某台屏不在线就跳过它的窗口，等它回来再摆。
- 最大化窗口记 `rcNormalPosition`；停靠靠形状识别（三边齐、一边切），
  分割比例存千分比，半屏从 `rcWork` 工作区算。
- 「已到位」分两种判定：普通窗口比四边（容差 2px）；最大化窗口看「现在是否最大化、
  且还原矩形落在目标显示器上」。
- 自动恢复用 `Homecoming` 语义（不推翻用户手动分隔），手动「恢复布局」用 `Exact`。
- `layout.txt` 支持 v1 / v2 / v3：v3 首行带版本标记并多一列 `splitPermille`，
  老文件按列数分支读；取消勾选写成 flags 列里的 `-`，行不删。

### 2.6 面板渲染

- 全部绘制在 32bpp DIB 上，无子控件；`PaintContent` 用 `MM_ANISOTROPIC` 把设计单位映射到设备像素，
  文字按设备字号重新栅格化。
- 量字 DC 与绘制 DC 使用同一套横纵缩放，所有按文本宽度排版的控件（按钮、chip、标签板）因此对齐。
- 圆角用像素级距离场绘制，直边仍走块填充；描边按中心线距离场计算覆盖度。
- 组外框 `DrawGroupOutline()` 逐成员屏取四条边，用同组其他成员把内部接缝挖掉，只画真实并集轮廓；
  选中组的外发光是 16% 真透明叠加。
- 屏编号标签按屏宽分三级布局：数字 + 分辨率 → 只留数字 → 不画，绝不溢出所属屏。

### 2.7 数据与配置

所有运行时文件集中在一个目录：

```
%LOCALAPPDATA%\WinTools\
├── settings.txt        主题、自动恢复、自启、当前组合、去抖秒数
├── groups.tsv          组合定义（当前壁纸配置的主文件）
├── layout.txt          窗口布局快照
├── wallpapers.txt      旧格式迁移来源
├── WinTools.log        诊断日志
└── wallpaper\
    ├── state.txt       换壁纸之前的状态
    └── pieces\         每台屏一张切片，文件名带几何信息
```

开机自启写 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 的 `WinTools` 值；
单实例用 `Local\WinTools.SingleInstance.*` 互斥体。配置文件都是纯文本 TSV / key=value，
读取时按列数与字段分别校验。

---

## 三、构建与测试

前置：Visual Studio Build Tools（「使用 C++ 的桌面开发」）+ Windows 10/11 SDK。
CMake、Ninja 由 VS 自带，`build.bat` 会自己进 `vcvars64` 并补 SDK 环境；
不需要 .NET / Windows App SDK / vcpkg。

```bat
scripts\build.bat                          :: debug
scripts\build.bat windows-x64-release      :: release
build\debug\bin\wintools_tests.exe         :: 102 个用例
```

产物在 `build\<配置>\bin\`，Release 的 `WinTools.exe` 可单独拷走。
单测分组：几何 14、摆放计划/停靠识别 24、布局文件 8、逐屏壁纸 8、组合模型 17、
切片缓存 5、预览 3、面板 23。

---

## 四、目录结构

```
WinTools/
├── CMakeLists.txt / CMakePresets.json   单一构建入口，Ninja + cl.exe
├── assets/        wintools.ico、app.rc（版本信息）、app.manifest（Per-Monitor V2）
├── scripts/       构建、测试图生成、切片逐像素验证、面板截图、点击自动化、端到端恢复
├── docs/panel.png 面板截图
├── src/           main / app / core / platform/win / util
├── tests/         单测
└── build/         生成目录，已 gitignore
```

---

## 五、版本与许可

- 二进制版本资源为 `0.1.0.0`（`assets/app.rc`），与 Release tag 保持一致。
- 仓库目前没有 LICENSE 文件，默认「保留所有权利」；若公开源码，发版前需补许可证。
