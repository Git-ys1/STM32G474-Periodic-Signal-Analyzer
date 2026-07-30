# G题周期信号测量分析装置

本仓库保存2026年电赛G题的队友采集/分析快照、淘晶驰显示基线和当前可烧录的全量融合工程。主控为STM32G474VET6，显示器为淘晶驰X2系列7英寸串口屏TJC8048X270_011R。

## 当前版本

当前融合版本为 **V1.9.0队友ADC1底座融合版（2026-07-31）**。

本版遵循“队友工程作为底座、我们的显示功能作为叠加层”：

- 队友底座：ADC1（PA0/IN1）、TIM3 TRGO、DMA1 Channel 1、2048点采集、CMSIS-DSP FFT、Vpp、RMS和Goertzel源码；
- 我们叠加：`AnalyzerBridge`、256槽完整缓冲区相位折叠、淘晶驰dashboard显示、USART3协议和KEY1控制；
- KEY1短按复用原1T/3T命令，长按复用原“刷新”命令，恢复真实ADC自动显示；
- 保留第一次FFT谱峰快照，避免`Vpp_R()`二次FFT覆盖对外结果；
- 保留`__ARM_use_no_argv`、静态大对象、UART中断接收、动态曲线ID和FE/FD握手等已验证修复；
- Keil ArmClang 6.7 Clean Build通过：0 errors、0 warnings；
- 固件已通过ST-Link下载、校验和复位；
- 运行态发布序号3秒内由`0x041A`增长至`0x0435`，CFSR/HFSR均为0。

详细说明见：

- [V1.9队友ADC1底座融合发布说明](docs/V1.9_ADC1_TEAMMATE_BASE_INTEGRATION.md)
- [当前开发状态](docs/CURRENT_STATUS.md)
- [队友工程双向融合速查](docs/TEAMMATE_INTEGRATION_QUICK_GUIDE.md)
- [队友输出映射](docs/TEAMMATE_OUTPUT_MAP.md)
- [CMSIS-DSP依赖审计](docs/DSP_DEPENDENCY_AUDIT.md)
- [V1.8波形实验室与时域方向修复](docs/V1.8_WAVEFORM_LAB_AND_TIME_DIRECTION_RELEASE.md)

## 目录

```text
projects/
├─ tjc_display_demo/              独立显示回归基线
├─ teammate_adc_reference/        队友早期参考快照
├─ teammate_adc_newest/           队友上一版原始快照
├─ teammate_adc_reallynewest!/    本次ADC1/Goertzel原始快照
└─ g474_full_integration_test/    当前V1.9全量融合与烧录工程
```

队友原始快照独立归档，不在原目录内做显示融合。正式运行与后续联调以`g474_full_integration_test`为唯一融合工程。

## 当前主链路

```text
TIM3 TRGO
→ ADC1 PA0/IN1（2倍过采样）
→ DMA1 Channel 1采集adc_b[2048]
→ 队友fft()提取2～3个谱峰（±8频点RSS幅值）
→ 立即快照F/V、FB/VB、FC/VC
→ 队友Vpp_Robust()与Vpp_R()
→ AnalyzerBridge_PublishReal(adc_b, ...)
→ 相关搜索细化折叠频率
→ 2048个真实ADC点折叠到256个相位槽
→ AnalyzerResult稳定快照
→ Display_Task()
→ cle/addt + FE/FD握手
→ USART3 PC10/PC11，115200 bit/s
→ 淘晶驰dashboard
```

队友新增的三路Goertzel已在真实`VO[2048]`上计算并保留，但本版尚未用它替换对外显示幅值；在实机标定完成前，频谱文本仍沿用队友FFT的结果口径。

## 冻结边界

- 不反向修改任何队友原始快照；
- 不把ADC/DMA/FFT算法塞入`display.c`；
- 不恢复“只截第一个周期再拉伸”的旧时域路径；
- 不改变已稳定的HMI协议、曲线ID上报、FE/FD握手和时域方向补偿；
- 后续实际信号联调从V1.9.0继续，先验证ADC1输入、频率/幅值误差和相位覆盖，再决定是否让Goertzel结果成为正式输出。
