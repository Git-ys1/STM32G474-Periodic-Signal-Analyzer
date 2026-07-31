# 编译、烧录、ST-Link诊断与发布手册

## 1. 开始前

先确认 Git 状态和当前版本：

```powershell
git -C 'F:\Project\stm32G474VETx\TI' status --short --branch
git -C 'F:\Project\stm32G474VETx\TI' log -n 12 --oneline --decorate
```

权威工程：

```text
F:\Project\stm32G474VETx\TI\G_Periodic_Signal_Analyzer
\projects\g474_full_integration_test
```

不要先全盘搜索 Keil 或 Programmer。固定工具路径是：

```text
Keil:
D:\Work\Keil5\UV4\UV4.exe

STM32CubeProgrammer:
F:\AcademicHub\STMicroelectronics\stm32cubeprogrammer
\bin\STM32_Programmer_CLI.exe
```

Keil 项目：

```text
...\projects\g474_full_integration_test
\ADC\MDK-ARM\ADC.uvprojx
```

HEX：

```text
...\projects\g474_full_integration_test
\ADC\MDK-ARM\ADC\ADC.hex
```

## 2. Keil命令行构建

在 `ADC\MDK-ARM` 目录执行：

```powershell
& 'D:\Work\Keil5\UV4\UV4.exe' `
  -r 'ADC.uvprojx' `
  -j0 `
  -o 'handoff-build.log'
```

注意 `UV4.exe` 可能返回得比日志写完更早，或留有后台进程。不要只看 PowerShell 退出码；要检查日志尾部、HEX 的修改时间和大小：

```powershell
Get-Content -LiteralPath 'handoff-build.log' -Tail 80
Get-Item -LiteralPath 'ADC\ADC.hex' |
  Select-Object FullName,Length,LastWriteTime
Get-FileHash -LiteralPath 'ADC\ADC.hex' -Algorithm SHA256
```

验收应出现 `0 Error(s), 0 Warning(s)`。若构建日志打不开或 HEX 时间没变，说明并未真正得到新产物。不要把旧 HEX 当新固件烧录。当前 `.gitignore` 忽略 Keil 输出和 build log，它们用于本地证据，不应随便加入仓库。

## 3. SWD烧录

```powershell
$cli = 'F:\AcademicHub\STMicroelectronics\stm32cubeprogrammer\bin\STM32_Programmer_CLI.exe'
$hex = 'F:\Project\stm32G474VETx\TI\G_Periodic_Signal_Analyzer\projects\g474_full_integration_test\ADC\MDK-ARM\ADC\ADC.hex'

& $cli `
  -c port=SWD mode=UR reset=HWrst `
  -w $hex `
  -v `
  -rst
