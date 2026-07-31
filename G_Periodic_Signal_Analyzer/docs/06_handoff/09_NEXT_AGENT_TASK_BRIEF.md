# 下一个Codex接手任务书

## 当前基线

```text
仓库：F:\Project\stm32G474VETx\TI
项目：G_Periodic_Signal_Analyzer
工程：firmware
发布：v2.5.0
队友底座：teammate/current（4096-2）
```

当前采集是ADC1/PA0与ADC2/PA1各2048点，交错为`VO[4096]`，等效约
2.048193 MS/s。桥接层直接读取VO。不要从旧2048点单ADC教程恢复
`AnalyzerBridge_PublishReal(adc_b,...)`。

## 开工顺序

1. 读根`README.md`和本目录`README.md`；
2. 读`docs/04_releases/V2.5_DUAL_ADC_4096_VO_BRIDGE_2026-08-01.md`；
3. 检查Git状态，保留用户未提交文件；
4. 打开`firmware/Core/Src/main.c`、`analyzer_bridge.c`、`display.c`；
5. 确认本地KEY1长按测试、队友生产包长按刷新；
6. 改代码后必须零警告构建，涉及运行行为必须烧录并查Fault。

## 当前接口

```c
AnalyzerBridge_PrepareReal(VO, ADC_SIZE, ANALYZER_SAMPLE_RATE_HZ,
                           spectrum_flag,
                           spectrum_frequencies_hz,
                           spectrum_amplitudes_v);
float Vrms=Vpp_R();
AnalyzerBridge_PublishPreparedReal(vpp, Vrms);
```

Prepare必须位于`Vpp_R()`前。队友包不含`main.c`，任何main修改都必须写出
原代码、修改后代码和原因。

## 下一项实测

把同一信号同时送入PA0和PA1，保存信号源、双通道示波器、两路原始DMA、
`VO[4096]`及屏幕结果。先审计双ADC交错和FFT频响校正，再讨论Vpp替代方案。

不得修改队友旧`Vpp_Robust()`，除非用户收到队友新函数并明确要求。
