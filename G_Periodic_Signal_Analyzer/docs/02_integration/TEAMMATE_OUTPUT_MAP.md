# 队友信号分析输出映射

更新时间：2026-08-01

审计基线：`teammate/current`，来源`4096-2`
融合工程：`firmware`

## 一次真实分析流程

```text
TIM3 CH4上升/下降沿
→ ADC1/PA0 DMA得到adc_b[2048]
→ ADC2/PA1 DMA得到adc_b1[2048]
→ 交错并换算为VO[4096]浮点电压
→ 第一次4096点fft()
→ 立即快照F/V、FB/VB、FC/VC
→ Vpp_Robust(VO,4096)
→ 三路goertzel_sync(VO,4096,...)
→ AnalyzerBridge_PrepareReal(VO,...)
→ Vpp_R()原地处理VO并再次fft()
→ AnalyzerBridge_PublishPreparedReal(vpp,Vrms)
→ 重新启动双ADC DMA
```

准备调用必须位于`Vpp_R()`之前，因为它会去直流并清零VO尾部。最终发布必须
位于DMA重新启动前，保证本次测量和已准备波形属于同一帧。

## 主要变量

| 变量 | 单位 | 当前含义 |
|---|---:|---|
| `adc_b[2048]` | ADC码 | ADC1/PA0上升沿原始半帧 |
| `adc_b1[2048]` | ADC码 | ADC2/PA1下降沿原始半帧 |
| `VO[4096]` | V | 队友交错并换算后的完整浮点电压帧，也是桥接真实输入 |
| `F/V` | Hz / Vpk | 第一条FFT谱峰 |
| `FB/VB` | Hz / Vpk | 第二条FFT谱峰 |
| `FC/VC` | Hz / Vpk | 第三条FFT谱峰 |
| `flag` | 无 | 当前采用2或3条谱峰 |
| `vpp` | Vpp | 队友`Vpp_Robust()`结果，本轮未修改 |
| `Vrms` | Vrms | 队友整数周期真有效值 |
| `r/rB/rC` | 结构体 | 三个FFT频率上的Goertzel结果，本版仍未发布 |

`4096-2`先按频率区间修正FFT幅值数组，再在峰值附近±4个频点做平方和开方并
乘`2/N`。该逻辑属于队友测量层，桥接层只复制结果。

## 发布映射

| 队友结果 | `AnalyzerResult` | 转换 |
|---|---|---|
| 第一次FFT的三路快照 | `components[]` | Hz保持；Vpk乘1000转mVpk；按频率升序 |
| 最低有效谱峰 | `fundamental_hz` | 对外基频与折叠初值 |
| `vpp` | `vpp_mv` | V乘1000 |
| `Vrms` | `vrms_mv` | V乘1000 |
| `flag` | `component_count/status_flags` | 决定采用2或3条分量 |
| `VO[4096]` | `waveform_mv[256]` | 全部浮点样本参与细化、折叠、Huber和模型拟合 |

桥接层不再读取原始ADC码，也不再执行`3.3/4096`。测试模式的历史uint16数组
只在测试入口换算一次，随后复用同一浮点波形链。

## Goertzel边界

三路Goertzel仍只计算不显示。FFT已加入频率分段校正，Goertzel尚未完成相同的
幅值口径标定；没有同帧信号源、示波器和VO证据前，不得直接替换正式频谱值。
