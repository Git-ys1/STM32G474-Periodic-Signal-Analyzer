# 当前开发状态

更新时间：2026-07-31
发布版本：V2.3.0工作区分层与真实ADC谐波投影定版
当前工作态：V2.3已发布；V2.2算法已由用户实屏确认并并入本版本
当前基线：`v2.3.0 / firmware`

> 完整恢复入口见[新会话永久交接文档包](../06_handoff/README.md)。后续代理必须先读
> 交接包，再继续本页所述真实ADC联调，不得从旧显示演示或旧任务书重新融合。

## 本轮底座

- 原始快照：`teammate/current`
- 原始快照提交：`44fc481`
- 融合工程：`firmware`
- 原则：队友最新版负责采集与信号分析；我们只叠加桥接、显示和实体按键。

## V2.3目录结论

- `firmware/`是唯一主线工程；
- `teammate/`只保存队友输入，`archive/`只保存废弃工程；
- `docs/`已按总览、架构、融合、验证、版本、报告、交接和旧档分类；
- `plan/`保持用户原组织，`tools/`和`tests/`分别保存工具与结果；
- 每个项目维护层级均用`README.md`表格列出本层直接内容。

V2.3整理和验证证据见
[V2.3工作区结构整理](../04_releases/V2.3_WORKSPACE_REORGANIZATION_2026-07-31.md)。

## 已融合

- ADC2/PA7/DMA1 Channel 2升级为队友最新的ADC1/PA0/DMA1 Channel 1；
- ADC1启用2倍过采样、右移1位、TIM3单次触发和6.5周期采样时间；
- FFT谱峰幅值升级为峰值附近±8个频点的平方和开方；
- Vpp鲁棒统计升级为排序后第1～10个低值和高值的均值差；
- RMS整数周期长度采用`round()`；
- 加入队友`goertzel_sync.c/.h`，三路结果在真实`VO[2048]`上计算但暂不替换显示口径；
- 真实`adc_b[2048]`继续进入256槽完整缓冲区相位折叠；
- 不可触摸屏当前使用KEY1：短按复用1T/3T命令，长按复用“测试”命令；
  V2.2已按用户现场要求从V1.9的“刷新真实ADC”恢复为“测试”。
- 显示层增加无触发、上升过零、下降过零、正峰值四种相位锚；
- 淘晶驰下拉框通过`A5 02 07 mode 5A`发送最终触发模式，默认`mode=1`。
- 桥接层增加两遍Huber鲁棒相位折叠，参数为`k=1.345`、阈值下限`1.5 LSB`；
- V2.2在Huber之后只保留队友FFT已识别出的整数次谐波，删除模型外相位槽
  抖动；普通模式不变；
- 淘晶驰状态开关通过`A5 02 08 enabled 5A`选择普通或Huber，V2.2定版默认
  `enabled=1`；
- Huber只改变256点时域显示波形，不修改队友`Upp/Urms/频率/谱线`。

## 交叉对照后保留的历史修复

- 第一次FFT后立即快照谱峰，防止`Vpp_R()`二次FFT覆盖；
- `AnalyzerResult`和大型临时缓冲保持静态存储，避免1 KB主栈溢出；
- 保留`__ARM_use_no_argv`，避免ArmClang启动阶段`BKPT 0xAB`；
- USART3保持中断收字节，避免FFT期间主循环轮询造成ORE；
- HMI与KEY1统一进入`Display_ProcessButtonCommand()`，不维护第二套状态机；
- dashboard继续动态接收两条曲线真实数字ID；
- 保留时域和频谱各自的横向写入方向补偿；
- 保留`addt → FE → 原始数据 → FD`握手；
- Keil继续使用本机ArmClang 6.7和仓库相对CMSIS-DSP路径。

## 当前主链路

```text
TIM3 TRGO
→ ADC1 + DMA1 Channel 1
→ adc_b[2048]
→ fft()并快照谱峰
→ Vpp_Robust() / Vpp_R()
→ AnalyzerBridge_PublishReal()
→ 相关搜索细化频率
→ 2048点普通折叠，或可选两遍Huber折叠到256槽
→ Huber侧把256点投影到FFT已识别的整数谐波子空间
→ Display_Task()
→ USART3
→ dashboard
```

## 构建与运行证据

V2.0.0发布构建：

```text
Arm Compiler 6.7
0 Error(s), 0 Warning(s)
Code=64652
RO-data=59000
RW-data=52
ZI-data=50756
```

固件：

```text
ADC.hex
长度：348008 bytes
SHA-256：AB2505B9D3C280B243558176C1FCDC615E741C6AF7462095026A390F9CEDEC10
```

用户已确认将上述V2.0.0 HEX烧录到实机。

V2.1.0发布构建：

