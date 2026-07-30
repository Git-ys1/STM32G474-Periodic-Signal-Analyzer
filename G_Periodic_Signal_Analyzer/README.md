# G题周期信号测量分析装置

本仓库保存2026年电赛G题的显示基线、队友信号处理快照和当前可烧录的全量融合工程。主控为STM32G474VET6，显示器为淘晶驰X2系列7英寸串口屏TJC8048X270_011R。

## 当前冻结版本

当前阶段基线为 **V1.4.1实体按键兼容版（2026-07-30）**：

- ADC2、TIM3、DMA、2048点FFT、Vpp与RMS沿用队友工程；
- `AnalyzerBridge`把真实结果和测试结果统一成稳定快照；
- `display.c`负责dashboard单页面、时域、频谱、文本与HMI协议；
- USART3使用PC10/PC11、115200 bit/s，接收采用中断，绘图仍在主循环；
- 不可触摸屏使用板载KEY1：短按切换1T/3T，长按执行原“测试”命令；
- KEY1与原HMI按钮共用同一个按钮命令处理入口，不维护第二套显示逻辑；
- 已通过Keil Clean Build、ST-Link运行态检查和HMI模拟器实测；
- 已验证1T、3T、刷新、测试按钮、复合时域波形、三根频谱线和六项文本。

冻结说明见：

- [V1.4融合稳定基线](docs/V1.4_FUSION_BASELINE_FREEZE_2026-07-30.md)
- [V1.4.1实体按键兼容版](docs/V1.4.1_KEY1_RELEASE_2026-07-30.md)
- [V1.4当前融合架构](docs/V1.4_INTEGRATION_ARCHITECTURE.md)
- [不可触摸屏KEY1实体按键控制](docs/KEY1_PHYSICAL_CONTROL.md)
- [当前开发状态](docs/CURRENT_STATUS.md)

## 目录

```text
projects/
├─ tjc_display_demo/             已验证的V1.3.1独立显示基线
├─ teammate_adc_reference/       队友早期参考快照
├─ teammate_adc_newest/          2026-07-30队友最新原始快照
└─ g474_full_integration_test/   当前V1.4全量融合与烧录工程
```

## 当前主链路

```text
ADC2 + TIM3 TRGO + DMA循环采集
→ 队友fft()提取2～3个谱峰
→ 立即快照谱峰，避免Vpp_R()二次fft()覆盖
→ 队友Vpp与真RMS计算
→ AnalyzerBridge_PublishReal()
→ AnalyzerResult稳定快照
→ Display_Task()
→ cle/addt + FE/FD握手
→ USART3
→ 淘晶驰dashboard
```

测试按钮走同一显示路径，只在分析结果层注入一致的波形、Vpp、RMS与谱峰，不伪造ADC原始数据。

## 工程边界

- `tjc_display_demo`和两个队友快照作为历史/对照工程冻结，不被融合工程反向覆盖；
- 正式开发以`g474_full_integration_test`为当前底座；
- HMI通信、FE/FD握手和页面协议不再随意重构；
- 下一阶段以真实输入标定、算法微调和最终实物屏回归为主。