```

必须看到连接、擦除、写入、verify 和 reset 完成。若用户已在 Keil 或 CubeProgrammer GUI 中占用 ST-Link，先关闭/断开，不要同时启动多个连接。烧录后记录 HEX 路径、长度、SHA-256、写入时间和 Programmer 结果。

## 4. ST-Link运行态诊断

当屏幕无反应时，不要只盯 HMI 窗口。使用 ST-Link/HOTPLUG 检查：

- PC 是否在主循环、HardFault、Error_Handler 或启动 BKPT；
- `AdcConvEnd` 是否变化；
- `adc_b[]` 是否有非零、非饱和且随输入变化的数据；
- `AnalyzerResult.sequence` 或桥接内部序号是否持续增长；
- `s_uart` 是否为有效 `huart3`；
- `s_dashboard.valid`、time/spec ID 是否已收到；
- `s_pending_action`、`s_run_mode` 和 `s_last_hmi_status`；
- SCB CFSR/HFSR 是否非零。

过去用 ST-Link 发现过：大型局部结构体压坏主栈、ArmClang 启动 `BKPT 0xAB`、固件实际仍在跑但 UART ORE、桥接序号持续增长而页面没握手。这个方法属于长期调试经验：先确认 MCU 停在哪里、关键变量有没有推进，再决定查 ADC、桥接还是 HMI。

若需要导出 `adc_b[2048]`，应在 DMA 完成后、重新启动前暂停，用 debugger memory view/save 读取 4096 字节。不要通过 HMI 的 USART3 在运行中打印 2048 个数字，以免干扰协议。导出时同时记下采样率、VDDA、信号源和示波器设置。

## 5. HMI模拟器与实物验收

HMI 后初始化应产生：

```text
A5 20 01 time_id spec_id 5A
A5 02 07 mode 5A
A5 02 08 enabled 5A
```

按钮产生：

```text
A5 01 01 5A  1T
A5 01 02 5A  刷新真实
A5 01 03 5A  3T
A5 01 04 5A  测试自动
A5 01 05 5A  清除
A5 01 06 5A  停止
```

触发方式下拉框在收回并确定选项后产生：

```text
A5 02 07 00 5A  无触发
A5 02 07 01 5A  上升过零（默认）
A5 02 07 02 5A  下降过零
A5 02 07 03 5A  正峰值
```

折叠方式状态开关在释放并确定状态后产生：

```text
A5 02 08 00 5A  普通折叠（默认）
A5 02 08 01 5A  Huber折叠
```

完整画面测试顺序：

```text
上电
→ dashboard握手
→ 刷新
→ 1T
→ 3T
→ 依次选择无触发、上升过零、下降过零、正峰值
→ 依次切换普通、Huber、普通折叠
→ 测试
→ 清除
→ 等待自动恢复
→ 停止
→ 确认画面冻结
→ KEY1短按
→ KEY1长按
```

每条曲线正常返回 FE、FD。禁止出现 0x12、0x1A、0x1C、0x24。测试
100、250、500 kHz 的谱线位置，500 kHz 不应贴屏幕最右边；用非对称复合
波形检查时域没有左右镜像；确认 1T 与 3T 只改变周期数量，不改变幅值与参数。
触发模式需要连续记录至少20帧，并分别用2/3/5秒刷新间隔验证：无触发应保留
原始相位漂移，三种锚定模式应稳定起点，且所有测量文本和频谱保持不变。
Huber A/B必须用同一信号设置和包含少量异常点的原始ADC帧；切换前后
Upp、Urms、频率和频谱文本必须不变。若干净波形看起来重合属于正常现象。

实物不可触摸，因此最终现场操作重点验证 KEY1：短按切换周期，长按刷新真实 ADC。PB8 复用 BOOT0，上电或复位时不要一直按住，避免启动模式受影响。

## 6. 离线工具和算法验证

工具目录：

```text
tools\analyze_phase_coverage.py
tools\analyze_256k_frequency_sensitivity.py
tools\custom_waveform_lab.py
tools\waveform_lab_core.py
tools\generate_g_problem_adc_tests.py
```

测试输出主要在：

```text
tests\phase_coverage
tests\custom_waveforms
```

相位覆盖扫描应包含均匀频率网格、低分母有理共振和共振邻域，不能只每 50 kHz 抽查。PC 上可以用 1/10/100 Hz 步进和 `q≤256/512` 的有理比枚举，比在 STM32 上穷举更合适。任何要上板的重建算法先在固定 ADC 数组上比较 RMSE、覆盖率、最大空洞、残差和运行量，再写 C。

## 7. 发布前审计

```powershell
git -C 'F:\Project\stm32G474VETx\TI' diff --check
git -C 'F:\Project\stm32G474VETx\TI' status --short
git -C 'F:\Project\stm32G474VETx\TI' diff --stat
```

逐一确认：

- 未修改队友原始快照；
- 未混入 Keil 用户文件、二进制或无关未跟踪文件；
- `main.c` 的 CubeMX 初始化和 USER CODE 标记未被破坏；
- `analyzer_bridge`、`display`、HMI 协议和文档一致；
- 构建 0 error/0 warning；
- 新 HEX 已烧录；
- 实机通过本轮针对性验收；
- `CURRENT_STATUS` 和交接文档已更新；
- 提交信息说明版本、根因和验证证据。

Git 提交和 push 只有在用户明确要求发布时执行。若只要求诊断或写文档，不要自动把未验证算法推到远端。发布后记录标签/提交哈希，下一轮从该点继续，禁止靠聊天记忆猜基线。

## 8. 常见失败的最短分层路线

若上电后整屏无图，先看 CPU 是否进入 `main()`、是否 HardFault/BKPT，再看
`s_dashboard.valid`；不要先改曲线算法。若按钮帧在 HMI 模拟器可见、MCU变量
不变，检查物理串口、PC10/PC11、共地和USART3 ORE。若时域报`0x12`而频谱正常，
检查dashboard上报的两条ID是否被分别保存，不要重新写死历史ID。若出现FE但没有
FD，核对`addt`声明点数与实际发送字节数、页面是否仍有效，以及同步等待期间是否
收到其他错误状态帧。若曲线正常而文本数字为空，检查newlib-nano浮点格式化，不要
误删文本控件。若测试模式正常而真实模式异常，说明HMI和绘图链大概率可用，应转向
`adc_b`、队友测量结果和桥接快照；反之真实分析序号增长但两种模式都无图，应查
显示协议、UART和页面握手。

若屏幕数值与示波器不一致，必须先确认两者是同一输入位置、同一负载、同一稳定
时段，并获取同一帧ADC数组。禁止以“画面像正弦”代替幅值准确性，也禁止用调节
`Display_GetTimeFullScale()`让曲线视觉高度接近示波器，因为纵向满量程只影响
显示比例，不会修正`t_vpp`。若波形每帧左右移动但参数稳定，优先查相位锚而不是
3秒定时器；若低分母频点出现阶梯或形状缺失，先检查真实相位数和最大空洞，不要
直接把线性插值升级为样条。

所有故障都应留下“输入条件、固件哈希、关键变量、屏幕/示波器截图、根因和修复后
回归”六类证据。这样下一个代理可以从证据继续，而不是从症状重新猜测。
