# 队友信号分析输出映射

更新时间：2026-07-30  
审计基线：`projects/teammate_adc_newest`  
融合工程：`projects/g474_full_integration_test`

## 一次分析流程

队友最新版以 `Core/Src/main.c` 为算法主体：

```text
ADC2(PA7)
→ TIM3触发
→ DMA1 Channel 2采集2048点adc_b
→ HAL_ADC_ConvCpltCallback置AdcConvEnd
→ ADC码转换为VO/input（单位V）
→ 第一次fft()得到谱峰
→ Vpp_Robust()得到峰峰值
→ Vpp_R()截取整数周期、去直流并计算RMS
→ 重新启动ADC DMA
```

融合工程在 `Vpp_R()` 会再次覆盖谱峰变量这一行为之前，保存第一次
`fft()` 的谱峰；待 `vpp`、`Vrms` 都计算完成后调用
`AnalyzerBridge_PublishReal()`。该调用点就是本版本“一次真实分析完成”的
准确发布位置。

## 变量清单

| 变量 | 类型与作用域 | 定义/更新位置 | 单位 | 含义与有效时刻 |
|---|---|---|---|---|
| `adc_b[2048]` | `uint16_t`，全局 | `main.c`；ADC2 DMA写入 | ADC码 | 一帧原始采样。DMA完成且停止后稳定；重新启动DMA后会再次被覆盖。 |
| `AdcConvEnd` | `__IO uint8_t`，全局 | DMA完成回调置1，主循环清0 | 无 | ADC帧完成标志，不是分析结果有效标志。 |
| `input[]` | `float32_t`，全局 | 主循环和`Vpp_R()` | V，复数交错 | CMSIS CFFT输入工作区，会被FFT原地修改，不能直接交给显示层。 |
| `output[1024]` | `float32_t`，全局 | `fft()` | FFT幅度中间值 | 正频率幅度工作区；找峰时会清除峰附近±8个频点，分析后不再是完整原始频谱。 |
| `VO[2048]` | `float32_t`，全局 | 主循环、`Vpp_R()` | V | 时域电压工作区。`Vpp_R()`会截断尾部并减去直流分量，之后不再等同原始ADC帧。 |
| `F` / `V` | `float`，全局 | `fft()` | Hz / Vpk | 第一次找到的最强谱峰及峰值幅度；若有三个有效峰，`sof()`最终会按频率排序。 |
| `FB` / `VB` | `float`，全局 | `fft()` | Hz / Vpk | 第二个谱峰及峰值幅度。 |
| `FC` / `VC` | `float`，全局 | `fft()` | Hz / Vpk | 第三个谱峰及峰值幅度；`VC < 0.004 V`时队友代码认为只有两个有效峰。 |
| `V0` | `float32_t`，全局 | `fft()` | V | FFT直流分量估计，`Vpp_R()`用它从`VO`中去直流。 |
| `flag` | `uint16_t`，全局 | `fft()` | 无 | 当前代码中值2表示采用两个谱峰，值3表示采用三个谱峰；不是ADC完成标志。 |
| `vpp` | `float`，`main()`局部 | `Vpp_Robust(VO,2048)` | Vpp | 当前分析帧的鲁棒峰峰值；函数对2048点排序后取最低/最高各5点均值之差。 |
| `Vrms` | `float`，`main()`局部 | `Vpp_R()`返回值 | Vrms | 当前分析帧去直流后的真有效值。 |
| `valid_len` | `uint32_t`，全局 | `Vpp_R()` | 点数 | 为RMS截取的整数周期样本数。 |
| `discard_len` | `uint32_t`，全局 | `Vpp_R()` | 点数 | 被排除的尾部样本数。 |
| `max` / `index` | `float32_t` / `uint32_t`，全局 | `fft()` | 中间量 | `arm_max_f32()`的峰值与索引，不是可发布的最终结果。 |
| `TE` | `float`，全局 | 当前未更新 | 未确定 | 最新代码中未参与分析流程，不接入显示。 |

最新版没有发现独立的 `result_flag`、`ready_flag`、FFT完成标志、最终频率
数组或最终幅值数组。`vpp`、`Vrms`仍是 `main()` 局部变量，所以融合工程
没有用大量 `extern` 读取散乱变量，而是在完成点一次性发布。

## 统一接口映射

`AnalyzerBridge_PublishReal()`统一完成以下转换：

| 队友结果 | `AnalyzerResult` | 转换 |
|---|---|---|
| `F/V`、`FB/VB`、`FC/VC`的第一次FFT快照 | `components[]` | 频率保持Hz；峰值幅度从V乘1000转换为mV；再按频率升序排列。 |
| 最低有效谱峰频率 | `fundamental_hz` | 取排序后的第一个分量。 |
| `vpp` | `vpp_mv` | V乘1000。 |
| `Vrms` | `vrms_mv` | V乘1000。 |
| `flag` | `component_count`、`status_flags` | 值2/3决定采用两个/三个峰；原值保存在状态字低8位。 |
| `adc_b` | `waveform_mv[256]` | 只抽取一个周期并线性重采样为256点，减去均值；不复制完整2048点工作区。 |

真实模式的频谱显示直接使用队友第一次FFT产生的谱峰快照，不再执行显示侧
DFT。测试模式则注入结构化最终结果，两种模式最终都经过同一个
`AnalyzerResult`和显示路径。

