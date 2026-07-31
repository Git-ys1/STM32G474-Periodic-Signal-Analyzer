# V1.4全量融合变更记录

更新时间：2026-07-30  
新工作区：`firmware`

## 架构决定

本工程以 `teammate/archive/adc_newest` 完整副本为硬件和算法基线，保留
ADC2、TIM3、DMA、FFT、Vpp和RMS计算。显示采用当前已经验证的V1.3.1
dashboard单页面基线。

当前显示基线与任务书中的旧假设不同：实际工程没有独立
`tjc_hmi.c/.h`，HMI通信、`addt`握手和帧解析均已稳定封装在
`display.c`中。本轮沿用实际基线，没有为了满足文件名重新制造一套重复通信
模块。

数据链路为：

```text
ADC2 + DMA
→ 队友fft/Vpp/RMS
→ AnalyzerBridge_PublishReal()
→ 稳定AnalyzerResult快照
→ display.c
→ USART3
→ 淘晶驰dashboard
```

测试链路为：

```text
A5 01 04 5A
→ AnalyzerBridge_RunRandomTest()
→ 结构化最终结果 + 一致时域波形
→ 同一AnalyzerResult
→ 同一显示路径
```

测试不会伪造ADC原始采样，也不会调用或替换队友FFT。正式频谱直接采用队友
FFT的谱峰结果；原显示模块的演示DFT已删除。

## 新增文件

- `Core/Inc/analyzer_bridge.h`
- `Core/Src/analyzer_bridge.c`
- `Drivers/CMSIS/DSP/Include/*`
- `docs/02_integration/TEAMMATE_OUTPUT_MAP.md`
- `docs/02_integration/DSP_DEPENDENCY_AUDIT.md`
- `docs/04_releases/INTEGRATION_CHANGES.md`
- `docs/03_validation/SCREEN_TEST_GUIDE.md`

## 修改文件

| 文件 | 修改内容 |
|---|---|
| `Core/Src/main.c` | 保留队友算法；初始化桥接层和显示；保存第一次FFT谱峰；Vpp/RMS完成后发布真实快照；主循环调用`Display_Task()`。 |
| `Core/Inc/display.h` | 对外提供`Display_Init()`、`Display_Task()`和`Display_RedrawCurrentPage()`。 |
| `Core/Src/display.c` | 改为读取`AnalyzerResult`；删除显示侧DFT；支持测试按钮；保持动态曲线ID、频谱方向和FE/FD握手。 |
| `ADC/MDK-ARM/ADC.uvprojx` | 加入桥接/显示源文件；DSP头文件改相对路径；编译器从不可用的6.23改为本机6.7。 |
| `ADC/MDK-ARM/RTE/_ADC/RTE_Components.h` | 本机Keil Clean Rebuild自动重建RTE配置并补入`CMSIS_device_header`，不是手工算法改动。 |
| 根目录`.gitignore` | 忽略新融合工程的Keil目标输出和构建日志，防止生成物进入提交。 |

## 测试注入

桥接层内置6个完整场景，使用xorshift32随机选择并避免连续重复。场景表同时
定义频谱分量和相位，桥接层据此生成256点单周期波形，再从波形计算
`Vpp=max-min`和真RMS。频谱直接使用同一场景表的分量，因此图形和参数一致。

按下测试键后设置 `test_override=true`。真实ADC和算法继续运行并更新真实
缓存，但显示优先读取测试快照，不会被无输入时的零值或噪声立即覆盖。将
`ANALYZER_TEST_ENABLE`设为0可在正式构建中关闭测试注入。

## 外设和工程冲突检查

- USART3仅初始化一次，继续使用PC10/PC11、115200 bit/s；
- USART3未被队友代码用于其他调试输出；
- ADC2继续使用PA7和DMA1 Channel 2；
- TIM3继续作为ADC外部触发来源；
- 没有新增UART DMA、ADC中断发送或第二套时钟初始化；
- 新工程只有一个`main.c`、一套启动文件和一套HAL驱动；
- 显示发送仍在主循环中，不进入ADC DMA完成回调。

## 现场联调后的稳定性修复

初次融合能够编译，但先后暴露出三类运行时问题：

1. ArmClang启动阶段进入semihosting `BKPT 0xAB`，通过加入
   `__ARM_use_no_argv`消除启动时命令行探测；
2. 2048点FFT长计算期间轮询接收不及时，USART3出现ORE，改为
   `HAL_UART_Receive_IT()`持续接收并在错误回调中清ORE、flush和重启接收；
3. `AnalyzerResult`大于1 KB，作为局部变量时叠加调用栈导致启动栈越界，
   改为模块静态快照，避免把大结构体放在栈上。

修复后的显示发送仍由主循环执行，没有把`addt`阻塞传输放进中断。

## 最终编译结果

本机执行Clean Rebuild成功：

```text
Arm Compiler 6.7
0 Error(s), 0 Warning(s)
Code=56544, RO-data=25700, RW-data=52, ZI-data=48276
```

生成：

- `ADC/MDK-ARM/ADC/ADC.hex`：231548字节；
- `ADC/MDK-ARM/ADC/ADC.axf`：451428字节。

SHA-256：

```text
ADC.hex 80F21985D63D5E0968E4EEB423243C1277CB32EDDC02DF4A3781F5BDA5676693
ADC.axf DB4164111854473A0E4D1913C924C6B81C51CBEFF30BCCFFAF105C42FE8C3511
```

## 原目录保护

`git diff -- teammate/archive/adc_newest archive/firmware/tjc_display_demo`为空。
两个原始工程没有修改；所有融合代码仅位于新工作区。

## 现场验证结论

- 固件已通过ST-Link下载、校验并复位；
- ST-Link运行态检查确认无HardFault/BusFault、MSP位于合法栈区、USART3无ORE；
- HMI模拟器实测1T、3T、刷新、测试按钮均正常；
- 六个随机场景可稳定切换，时域、频谱和文本保持一致；
- 10.5/31.5/42 kHz与120/240/480 kHz两组结果已截图归档。

完整原因、修复方式、判断证据与冻结边界见：

- `V1.4_FUSION_BASELINE_FREEZE_2026-07-30.md`
- `V1.4_INTEGRATION_ARCHITECTURE.md`

## V1.4.1不可触摸屏KEY1兼容

商家误发不可触摸版本后，融合工程使用最小系统板KEY1（PB8/BOOT0）补充实体
控制。原HMI页面、串口协议、曲线ID、FE/FD握手和信号处理主链均未改变。

最终实现没有建立第二套显示状态机，而是把HMI四字节按钮帧和KEY1动作汇入
`Display_ProcessButtonCommand()`：

```text
HMI A5 01 01/03 5A ─┐
                     ├→ Display_ProcessButtonCommand()
KEY1短按 1T/3T  ────┘

HMI A5 01 04 5A ────┐
                     ├→ Display_ProcessButtonCommand()
KEY1长按测试     ────┘
```

中断回调只记录按下时刻，松开和时长判断在主循环完成；曲线绘制和UART发送仍由
`Display_Task()`执行。编译开关`BOARD_KEY_CONTROL_ENABLE`可关闭全部实体按键
逻辑。

实物验收确认：

- 短按可在1T和3T之间切换；
- 长按可执行原测试动作；
- HMI按钮与KEY1可以共存；
- 两种输入方式行为一致。
