# 当前队友快照

来源：原`teammate/win/`目录，收到时间为2026-08-01；它是在`4096-2`
基础上继续增加初相差识别和公式峰峰值重构的最新队友版本。

| 项目 | 说明 |
|---|---|
| `.mxproject` | CubeMX工程元数据 |
| `ADC.ioc` | ADC1/ADC2、DMA1 Channel 1/2与TIM3双沿触发配置 |
| `ADC/` | 队友Keil工程及其收到时的构建产物 |
| `Core/` | 双ADC交错、4096点FFT、Vpp/RMS、Goertzel及已融合显示源码 |
| `Drivers/` | STM32 HAL、CMSIS与CMSIS-DSP依赖 |

当前采集链仍为ADC1/PA0上升沿与ADC2/PA1下降沿各采2048点，在`main.c`中直接
交错并换算为浮点电压`VO[4096]`。相对`4096-2`，本版在`main.c`增加：

- 用Goertzel分别提取基波与谐波相位；
- 消除公共时间原点，计算各谐波相对初相差；
- 根据幅值、谐波次数和相对相位合成波形并计算峰峰值；
- 预留`bw9_phase()`作为前级系统相移扫频表入口，当前仍返回0。

收到的原始实现仍保存在
`../archive/dual_adc_phase_vpp_20260801_0522/`。本目录保持只读；正式适配、
`Vpp_Robust()`停用和公式峰峰值修正均进入`../../firmware/`。
