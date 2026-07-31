# 新会话永久交接入口

更新时间：2026-07-31
适用仓库：`STM32G474-Periodic-Signal-Analyzer`
当前发布基线：`v2.1.0`；融合底座：`v1.9.0 / 9b023b5`

## 1. 这份目录为什么存在

本目录不是普通开发日志，而是为 Codex 上下文压缩、新会话接管和比赛期间紧急换人准备的“唯一恢复入口”。过去发生过多次严重偏航：新会话只记住了上一轮的一句结论，却忘记项目目标、当前版本、用户和队友的职责、HMI 已从双页面改为单页面、屏幕实物不可触摸、曲线 ID 需要动态上报、真实 ADC 已经融合等关键事实；随后又从旧版 `main.c`、旧任务书或旧曲线 ID 重新改代码，造成已经能运行的成果回退。以后任何代理开始本项目工作，必须先读完本文件和下面列出的七篇文档，再查看当前 Git 状态和实际源码。不得只根据聊天摘要、单张截图或某篇旧任务书直接修改固件。

当前项目是 2026 年电赛 G 题“周期信号测量分析装置”的显示与融合工程。主控是 STM32G474VET6，显示设备是淘晶驰 X2 系列 7 英寸 800×480 串口屏。商家实际发来的屏幕不可触摸，因此既保留 HMI 模拟器按钮协议，也用最小系统板 KEY1（PB8/BOOT0）提供实体操作。队友负责 ADC 采集和信号分析；用户负责屏幕、桥接、绘图、协议、实体按键以及两边代码的融合验证。当前权威工程不是最早的正弦演示，也不是独立队友快照，而是：

```text
F:\Project\stm32G474VETx\TI\G_Periodic_Signal_Analyzer
\projects\g474_full_integration_test
```

它以队友最新ADC1/Goertzel工程为底座，叠加`analyzer_bridge`、`display`、
USART3和KEY1。V1.9.0完成真实ADC融合并验证主链运行；V2.0.0在其上增加
时域触发与相位锚，下拉框协议、固件构建和用户烧录已完成，但真实连续帧触发
回归仍需补齐。后续应从V2.0.0继续真实ADC联调，不能再从V1.4、V1.8或
`tjc_display_demo`重新融合。

V2.1.0在V2.0.0之上发布普通/Huber折叠状态开关：状态开关发送
`A5 02 08 00/01 5A`，上电和HMI默认值均为普通折叠。该版本已经完成
PC端72组量化对照、Keil零警告构建、烧录和板上普通→Huber→普通无故障
冒烟；真实HMI开关与异常点波形的可视A/B仍待用户配置后验收。

## 2. 必读顺序

1. [00_PROJECT_MASTER_CONTEXT.md](00_PROJECT_MASTER_CONTEXT.md)：项目目的、当前状态、事实优先级和新会话恢复规则。
2. [01_CONTEST_MATERIALS_AND_REQUIREMENTS.md](01_CONTEST_MATERIALS_AND_REQUIREMENTS.md)：G 题、官方问答、本地资料工作区和指标口径。
3. [02_WORKSPACES_GIT_AND_RELEASES.md](02_WORKSPACES_GIT_AND_RELEASES.md)：代码目录、Git 根、远端、各快照用途、构建产物和版本纪律。
4. [03_ROLES_AND_INTEGRATION_BOUNDARIES.md](03_ROLES_AND_INTEGRATION_BOUNDARIES.md)：用户、队友和共享接口分别负责什么，哪些层不能互相替罪。
5. [04_HARDWARE_WIRING_AND_HMI.md](04_HARDWARE_WIRING_AND_HMI.md)：主控、ADC、屏幕、串口、供电、KEY1 和 HMI 工程的硬件事实。
6. [05_SOFTWARE_ARCHITECTURE_AND_DATAFLOW.md](05_SOFTWARE_ARCHITECTURE_AND_DATAFLOW.md)：从 ADC 到屏幕的完整主链路、核心结构体、状态机和显示算法。
7. [06_DEBUG_HISTORY_AND_FROZEN_DECISIONS.md](06_DEBUG_HISTORY_AND_FROZEN_DECISIONS.md)：曾经导致整天回退的故障、根因、修复和禁止回退项。
8. [07_CURRENT_ISSUES_AND_NEXT_ACTIONS.md](07_CURRENT_ISSUES_AND_NEXT_ACTIONS.md)：2026-07-31 真正做到哪里、当前实测结论和下一轮执行清单。
9. [08_BUILD_FLASH_AND_VALIDATION_RUNBOOK.md](08_BUILD_FLASH_AND_VALIDATION_RUNBOOK.md)：Keil 编译、CubeProgrammer 烧录、ST-Link 检查、HMI 验收和证据保存。
10. [09_NEXT_AGENT_TASK_BRIEF.md](09_NEXT_AGENT_TASK_BRIEF.md)：给下一位 Codex 的一页式任务书，明确先看什么、先做什么和禁止事项。

## 3. 当前事实优先级

当文档、聊天和代码冲突时，按以下顺序判断：

```text
当前实机配对测量
> 当前工作树源码与HMI工程
> v2.1.0发布文档和本交接包
> 旧版本文档
> 聊天摘要或单张截图推断
```

尤其注意：当前 `display.c` 与 `main.c` 是最终判断运行行为的依据。例如旧文档曾写 KEY1 长按启动“测试”，但当前权威代码明确为长按调用 `Display_RequestRefresh()`，恢复真实 ADC；这类漂移必须以源码和实机为准，并在文档中标出。再如旧双页面版本曾固定 `time.s0=11`、`spectrum.s0=1`，当前单页 dashboard 已改成页面初始化时上报 `s_time.id` 和 `s_spec.id`，不得重新硬编码。

## 4. 新会话开始时必须执行

```text
1. 读取本目录全部文档；
2. git -C F:\Project\stm32G474VETx\TI status --short --branch；
3. git -C F:\Project\stm32G474VETx\TI log -n 12 --oneline --decorate；
4. 打开 projects/g474_full_integration_test 的当前源码；
5. 检查资料侧 testv2.HMI 和官方问答；
6. 确认用户这轮要求是诊断、修改、编译、烧录还是发布；
7. 修改前列出将触碰的文件，保留用户和队友无关改动；
8. 只有代码、构建、实机证据一致时才宣布完成。
```

当前 Git 工作树存在用户自己的未跟踪文件时，不能擅自删除、移动或加入提交。队友原始快照也只能作为只读底座与对照，不得在不说明的情况下“顺手修复”。真实 ADC 的幅值、伪谐波、毛刺和帧间相位漂移仍在审计中，不能把显示好看等同于测量正确。

## 5. 一句话恢复

如果时间极紧，只记住这一句：

> 这是STM32G474电赛G题的队友ADC1底座融合工程；用户负责TJC单页显示和
> 桥接，当前从`v2.1.0 / projects/g474_full_integration_test`继续，先验证
> 已烧录的相位锚，再解决ADC引脚示波器与屏幕Vpp差异、伪谐波和相位折叠
> 质量，绝不从旧显示演示重新做。
