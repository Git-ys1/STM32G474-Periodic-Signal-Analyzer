# 主线固件

本目录是仓库中唯一允许继续开发、编译和烧录的 STM32 工程。

| 项目 | 说明 |
|---|---|
| [`.mxproject`](.mxproject) | STM32CubeMX 工程元数据 |
| [`ADC.ioc`](ADC.ioc) | STM32G474VET6 外设配置源文件 |
| [`ADC/`](ADC/README.md) | Keil MDK 工程、启动文件和链接脚本 |
| [`Core/`](Core/README.md) | 我方与队友的应用源码、头文件 |
| [`Drivers/`](Drivers/README.md) | STM32 HAL 与 CMSIS-DSP 依赖 |
| [`HMI/`](HMI/README.md) | 当前 HMI 源文件状态说明 |

## 编译

```powershell
cd firmware\ADC\MDK-ARM
& 'D:\Work\Keil5\UV4\UV4.exe' -r 'ADC.uvprojx' -j0 -o 'v24-compact-axis-build.log'
```

验收：`0 Error(s), 0 Warning(s)`。

## 关键入口

| 文件 | 作用 |
|---|---|
| `Core/Src/main.c` | 队友测量链、KEY1和显示任务集成 |
| `Core/Src/analyzer_bridge.c` | 真实ADC折叠、Huber、谐波投影和独立2048点模型Mpp |
| `Core/Src/display.c` | 512/256曲线、动态坐标、淘晶驰协议和运行状态机 |
