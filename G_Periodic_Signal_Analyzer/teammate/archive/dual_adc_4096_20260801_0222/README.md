# 当前队友快照

来源：`source_packages/4096.zip`，收到时间为2026-08-01。

| 项目 | 说明 |
|---|---|
| `.mxproject` | CubeMX工程元数据 |
| `ADC.ioc` | ADC1/ADC2、DMA1 Channel 1/2和TIM3触发配置 |
| `ADC/` | 队友Keil工程及原始构建产物 |
| `Core/` | 双ADC交替采样、4096点FFT及已融合的显示源码 |
| `Drivers/` | STM32 HAL、CMSIS与DSP依赖 |

当前采集链为ADC1/PA0上升沿与ADC2/PA1下降沿各采2048点，再交错组成
4096点、约2.048193 MS/s的数据。此目录只作原始对照；正式修改进入
`../../firmware/`。
