# 队友工程加入淘晶驰显示速查

适用基线：V1.9.0，`projects/teammate_adc_reallynewest!`。

## 结论

- 以队友完整CubeMX/Keil工程为底座；
- 只加入`analyzer_bridge.c/.h`和`display.c/.h`；
- 不零散重建ADC、DMA、TIM和IRQ；
- 队友发新版时先独立归档，再重新叠加，禁止覆盖原始快照。

## 1. 硬件

```text
PC10 / USART3_TX → 屏幕RX
PC11 / USART3_RX ← 屏幕TX
MCU GND          ─ 屏幕GND
外部5 V          → 屏幕5 V
```

KEY1为PB8/BOOT0，高电平有效。短按切换1T/3T，长按刷新真实ADC。

## 2. 复制文件

```text
Core/Inc/analyzer_bridge.h
Core/Src/analyzer_bridge.c
Core/Inc/display.h
Core/Src/display.c
```

Keil的`Application/User/Core`组加入两个`.c`文件。CMSIS-DSP Include Path使用：

```text
../../Drivers/CMSIS/DSP/Include
```

不得使用队友个人电脑的`E:`绝对路径，不得`#include "*.c"`。

## 3. main.c头部

放入对应CubeMX `USER CODE`区：

```c
/* USER CODE BEGIN Includes */
#include "analyzer_bridge.h"
#include "display.h"
#include "goertzel_sync.h"
/* USER CODE END Includes */

/* USER CODE BEGIN PD */
#define ANALYZER_SAMPLE_RATE_HZ  1024090.0f
#define ADC_VOLTS_PER_CODE       (3.3f / 4096.0f)
#define BOARD_KEY_CONTROL_ENABLE 1U
#define BOARD_KEY_DEBOUNCE_MS    30U
#define BOARD_KEY_LONG_PRESS_MS  1000U
/* USER CODE END PD */

/* USER CODE BEGIN PV */
#if defined(__ARMCC_VERSION)
__attribute__((used)) int __ARM_use_no_argv;
#endif
#if BOARD_KEY_CONTROL_ENABLE
static volatile uint32_t s_key_press_tick;
static volatile uint8_t s_key_press_active;
#endif
/* USER CODE END PV */
```

## 4. KEY1

PB8配置为上升沿EXTI。在`USER CODE BEGIN 0`加入：

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
    uint32_t held_ms;

    if (!s_key_press_active ||
        (HAL_GPIO_ReadPin(KEY_GPIO_Port, KEY_Pin) == GPIO_PIN_SET)) {
        return;
    }

    held_ms = HAL_GetTick() - s_key_press_tick;
    s_key_press_active = 0U;
    if (held_ms < BOARD_KEY_DEBOUNCE_MS) return;

    if (held_ms >= BOARD_KEY_LONG_PRESS_MS) {
        Display_RequestRefresh();
    } else {
        Display_TogglePeriods();
    }
}
#endif
```

实体键必须调用原显示命令接口，不得另写一套绘图或运行模式状态机。

## 5. 初始化插入点

队友当前底座初始化顺序：

```c
MX_DMA_Init();
MX_DAC1_Init();
MX_USART3_UART_Init();
MX_TIM3_Init();
MX_ADC1_Init();
```

在ADC1校准、TIM3启动、ADC1 DMA启动之后加入：

```c
HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED);
HAL_TIM_Base_Start(&htim3);
HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc_b, ADC_SIZE);

AnalyzerBridge_Init();
Display_Init(&huart3);
```

## 6. 分析完成插入点

第一次`fft();`之后立即保存谱峰。`Vpp_R()`内部还会FFT一次，晚保存会拿到错误结果：

```c
float spectrum_frequencies_hz[ANALYZER_MAX_COMPONENTS] = {F, FB, FC};
float spectrum_amplitudes_v[ANALYZER_MAX_COMPONENTS] = {V, VB, VC};
uint8_t spectrum_flag = (uint8_t)flag;
```

完成`vpp`和`Vrms`后、重新启动ADC1 DMA前发布：

```c
AnalyzerBridge_PublishReal(
    adc_b,
    ADC_SIZE,
    ADC_VOLTS_PER_CODE,
    ANALYZER_SAMPLE_RATE_HZ,
    vpp,
    Vrms,
    spectrum_flag,
    spectrum_frequencies_hz,
    spectrum_amplitudes_v
);
```

必须传原始实采`adc_b[2048]`。桥接层会用所有2048点做相关频率细化和256槽相位折叠。

## 7. 主循环末尾

```c
#if BOARD_KEY_CONTROL_ENABLE
BoardKey_Task();
#endif
Display_Task();
```

## 8. UART回调

`display.c`已经实现USART3接收完成和错误回调。若队友也实现HAL UART回调，必须合并并按`huart->Instance`分发；禁止保留两个同名回调。

## 9. Keil工程

- 保留队友`goertzel_sync.c`；
- 加入`analyzer_bridge.c`和`display.c`；
- 保留仓库内`arm_cortexM4lf_math.lib`；
- 保留`__ARM_use_no_argv`；
- 使用ArmClang 6.7和相对DSP路径；
- Clean Rebuild必须为0 errors、0 warnings。

## 10. 回归顺序

```text
上电
→ dashboard初始化并上报两条曲线ID
→ 1T
→ 3T
→ 长按KEY1刷新真实ADC
→ 检查时域、频谱和六项文本
→ 检查FE/FD且无0x12/0x1A/0x24
```

若屏幕无图，先用ST-Link检查CPU Fault、发布序号和USART3状态，不要先重写显示协议。
