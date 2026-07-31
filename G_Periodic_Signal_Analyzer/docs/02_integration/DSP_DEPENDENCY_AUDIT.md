# CMSIS-DSP依赖审计

更新时间：2026-07-31
融合工程：`firmware`

## 实际依赖

队友最新版 `Core/Src/main.c` 实际使用：

- `arm_math.h`
- `arm_const_structs.h`
- `arm_cfft_f32()`
- `arm_cmplx_mag_f32()`
- `arm_max_f32()`
- `arm_rms_f32()`
- `arm_cfft_sR_f32_len4096`

队友ADC1新快照还增加自有源码：

- `goertzel_sync.c/.h`
- `goertzel_sync()`
- `GoertzelResult`

Goertzel实现仍调用标准数学函数，但不是额外的第三方库；已加入融合工程
Keil的`Application/User/Core`组。

`arm_min_f32()`仅存在于注释代码中，不参与当前构建。

## 仓库内文件

原队友目录只有预编译库，缺少DSP头文件；其Keil工程原先通过个人电脑
`E:`盘绝对路径寻找头文件。融合工程已从本机官方
`STM32Cube_FW_G4_V1.6.3`补入：

```text
Drivers/CMSIS/DSP/Include/arm_math.h
Drivers/CMSIS/DSP/Include/arm_const_structs.h
Drivers/CMSIS/DSP/Include/arm_common_tables.h
```

实际复制的是该官方版本完整的 `DSP/Include` 目录，避免只复制入口头文件后
继续缺少间接依赖。

算法实现继续链接：

```text
Drivers/CMSIS/arm_cortexM4lf_math.lib
```

它是Keil/Arm Compiler格式的预编译库，不是GCC可用的 `.a`。仓库内该库与
本机官方Cube包对应库的SHA-256一致：

```text
0216041E95180FA7B6595408A8ECCE7D603076A882F48533D4DC725CEC626D24
```

没有创建任何DSP桩函数或假实现。

## Keil工程配置

`ADC/MDK-ARM/ADC.uvprojx`当前配置：

- 工具链：Arm Compiler 6.7 / ArmClang；
- CPU：Cortex-M4，单精度FPU（`FPU2`）；
- 宏：`USE_HAL_DRIVER, STM32G474xx, ARM_MATH_CM4`；
- DSP头文件路径：`../../Drivers/CMSIS/DSP/Include`；
- DSP库路径：`../../Drivers/CMSIS/arm_cortexM4lf_math.lib`；
- C语言模式：C99；
- LTO：关闭。

队友工程原先固定为本机不存在的 Arm Compiler 6.23。只在新的融合工程中改为
本机已安装的6.7，原始队友工程没有修改。

## 可移植性结论

- 融合工程的源码、DSP头文件和Keil库均已纳入新工作区；
- 工程Include Path和DSP库路径均为仓库相对路径；
- 未发现 `E:/...` 或其他个人电脑绝对路径；
- 当前工程是Keil工程，不能直接把 `.lib` 用于GCC/CubeIDE；
- 如果后续改用CubeIDE，必须换为CMSIS-DSP源码或GCC匹配的 `.a`，不能沿用
  当前Keil库。

## 编译验证

2026-07-30使用本机 Keil MDK 5.24a / Arm Compiler 6.7执行Clean Rebuild：

```text
0 Error(s), 0 Warning(s)
Code=55304
RO-data=25700
RW-data=52
ZI-data=46108
```

已成功生成 `ADC.hex` 和 `ADC.axf`，最终链接未出现DSP未定义符号。

## V1.9 ADC1底座复验

2026-07-31将队友ADC1/Goertzel快照同步至融合工程后再次Clean Rebuild：

```text
0 Error(s), 0 Warning(s)
Code=62276
RO-data=59000
RW-data=52
ZI-data=50756
```

这次构建同时编译了`goertzel_sync.c`、`analyzer_bridge.c`和`display.c`，
证明队友新增源码与原CMSIS-DSP、显示叠加层可在同一Keil工程中链接。
