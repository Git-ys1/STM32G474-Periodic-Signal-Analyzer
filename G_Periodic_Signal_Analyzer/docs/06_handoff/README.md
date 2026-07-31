# 新会话永久交接入口

更新时间：2026-07-31
适用仓库：`STM32G474-Periodic-Signal-Analyzer`
当前发布基线：`v2.4.0`；融合底座：`v1.9.0 / 9b023b5`

## 1. 这份目录为什么存在

本目录不是普通开发日志，而是为 Codex 上下文压缩、新会话接管和比赛期间紧急换人准备的“唯一恢复入口”。过去发生过多次严重偏航：新会话只记住了上一轮的一句结论，却忘记项目目标、当前版本、用户和队友的职责、HMI 已从双页面改为单页面、屏幕实物不可触摸、曲线 ID 需要动态上报、真实 ADC 已经融合等关键事实；随后又从旧版 `main.c`、旧任务书或旧曲线 ID 重新改代码，造成已经能运行的成果回退。以后任何代理开始本项目工作，必须先按下表读取全部交接文档，再查看当前 Git 状态和实际源码。不得只根据聊天摘要、单张截图或某篇旧任务书直接修改固件。

当前项目是 2026 年电赛 G 题“周期信号测量分析装置”的显示与融合工程。主控是 STM32G474VET6，显示设备是淘晶驰 X2 系列 7 英寸 800×480 串口屏。商家实际发来的屏幕不可触摸，因此既保留 HMI 模拟器按钮协议，也用最小系统板 KEY1（PB8/BOOT0）提供实体操作。队友负责 ADC 采集和信号分析；用户负责屏幕、桥接、绘图、协议、实体按键以及两边代码的融合验证。当前权威工程不是最早的正弦演示，也不是独立队友快照，而是：

```text
F:\Project\stm32G474VETx\TI\G_Periodic_Signal_Analyzer
\firmware
```

它以队友最新ADC1/Goertzel工程为底座，叠加`analyzer_bridge`、`display`、
USART3和KEY1。V1.9.0完成真实ADC融合并验证主链运行；V2.0.0在其上增加
时域触发与相位锚，下拉框协议、固件构建和用户烧录已完成，但真实连续帧触发
回归仍需补齐。后续应从V2.0.0继续真实ADC联调，不能再从V1.4、V1.8或
`tjc_display_demo`重新融合。

V2.1.0在V2.0.0之上发布普通/Huber折叠状态开关；V2.2继续完成真实ADC平滑：
`tests/三组实际ADC数据.xlsx`的三组2048点数组已经按当前MCU链路重放；
Huber后投影到FFT已识别整数谐波，把额外折返点从138/138/124降至
16/6/14，且与独立原始ADC稳健拟合的相位RMSE不超过0.150 mV。该固件已经
零警告构建、SWD烧录并通过运行态冒烟。HMI协议不变，状态1现在表示
“Huber + 已识别谐波投影”。详见
`docs/04_releases/V2.2_REAL_ADC_HARMONIC_PROJECTION_2026-07-31.md`。

用户已根据实屏效果确认V2.2定版，当前工作树上电默认改为状态1。发给队友的
生产融合包保持KEY1长按刷新真实ADC，并关闭随机测试；本地验证工程仍保留
KEY1长按测试。最小四文件包和自包含教程位于`deliverables/`。

V2.3把原`projects/g474_full_integration_test`提升为根目录唯一
`firmware/`，队友快照统一进入`teammate/`，旧显示demo进入`archive/`，
文档按用途分为八类。V2.2定版算法与V2.3目录结构已统一发布为`v2.3.0`，
此结构是后续唯一有效路径。

V2.4.0在唯一`firmware/`上发布512×256时域、256×256频谱、实际数值动态
坐标、KEY1/HMI周期状态回写和独立2048点模型`Mpp`。Keil全量重建为
0错误0警告；配套HMI仍由用户在淘晶驰编辑器中手工保存，仓库尚未归档其源文件。

## 2. 文件表

按表格顺序阅读：

