# V2.4.0队友工程融合速查

适用底座：队友完整 CubeMX/Keil 工程，当前仓库参考快照为
`teammate/current`。

## 先看结论

- 始终以队友工程为底座，保留其 ADC1、TIM3、DMA、FFT、Vpp/RMS 和
  `goertzel_sync`；
- 只叠加 `analyzer_bridge.c/.h`、`display.c/.h`，不要互相覆盖整份
  `main.c`；
- V2.2 没有改变 `AnalyzerBridge_PublishReal()` 接口，旧融合插入点不变；
- V2.4实验模型Vpp仍未改变发布接口，也不要求队友改`main.c`；它完全封装在
  `analyzer_bridge.c/.h`并由`display.c`写入独立文本`t_vpp2`；
- V2.2 的 Huber 模式已升级为“两遍 Huber + FFT 已识别整数谐波投影”，
  只平滑显示波形，不改变队友测得的频率、Vpp、RMS 和谱线；
- 发给队友的生产包关闭随机测试，上电默认 Huber；
- 队友 KEY1 短按切换 1T/3T，长按刷新真实 ADC；长按测试只保留在我们的
  本地验证工程。

禁止从 `tjc_display_demo`、旧 `main.c` 或旧任务书重新拼接。

## 1. 交付包内容

把交付包内文件按原目录覆盖或加入队友工程：

```text
Core/Inc/analyzer_bridge.h
Core/Inc/display.h
Core/Src/analyzer_bridge.c
Core/Src/display.c
README_队友融合.md
```

交付包中的 `analyzer_bridge.h` 已将：

```c
#define ANALYZER_TEST_ENABLE 0U
```

因此不需要复制 `generated_adc_tests.h` 和
`generated_custom_adc_tests.h`，也不会把我们的随机测试数据编进队友固件。

如果不是使用交付包，而是直接从
`firmware` 复制源码，必须手工把上面的宏从
`1U` 改成 `0U`。

## 2. 硬件和 CubeMX

```text
PC10 / USART3_TX → 屏幕 RX
PC11 / USART3_RX ← 屏幕 TX
MCU GND          ─ 屏幕 GND
外部 5 V         → 屏幕 5 V
PB8 / KEY1       → 板载按键，高电平有效
```

USART3 必须为：

```text
115200 baud / 8 data bits / 1 stop bit / no parity
TX_RX / no flow control
PC10 AF7 TX / PC11 AF7 RX
USART3 global interrupt enabled
```

PB8 配置为 GPIO EXTI 上升沿。当前队友参考底座已经具备 USART3；若其新版
CubeMX 工程也保留这些配置，不要重复重建外设。

## 3. Keil 工程

在 `Application/User/Core` 组加入：

```text
Core/Src/analyzer_bridge.c
Core/Src/display.c
```

并确认：

```text
Core/Inc
../../Drivers/CMSIS/DSP/Include
```

位于 Include Path。保留队友的 `goertzel_sync.c`、仓库内
`arm_cortexM4lf_math.lib` 和 ArmClang 6.7 配置。不得使用个人电脑的
绝对路径，也不得 `#include "*.c"`。

## 4. main.c 头部

以下内容分别放入 CubeMX 对应 `USER CODE` 区，不覆盖队友已有内容。

```c
/* USER CODE BEGIN Includes */
#include "analyzer_bridge.h"
#include "display.h"
#include "goertzel_sync.h"
/* USER CODE END Includes */
```

```c
/* USER CODE BEGIN PD */
#define ANALYZER_SAMPLE_RATE_HZ  1024090.0f
#define ADC_VOLTS_PER_CODE       (3.3f / 4096.0f)
#define BOARD_KEY_CONTROL_ENABLE 1U
#define BOARD_KEY_DEBOUNCE_MS    30U
#define BOARD_KEY_LONG_PRESS_MS  1000U
/* USER CODE END PD */
```

`ANALYZER_SAMPLE_RATE_HZ` 必须换成队友当前真实采样率；如果 ADC 参考电压
不是 3.3 V，也必须同步修改 `ADC_VOLTS_PER_CODE`。

```c
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

`__ARM_use_no_argv` 必须保留，否则当前 ArmClang 裸机运行时可能在进入
`main()` 前触发 `BKPT 0xAB`。

## 5. KEY1：队友版长按刷新

在 `USER CODE BEGIN 0` 加入：

```c
#if BOARD_KEY_CONTROL_ENABLE
void HAL_GPIO_EXTI_Callback(uint16_t pin)
{
    if ((pin == KEY_Pin) &&
        (s_key_press_active == 0U) &&
        (HAL_GPIO_ReadPin(KEY_GPIO_Port, KEY_Pin) == GPIO_PIN_SET))
    {
        s_key_press_tick = HAL_GetTick();
        s_key_press_active = 1U;
    }
}

static void BoardKey_Task(void)
{
    uint32_t held_ms;

    if ((s_key_press_active == 0U) ||
        (HAL_GPIO_ReadPin(KEY_GPIO_Port, KEY_Pin) == GPIO_PIN_SET))
    {
        return;
    }

    held_ms = HAL_GetTick() - s_key_press_tick;
    s_key_press_active = 0U;

    if (held_ms < BOARD_KEY_DEBOUNCE_MS)
    {
        return;
    }

    if (held_ms >= BOARD_KEY_LONG_PRESS_MS)
    {
        Display_RequestRefresh();
    }
    else
    {
        Display_TogglePeriods();
    }
}
#endif
```

若队友已有 `HAL_GPIO_EXTI_Callback()`，只能把 `KEY_Pin` 分支并入原回调，
不能再定义第二个同名函数。UART 发送和绘图不能放进中断。

## 6. 初始化插入点

队友参考底座外设初始化顺序为：

```c
MX_GPIO_Init();
MX_DMA_Init();
MX_DAC1_Init();
MX_USART3_UART_Init();
MX_TIM3_Init();
MX_ADC1_Init();
```

在 ADC1 校准、TIM3 启动、ADC1 DMA 启动之后加入：

```c
HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED);
HAL_TIM_Base_Start(&htim3);
HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc_b, ADC_SIZE);

