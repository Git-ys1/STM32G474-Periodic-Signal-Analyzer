# 队友信号分析输出映射

更新时间：2026-07-31
审计基线：`teammate/current`
融合工程：`firmware`

## 一次真实分析流程

```text
ADC1 PA0/IN1
→ TIM3 TRGO
→ DMA1 Channel 1采集adc_b[2048]
→ ADC码转换为VO/input（V）
→ 第一次fft()
→ 立即快照F/V、FB/VB、FC/VC
→ Vpp_Robust()
→ 三路goertzel_sync()
→ Vpp_R()与第二次fft()
→ AnalyzerBridge_PublishReal()
→ 重新启动ADC1 DMA
```

发布点必须位于DMA重新启动前，保证`adc_b`仍是当前稳定帧。

## 主要变量

| 变量 | 单位 | 当前含义 |
|---|---:|---|
| `adc_b[2048]` | ADC码 | ADC1 DMA原始帧，显示波形的真实数据源 |
| `VO[2048]` | V | 去偏置前后的时域工作区 |
| `F/V` | Hz / Vpk | 第一条FFT谱峰 |
| `FB/VB` | Hz / Vpk | 第二条FFT谱峰 |
| `FC/VC` | Hz / Vpk | 第三条FFT谱峰 |
| `flag` | 无 | 当前采用2或3条谱峰 |
| `vpp` | Vpp | 排序后第1～10个低/高样本均值差 |
| `Vrms` | Vrms | 整数周期、去直流后的真有效值 |
| `goertzel_a/b/c` | 结构体 | 在三个FFT频率上计算的Goertzel结果，本版未发布 |

队友最新版FFT幅值不是单频点最大值，而是峰附近±8个频点的平方和开方后乘`2/N`。

## 发布映射

| 队友结果 | `AnalyzerResult` | 转换 |
|---|---|---|
| 第一次FFT的F/V、FB/VB、FC/VC快照 | `components[]` | Hz保持；Vpk乘1000转mVpk；按频率升序 |
| 最低有效谱峰 | `fundamental_hz` | 作为对外基频与折叠初值 |
| `vpp` | `vpp_mv` | V乘1000 |
| `Vrms` | `vrms_mv` | V乘1000 |
| `flag` | `component_count/status_flags` | 决定采用2或3条分量 |
| `adc_b[2048]` | `waveform_mv[256]` | 所有实采点参与相关细化和相位折叠 |

## 波形路径

`AnalyzerBridge_BuildRealWaveform()`当前执行：

1. ADC码转mV并去均值；
2. 以队友基频为中心做相关搜索和抛物线细化；
3. 用细化频率计算2048个采样点的相位；
4. 线性分配到256个环形相位槽；
5. 对没有覆盖的槽用左右最近有效槽插值；
6. 再次去均值并锁存。

这不是首周期截取，也不是依据频率/幅值合成波形。

## Goertzel边界

融合版保留队友三路Goertzel计算，但当前故意不把它替换成正式显示幅值，原因是：

- 队友原始版同样未消费结果；
- FFT幅值已改变为±8频点RSS，二者量纲和标定口径需实测；
- 未经信号源校准直接替换会造成“融合成功但测量口径悄然变化”。

实际ADC联调完成后，再决定发布Goertzel幅值、频率或仅作为置信度/校验量。
