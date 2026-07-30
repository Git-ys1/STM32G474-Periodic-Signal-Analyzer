# 队友信号处理与淘晶驰显示双向融合速查

适用基线：`projects/g474_full_integration_test`（V1.4.1）。

## 结论

- 推荐以队友完整CubeMX/Keil工程为宿主，加入`analyzer_bridge`和`display`。
- 反向融合也应复制队友最新版为新目录，再叠加显示模块；不要零散搬运ADC生成文件。
- 两块MCU时一根Ready线不能传波形和测量结果，还需UART/SPI协议，不建议比赛采用。

## A. 显示模块加入队友工程

硬件：PC10/TX→屏RX，PC11/RX←屏TX，GND共地，外部5 V→屏5 V；USART3保持115200、8N1。

复制`Core/Inc/analyzer_bridge.h`、`display.h`及`Core/Src/analyzer_bridge.c`、`display.c`。

Keil的`Application/User`组加入两个`.c`；Include Path加入`../../Drivers/CMSIS/DSP/Include`，禁止`#include "*.c"`。

## `main.c`可复制代码

将各段放入同名CubeMX `USER CODE`区：

```c
/* USER CODE BEGIN Includes */
#include "analyzer_bridge.h"
#include "display.h"
/* USER CODE END Includes */

/* USER CODE BEGIN PD */
#define ANALYZER_SAMPLE_RATE_HZ  1024000.0f
#define ADC_VOLTS_PER_CODE       (3.3f / 4096.0f)
#define BOARD_KEY_CONTROL_ENABLE 1U
#define BOARD_KEY_DEBOUNCE_MS    30U
#define BOARD_KEY_LONG_PRESS_MS  1000U
/* USER CODE END PD */

/* USER CODE BEGIN PV */
#if defined(__ARMCC_VERSION)
__attribute__((used)) int __ARM_use_no_argv; /* 防止BKPT 0xAB */
#endif
#if BOARD_KEY_CONTROL_ENABLE
static volatile uint32_t s_key_press_tick;
static volatile uint8_t s_key_press_active;
#endif
/* USER CODE END PV */
```

KEY1在CubeMX设为PB8上升沿EXTI、高电平有效；放入`USER CODE BEGIN 0`：

```c
#if BOARD_KEY_CONTROL_ENABLE
void HAL_GPIO_EXTI_Callback(uint16_t pin)
{
    if ((pin == KEY_Pin) && !s_key_press_active &&
        (HAL_GPIO_ReadPin(KEY_GPIO_Port, KEY_Pin) == GPIO_PIN_SET)) {
        s_key_press_tick = HAL_GetTick();
        s_key_press_active = 1U;
    }
}
static void BoardKey_Task(void)
{
    if (!s_key_press_active ||
        (HAL_GPIO_ReadPin(KEY_GPIO_Port, KEY_Pin) == GPIO_PIN_SET)) return;
    uint32_t held_ms = HAL_GetTick() - s_key_press_tick;
    s_key_press_active = 0U;
    if (held_ms < BOARD_KEY_DEBOUNCE_MS) return;
    if (held_ms >= BOARD_KEY_LONG_PRESS_MS) Display_RequestTest();
    else Display_TogglePeriods();
}
#endif
```

在`USER CODE BEGIN 2`中，紧跟ADC校准、TIM3启动和`HAL_ADC_Start_DMA()`：

```c
AnalyzerBridge_Init();
Display_Init(&huart3);
```

在`USER CODE BEGIN 3`中，紧跟队友第一次`fft();`，必须先快照，因`Vpp_R()`会再次FFT并覆盖全局谱峰：

```c
float spectrum_frequencies_hz[ANALYZER_MAX_COMPONENTS] = {F, FB, FC};
float spectrum_amplitudes_v[ANALYZER_MAX_COMPONENTS] = {V, VB, VC};
uint8_t spectrum_flag = (uint8_t)flag;
```

紧跟`vpp = Vpp_Robust(...)`和`Vrms = Vpp_R()`，并放在重启ADC DMA之前：

```c
AnalyzerBridge_PublishReal(
    adc_b, ADC_SIZE, ADC_VOLTS_PER_CODE, ANALYZER_SAMPLE_RATE_HZ,
    vpp, Vrms, spectrum_flag,
    spectrum_frequencies_hz, spectrum_amplitudes_v);
```

在`while(1)`末尾加入：

```c
#if BOARD_KEY_CONTROL_ENABLE
BoardKey_Task();
#endif
Display_Task();
```

`display.c`已实现USART3的两个HAL UART回调；若队友也定义，必须合并并按`huart->Instance`分发。

## B. 队友代码加入我的工程

1. 推荐：复制`teammate_adc_newest`为新融合目录，再执行方案A；这是可持续的“反向融合”。
2. 坚持以显示工程为宿主时，须完整迁入`.ioc`、ADC2/TIM3/DMA、DAC/AD9833、IRQ、Drivers、DSP和Keil配置。
3. 换板时模拟前端接PA7/ADC2_IN4并共地；其他外设按队友原理图迁移，不能只接“计算完成”线。
4. 队友发新版时新建快照，再叠加四个文件和上述钩子；Clean Build后回归1T、3T、测试、时域、频谱、文本和FE/FD。