AnalyzerBridge_Init();
Display_Init(&huart3);
```

V2.2 源码在 `AnalyzerBridge_Init()` 后默认进入 Huber 平滑，不依赖触摸屏
发送模式切换命令。

## 7. 分析完成插入点

第一次 `fft();` 之后立刻保存谱峰。队友的 `Vpp_R()` 内部还会再执行一次
FFT，若晚保存，`F/V/FB/VB/FC/VC/flag` 会被覆盖。

```c
float spectrum_frequencies_hz[ANALYZER_MAX_COMPONENTS];
float spectrum_amplitudes_v[ANALYZER_MAX_COMPONENTS];
uint8_t spectrum_flag;

fft();

spectrum_frequencies_hz[0] = F;
spectrum_frequencies_hz[1] = FB;
spectrum_frequencies_hz[2] = FC;
spectrum_amplitudes_v[0] = V;
spectrum_amplitudes_v[1] = VB;
spectrum_amplitudes_v[2] = VC;
spectrum_flag = (uint8_t)flag;
```

完成 `vpp` 和 `Vrms` 后、清除完成标志和重启 ADC1 DMA 前发布：

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

AdcConvEnd = 0;
HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc_b, ADC_SIZE);
```

必须传原始 `adc_b[2048]`，不能传 FFT 数组、已经排序的电压数组或 256 点
显示数组。桥接层会自己用 2048 点完成相关频率细化、相位折叠、Huber 和
谐波投影。

接口单位固定：

```text
adc_b                 ADC原始码
volts_per_code        V/code
sample_rate_hz        Hz
vpp、Vrms             V
频率数组              Hz
幅值数组              Vpk
```

## 8. 主循环末尾

无论本轮有没有新 ADC 数据，都必须运行：

```c
#if BOARD_KEY_CONTROL_ENABLE
BoardKey_Task();
#endif
Display_Task();
```

不要把 `Display_Task()` 只放进 `if (AdcConvEnd == 1)`，否则串口接收、
按键动作和定时刷新都会失效。

## 9. HAL 回调冲突

`display.c` 已实现：

```c
HAL_UART_RxCpltCallback()
HAL_UART_ErrorCallback()
```

若队友新版也实现同名回调，必须合并成一个函数并按
`huart->Instance` 分发；禁止保留两份同名实现。ADC 完成回调和 GPIO EXTI
回调同理。

## 10. V2.2 相比旧教程到底变了什么

| 项目 | 旧版 | 队友生产融合版 V2.2 |
|---|---|---|
| 发布接口 | `AnalyzerBridge_PublishReal()` | 不变 |
| ADC/FFT/Vpp/RMS | 队友原实现 | 不变 |
| Huber侧波形 | 两遍 Huber | 两遍 Huber + 已识别谐波投影 |
| 上电折叠模式 | 普通 | Huber |
| KEY1短按 | 1T/3T | 不变 |
| KEY1长按 | 刷新 | 刷新 |
| 随机测试数据 | 可编译 | 队友包关闭，不携带 |
| 需要替换的模块 | 4个文件 | 仍是4个文件，但 `analyzer_bridge.c/.h` 必须成对更新 |

所以队友不需要改其采样和 FFT 架构，只需用新 4 文件替换旧显示模块，并
保留上述 `main.c` 插入点。

## 11. 最短验收顺序

```text
Clean Rebuild：0 errors、0 warnings
→ 上电自动显示真实ADC，模式为Huber
→ dashboard上报两条动态曲线ID
→ KEY1短按：1T/3T切换
→ KEY1长按：立即刷新真实ADC，不进入随机测试
→ 检查512点时域、256点频谱、动态坐标和测量文本
→ 检查FE/FD透传且无0x12/0x1A/0x24
→ 检查CFSR/HFSR均为0
```

若屏幕无图，先查 CPU Fault、USART3、动态曲线 ID 和
`AnalyzerBridge_PublishReal()` 的发布序号，不要先重写显示协议。

## 12. V2.4模型Vpp怎么接

V2.4尚待实屏三方对照，当前不要删除或替换队友`Vpp_Robust()`。需要试用时：

1. 成对复制最新`analyzer_bridge.c/.h`，再复制最新`display.c`；
2. 保留原来的`AnalyzerBridge_PublishReal()`调用，参数和插入点不变；
3. 按[`HMI_COMPACT_LAYOUT_DRAFT.md`](../01_architecture/HMI_COMPACT_LAYOUT_DRAFT.md)
   增加`t_vpp2`、动态坐标、512点时域、
   256点频谱和`sw_period`；
4. 确认队友`Upp`的最新前端增益宏只除以6一次；桥接`Mpp`内部已经按
   `ANALYZER_FRONTEND_VOLTAGE_GAIN=6.0f`折算，不能在外面再除一次；
5. 同屏记录信号源/示波器Vpp、`Upp`和`Mpp`，验证通过后再决定是否替换口径。

真正独立的计算入口是：

```c
AnalyzerBridge_CalculateRobustModelVpp(...);
```

正常融合无需在`main.c`直接调用它；`AnalyzerBridge_BuildRealWaveform()`会在桥接
内部自动计算并写入`AnalyzerResult.model_vpp_mv`。若前端重新标定，只覆盖
`ANALYZER_FRONTEND_VOLTAGE_GAIN`宏，不要改队友采集或FFT流程。
