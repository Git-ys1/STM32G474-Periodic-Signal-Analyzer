# 当前队友快照

来源：`source_packages/4096-2.zip`，收到时间为2026-08-01。

| 项目 | 说明 |
|---|---|
| `.mxproject` | CubeMX工程元数据 |
| `ADC.ioc` | ADC1/ADC2、DMA1 Channel 1/2与TIM3双沿触发配置 |
| `ADC/` | 队友Keil工程及其收到时的构建产物 |
| `Core/` | 双ADC交错、4096点FFT、Vpp/RMS、Goertzel及已融合显示源码 |
| `Drivers/` | STM32 HAL、CMSIS与CMSIS-DSP依赖 |

当前采集链为ADC1/PA0上升沿与ADC2/PA1下降沿各采2048点，在`main.c`中直接
交错并换算为浮点电压`VO[4096]`。`4096-2`相对上一包只修改队友`main.c`中的
FFT频响校正位置；本目录保持只读，正式适配进入`../../firmware/`。
