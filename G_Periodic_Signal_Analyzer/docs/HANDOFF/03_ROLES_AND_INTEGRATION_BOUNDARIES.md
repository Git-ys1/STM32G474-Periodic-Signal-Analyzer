# 人员职责、模块边界与融合原则

## 1. 为什么必须写清职责

这个项目曾经多次把问题归错层：时域不显示时先怀疑 FFT，频谱伪峰时去改曲线，Vpp 不准时想通过屏幕缩放修正，融合失败时又把两个 `main.c` 相互覆盖。根本原因是没有长期冻结“谁生产什么数据、谁只消费数据、谁可以改哪些文件”。本文件给出当前责任边界。它不是为了推卸责任，而是为了让实测问题能够沿数据链定位，并使用户与队友可以独立推进。

## 2. 用户负责的板块

用户是显示与系统融合负责人，主要交付物包括：

1. 淘晶驰 X2 7 英寸 800×480 串口屏的 HMI 页面、控件命名、布局、字库和事件协议；
2. `display.c/.h`：USART3 通信、HMI 指令、FE/FD 透传、动态曲线 ID、时域/频谱绘制、文本更新、1T/3T、刷新、测试、清除和停止状态机；
3. `analyzer_bridge.c/.h`：将队友散乱的 ADC、Vpp/RMS、频率和幅值变量复制为一致的 `AnalyzerResult`，并生成显示所需的一周期 256 点波形；
4. PC10/PC11 与屏幕 RX/TX 的接线、共地、5 V 屏幕供电和串口联调；
5. 不可触摸屏的 KEY1 PB8 操作，以及实体按键与 HMI 按钮共享同一业务入口；
6. 显示独立回归测试、自定义波形实验室、相位覆盖扫描、融合验证、构建烧录和 Git 文档；
7. 在不改变队友核心算法口径的前提下，为显示侧保存稳定快照并处理页面协议。

用户不应在没有同步证据时擅自更改队友 FFT、峰值判定、Vpp/RMS 和 ADC 时序。若必须修改，应明确升级为“联合算法修复”，先在队友原始快照和融合工程之间建立 diff，并让队友知道对外输出口径改变。

## 3. 队友负责的板块

队友负责信号采集与分析主体：

1. STM32G474 的 CubeMX/Keil 底座和系统时钟；
2. ADC1 PA0/IN1、TIM3 TRGO、DMA1 Channel 1、2048 点采样缓冲 `adc_b[]`；
3. ADC 码转换到 `VO[]`，CMSIS-DSP 2048 点 FFT；
4. 三个候选谱峰 `F/V、FB/VB、FC/VC` 以及分量数 `flag`；
5. `Vpp_Robust()`、`Vpp_R()` 和真有效值口径；
6. `goertzel_sync.c/.h` 的精测试验；
7. 与 ADC 输入相关的模拟信号处理器件、连接和调试。当前代码仓库没有完整模拟前端原理图或 BOM，因此具体器件型号仍需队友或实物资料补齐，不能由显示侧猜测。

队友目前提供的是测量结果和原始 ADC 数据，不是已经完成全部屏幕绘图的库。曲线“还原”分成两层：队友的 `adc_b[2048]` 是真实离散采样；用户的桥接层将多周期采样按基频相位折叠为一个周期，再由显示层映射到屏幕。队友 FFT 提供频率和幅值，但没有提供每个分量的相位，因此不能单靠三组频率/幅值唯一复原一般复合波形；当前时域图必须依赖原始 ADC 相位信息。

## 4. 双方共享的数据契约

当前统一输出结构是 `AnalyzerResult`：

```text
valid / sequence / source
fundamental_hz
vpp_mv
vrms_mv
component_count
components[3] = {frequency_hz, amplitude_mv}
waveform_count
waveform_mv[256]
test_case_number
status_flags
```

队友算法完成一次分析后，融合 `main.c` 调用：

```c
AnalyzerBridge_PublishReal(
    adc_b,
    ADC_SIZE,
    ADC_VOLTS_PER_CODE,
    ANALYZER_SAMPLE_RATE_HZ,
    vpp,
    Vrms,
    spectrum_flag,
    spectrum_frequencies_hz,
    spectrum_amplitudes_v
);
```

桥接层负责复制和转换，不允许显示层直接读取会在下一次 FFT 中变化的全局变量。`AnalyzerBridge_GetLatest()` 返回一个一致快照，`Display_Task()` 才开始绘制。大型结果对象必须使用静态存储，不能放在 1 KB 主栈上。

这里的单位必须固定：队友传入 Vpp、RMS、幅值使用 V，频率使用 Hz；桥接层统一转成 mV 和 Hz；显示文本将频率转成 kHz，将频谱幅值标为 `mVpk`。任何一方改变单位都必须同步修改接口注释和测试，否则会出现 1000 倍错误。

## 5. 两种融合方向

推荐且当前已经采用的方向是“用户代码加入队友底座”：

```text
保留队友Core、ADC、TIM、DMA、FFT和Keil工程
→ 加入analyzer_bridge.c/.h和display.c/.h
→ USART3初始化后Display_Init(&huart3)
→ ADC分析完成后AnalyzerBridge_PublishReal(...)
→ 主循环末尾BoardKey_Task()和Display_Task()
```

原因是实际信号处理器件和 ADC 工程在队友一侧，后续队友升级通常仍会交付一个 Keil/CubeMX 工程。以队友为底座可以减少 ADC 外设和算法迁移风险。融合时必须保留历史修复：第一次 FFT 结果快照、no-argv、静态大对象、UART 中断收字节、动态曲线 ID、方向补偿和统一按键入口。

反向“队友代码加入用户工程”只适合作为备用研究路线。理论上需要迁入 ADC、TIM、DMA、CMSIS-DSP、FFT、Goertzel 和 `main.c` 处理链，文件更多、CubeMX 依赖更强，容易漏中断、DMA request、链接库或 Keil include path。即使硬件上把队友板换成用户板，软件上仍然更合理地以队友工程为主，再接用户两个模块。

## 6. 问题归属规则

```text
PA0示波器已错：模拟前端/接线/负载问题
adc_b码值比例错：ADC参考/校准/码转换问题
VO正确但Vpp错：Vpp_Robust/Vpp_R算法问题
FFT多出峰：窗函数/阈值/噪声底/谐波判定问题
AnalyzerResult正确但屏幕文本错：显示格式或控件问题
AnalyzerResult波形错误：频率细化/相位折叠/低覆盖问题
发送cle/addt报0x12：曲线ID或通道问题
FE后无FD：透传长度、UART或页面状态问题
按钮无反应：HMI帧、UART ORE、KEY状态机或停止模式问题
```

每次诊断都应在边界处取证，而不是根据最终屏幕现象猜根因。用户已经明确：源与示波器 Vpp 通常差小于 5 mV，示波器与屏幕经常差超过 10 mV，所以当前幅值问题的主要审计范围是 ADC 码值到队友 Vpp 输出，不是先调 UI。

## 7. 变更纪律

队友原始快照只读；用户显示独立工程用于回归；全量融合工程用于实际修改。若队友再给新版本，先归档成新的只读目录和提交，再用 diff 把用户模块及全部历史修复叠加，编译后逐项验收。任何“优化”都必须回答三个问题：它属于哪一层；是否改变队友测量口径；怎样证明没有破坏 HMI、KEY1、ADC 或旧用例。
