# 当前显示基线路径

更新时间：2026-07-30

本文说明当前已验证的淘晶驰显示代码从哪里进入、各函数负责什么、数据如何流动，以及后续接入真实信号时允许修改的边界。

故障根因、HMI数字ID和回归要求见：

- `docs/DISPLAY_BASELINE_FREEZE_2026-07-30.md`

## 1. 当前工程边界

- 显示工程：`projects/tjc_display_demo`
- 主控：STM32G474VET6
- 屏幕：淘晶驰X2系列TJC8048X270_011R，7英寸，800×480
- 通信：USART3，PC10/PC11，115200 bit/s，8N1
- 当前信号：显示模块内部生成的100 kHz、100 mV峰值正弦波
- 当前频谱：256点演示DFT，只用于验证显示链路
- 队友工程：`projects/teammate_adc_reference`，当前未接入且不得随意修改

## 2. 当前代码结构

当前不是把全部逻辑写进一个巨大的`display()`函数，而是将显示相关逻辑集中在一个`display.c`模块中。

```text
projects/tjc_display_demo/Core/
├─ Inc/
│  └─ display.h
└─ Src/
   ├─ main.c
   └─ display.c
```

只对外公开两个入口：

```c
void Display_Init(UART_HandleTypeDef *huart);
void Display_Task(void);
```

其余函数均为`display.c`内部的`static`私有函数。

## 3. main.c入口

`main.c`只负责CubeMX初始化和调用显示模块：

```c
Display_Init(&huart3);

while (1)
{
    Display_Task();
}
```

完整数据流为：

```text
main.c
  ├─ Display_Init(&huart3)
  │    ├─ 保存USART3句柄
  │    ├─ 生成内部测试正弦和DFT输入
  │    └─ 切换到time页面
  │
  └─ Display_Task()
       ├─ 接收HMI页面和按钮消息
       ├─ 调度1周期时域绘图
       ├─ 调度3周期时域绘图
       └─ 调度演示频谱绘图
```

## 4. 两个公开函数

### 4.1 Display_Init()

位置：`Core/Src/display.c`

上电后执行一次，负责：

1. 保存传入的UART句柄；
2. 将当前页面初始化为时域页；
3. 将显示周期数初始化为1；
4. 调用`Demo_PrepareInputData()`生成内部测试数据；
5. 等待屏幕启动；
6. 发送`page time`。

屏幕进入时域页后返回：

```text
A5 20 01 5A
```

程序收到页面就绪帧后才安排时域绘图。

### 4.2 Display_Task()

位置：`Core/Src/display.c`

在`while (1)`中持续调用，负责：

1. 调用`TJC_PollReceive()`读取HMI消息；
2. 读取待执行动作`g_pending_action`；
3. 根据动作调用时域或频谱绘图函数；
4. 没有新动作时不重复刷新曲线。

可执行动作包括：

```text
UI_ACTION_DRAW_TIME_1
UI_ACTION_DRAW_TIME_3
UI_ACTION_DRAW_SPECTRUM
```

## 5. HMI消息接收路径

### 5.1 TJC_PollReceive()

轮询USART3，每轮最多读取有限字节，避免长期阻塞主循环。

### 5.2 TJC_ParserPushByte()

逐字节拼接固定4字节协议：

```text
A5 功能码 参数 5A
```

### 5.3 TJC_ProcessCustomFrame()

解析页面和按钮事件：

| 数据帧 | 含义 |
|---|---|
| `A5 20 01 5A` | time页面加载完成，安排时域绘图 |
| `A5 20 02 5A` | spectrum页面加载完成，安排频谱绘图 |
| `A5 01 01 5A` | 请求显示1个周期 |
| `A5 01 03 5A` | 请求显示3个周期 |
| `A5 02 02 5A` | 正在切换到频谱页，等待频谱页就绪 |
| `A5 02 01 5A` | 正在切换到时域页，等待时域页就绪 |

页面切换通知本身不立即触发曲线发送。程序等待目标页面返回`A5 20 ... 5A`后再绘图，防止向错误页面发送曲线数据。

## 6. 时域显示路径

### 6.1 Demo_PrepareInputData()

当前生成的内部测试信号为：

```text
频率：100 kHz
峰值：100 mV
峰峰值：200 mVpp
有效值：约70.71 mV
```

该函数准备：

- 一个周期的256点正弦数组，用于时域显示；
- 一段256点采样序列，用于演示DFT。