| 顺序 | 文件 | 内容 |
|---:|---|---|
| 1 | [00_PROJECT_MASTER_CONTEXT.md](00_PROJECT_MASTER_CONTEXT.md) | 项目目的、当前状态和事实优先级 |
| 2 | [01_CONTEST_MATERIALS_AND_REQUIREMENTS.md](01_CONTEST_MATERIALS_AND_REQUIREMENTS.md) | 赛题、官方问答和指标口径 |
| 3 | [02_WORKSPACES_GIT_AND_RELEASES.md](02_WORKSPACES_GIT_AND_RELEASES.md) | 工作区、Git、构建与版本纪律 |
| 4 | [03_ROLES_AND_INTEGRATION_BOUNDARIES.md](03_ROLES_AND_INTEGRATION_BOUNDARIES.md) | 用户、队友和接口边界 |
| 5 | [04_HARDWARE_WIRING_AND_HMI.md](04_HARDWARE_WIRING_AND_HMI.md) | 硬件、接线、KEY1和HMI事实 |
| 6 | [05_SOFTWARE_ARCHITECTURE_AND_DATAFLOW.md](05_SOFTWARE_ARCHITECTURE_AND_DATAFLOW.md) | ADC到屏幕的完整数据流 |
| 7 | [06_DEBUG_HISTORY_AND_FROZEN_DECISIONS.md](06_DEBUG_HISTORY_AND_FROZEN_DECISIONS.md) | 故障史和禁止回退项 |
| 8 | [07_CURRENT_ISSUES_AND_NEXT_ACTIONS.md](07_CURRENT_ISSUES_AND_NEXT_ACTIONS.md) | 当前问题与后续行动 |
| 9 | [08_BUILD_FLASH_AND_VALIDATION_RUNBOOK.md](08_BUILD_FLASH_AND_VALIDATION_RUNBOOK.md) | 编译、烧录、诊断和验收 |
| 10 | [09_NEXT_AGENT_TASK_BRIEF.md](09_NEXT_AGENT_TASK_BRIEF.md) | 下一位Codex的一页任务书 |
| - | [README.md](README.md) | 本交接入口 |

## 3. 当前事实优先级

当文档、聊天和代码冲突时，按以下顺序判断：

```text
当前实机配对测量
> 当前V2.4工作树中的`firmware/`源码、真实ADC离线证据与用户最新HMI工程
> v2.4.0发布文档和本交接包
> 旧版本文档
> 聊天摘要或单张截图推断
```

尤其注意：当前 `display.c` 与 `main.c` 是最终判断运行行为的依据。V1.9曾把
KEY1长按从“测试”改成“刷新真实ADC”，但2026-07-31用户因现场屏不可触摸，
已明确要求V2.2长按恢复调用`Display_RequestTest()`；短按仍切换1T/3T。
再如旧双页面版本曾固定 `time.s0=11`、`spectrum.s0=1`，当前单页 dashboard
已改成页面初始化时上报 `s_time.id` 和 `s_spec.id`，不得重新硬编码。

## 4. 新会话开始时必须执行

```text
1. 读取本目录全部文档；
2. git -C F:\Project\stm32G474VETx\TI status --short --branch；
3. git -C F:\Project\stm32G474VETx\TI log -n 12 --oneline --decorate；
4. 打开 firmware 的当前源码；
5. 检查资料侧 testv2.HMI 和官方问答；
6. 确认用户这轮要求是诊断、修改、编译、烧录还是发布；
7. 修改前列出将触碰的文件，保留用户和队友无关改动；
8. 只有代码、构建、实机证据一致时才宣布完成。
```

当前 Git 工作树存在用户自己的未跟踪文件时，不能擅自删除、移动或加入提交。队友原始快照也只能作为只读底座与对照，不得在不说明的情况下“顺手修复”。真实 ADC 的幅值、伪谐波、毛刺和帧间相位漂移仍在审计中，不能把显示好看等同于测量正确。

## 5. 一句话恢复

如果时间极紧，只记住这一句：

> 这是STM32G474电赛G题的队友ADC1底座融合工程；发布基线是`v2.4.0`，
> V2.2已把三组真实ADC的Huber结果投影到FFT已识别谐波并由用户确认定版；
> V2.3唯一主线是根目录`firmware/`，队友输入只在`teammate/`，旧demo只在
> `archive/`；V2.4已加入独立2048点Huber模型`Mpp`、512/256曲线、动态坐标和
> 周期开关回写，当前等待用户归档配套HMI并做实屏三方对照，不得从历史工程重新融合。
