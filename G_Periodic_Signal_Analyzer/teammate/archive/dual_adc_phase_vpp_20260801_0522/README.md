# 双ADC相位差与公式Vpp原始快照

来源：用户放入`teammate/win/`的最新队友工程，归档时间为2026-08-01。

| 项目 | 内容 | 状态 |
|---|---|---|
| `.mxproject`、`ADC.ioc` | 双ADC、DMA和TIM3配置 | 与`4096-2`一致 |
| `ADC/` | 队友Keil工程及收到时的构建产物 | 原样留证 |
| `Core/` | 新增Goertzel相位差和公式Vpp的源码 | 原样留证 |
| `Drivers/` | STM32 HAL、CMSIS与CMSIS-DSP | 与上一底座一致 |

相对上一份`4096-2`，有效源码变化集中在`Core/Src/main.c`以及已接收的
`analyzer_bridge.c/.h`。原始`main.c`中的`bw9_phase()`仍为0，且原`getup()`
存在相位网格使用错误；这些问题只在正式`firmware/`中修正，本归档不改写。
