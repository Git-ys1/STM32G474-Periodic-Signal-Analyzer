# 下一个 Codex 接手任务书

## 一、当前任务

接手 2026 年电赛 G 题“周期信号测量分析装置”的真实 ADC 联调。
先恢复项目上下文、核对当前源码和实机证据，不要一上来改固件。

当前唯一开发基线：

```text
仓库：F:\Project\stm32G474VETx\TI
项目：F:\Project\stm32G474VETx\TI\G_Periodic_Signal_Analyzer
工程：projects\g474_full_integration_test
发布：v2.1.0（基于v1.9.0 / 9b023b5）
```

V2.1.0普通/Huber折叠已发布：固件已零警告构建并烧录，板上
普通→Huber→普通冒烟无故障，当前模式恢复普通默认。
HMI配置与验证证据见`docs/V2.1_HUBER_FOLD_SWITCH_2026-07-31.md`；不要
覆盖用户后续真实ADC测试文件。

## 二、必须按顺序阅读

1. `docs/HANDOFF/README.md`
2. `docs/HANDOFF/00_PROJECT_MASTER_CONTEXT.md`
3. `docs/HANDOFF/03_ROLES_AND_INTEGRATION_BOUNDARIES.md`
4. `docs/HANDOFF/05_SOFTWARE_ARCHITECTURE_AND_DATAFLOW.md`
5. `docs/HANDOFF/06_DEBUG_HISTORY_AND_FROZEN_DECISIONS.md`
6. `docs/HANDOFF/07_CURRENT_ISSUES_AND_NEXT_ACTIONS.md`
7. `docs/HANDOFF/08_BUILD_FLASH_AND_VALIDATION_RUNBOOK.md`
8. `docs/REAL_ADC_FIRST_INTEGRATION_REVIEW_2026-07-31.md`
9. 当前源码：

```text
projects/g474_full_integration_test/Core/Src/main.c
projects/g474_full_integration_test/Core/Src/analyzer_bridge.c
projects/g474_full_integration_test/Core/Src/display.c
projects/g474_full_integration_test/Core/Inc/analyzer_bridge.h
projects/g474_full_integration_test/Core/Inc/display.h
```

10. 资料侧的 G 题 PDF、官方问答和 `testv2.HMI`：

```text
F:\AcademicHub\000资料相关\电赛\2026TI杯
```

## 三、开始前先执行

```powershell
git -C 'F:\Project\stm32G474VETx\TI' status --short --branch
git -C 'F:\Project\stm32G474VETx\TI' log -n 12 --oneline --decorate
```

保留用户和队友已有修改，不得清理、覆盖或擅自加入提交。

## 四、当前职责与主链路

- 队友负责：ADC1/PA0、TIM3、DMA、2048 点采样、CMSIS-DSP FFT、Vpp/RMS、Goertzel。
- 用户负责：淘晶驰 X2 7 英寸单页显示、USART3、KEY1、`analyzer_bridge`、`display`、融合和实机验证。

当前主链路：

```text
ADC1/PA0 → DMA adc_b[2048] → 队友FFT和幅值计算
→ AnalyzerBridge_PublishReal()
→ 频率细化和256槽相位折叠
→ 可选Huber第二遍折叠（默认关闭，仅改时域波形）
→ Display_Task()
→ 794点时域曲线、频谱线和文本
→ USART3 FE/FD透传 → 淘晶驰dashboard
```

## 五、下一轮第一件事

采集同一帧的配对证据，定位真实 ADC 幅值误差：

```text
信号源设定
+ PA0处示波器Vpp
+ 原始adc_b[2048]
+ 屏幕Upp/Urms/频谱
+ 当前VDDA与换算参数
```

已确认的实际现象是：

- 信号源与 PA0 示波器的 Vpp 通常相差不足 5 mV；
- PA0 示波器与屏幕 Upp 经常相差超过 10 mV。

因此必须先检查 ADC 标定、VDDA、码值换算、采样相位和 `Vpp_Robust()`，不能先修改显示缩放来掩盖误差。

同时记录但暂不混改：

- 纯正弦时可能出现伪谐波；
- V2.0.0已加入并烧录四种显示触发，仍需真实连续帧回归；
- 低分母锁频点的相位覆盖不足；
- 时域曲线已经做淘晶驰水平方向补偿，不能再次反转。

## 六、禁止事项

- 不从 `tjc_display_demo`、旧 `main.c`、V1.4/V1.8 或旧任务书重新融合；
- 不恢复双页面和固定曲线 ID；
- 不删除 `addt → FE → 数据 → FD` 握手；
- 不让显示层替测量层背锅，也不以“图像好看”代替测量准确；
- 不修改队友原始快照；
- 没有原始 ADC 配对证据前，不大改 Vpp、FFT、折叠或插值算法；
- 修改后必须编译，涉及运行行为时必须烧录并实机验收。

## 七、本次接手应交付

1. 一份基于同帧数据的根因判断；
2. 明确问题属于采集、测量、桥接、重建还是显示层；
3. 一份最小修改方案及影响范围；
4. 用户同意后再改代码、编译、烧录和发布。
