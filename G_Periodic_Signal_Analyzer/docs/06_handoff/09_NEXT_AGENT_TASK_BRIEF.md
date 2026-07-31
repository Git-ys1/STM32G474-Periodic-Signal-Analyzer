# 下一个 Codex 接手任务书

## 一、当前任务

接手 2026 年电赛 G 题“周期信号测量分析装置”的真实 ADC 联调。
先恢复项目上下文、核对当前源码和实机证据，不要一上来改固件。

当前唯一开发基线：

```text
仓库：F:\Project\stm32G474VETx\TI
项目：F:\Project\stm32G474VETx\TI\G_Periodic_Signal_Analyzer
工程：firmware
发布：v2.1.0（基于v1.9.0 / 9b023b5）
当前工作树：V2.3结构整理；V2.2算法已定版，尚未Git发布
```

V2.1.0普通/Huber折叠已发布：固件已零警告构建并烧录，板上
普通→Huber→普通冒烟无故障，当前模式恢复普通默认。
HMI配置与验证证据见
`docs/04_releases/V2.1_HUBER_FOLD_SWITCH_2026-07-31.md`；不要
覆盖用户后续真实ADC测试文件。

V2.2已经严格读取`tests/三组实际ADC数据.xlsx`并重放当前链路。选定算法是
“两遍Huber + FFT已识别整数谐波正交投影”，三组额外折返点均降到独立稳健
参考的合法数量，固件已于2026-07-31零警告构建、烧录并持续运行。下一位先读
`docs/04_releases/V2.2_REAL_ADC_HARMONIC_PROJECTION_2026-07-31.md`，不要退回普通
Savitzky-Golay或移动平均，也不要在没有用户指令时提交、打tag或发布。

现场屏当前不可触摸。V2.2已按用户明确要求把KEY1长按从V1.9的“刷新真实ADC”
恢复为“测试”：短按切换1T/3T，长按至少1秒并松开后进入随机测试自动换组。
当前板上旧HEX由ST-Link置为增强Huber模式1；用户确认定版后，工作树和队友
生产包已改为上电默认Huber，最终HEX尚未再次烧录。

V2.3已经删除`projects/`层级：只允许在根目录`firmware/`开发；队友输入位于
`teammate/`，废弃demo位于`archive/`。目录总览见根`README.md`和
`docs/04_releases/V2.3_WORKSPACE_REORGANIZATION_2026-07-31.md`。

## 二、必须按顺序阅读

1. 根目录`README.md`与`docs/README.md`
2. `docs/06_handoff/README.md`
3. `docs/06_handoff/00_PROJECT_MASTER_CONTEXT.md`
4. `docs/06_handoff/03_ROLES_AND_INTEGRATION_BOUNDARIES.md`
5. `docs/06_handoff/05_SOFTWARE_ARCHITECTURE_AND_DATAFLOW.md`
6. `docs/06_handoff/06_DEBUG_HISTORY_AND_FROZEN_DECISIONS.md`
7. `docs/06_handoff/07_CURRENT_ISSUES_AND_NEXT_ACTIONS.md`
8. `docs/06_handoff/08_BUILD_FLASH_AND_VALIDATION_RUNBOOK.md`
9. `docs/03_validation/REAL_ADC_FIRST_INTEGRATION_REVIEW_2026-07-31.md`
10. `docs/04_releases/V2.2_REAL_ADC_HARMONIC_PROJECTION_2026-07-31.md`
11. 当前源码：

```text
firmware/Core/Src/main.c
firmware/Core/Src/analyzer_bridge.c
firmware/Core/Src/display.c
firmware/Core/Inc/analyzer_bridge.h
firmware/Core/Inc/display.h
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
→ 默认状态1执行Huber第二遍折叠和已识别整数谐波投影（仅改时域波形）
→ Display_Task()
→ 794点时域曲线、频谱线和文本
→ USART3 FE/FD透传 → 淘晶驰dashboard
```

## 五、下一轮第一件事

先在当前已烧录固件上做屏幕A/B：同一稳定真实输入快速切换
`A5 02 08 00/01 5A`，确认状态1消除斜坡上的细碎峰谷，同时频谱与六项文本
不变。保存普通/增强Huber录屏；若仍不平滑，同步导出该帧`adc_b[2048]`，用
`tools/analyze_real_adc_smoothing.py`复现后再判断是FFT分量识别、折叠频率
还是屏幕映射问题。

随后继续采集同一帧的配对证据，定位真实 ADC 幅值误差：

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
- 不用移动平均、宽窗口Savitzky-Golay或显示缩放覆盖已验证的谐波投影；
- 没有新的原始ADC配对证据前，不再大改Vpp、FFT、折叠或插值算法；
- 修改后必须编译，涉及运行行为时必须烧录并实机验收。

## 七、本次接手应交付

1. 一份基于同帧数据的根因判断；
2. 明确问题属于采集、测量、桥接、重建还是显示层；
3. 一份最小修改方案及影响范围；
4. 屏幕A/B不通过时才做最小修正、重新编译和烧录；
5. 只有用户明确要求后才提交、打tag和发布。
