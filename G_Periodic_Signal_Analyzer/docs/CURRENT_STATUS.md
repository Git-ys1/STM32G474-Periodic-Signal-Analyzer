# 当前开发状态

更新时间：2026-07-31
版本：V2.0.0时域触发与相位锚定版

> 完整恢复入口见[新会话永久交接文档包](HANDOFF/README.md)。后续代理必须先读
> 交接包，再继续本页所述真实ADC联调，不得从旧显示演示或旧任务书重新融合。

## 本轮底座

- 原始快照：`projects/teammate_adc_reallynewest!`
- 原始快照提交：`44fc481`
- 融合工程：`projects/g474_full_integration_test`
- 原则：队友最新版负责采集与信号分析；我们只叠加桥接、显示和实体按键。

## 已融合

- ADC2/PA7/DMA1 Channel 2升级为队友最新的ADC1/PA0/DMA1 Channel 1；
- ADC1启用2倍过采样、右移1位、TIM3单次触发和6.5周期采样时间；
- FFT谱峰幅值升级为峰值附近±8个频点的平方和开方；
- Vpp鲁棒统计升级为排序后第1～10个低值和高值的均值差；
- RMS整数周期长度采用`round()`；
- 加入队友`goertzel_sync.c/.h`，三路结果在真实`VO[2048]`上计算但暂不替换显示口径；
- 真实`adc_b[2048]`继续进入256槽完整缓冲区相位折叠；
- KEY1短按复用1T/3T命令，长按改为复用“刷新真实ADC”命令。
- 显示层增加无触发、上升过零、下降过零、正峰值四种相位锚；
- 淘晶驰下拉框通过`A5 02 07 mode 5A`发送最终触发模式，默认`mode=1`。

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
→ 2048点相位折叠到256槽
→ Display_Task()
→ USART3
→ dashboard
```

## 构建与运行证据

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

用户已确认将上述V2.0.0 HEX烧录到实机；本轮未取得新的CubeProgrammer日志或
触发模式运行截图。以下HOTPLUG记录仍是V1.9.0融合底座的运行态证据：

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
- PA0/ADC1_IN1的模拟前端幅值、偏置和频响；
- 10～500 kHz范围内FFT频率与幅值误差；
- Goertzel结果是否经标定后替换FFT幅值；
- 低相位覆盖频点的相位折叠可信度；
- 队友工程中未使用但仍保留的AD9833源码与已删除GPIO定义之间的潜在风险。

后续不得从旧V1.4/V1.8重新融合，应直接从V2.0.0继续实际ADC联调。
