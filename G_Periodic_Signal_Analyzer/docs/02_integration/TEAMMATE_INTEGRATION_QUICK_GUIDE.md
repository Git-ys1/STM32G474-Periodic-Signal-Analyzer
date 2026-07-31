# V2.5队友工程融合速查

适用基线：队友`4096-2`工程，已经保留此前融合好的显示代码。

## 先看结论

- 不允许用我方`main.c`覆盖队友工程；
- 可直接覆盖的只有`analyzer_bridge.c/.h`，本轮`display.c/.h`没有变化；
- `main.c`只手工改下面三处；
- 队友`Vpp_Robust()`、ADC、DMA、TIM3和FFT主体不由我方重写；
- 队友生产版保持KEY1长按刷新，随机测试关闭。

最小交接包：`deliverables/V2.5更新包.zip`。

## 1. 覆盖桥接层

| 包内文件 | 覆盖到队友工程 |
|---|---|
| `Core/Inc/analyzer_bridge.h` | `Core/Inc/analyzer_bridge.h` |
| `Core/Src/analyzer_bridge.c` | `Core/Src/analyzer_bridge.c` |

确认头文件中：

```c
#define ANALYZER_TEST_ENABLE          0U
#define ANALYZER_CUSTOM_TEST_ENABLE   0U
```

## 2. main.c手工修改

### 2.1 采样率宏

```c
/* 删除旧的1024090和ADC_VOLTS_PER_CODE两行，保留： */
#define ANALYZER_SAMPLE_RATE_HZ  2048193.0f
```

队友原有的VO生成保持不动：

```c
for (int i=0; i<2048; i++)
{
    VO[2*i]   = adc_b[i]  * 3.3 / 4096.0;
    VO[2*i+1] = adc_b1[i] * 3.3 / 4096.0;
}
```

### 2.2 桥接调用

在`Vpp_R()`之前准备完整VO，在它之后提交Vpp/RMS：

```c
GoertzelResult r=goertzel_sync(VO,4096,ANALYZER_SAMPLE_RATE_HZ,F),rB=goertzel_sync(VO,4096,ANALYZER_SAMPLE_RATE_HZ,FB),rC=goertzel_sync(VO,4096,ANALYZER_SAMPLE_RATE_HZ,FC);

AnalyzerBridge_PrepareReal(
    VO,
    ADC_SIZE,
    ANALYZER_SAMPLE_RATE_HZ,
    spectrum_flag,
    spectrum_frequencies_hz,
    spectrum_amplitudes_v
);

float Vrms=Vpp_R();
AnalyzerBridge_PublishPreparedReal(vpp, Vrms);
```

旧的`AnalyzerBridge_PublishReal(adc_b, ADC_SIZE, ADC_VOLTS_PER_CODE, ...)`
整段删除。它会把只有2048点的`adc_b`当4096点读取，而且重复做电压换算。

### 2.3 条件括号

```c
if(VC<0.00477 || (FC<FB && FC<F))
```

只补括号以消除Arm Compiler 6.7警告，不改变原逻辑。

## 3. KEY1生产配置

队友工程保持：

```c
if (held_ms >= BOARD_KEY_LONG_PRESS_MS)
{
    Display_RequestRefresh();
}
else
{
    Display_TogglePeriods();
}
```

本地屏幕测试版长按为`Display_RequestTest()`，不要把该差异复制给队友。

## 4. 验收

1. Clean Rebuild必须为`0 Error(s), 0 Warning(s)`；
2. 同一信号同时接PA0和PA1；
3. 确认两路DMA都完成、`VO[0]`来自ADC1且`VO[1]`来自ADC2；
4. 确认桥接发布序号持续增长，CFSR/HFSR为0；
5. 实屏核对Upp、Mpp、Urms、频谱和1T/3T；
6. 保存同帧`VO[4096]`，不要再只导出单路`adc_b`后判断显示算法。
