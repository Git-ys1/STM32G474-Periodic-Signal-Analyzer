# 当前开发状态

更新时间：2026-07-30  
版本：V1.4.1实体按键兼容版

## 已完成

- 以`teammate_adc_newest`为硬件、采集和算法底座建立独立融合工程；
- 保留ADC2、TIM3 TRGO、DMA循环采集、2048点FFT、Vpp和真RMS；
- 新增`AnalyzerBridge`，用统一`AnalyzerResult`承接真实结果与测试结果；
- 在第一次FFT后立即保存谱峰，避免`Vpp_R()`内部二次FFT覆盖频谱输出；
- dashboard单页同时显示时域、频谱、Upp、Urms、基频和最多三个谱峰；
- 1T、3T、刷新和测试四个按钮均已实测；
- 测试按钮内置六组一致场景，波形、Vpp、RMS和谱峰来自同一场景定义；
- UART3接收改为中断，避免2048点FFT期间未及时读DR造成ORE；
- 大型结果结构体改为静态存储，消除1 KB启动栈溢出；
- ArmClang构建加入`__ARM_use_no_argv`，消除启动阶段semihosting `BKPT 0xAB`；
- ST-Link已确认CPU处于正常Thread状态、Fault寄存器清零、栈指针合法、UART无ORE；
- HMI模拟器已稳定显示复合时域波形、三根定性频谱线及全部文本。
- 商家误发不可触摸屏后，已启用板载KEY1（PB8/BOOT0）作为实体控制；
- KEY1短按直接复用原1T/3T按钮处理，长按直接复用原测试按钮处理；
- 实体KEY1与HMI按钮共用同一个命令处理入口，实物验收无误。

## 当前主链路

```text
ADC2 + TIM3 + DMA
→ fft()
→ 快照F/V、FB/VB、FC/VC
→ Vpp与Vrms
→ AnalyzerBridge_PublishReal()
→ AnalyzerResult
→ Display_Task()
→ TJC cle/addt + FE/FD
→ USART3
→ dashboard
```

测试链路：

```text
A5 01 04 5A
→ AnalyzerBridge_RunRandomTest()
→ 锁存测试快照
→ 同一Display_Task()路径
```

## 最终构建

```text
Arm Compiler 6.7
0 Error(s), 0 Warning(s)
Code=56880, RO-data=25700, RW-data=52, ZI-data=48284
```

输出位于本地忽略目录：

```text
projects/g474_full_integration_test/ADC/MDK-ARM/ADC/
```

## 实测证据

- 10.5/31.5/42 kHz复合场景：Upp 126.0 mV，Urms 40.93 mV；
- 120/240/480 kHz复合场景：Upp 136.1 mV，Urms 45.28 mV；
- 两组均正确显示1T/3T、复合波形、三根谱线和对应文本；
- 曲线透传均完成`FE FF FF FF`与`FD FF FF FF`握手。

详见：

- `V1.4_FUSION_BASELINE_FREEZE_2026-07-30.md`
- `V1.4_INTEGRATION_ARCHITECTURE.md`
- `V1.4.1_KEY1_RELEASE_2026-07-30.md`

## 当前冻结边界

- 不再重构已稳定的HMI协议、FE/FD握手或UART接收框架；
- 不反向修改`tjc_display_demo`和队友原始快照；
- 后续只做真实信号标定、算法精度验证、测试模式关闭和最终实体屏回归；
- 任何后续改动必须先分支/提交，再按“上电→1T→3T→测试→刷新”回归。
