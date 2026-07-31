# 淘晶驰显示基线冻结记录

冻结时间：2026-07-30 01:04（Asia/Shanghai）

## 冻结范围

- 工程：`archive/firmware/tjc_display_demo`
- 主控：STM32G474VET6
- 屏幕：淘晶驰X2系列TJC8048X270_011R
- 串口：USART3，PC10/PC11，115200 bit/s，8N1
- 测试信号：模块内部生成的100 kHz、100 mV峰值正弦
- 频谱：256点演示DFT，仅用于显示链路测试
- 队友工程`teammate/archive/adc_reference`未修改、未接入

## 本次故障根因

本次不是单一故障，而是两个独立错误叠加。

### 1. 封装改变了页面初始状态

原始已验证代码将当前页面初始化为`TIME`。错误封装曾改为`NONE`，导致没有先收到页面就绪帧时，`b1`和`b3`虽能被MCU接收，却不会触发时域绘图，表现为屏幕完全无反应。

最终恢复为：

```c
static uint8_t g_current_page = TJC_PAGE_TIME;
```

### 2. 两个页面错误共用曲线数字ID

两个页面的曲线控件`objname`都叫`s0`，但数字ID不同：

| 页面 | objname | 数字ID | 通道 | 宽度 |
|---|---|---:|---:|---:|
| time | s0 | 11 | 0 | 674 |
| spectrum | s0 | 1 | 0 | 674 |

错误代码在时域页发送`cle 1,0`和`addt 1,0,674`，屏幕因此返回：

```text
12 FF FF FF
```

`0x12`表示曲线ID或通道设置无效。频谱页正常，是因为频谱页的真实数字ID恰好就是1。

最终固定为：

```text
time:     cle 11,0 / addt 11,0,674
spectrum: cle 1,0  / addt 1,0,674
```

## 冻结后的代码结构

只保留一个显示模块，避免通信状态机再次被拆散：

```text
Core/Inc/display.h
Core/Src/display.c
Core/Src/main.c
```

`main.c`只执行：

```c
Display_Init(&huart3);

while (1)
{
    Display_Task();
}
```

原有`tjc_hmi.c/.h`已经删除。UART发送、四字节协议解析、FE/FD握手、模拟正弦、时域重采样和演示频谱全部保留在同一个`display.c`执行上下文中。

## HMI协议冻结

页面后初始化事件：

```text
time:     printh A5 20 01 5A
spectrum: printh A5 20 02 5A
```

按钮事件：

```text
b1:    printh A5 01 01 5A
b3:    printh A5 01 03 5A
bspec: printh A5 02 02 5A
       page spectrum
btime: printh A5 02 01 5A
       page time
```

不得恢复旧的`prints s0.type/id/x/y/w/h`动态几何上报。

## 构建与回归

STM32CubeIDE 2.1.0 Debug Clean Build结果：

```text
0 errors
0 warnings
```

烧录后按以下顺序回归：

```text
上电 -> b1 -> b3 -> bspec -> btime
```

时域和频谱发送曲线时均应收到：

```text
FE FF FF FF
FD FF FF FF
```

不得再出现：

```text
12 FF FF FF
```

## Git纪律

- 任何后续结构重构前，必须先提交当前可运行检查点。
- 重构只能做等价迁移，禁止同时修改协议、页面状态、曲线ID或测试信号。
- 每次修改后必须完成Clean Build和上述五步实机回归。
- 队友工程与显示工程保持独立，未经明确接口确认不得互相复制实现。
