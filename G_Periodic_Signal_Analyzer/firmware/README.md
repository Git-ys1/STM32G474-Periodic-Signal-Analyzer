# 主线固件

本目录是仓库中唯一允许继续开发、编译和烧录的 STM32 工程。当前工作基线
已同步队友2026-08-01 `4096-2`版本：ADC1/PA0与ADC2/PA1各采2048点，交错为
浮点电压`VO[4096]`，等效采样率约2.048193 MS/s。桥接层直接消费VO，不再
保存原始ADC副本或重复执行码值换算。

当前本地实物屏配置：KEY1短按切换1T/3T，长按进入随机ADC测试；测试宏开启。

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
& 'D:\Work\Keil5\UV4\UV4.exe' -r 'ADC.uvprojx' -j0 -o 'v25-4096-2-vo-build.log'
```

验收：`0 Error(s), 0 Warning(s)`。

## 关键入口

| 文件 | 作用 |
|---|---|
| `Core/Src/main.c` | 双ADC采集、VO[4096]交错、队友测量链、KEY1和显示任务集成 |
| `Core/Src/analyzer_bridge.c` | 直接读取VO的4096点折叠、Huber、谐波投影和独立模型Mpp |
| `Core/Src/display.c` | 512/256曲线、动态坐标、淘晶驰协议和运行状态机 |