当前屏幕显示的不是队友ADC数据。

### 6.2 Display_DrawTimeFrame()

时域绘图核心流程：

1. 接收一个完整周期的电压数组；
2. 通过线性插值重采样为674个显示点；
3. 根据选择在同一屏显示1个或3个完整周期；
4. 调用`Display_MapToByte()`将电压映射到8位曲线数据；
5. 清空时域曲线；
6. 批量发送674个曲线点；
7. 更新周期模式、峰峰值、真有效值和基频文本。

时域曲线固定参数：

```text
宏：TIME_CURVE_ID
数字ID：11
通道：0
宽度：674
```

对应命令：

```text
cle 11,0
addt 11,0,674
```

### 6.3 Display_UpdateTimeLabels()

更新：

```text
TIME / 1 PERIOD 或 TIME / 3 PERIODS
Upp
Urms
f1
```

当前不动态更新坐标轴刻度。

## 7. 频谱显示路径

### 7.1 Display_DrawDemoSpectrum()

当前演示频谱流程：

1. 对内部256点测试序列直接计算DFT；
2. 只处理正频率部分；
3. 只显示500 kHz以内分量；
4. 忽略幅值小于1 mV的演示计算噪声；
5. 将频率映射到674点横坐标；
6. 将幅值映射成8位曲线数据；
7. 清空并发送频谱曲线；
8. 更新主要频谱分量文本。

当前100 kHz谱线应位于0～500 kHz横轴从左向右约20%的位置。

频谱曲线固定参数：

```text
宏：SPECTRUM_CURVE_ID
数字ID：1
通道：0
宽度：674
```

对应命令：

```text
cle 1,0
addt 1,0,674
```

## 8. 淘晶驰底层通信函数

| 函数 | 职责 |
|---|---|
| `TJC_SendCommand()` | 发送字符串命令并自动追加`FF FF FF` |
| `TJC_SetText()` | 生成文本控件赋值命令 |
| `TJC_SetTextFormat()` | 使用格式化字符串更新文本 |
| `TJC_ClearCurve()` | 发送`cle id,0` |
| `TJC_WaitSpecialReply()` | 等待`FE`或`FD`握手帧 |
| `TJC_SendCurveBlock()` | 完成一整帧`addt`曲线传输 |
| `Display_MapToByte()` | 将真实数值线性映射为8位曲线数据 |

`TJC_SendCurveBlock()`必须维持以下顺序：

```text
发送 addt
  -> 等待 FE FF FF FF
  -> 发送674字节曲线数据
  -> 等待 FD FF FF FF
```

`FE`表示屏幕准备接收透传数据，`FD`表示透传完成，均为正常握手。

## 9. 关键状态变量

| 变量 | 含义 |
|---|---|
| `g_current_page` | 当前页面，time或spectrum |
| `g_visible_periods` | 当前显示1周期或3周期 |
| `g_pending_action` | 下一次`Display_Task()`需要执行的绘图动作 |
| `g_rx_frame[4]` | HMI固定4字节协议接收缓存 |
| `g_curve_buffer[674]` | 即将发送给屏幕的一整帧曲线数据 |
| `g_demo_one_period_mv[]` | 当前内部测试正弦的一个周期 |
| `g_demo_fft_input_mv[]` | 当前演示DFT输入数据 |

## 10. 后续接入真实信号的修改边界

后续与队友确定接口后，主要替换：

- `Demo_PrepareInputData()`的数据来源；
- `Display_DrawDemoSpectrum()`中的演示DFT输入或结果来源；
- 时域、频谱和测量参数的上层输入接口。

以下内容不得顺手修改：

- HMI四字节协议；
- `Display_Init()`的TIME初始页面状态；
- 页面就绪后再绘图的时序；
- 时域曲线ID 11；
- 频谱曲线ID 1；
- 通道0和宽度674；
- `addt -> FE -> 数据 -> FD`握手；
- USART3、PC10/PC11和115200 bit/s配置；
- 已验证的频谱横向方向。

## 11. 当前基线回归

每次修改后至少执行：

```text
上电 -> b1 -> b3 -> bspec -> btime
```

时域和频谱绘图应出现：

```text
FE FF FF FF
FD FF FF FF
```

不得出现：

```text
12 FF FF FF
```

任何结构调整前，先提交当前可运行检查点。结构调整必须是等价迁移，不得同时改变协议、曲线ID、页面状态和数据来源。
