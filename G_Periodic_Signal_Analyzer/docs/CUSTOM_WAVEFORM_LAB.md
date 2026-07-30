# 自定义周期波形实验室与相位覆盖扫描

## 1. 用途

本工具用于在不接信号源的情况下，按当前融合固件的真实链路验证：

```text
基波 + 0～2个谐波
→ 1.024 MSPS、2048点、12位ADC量化
→ 基频细化
→ 64/128/256槽相位折叠
→ 缺槽环形线性补点
→ 794列周期线性插值
→ 已知谐波阶次的sin/cos最小二乘重建
→ 与理想波形比较
→ 导出STM32可直接使用的uint16_t[2048]
```

它不是用理想参数绕过ADC链路，而是生成与真实`adc_b[2048]`格式一致的原生
ADC码值。导出的测试组与真实输入共用`AnalyzerBridge_BuildRealWaveform()`。

## 2. 启动

双击：

```text
tools\run_custom_waveform_lab.bat
```

或在仓库根目录运行：

```powershell
python tools\custom_waveform_lab.py
```

无界面自检：

```powershell
python tools\custom_waveform_lab.py --self-test
```

依赖Python、NumPy、Matplotlib和Python自带的Tkinter。工具不依赖SciPy。

## 3. 可以填写的内容

- 组号：1～255，最终可在屏幕“谐波2”一行最前面显示为`[T组号]`。
- 名称与对应题目要求。
- 基频、基波峰值。
- 最多两个谐波的实际频率、峰值和相对相位。
- 全局采样起始相位、ADC噪声和随机种子。
- 显示1T或3T。
- 相位槽数可选64、128或256，用于离线比较不同覆盖密度。

谐波频率必须是基频的整数倍，且所有分量不得超过500 kHz。

## 4. 相位如何理解

正式题目问答说明各分量之间没有相对相位差，因此正式测试建议：

```text
各谐波相对相位 = 0°
全局采样起始相位 = 可随机
```

全局起始相位随机表示ADC开始采样的时刻不固定，这是正式链路应承受的变化。
“各谐波独立随机相位”只用于额外压力测试，不代表题目正式输入。

工具生成第`m`次谐波时使用：

```text
u_m(t) = A_m sin(2πm f0 t + m φ0 + φm)
```

其中`φ0`是全局起始相位，`φm`是用户填写的相对相位。

## 5. 三张图分别表示什么

1. **完整2048点采集窗**：蓝线是用户参数定义的真实连续模型，橙色散点是经过
   ADC量化和可选噪声后的2048个采样值。蓝线才是本实验室的“真值”，橙点是算法
   实际能够获得的离散证据。
2. **一个周期重建对比**：绿线是理想周期；紫线是当前槽数下的相位折叠结果；
   红色虚线是利用已知谐波阶次对ADC样本做`sin/cos`最小二乘拟合后的结果；
   橙点是ADC样本在一个周期内的真实相位位置。这里直接比较“真实模型、折叠重建、
   谐波模型重建”三条路径。
3. **相位槽采样证据**：每根柱表示该相位槽获得的加权样本量。柱子稀疏或中间存在
   大片空洞，说明插值是在跨越未采样区域，曲线即使看起来平滑也不能视为高可信重建。

右侧指标同时显示：

- 当前相位槽数；
- 独立真实相位数量；
- 当前槽数的硬槽覆盖率；
- 左右加权后的槽覆盖率；
- 已命中硬槽的最小/最大样本数；
- 最大相位空洞；
- `f0/Fs`近似约分结果；
- 相位折叠和794列显示结果相对理想曲线的RMSE与最大误差；
- 谐波最小二乘矩阵秩、条件数、ADC样本残差和显示曲线误差；
- 同一输入在64、128、256槽下的离线覆盖率与RMSE对比。

## 6. 覆盖率的定义

覆盖质量必须根据真实ADC采样相位计算。每个样本只记入最近的一个硬槽：

```text
phase_n = frac(n f0 / Fs + φ0 / 360°)
bin_n   = floor(phase_n × B + 0.5) mod B
```

```text
hard_coverage = 实际被命中的硬槽数 / B
```

不能用当前“一个样本按距离分给左右两个槽”的权重结果代替硬覆盖率，否则一个
真实样本可能同时点亮两个槽，使覆盖率虚高。加权覆盖率只用于说明现有折叠器实际
写入了多少槽。`B`是本次选择的64、128或256；降低`B`可以减少空槽，却不会增加
ADC真正访问过的独立相位数量。

## 7. 为什么不是每50 kHz测一次

