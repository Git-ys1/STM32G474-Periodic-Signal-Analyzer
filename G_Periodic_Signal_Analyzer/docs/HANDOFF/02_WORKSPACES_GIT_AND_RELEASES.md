# 软件工作区、Git真相与版本纪律

## 1. Git根与项目目录不是同一个层级

当前代码仓库工作目录常写作：

```text
F:\Project\stm32G474VETx\TI\G_Periodic_Signal_Analyzer
```

但实际 Git 根是它的父目录：

```text
F:\Project\stm32G474VETx\TI
```

因此查看状态、提交和推送时，应在 Git 根运行，或明确使用 `git -C F:\Project\stm32G474VETx\TI ...`。远端仓库是：

```text
https://github.com/Git-ys1/STM32G474-Periodic-Signal-Analyzer.git
```

V1.9.0提交`9b023b5`是队友ADC1真实融合底座；当前发布标签升级为`v2.1.0`，
在该底座上增加显示触发、相位锚和可选Huber鲁棒折叠。不要因为子目录名里有`test`就误以为它可以
随便覆盖；`g474_full_integration_test`虽保留“test”命名，实际上已经是当前
权威全量融合工程。

## 2. 各工程目录的角色

```text
projects/
├─ tjc_display_demo/
├─ teammate_adc_reference/
├─ teammate_adc_newest/
├─ teammate_adc_reallynewest!/
└─ g474_full_integration_test/
```

`tjc_display_demo` 是用户显示侧独立回归基线。它保存淘晶驰 dashboard、显示协议、模拟/预生成测试和早期可运行链路，适合判断“屏幕和 UART 能不能画”，不包含队友最新真实 ADC 全链路。它不能替代融合工程，但在融合版显示突然完全失效时可用作行为对照。

`teammate_adc_reference` 是队友早期快照，用于理解原始 ADC/FFT 思路；`teammate_adc_newest` 是中间版本；`teammate_adc_reallynewest!` 是 V1.9 融合前收到的最新 ADC1/Goertzel 原始快照，对应提交 `44fc481`。这些目录的价值是保留来源和便于 diff，不是让代理直接在里面添加显示代码。除非用户明确要求升级队友快照，否则应只读。

`g474_full_integration_test` 以 `teammate_adc_reallynewest!` 为底座，叠加显示和桥接，是编译、烧录、实测和后续优化的唯一权威工程。它同时包含 CubeMX 生成的 `Core`、Keil 工程 `ADC/MDK-ARM`、CMSIS 依赖和用户模块。修改前必须与队友原始快照交叉 diff，确认没有无意改变 ADC、TIM、DMA 和 FFT 主体。

仓库还有：

```text
docs/    设计、协议、发布和调试文档
plan/    历史任务书，不是当前事实源
tests/   相位覆盖、定制波形和离线结果
tools/   Python分析、扫描、生成测试数组和GUI工具
```

`plan/V1.3单页面显示模块修改任务书.md`、`plan/V1.4任务书.md` 记录当时设计目标。任务书可以解释为什么这样演进，但其设想可能已因实机问题调整，不能覆盖当前源码。当前主架构应以 `docs/CURRENT_STATUS.md`、本交接包和实际代码为准。

## 3. HMI与赛事资料不在代码仓库同一位置

当前资料侧单页 HMI 是：

```text
F:\AcademicHub\000资料相关\电赛\2026TI杯\淘晶驰串口屏\testv2.HMI
```

代码仓库中另有归档副本：

```text
projects/tjc_display_demo/HMI/TJC8048X270_dashboard_v1.3.1.HMI
```

新会话修改 HMI 前必须确认用户实际打开和下载的是哪一份，并在修改后同步归档；不能只改仓库副本却让用户运行资料目录的旧 HMI，也不能只改资料目录而让 Git 无法追踪。当前屏幕页面名为 `dashboard`，不是旧版 `time`/`spectrum` 双页面。

## 4. 版本历史中的关键冻结点

```text
aa59f38  冻结最初可显示基线并修复时域曲线ID
c11c494  记录当前显示基线路径
65d9897  切换为dashboard单页面
277c534  v1.3.1单页稳定基线
799a15d  v1.4.0融合稳定基线
ac83be7  v1.4.1实体KEY1兼容
71e3670  v1.5.2原生ADC测试数组
c614982  v1.5.3测试编号与相位折叠说明
005e6dd  v1.8.0波形实验室与时域方向修复
44fc481  归档队友ADC1/Goertzel底座
9b023b5  v1.9.0全量融合
v2.0.0  时域触发、相位锚与下拉框参数协议
v2.1.0  PC端Huber量化验证与MCU普通/Huber状态开关
```

这些提交不是让代理逐个cherry-pick的菜单，而是用于定位回归起点。后续若
发现问题，应从`v2.1.0`建立小范围diff；只有证明融合层无法修复时才回看
独立基线，不得把早期`main.c`整体覆盖当前工程。

## 5. 当前工作树和文件所有权

V2.1.0发布继续保留真实ADC首次联调审计：

```text
G_Periodic_Signal_Analyzer/docs/
REAL_ADC_FIRST_INTEGRATION_REVIEW_2026-07-31.md
```

它记录“源与示波器小于5 mV，示波器与屏幕经常超过10 mV”的用户实测纠正，
以及Vpp、伪峰、毛刺和水平漂移的分层判断。

Git 根还存在两个用户自己命名的未跟踪 Markdown 文件，文件名含“说的就是你，codex”和“这个你也来看”。它们不属于本交接包，不能删除、移动、改写或混入提交。类似地，队友工程中的 Keil 用户状态、`EventRecorderStub.scvd` 等若出现无关差异，必须排除在显示或文档提交之外。

## 6. 构建和发布纪律

Keil 工程入口在：

```text
projects\g474_full_integration_test\ADC\MDK-ARM\ADC.uvprojx
```

构建产物在：

```text
projects\g474_full_integration_test\ADC\MDK-ARM\ADC\ADC.hex
```

Keil CLI 固定路径：

```text
D:\Work\Keil5\UV4\UV4.exe
```

STM32CubeProgrammer CLI 固定路径：

```text
F:\AcademicHub\STMicroelectronics\stm32cubeprogrammer
\bin\STM32_Programmer_CLI.exe
```

任何“已完成”至少要说明：修改文件、构建命令、0 error/0 warning、HEX 路径与时间、是否烧录、实机或模拟器验证结果。编译成功不等于烧录成功，烧录成功不等于算法正确，屏幕出现图像也不等于测量指标正确。

发布前执行 `git diff --check`，检查 `git status --short`，只暂存本轮文件，提交后再确认远端。禁止用 `git reset --hard` 或覆盖整个目录来“恢复”。比赛期间尤其要小步提交、可回滚、保留实机证据，不再允许一个未验证的大改动跨越显示、ADC、HMI 和文档四层。
