# G题周期信号测量分析装置

STM32G474VET6 + 淘晶驰 7 英寸串口屏的周期信号测量与显示工程。

当前发布版本：**V2.6.0 双ADC相位差与公式Vpp融合版**。
当前基线已经固定为ADC1/PA0与ADC2/PA1各2048点、交错得到4096点，等效
采样率约2.048193 MS/s。队友最新`win`版本的Goertzel初相差识别已经合入；
峰峰值由校正后的谐波幅相模型在4096点相位网格上直接求得，旧
`Vpp_Robust()`运行调用已停用。主线已完成编译、烧录和离线数学验证。
唯一主线固件位于 [`firmware/`](firmware/README.md)，不要再从历史 demo 或
队友快照中直接开发。

## 目录

| 路径 | 定位 | 是否参与当前主线 |
|---|---|---|
| [`firmware/`](firmware/README.md) | 唯一可编译、烧录、继续开发的 STM32 工程 | 是 |
| [`teammate/`](teammate/README.md) | 队友当前快照、旧快照和收到的原始压缩包 | 只读参考 |
| [`docs/`](docs/README.md) | 架构、融合、验证、版本、报告和交接文档 | 是 |
| [`plan/`](plan/README.md) | 用户编写的各阶段任务书，保持原组织 | 只读依据 |
| [`tools/`](tools/README.md) | PC 端波形、覆盖率和真实 ADC 分析工具 | 是 |
| [`tests/`](tests/README.md) | 测试输入和工具生成的验证结果 | 是 |
| [`experiments/`](experiments/README.md) | 与主线隔离的硬件能力验证工程 | 否 |
| [`deliverables/`](deliverables/README.md) | 发给队友的独立交付包 | 按版本使用 |
| [`tmp/`](tmp/README.md) | 并行任务的临时构建源和中间稿，默认不提交 | 否 |
| [`archive/`](archive/README.md) | 已废弃工程和仅供追溯的内容 | 否 |
| [`.gitignore`](.gitignore) | 构建产物、缓存和可重建结果忽略规则 | 是 |

## 当前主链

```text
TIM3 CH4上升/下降沿
→ ADC1/PA0 + ADC2/PA1
→ DMA adc_b[2048] + adc_b1[2048]
→ 队友生成浮点电压VO[4096]，约2.048193 MS/s
→ 队友FFT分量幅值 + Goertzel分量相位
→ 前级相移校正入口 + 谐波相对初相差
→ 4096点谐波公式合成 → Upp / 频谱RMS
→ AnalyzerBridge直接消费VO，不再读取原始ADC码
→ Huber + 已识别谐波投影
→ Display_Task
→ USART3 → 淘晶驰 dashboard
```

## 快速入口

| 目的 | 入口 |
|---|---|
| 编译和烧录 | [`firmware/README.md`](firmware/README.md) |
| 查看当前状态 | [`docs/00_overview/CURRENT_STATUS.md`](docs/00_overview/CURRENT_STATUS.md) |
| 新会话接管 | [`docs/06_handoff/README.md`](docs/06_handoff/README.md) |
| 队友融合 | [`docs/02_integration/TEAMMATE_INTEGRATION_QUICK_GUIDE.md`](docs/02_integration/TEAMMATE_INTEGRATION_QUICK_GUIDE.md) |
| 查看V2.6发布 | [`docs/04_releases/V2.6_PHASE_DIFFERENCE_FORMULA_VPP_2026-08-01.md`](docs/04_releases/V2.6_PHASE_DIFFERENCE_FORMULA_VPP_2026-08-01.md) |

## 冻结规则

- `firmware/` 是唯一主线；
- `teammate/` 和 `archive/` 不直接修改；
- 测量值与显示波形分层，平滑不能替代 Vpp/频谱正确性；
- 任何目录新增文件时，同步更新该目录的 `README.md` 表格。
