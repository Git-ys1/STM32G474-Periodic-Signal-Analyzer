# 废弃工程：tjc_display_demo

该工程只保留早期淘晶驰显示链路，不包含当前真实ADC融合。

| 项目 | 说明 |
|---|---|
| `.cproject`、`.project`、`.mxproject` | 旧Eclipse/CubeMX元数据 |
| `TJC_sine_test.ioc` | 旧演示外设配置 |
| `STM32G474VETX_FLASH.ld` | 旧Flash链接脚本 |
| `STM32G474VETX_RAM.ld` | 旧RAM链接脚本 |
| `.settings/` | 旧IDE设置 |
| `Core/` | 旧模拟正弦显示源码 |
| `Drivers/` | 旧HAL/CMSIS依赖 |
| `HMI/` | V1.3.1历史HMI工程与说明 |

可重建的`Debug/`输出已清理。禁止从本目录重新融合；当前权威代码在根目录
`firmware/`。
