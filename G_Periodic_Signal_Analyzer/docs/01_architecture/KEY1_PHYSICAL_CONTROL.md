# KEY1实体按键控制

更新时间：2026-07-30

适用工程：`firmware`

> 本文主体记录V1.4.1首次引入KEY1时的历史验收。V1.9.0曾把长按动作从
> “测试”改为“刷新真实ADC”；V2.2因现场屏不可触摸，用户已明确要求长按
> 恢复为`Display_RequestTest()`。当前行为以V2.2源码和
> [永久交接文档](../06_handoff/04_HARDWARE_WIRING_AND_HMI.md)为准。

## 1. 使用原因

当前实物为淘晶驰X2系列7英寸不可触摸屏，因此在V1.4融合稳定基线上增加
最小实体按键控制。屏幕页面、HMI协议、时域/频谱绘制和信号分析主链均不改变。

## 2. 硬件连接

最小系统板的非复位按键为KEY1：

```text
PB8 / BOOT0（STM32G474VET6第95脚）

松开：R6 10 kΩ下拉，PB8 = 0
按下：接通3.3 V，PB8 = 1
```

按键为高电平有效，工程原有CubeMX配置已经是：

```text
GPIO_MODE_IT_RISING
GPIO_PULLDOWN
EXTI9_5_IRQn
```

因此本次没有修改`.ioc`、`gpio.c`或`stm32g4xx_it.c`。

> PB8同时复用BOOT0。上电或复位时不要一直按住KEY1，否则BOOT0可能被采样为
> 高电平并影响正常从Flash启动。

## 3. 操作定义

| 操作 | 判定 | 动作 |
|---|---:|---|
| 抖动 | 按住不足30 ms | 忽略 |
| 短按 | 30～999 ms后松开 | 在1T和3T之间切换 |
| 长按 | 至少1000 ms后松开 | V2.2执行原“测试”按钮命令，进入随机测试 |

长按在松开后执行，避免在按键中断里等待、发送串口或绘制曲线。

## 4. 软件实现

编译期开关位于`Core/Src/main.c`：

```c
#define BOARD_KEY_CONTROL_ENABLE 1U
#define BOARD_KEY_DEBOUNCE_MS    30U
#define BOARD_KEY_LONG_PRESS_MS  1000U
```

将`BOARD_KEY_CONTROL_ENABLE`改为`0U`即可关闭实体按键逻辑，原有HMI与显示
链路保持不变。

处理流程：

```text
KEY1按下
  → PB8上升沿触发EXTI9_5
  → HAL_GPIO_EXTI_Callback()只记录按下时刻
  → 主循环BoardKey_Task()轮询PB8
  → 检测松开并计算按压时间
  ├─ 短按：复用原0x01/0x03按钮处理，切换1T/3T
  └─ 长按：复用原0x04按钮处理，进入随机测试
  → Display_Task()在主循环中执行曲线和UART操作
```

中断不调用阻塞式UART函数，也不执行`cle/addt`，避免破坏V1.4已验证的
`addt → FE → 数据 → FD`通信链。

## 5. 构建结果

Keil Clean Rebuild：

```text
Arm Compiler 6.7
Code    = 56880 B
RO-data = 25700 B
RW-data = 52 B
ZI-data = 48284 B
0 Error(s), 0 Warning(s)
```

最终HEX：

```text
ADC.hex
232493 bytes
SHA-256: 36BC21B04451250138FFFA0F95174AC218E66342375379CFE893A96FAFB268D0
```

STM32CubeProgrammer 2.22.0已完成下载、校验和复位，下载后Fault寄存器为0。

## 6. 实物验收

1. 上电后不按KEY1，系统正常进入dashboard；
2. 短按一次，从1T切换到3T；
3. 再短按一次，从3T切回1T；
4. V2.2长按至少1秒并松开，执行与原“测试”按钮相同的随机测试；
5. KEY1与原HMI按钮可以共存，均进入同一按钮命令处理函数；
6. 长按、短按期间ADC分析、时域和频谱显示无异常。

本版本已经实物验收，作为V1.4.1冻结发布。

## 7. V2.2现场恢复测试映射

2026-07-31因现场屏已经更换为不可触摸版本，用户要求KEY1重新承担测试入口：

```text
短按：Display_TogglePeriods()
长按至少1秒并松开：Display_RequestTest()
```

Keil重建为0 error、0 warning；`ADC.hex`长度362048 bytes，SHA-256为
`57DCBB0326D2D2DEE4CBC466559C497FA0B27842C0AFD41ED03566A87349C657`。
固件已完成SWD下载、校验和复位。烧录后RAM回读显示
`s_test_override=1`，且测试编号在3.4秒内继续变化，证明长按测试入口和
自动换组状态已经运行。
