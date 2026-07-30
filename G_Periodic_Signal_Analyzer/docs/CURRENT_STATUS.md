# 当前开发状态

更新时间：2026-07-31
版本：V1.9.0队友ADC1底座融合版

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
Code=62276
RO-data=59000
RW-data=52
ZI-data=50756
```

固件：

```text
ADC.hex
长度：341332 bytes
SHA-256：B3C0F656FCEB10C57E68EE742B9AFCA77A8E0D93683D79F3EB8B2A80A403C803
```

STM32CubeProgrammer 2.22.0已完成SWD下载、校验和复位。HOTPLUG运行态检查：

- 3秒内`s_next_sequence`：`0x041A → 0x0435`；
- CFSR：`0x00000000`；
- HFSR：`0x00000000`。

这证明ADC1/DMA/分析/桥接链路持续运行，且没有HardFault或可配置Fault。

## 尚待实际ADC联调确认

- PA0/ADC1_IN1的模拟前端幅值、偏置和频响；
- 10～500 kHz范围内FFT频率与幅值误差；
- Goertzel结果是否经标定后替换FFT幅值；
- 低相位覆盖频点的相位折叠可信度；
- 队友工程中未使用但仍保留的AD9833源码与已删除GPIO定义之间的潜在风险。

后续不得从旧V1.4/V1.8重新融合，应直接从V1.9.0继续实际ADC联调。
