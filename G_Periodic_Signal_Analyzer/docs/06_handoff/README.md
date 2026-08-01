# 新会话永久交接入口

更新时间：2026-08-01

当前发布：`v2.6.0`

唯一主线：`firmware/`
队友底座：`teammate/current/`，来源原`teammate/win/`

## 当前一句话结论

这是STM32G474电赛G题的双ADC融合工程：ADC1/PA0和ADC2/PA1各采2048点，
交错并换算为约2.048193 MS/s的浮点电压`VO[4096]`；队友最新版本已经增加
Goertzel分量相位和公式Vpp，桥接层继续直接消费VO，
不再读取原始ADC码或重复换算。V1.x至V2.4的2048点材料是历史证据，不能
作为当前接口重新融合。

## 当前数据链

```text
TIM3 CH4双沿
→ ADC1/PA0 + ADC2/PA1
→ adc_b[2048] + adc_b1[2048]
→ VO[4096]浮点电压
→ 队友4096点FFT幅值 / Goertzel相位
→ 去除公共时间原点与前级系统相移
→ 4096点谐波公式合成Upp（Vpp_Robust运行调用已停用）
→ AnalyzerBridge_PrepareReal(VO,...)
→ 频率细化 / 256槽折叠 / Huber / 谐波投影 / 模型Mpp
→ 队友Vpp_R()
→ AnalyzerBridge_PublishPreparedReal(Upp,Vr)
→ Display_Task → USART3 → 淘晶驰dashboard
```

`PrepareReal()`必须位于`Vpp_R()`之前，因为当前`Vpp_R()`会原地修改VO。
发布采用两阶段提交，显示层只读取完整`AnalyzerResult`。

## 职责与覆盖边界

- 队友负责ADC1/ADC2、DMA、TIM3、VO生成、FFT、Vpp/RMS、Goertzel；
- 用户负责`analyzer_bridge.c/.h`、`display.c/.h`、HMI、USART3、KEY1和融合；
- 发给队友的包可以覆盖桥接层和显示层；
- `main.c`、CubeMX和队友算法文件不得覆盖，只能逐处给出手工修改代码块；
- 当前交接包`deliverables/V2.5更新包.zip`不包含`main.c`。

## 本地与队友按键差异

| 版本 | KEY1短按 | KEY1长按 | 随机测试 |
|---|---|---|---|
| 本地实物屏 | 1T/3T | 测试 | 开启 |
| 队友生产版 | 1T/3T | 刷新真实ADC | 关闭 |

两者都必须复用`Display_ProcessButtonCommand()`，不得新建第二套显示状态机。

## 必读文件

| 顺序 | 文件 | 内容 |
|---:|---|---|
| 1 | [00_PROJECT_MASTER_CONTEXT.md](00_PROJECT_MASTER_CONTEXT.md) | 项目目标和当前冻结事实 |
| 2 | [01_CONTEST_MATERIALS_AND_REQUIREMENTS.md](01_CONTEST_MATERIALS_AND_REQUIREMENTS.md) | 赛题与指标口径 |
| 3 | [02_WORKSPACES_GIT_AND_RELEASES.md](02_WORKSPACES_GIT_AND_RELEASES.md) | 工作区与发布纪律 |
| 4 | [03_ROLES_AND_INTEGRATION_BOUNDARIES.md](03_ROLES_AND_INTEGRATION_BOUNDARIES.md) | 职责和文件边界 |
| 5 | [04_HARDWARE_WIRING_AND_HMI.md](04_HARDWARE_WIRING_AND_HMI.md) | 双ADC接线、屏幕和KEY1 |
| 6 | [05_SOFTWARE_ARCHITECTURE_AND_DATAFLOW.md](05_SOFTWARE_ARCHITECTURE_AND_DATAFLOW.md) | 4096点VO完整链路 |
| 7 | [06_DEBUG_HISTORY_AND_FROZEN_DECISIONS.md](06_DEBUG_HISTORY_AND_FROZEN_DECISIONS.md) | 历史故障与冻结修复 |
| 8 | [07_CURRENT_ISSUES_AND_NEXT_ACTIONS.md](07_CURRENT_ISSUES_AND_NEXT_ACTIONS.md) | 当前实测任务 |
| 9 | [08_BUILD_FLASH_AND_VALIDATION_RUNBOOK.md](08_BUILD_FLASH_AND_VALIDATION_RUNBOOK.md) | 构建、烧录与取证 |
| 10 | [09_NEXT_AGENT_TASK_BRIEF.md](09_NEXT_AGENT_TASK_BRIEF.md) | 下一位代理一页任务书 |

## 当前构建证据

```text
Arm Compiler 6.7
0 Error(s), 0 Warning(s)
Code=80244, RO-data=78048, RW-data=172, ZI-data=100212
ADC.hex SHA-256=12550422C8989B88B920995E7D5B5D742B77030F58DC761D79F30B2C5B64C53D
```

固件已通过STM32CubeProgrammer下载、校验和复位。72组离线幅相重构的最大
峰峰值误差为0.037693 mV。后续第一优先级是完成前级系统复频响扫频标定，并把
同一信号同时送入PA0和PA1，保存同帧`VO[4096]`及屏幕/示波器证据。
