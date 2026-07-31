# 项目总上下文

## 项目目标

2026年电赛G题周期信号测量分析装置，主控STM32G474VET6，显示为淘晶驰X2
系列7英寸800×480串口屏。屏幕实物不可触摸，因此保留HMI协议并使用KEY1。

## 当前权威状态

| 项目 | 当前事实 |
|---|---|
| 发布 | `v2.5.0` |
| 主线 | `firmware/` |
| 队友快照 | `teammate/current/`，`4096-2` |
| 采集 | ADC1/PA0与ADC2/PA1各2048点 |
| 完整输入 | `VO[4096]`浮点电压 |
| 采样率 | 约2.048193 MS/s |
| FFT | 4096点、约500 Hz/bin |
| 时域/频谱 | 512×256 / 256×256 |

## 当前关键接口

```c
AnalyzerBridge_PrepareReal(VO, 4096, sample_rate_hz,
                           flag, frequencies_hz, amplitudes_v);
/* 队友Vpp_R()在这里运行并修改VO */
AnalyzerBridge_PublishPreparedReal(vpp, Vrms);
```

桥接不再接收`adc_b`或`volts_per_code`。大数组保持静态；真实VO不在桥接层
重复缓存，折叠模式切换会由下一帧真实数据重建。

## 禁止回退

- 不从旧V1.4/V1.8、旧`main.c`、双页面或`tjc_display_demo`重新融合；
- 不恢复固定曲线ID，继续由dashboard初始化帧动态上报；
- 不删除`addt → FE → 数据 → FD`握手；
- 不让显示平滑替代FFT、Vpp或模拟前端正确性；
- 不用我方`main.c`覆盖队友工程；
- 不把历史2048点验证文档当成当前4096点接口说明。
