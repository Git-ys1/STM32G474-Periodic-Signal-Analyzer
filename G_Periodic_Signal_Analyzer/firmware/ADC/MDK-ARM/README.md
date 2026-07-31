# MDK-ARM

| 项目 | 说明 |
|---|---|
| `ADC.uvprojx` | Keil工程定义，必须保留 |
| `ADC.uvoptx` | 本地目标配置，保留 |
| `ADC.uvguix.0` | 本机Keil界面状态，不参与源码逻辑 |
| `startup_stm32g474xx.s` | 启动文件 |
| `stm32g474xx_flash.sct` | 链接布局 |
| `EventRecorderStub.scvd` | Event Recorder描述 |
| `DebugConfig/` | 调试配置 |
| `RTE/` | Keil运行环境配置 |
| `old` | 历史工程保留文件，当前不修改 |
| `ADC/` | 构建输出目录，已由`.gitignore`忽略 |
| `v23-build.log` | V2.3构建后生成，已由`.gitignore`忽略 |

本目录中的输出和日志都可由 `ADC.uvprojx` 重新生成。
