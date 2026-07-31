# 职责与融合边界

## 队友负责

- ADC1/PA0、ADC2/PA1、DMA1 Channel 1/2和TIM3双沿触发；
- 两路各2048点采集并生成`VO[4096]`；
- 4096点CMSIS-DSP FFT、频响分段校正、谱峰筛选；
- `Vpp_Robust()`、`Vpp_R()`和Goertzel。

## 用户负责

- `analyzer_bridge.c/.h`和`display.c/.h`；
- 直接消费VO的相位折叠、Huber、谐波投影和独立Mpp；
- `AnalyzerResult`稳定快照；
- USART3、淘晶驰协议、动态曲线ID和FE/FD握手；
- dashboard、坐标轴、文本和KEY1交互；
- 融合、构建、烧录、实机验证和发布。

## 文件规则

| 文件类型 | 交给队友时 |
|---|---|
| `Core/Src/analyzer_bridge.c` | 可覆盖 |
| `Core/Inc/analyzer_bridge.h` | 可覆盖 |
| `Core/Src/display.c`、`Core/Inc/display.h` | 仅有显示改动时可覆盖 |
| `main.c` | 禁止覆盖，只给定位、原代码、新代码块 |
| ADC/DMA/TIM/GPIO/IRQ/CubeMX文件 | 禁止覆盖，除非队友明确要求 |
| `Vpp_Robust()`等队友算法 | 不由显示负责人擅改 |

当前`main.c`只需手工维护：采样率宏、VO准备调用、Vpp/RMS提交调用和KEY1差异。
完整代码块见`deliverables/V2.5更新包/V2.5更新摘要.md`。