默认使用混合扫描，共77430个去重点：

1. 10～500 kHz按10 Hz均匀扫描；
2. 直接枚举`f=Fs·p/q`、`2≤q≤256`的全部互质精确共振点；
3. 在每个精确共振点继续加入`-1 Hz`和`+1 Hz`邻域点。

```powershell
python tools\analyze_phase_coverage.py
```

输出：

```text
tests\phase_coverage\phase_coverage_sweep.csv
tests\phase_coverage\phase_coverage_summary.json
tests\phase_coverage\phase_coverage_sweep.svg
tests\phase_coverage\phase_coverage_sweep.png
```

如需调整扫描强度：

```powershell
# 复现旧版100 Hz纯均匀扫描
python tools\analyze_phase_coverage.py --mode uniform --step-hz 100

# 只枚举精确有理共振，不扫邻域
python tools\analyze_phase_coverage.py --mode rational `
  --max-denominator 256 --resonance-radius-hz 0

# 更重的PC离线扫描：1 Hz均匀 + q≤512 + 共振点±5 Hz、0.5 Hz步进
python tools\analyze_phase_coverage.py --mode hybrid --step-hz 1 `
  --max-denominator 512 --resonance-radius-hz 5 `
  --resonance-step-hz 0.5
```

最后一项会产生大量扫描点，运行时间和CSV体积都会明显增加；它适合离线分析，
不需要也不应搬到STM32上执行。

任何1 Hz、0.1 Hz等十进制均匀网格仍只会精确命中分母由2和5组成的比例，
所以“进一步缩小十进制步进”不能替代有理共振枚举。每50 kHz的结果仍保留为
摘要检查点，但不能发现分母3、7、9、11以及128、204.8、256、307.2、
409.6 kHz等相干采样陷阱。当前最差典型点为：

原因可以直接写成约分关系。若均匀步进为100 Hz，扫描点为`f=100m`，则：

```text
f/Fs = m/10240
q = 10240 / gcd(m, 10240)
10240 = 2^11 × 5
```

因此约分后的分母`q`必为10240的因数，素因子只能来自2和5；把步进改成
10 Hz或1 Hz只会增加2、5的幂次，仍不会精确命中`q=3、7、9、11`。
脚本的`rational`部分不依赖十进制网格，而是直接枚举全部互质`p/q`。

默认只枚举到`q≤256`不是为了省事，而是因为当前只有256个相位槽：精确共振时，
`q<256`最多只能访问`q`个等距相位，必然无法填满256槽；`q≥256`且完整访问一个
有理周期时已经具备填满全部槽的可能。若要研究更长有限记录下的近共振细节，
可以显式把`--max-denominator`提高到512或2048。

共振邻域的Hz步进还可换算成“2048点记录期间累计漂移了多少相位槽”：

```text
drift_bins = N × |Δf| / Fs × 256
           = 0.512 × |Δf/Hz|
