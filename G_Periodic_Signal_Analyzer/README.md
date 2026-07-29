# G 题周期信号测量分析装置工作区

本目录集中保存 G 题两条相互独立的实际固件开发线。它们属于同一套竞赛装置，但由不同成员分别负责，当前没有合并成一个可直接烧录的工程。

## 目录

```text
G_Periodic_Signal_Analyzer/
├─ projects/
│  ├─ tjc_display_demo/
│  └─ teammate_adc_reference/
└─ docs/
   └─ integration-notes.md
```

## `tjc_display_demo`

- 来源：原 `test/TJC_sine_test`；
- 工具链：STM32CubeIDE；
- 主控：STM32G474VET6；
- 显示器：淘晶驰 X2 系列 TJC8048X270_011R；
- 当前用途：独立验证串口屏页面、1/3 周期时域显示、参数文本、定性频谱和触摸按钮；
- 当前演示数据：内部 100 kHz 正弦和 256 点演示 DFT；
- 当前状态：显示链路已模块化为 `display` 与 `tjc_hmi`。

## `teammate_adc_reference`

- 来源：原 `test/Core`；
- 工具链：STM32CubeMX + Keil MDK；
- 主控：STM32G474VET6；
- 当前内容：ADC、DAC、DMA、定时器、USART、AD9833 及 CMSIS/HAL；
- 当前用途：信号采集和处理负责人的独立工程快照。

## 当前边界

- 两个工程分别构建和验证；
- 不直接互相覆盖 `main.c`、`.ioc` 或工程配置；
- 不在显示工程内重复实现正式 ADC/FFT 算法；
- 不在信号处理工程内复制淘晶驰协议实现；
- 等双方明确数据接口后，再决定最终整机工程以哪一套配置为底座。

具体接口约定见 [docs/integration-notes.md](docs/integration-notes.md)。