```text
Arm Compiler 6.7
0 Error(s), 0 Warning(s)
Code=68532
RO-data=59000
RW-data=52
ZI-data=63068
ADC.hex长度：358927 bytes
SHA-256：FA1CC4CCB29C1B29C0832A31970DCF5C91CCF823962DA71FF4A04902F8252600
```

相对V2.0.0增加3880字节Code和12312字节ZI-data；大数组均为静态存储。
STM32CubeProgrammer 2.22.0已在3.19 V目标电压、约950 kHz SWD下完成下载、
校验和复位，返回`Download verified successfully`。

运行态用调试RAM标志模拟状态开关完成`普通→Huber→普通`：

```text
mode：0 → 1 → 0
sequence：0x100 → 0x236 → 0x2A9
CFSR/HFSR：始终为0
```

当前板上已恢复默认普通模式。真实HMI状态开关帧和曲线效果需在上位机新增
`sw_huber`后验收。

V2.2定版本地验证构建：

```text
Arm Compiler 6.7
0 Error(s), 0 Warning(s)
Code=69652
RO-data=59000
RW-data=52
ZI-data=63060
ADC.hex长度：362077 bytes
SHA-256：C4F6BADEAB9B45F0CFB7C5F50FF58912CC9A4C2C64259FA14DBC5991C9A2DCCB
```

三组真实2048点ADC回放中，普通/Huber/投影折返点分别为
`140/138/16`、`144/138/6`、`126/124/14`；投影结果与独立原始ADC稳健
谐波拟合的相位RMSE为0.150、0.070、0.118 mV。MCU单精度递推与PC双精度
参考最大差异0.000261 mV。固件已通过SWD写入、校验和复位；运行态序号在
1.5秒内由`0x1E1`增至`0x1F1`，CFSR/HFSR均为0。详细证据见
[V2.2真实ADC谐波投影](../04_releases/V2.2_REAL_ADC_HARMONIC_PROJECTION_2026-07-31.md)。

同日因现场屏不可触摸，KEY1长按已恢复为测试命令并重新烧录。用户长按后
RAM回读`s_test_override=1`，测试组继续自动变化；随后通过ST-Link将当前
运行态`s_waveform_fold_mode`置为1，测试结果结构回读为256点、模式1，
当前屏幕运行增强Huber。该RAM模式在MCU复位后仍会回到固件/HMI默认普通。

用户已经根据实屏效果确认V2.2定版。最终源码上电默认已改为增强Huber；
上面提到的复位回普通只适用于板上尚未重烧的旧HEX。队友生产交付版保持
KEY1长按刷新真实ADC、关闭随机测试，并已完成`0 Error(s), 0 Warning(s)`构建。

板上连续回读T108/T107/T105/T108四个增强Huber结果，均为256点、模式1，
折返点均为2；相对其已识别谐波子空间的模型外RMSE只有
0.00000344～0.00001111 mV。STM32内部波形已达到数学光滑，若屏幕仍出现细碎
折返，应转查794点映射、像素量化和刷新链。T108幅值仅44.91 mVpk而名义值为
55 mVpk，继续保留低相位覆盖幅值风险。

V1.9.0融合底座的历史HOTPLUG证据：

- 3秒内`s_next_sequence`：`0x041A → 0x0435`；
- CFSR：`0x00000000`；
- HFSR：`0x00000000`。

这证明ADC1/DMA/分析/桥接链路持续运行，且没有HardFault或可配置Fault。

## 尚待实际ADC联调确认

- 已确认信号源与ADC引脚示波器Vpp通常相差不到5 mV，但ADC引脚示波器与
  屏幕Upp经常相差超过10 mV；下一步应导出同一帧`adc_b[2048]`，联合审计
  VDDA、码值换算、采样相位和`Vpp_Robust()`，不能用显示比例补偿；
- 队友`fft()`当前至少发布两个峰，第二峰尚无“不存在”的阈值和谐波整数倍
  判定，纯正弦的伪峰需要原始ADC和同步频谱证据；
- V2.0.0相位锚已烧录，但真实ADC连续20帧、2/3/5秒刷新间隔、四种触发模式和
  1T/3T方向回归仍需补齐；
- V2.2已使用三组真实ADC完成PC量化并烧录；仍需用户在当前屏幕上确认
  `sw_huber=1`时斜坡细小峰谷实际消失，并保存普通/增强Huber同信号录屏；
- PA0/ADC1_IN1的模拟前端幅值、偏置和频响；
- 10～500 kHz范围内FFT频率与幅值误差；
- Goertzel结果是否经标定后替换FFT幅值；
- 低相位覆盖频点的相位折叠可信度；
- 队友工程中未使用但仍保留的AD9833源码与已删除GPIO定义之间的潜在风险。

后续不得从旧V1.4/V1.8重新融合，应直接从V2.1.0发布基线和当前V2.2工作树
继续实际ADC联调；没有发布指令前不得把V2.2误写成已发布版本。
