# Core 源码索引

CubeMX应用代码分为 `Inc/` 和 `Src/`。下表覆盖本层全部源码文件。

## Inc

| 文件 | 作用 |
|---|---|
| `AD9833.h` | 历史DDS接口，当前主链未调用 |
| `adc.h` | ADC1外设声明 |
| `analyzer_bridge.h` | 稳定测量结果和桥接API |
| `dac.h` | DAC外设声明 |
| `display.h` | 显示模块公开API |
| `dma.h` | DMA外设声明 |
| `generated_adc_tests.h` | 题内随机测试ADC数组 |
| `generated_custom_adc_tests.h` | 自定义测试ADC数组 |
| `goertzel_sync.h` | 队友Goertzel接口 |
| `gpio.h` | GPIO声明 |
| `main.h` | 主工程引脚和公共声明 |
| `stm32g4xx_hal_conf.h` | HAL模块配置 |
| `stm32g4xx_it.h` | 中断声明 |
| `tim.h` | TIM3声明 |
| `usart.h` | USART3声明 |

## Src

| 文件 | 作用 |
|---|---|
| `AD9833.c` | 历史DDS实现，当前主链未调用 |
| `adc.c` | ADC1配置 |
| `analyzer_bridge.c` | ADC快照、折叠、Huber和谐波投影 |
| `dac.c` | DAC配置 |
| `display.c` | 淘晶驰通信与绘图状态机 |
| `dma.c` | DMA配置 |
| `goertzel_sync.c` | 队友Goertzel实现 |
| `gpio.c` | GPIO与KEY1配置 |
| `main.c` | 当前唯一业务集成入口 |
| `stm32g4xx_hal_msp.c` | HAL底层初始化 |
| `stm32g4xx_it.c` | 中断处理 |
| `system_stm32g4xx.c` | 系统时钟底层 |
| `tim.c` | TIM3触发配置 |
| `usart.c` | USART3配置 |

`Inc/`、`Src/`由CubeMX/Keil直接使用，不再继续插入说明文件。