```

所以当前`±1 Hz`邻域相当于整段记录累计漂移约`±0.512槽`；若想以约
`0.1槽`为间隔细看共振谷，可把`--resonance-step-hz`设为约`0.1953125`。

| 基频 | 独立相位 | 256硬槽 | 最大空洞 |
|---:|---:|---:|---:|
| 341.333 kHz（1/3 Fs） | 3 | 3 | 85.33槽 |
| 256.0 kHz（1/4 Fs） | 4 | 4 | 64槽 |
| 204.8 / 409.6 kHz | 5 | 5 | 51.2槽 |
| 170.667 kHz（1/6 Fs） | 6 | 6 | 42.67槽 |
| 146.286 / 292.571 / 438.857 kHz（p/7 Fs） | 7 | 7 | 36.57槽 |
| 128.0 / 384.0 kHz | 8 | 8 | 32槽 |
| 102.4 / 307.2 kHz | 10 | 10 | 25.6槽 |

这说明覆盖度不随频率单调变化，不能按“频率越高就固定用更少槽”处理。

## 8. 内置典型组

首次启动会在`tests/custom_waveforms/registry.json`写入8组预设，不覆盖后续用户记录：

| 组号 | 场景 | 独立相位 | B=64/128/256显示RMSE | 谐波LS显示RMSE |
|---:|---|---:|---:|---:|
| T101 | 10 kHz纯基波 | 256 | 约0.85/0.85/0.85 mV | 0.846 mV |
| T102 | 10.5 kHz，1/3/4次分量 | 256 | 0.573/0.428/0.420 mV | 0.411 mV |
| T103 | 128 kHz，1/2/3次相干场景 | 8 | 5.492/4.991/4.870 mV | 0.208 mV |
| T104 | 160 kHz，1/2/3次相干场景 | 32 | 2.983/1.519/0.815 mV | 0.116 mV |
| T105 | 200 kHz，1/2次分量 | 128 | 0.166/0.192/0.661 mV | 0.049 mV |
| T106 | 250 kHz + 500 kHz边界 | 256 | 均小于0.2 mV | 小于0.2 mV |
| T107 | 249.75 kHz + 499.5 kHz，37°起始相位 | 166 | 均小于0.6 mV | 小于0.3 mV |
| T108 | 精确256 kHz纯基波 | 4 | 8.375/8.222/8.191 mV | 0.236 mV |

T103证明：问题不是“256点插值到794点不够高级”，而是ADC本身只访问了8个
不同相位。任何插值方法都不能凭空恢复未采到的信息。

T108进一步证明：把256槽降到64槽仍只有4个真实相位，折叠误差没有本质改善；
但在题目“基波加至多两个谐波”的先验成立、频率估计正确且设计矩阵满秩时，谐波
最小二乘可以从少量相位恢复模型参数。该结论当前仅属于PC离线验证，尚未替换固件
中的相位折叠主链。

## 9. 保存、导出和固件接入

界面中先“保存/覆盖记录”，再点击“导出全部记录到固件”。生成：

```text
projects\g474_full_integration_test\Core\Inc\generated_custom_adc_tests.h
```

每组还可单独导出：

```text
tests\custom_waveforms\Txxx_name\
├─ case.json
├─ samples_2048.csv
├─ comparison.png
└─ comparison.svg
```

在`Core/Inc/analyzer_bridge.h`中切换：

```c
#define ANALYZER_TEST_ENABLE          1U
#define ANALYZER_CUSTOM_TEST_ENABLE   1U
```

此时“测试”命令从自定义记录中随机选择一组。恢复原T1～T9：

```c
#define ANALYZER_CUSTOM_TEST_ENABLE   0U
```

正式比赛关闭测试覆盖：

```c
#define ANALYZER_TEST_ENABLE          0U
```

测试编号已移到“谐波2”文本最前面，避免末尾被控件裁掉：

```text
[T103] 谐波2: ...
```

## 10. 自适应槽数的可行性

让程序在64、128、256槽之间选择是可行的，但不能只按频率区间或一个覆盖率阈值
硬切换。不同槽数是在“相位分辨率”和“每槽证据量”之间取舍：

| 观察结果 | 建议 |
|---|---|
| 覆盖充分、空洞小 | 优先256槽，保留较高相位分辨率 |
| 256槽较稀疏、128槽明显降低误差 | 可退到128槽 |
| 只有少量独立相位 | 降槽不能创造新信息，应转入模型拟合或报告低可信 |

T105说明降低槽数有时有效：200 kHz场景在64槽下的显示RMSE约0.166 mV，
优于256槽的0.661 mV。T108说明它不是万能方案：精确256 kHz只有4个真实相位，
64、128、256槽的显示RMSE都约8.2 mV。

因此以后固件若实现自动决策，至少应同时检查：

```text
频率估计在相邻帧是否稳定
独立真实相位数、硬槽覆盖率、最大空洞
相位折叠对原ADC样本的残差
谐波最小二乘的样本残差、矩阵秩和条件数
```

当前GUI只负责让64/128/256三档可重复仿真，尚未把自动切槽策略写入STM32。

## 11. 精确256 kHz与频率误差风险

专用脚本：

```powershell
python tools\analyze_256k_frequency_sensitivity.py
```

输出：

```text
tests\phase_coverage\exact_256k_frequency_sensitivity.csv
tests\phase_coverage\exact_256k_frequency_sensitivity.json
tests\phase_coverage\exact_256k_frequency_sensitivity.png
tests\phase_coverage\exact_256k_frequency_sensitivity.svg
```

脚本区分两种容易混淆的情况：

1. 真实信号固定为精确256 kHz，但算法使用带误差的估计频率；
2. 真实信号本身位于256 kHz附近，算法使用对应的正确频率。

关键数据如下：

| 场景 | 计算硬覆盖率 | 相位折叠显示RMSE | 谐波LS样本残差 |
|---|---:|---:|---:|
| 真值256 kHz，估计正确 | 1.56% | 8.19 mV | 约0 mV |
| 真值256 kHz，误估为256.1 kHz | 82.03% | 27.33 mV | 13.80 mV |
| 真值256 kHz，误估为256.25 kHz | 100% | 47.53 mV | 30.12 mV |
| 真值256.1 kHz，估计正确 | 高覆盖 | 0.235 mV | 0.228 mV |

这证明一个重要风险：若覆盖率由“估计频率”推导，频率误差可能制造虚假的高覆盖。
本次±500 Hz、1 Hz步进扫描中，有804个点同时满足“计算覆盖率不低于80%”
和“真实折叠误差不低于5 mV”。所以不能看到覆盖率高就认定重建可靠。

谐波最小二乘的形式为：

```text
u[n] = c0 + Σ(am sin(2πm f0 tn) + bm cos(2πm f0 tn))
```

拟合后由`am、bm`重建连续周期。它对精确256 kHz有效，是因为题目最多只有3个
已知整数倍频率分量，待估参数很少；它不是一般意义上的“由4个点恢复任意曲线”。
若频率错误、谐波阶次判断错误、矩阵不满秩或残差过大，就必须拒绝该模型结果。

## 12. 插值、模型拟合与等效采样的边界

当前相位槽到794列继续使用周期线性插值。对既有测试集，周期三次样条、PCHIP、
Akima和傅里叶插值没有降低总体误差；高阶方法还可能追随量化噪声或产生过冲。

低覆盖时应优先处理信息来源，而不是升级插值阶数：

1. 为显示额外采一帧，并从可实现的稍有差异的采样率中选择覆盖最好者；
2. 使用稳定触发或可估计的帧起始相位，做多帧顺序等效时间采样；
3. 利用题目只有少量谐波的先验，对`sin/cos`系数做最小二乘拟合。

等效时间采样需要重复波形，并通过多次采集获得不同时间偏移。当前TIM3固定触发ADC、
单帧DMA连续采样并没有自动具备这种能力。实验室目前先暴露覆盖不足和模型残差，
不把PC离线结论冒充已经上板的正式算法。

## 13. 时域显示方向修复

实验室T102的理想周期在相位0后先上升，而旧屏幕画面在同一点后先下降。两者的
峰谷顺序严格满足`x → 1-x`，说明故障是整周期水平镜像，不是纵坐标上下反转。

淘晶驰曲线控件的整帧写入方向与显示算法使用的逻辑横坐标方向相反。频谱代码早已
做过横向补偿，时域此前遗漏。修复方式是在写入发送缓冲时执行：

```c
s_curve_buffer[(DISPLAY_CURVE_WIDTH - 1U) - i] = mapped_value;
```

该修改只反转横向发送顺序，没有改变ADC数据、相位折叠、纵坐标映射、1T/3T逻辑、
文本结果、频谱方向或FE/FD握手。

## 14. 已完成验证与烧录

- Python模块语法检查通过；
- GUI无界面自检覆盖8组记录及64/128/256三档槽数；
- 三图布局冒烟测试通过，标题、图例不再重叠；
- T102方向对比确认PC理想曲线相位0后先上升；
- T108精确256 kHz折叠与谐波最小二乘对比图生成并人工检查；
- 77430点混合覆盖扫描通过，其中含9564个精确有理共振点；
- 256 kHz±500 Hz、1 Hz步进灵敏度扫描通过；
- 8组典型数据、2048点ADC数组和C头文件导出通过；
- Keil ArmClang 6.7重新构建：0 errors、0 warnings；
- `ADC.hex`大小334267字节；
- SHA-256：
  `159E08A37C5BE6B3607226836095129A166AE0081AF866B3581D9695CDDFBD8C`；
- 2026-07-31 01:46 已通过STM32CubeProgrammer CLI执行SWD下载、校验和复位，
  日志明确返回`Download verified successfully`。

当前烧录固件启用了`ANALYZER_CUSTOM_TEST_ENABLE=1`，含T101～T108。正式比赛前
若需恢复原T1～T9，再将该开关设为0并重新构建烧录。

## 参考

- [Tektronix：Equivalent-time Sampling Mode](https://www.tek.com/en/support/faqs/what-equivalent-time-sampling-mode)
- [Tektronix：Real-Time Versus Equivalent-Time Sampling](https://www.tek.com/ru/documents/application-note/real-time-versus-equivalent-time-sampling)
- [Keysight：Sampling Oscilloscope Theory](https://helpfiles.keysight.com/scopes/FlexDCA-PG/Content/Topics/Quick-Start/theory_sampling_scope.htm)
- [NIST：Least-Squares Sine Fit Errors](https://www.nist.gov/publications/bounds-least-squares-four-parameter-sine-fit-errors-due-harmonic-distortion-and-noise)
