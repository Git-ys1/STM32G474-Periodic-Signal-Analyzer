# 当前开发状态

更新时间：2026-08-01

当前发布：V2.5.0双ADC 4096点VO浮点桥接版

唯一主线：`firmware/`
队友原始底座：`teammate/current/`，来源`4096-2`

## 当前冻结口径

| 项目 | 当前事实 |
|---|---|
| MCU | STM32G474VET6 |
| 采集 | ADC1/PA0上升沿与ADC2/PA1下降沿各2048点 |
| 完整帧 | 队友在`main.c`中交错并换算为`VO[4096]`浮点电压 |
| 等效采样率 | 约2.048193 MS/s |
| FFT | 4096点，约500 Hz/bin |
| 桥接输入 | 直接读取`VO[4096]`，不再读取原始ADC码或重复换算 |
| 时域显示 | 256点相位模型映射为512像素，支持1T/3T |
| 频谱显示 | 256像素，0至500 kHz |
| 本地KEY1 | 短按切换1T/3T，长按随机测试 |
| 队友生产KEY1 | 短按切换1T/3T，长按刷新，测试关闭 |

## 当前主链

```text
TIM3 CH4上升/下降沿
→ ADC1/PA0 + ADC2/PA1
→ adc_b[2048] + adc_b1[2048]
→ 队友生成VO[4096]浮点电压
→ 4096点FFT、Vpp_Robust、Goertzel
→ AnalyzerBridge_PrepareReal(VO, ...)
→ 4096点频率细化、相位折叠、Huber、谐波投影和模型Mpp
→ 队友Vpp_R()
→ AnalyzerBridge_PublishPreparedReal(vpp, Vrms)
→ AnalyzerResult稳定快照
→ Display_Task()
→ USART3 → 淘晶驰dashboard
```

采用两阶段桥接是因为队友`Vpp_R()`会原地去直流并清零`VO`尾部。桥接必须在
它之前同步读取完整VO，随后再提交Vpp/RMS；显示层只读取完整快照。

## V2.5验证

```text
Arm Compiler 6.7
0 Error(s), 0 Warning(s)
Code=79364
RO-data=75996
RW-data=172
ZI-data=116556
ADC.hex长度：437530 bytes
SHA-256：CEFA87F1953CEDE198AE476043E0E169BF66CAD7D808335D8B1743A545DB86C2
```

固件已通过STM32CubeProgrammer写入、校验并复位。队友生产变体同样完成
`0 Error(s), 0 Warning(s)`构建（`Code=78620`），交接包见
`deliverables/V2.5更新包.zip`。

## 仍需实测

- 同一信号必须同时送入PA0和PA1，并确认双路前端增益、偏置和相位匹配；
- 同步保存信号源、PA0/PA1示波器、`VO[4096]`、屏幕`Upp/Mpp/Urms`和VDDA；
- 用10至500 kHz、50至250 mVpp覆盖频率和幅值范围；
- 队友旧`Vpp_Robust()`本轮没有修改，等待队友后续替换口径；
- `4096-2`使用频率分段校正FFT幅值，仍需实物标定确认各分段边界。

V1.x至V2.4的2048点内容是对应版本的历史事实，保留在`docs/04_releases/`和
历史验证文档中，不再作为当前实现口径。下一位代理必须先读
[`docs/06_handoff/README.md`](../06_handoff/README.md)。
