# 显示模块架构

更新时间：2026-07-30  
适用版本：V1.3 dashboard单页面

## 工程边界

- 显示工程：`archive/firmware/tjc_display_demo`
- 信号处理参考工程：`teammate/archive/adc_reference`
- 本轮只修改显示工程，不接入或修改队友ADC、DMA、TIM3与FFT实现。

显示模块仍保持冻结后的单模块结构：

```text
Core/Inc/display.h
Core/Src/display.c
Core/Src/main.c
```

`main.c`只负责CubeMX初始化与两个公开入口：

```c
Display_Init(&huart3);

while (1)
{
    Display_Task();
}
```

## 初始化路径

```text
Display_Init()
  -> 保存USART3句柄
  -> 生成100 kHz模拟正弦与演示DFT输入
  -> 等待屏幕启动
  -> 发送 page dashboard
  -> 不立即发送曲线
```

屏幕刷新完成后上报：

```text
A5 20 01 time_id spec_id 5A
```

程序随后执行：

```text
保存动态曲线ID
  -> 绘制当前周期模式的时域曲线
  -> 绘制频谱
  -> 更新六项结果文本
```

## 任务调度

`Display_Task()`持续轮询UART，并只执行三个动作：

```text
UI_ACTION_DRAW_DASHBOARD
UI_ACTION_DRAW_TIME_1
UI_ACTION_DRAW_TIME_3
```

已删除旧版双页面状态、页面切换事件和`current_page`判断。

## 时域绘图

`Display_DrawTimeFrame()`：

1. 接收一个完整周期的电压数组；
2. 插值为794个显示点；
3. 按1T或3T将周期铺满整屏；
4. 将`[-满量程,+满量程]`映射到5～139；
5. 使用动态`s_time`数字ID发送`cle`和`addt`。

按钮事件只调用此路径，不刷新频谱和六项固定测量结果。

## 频谱绘图

`Display_DrawDemoSpectrum()`仍保留当前256点直接DFT，仅用于显示联调：

- `Fs = 1.024 MHz`
- `N = 256`
- 正频率轴显示0～500 kHz
- 频率映射到0～793
- 幅值映射到3～141

淘晶驰整帧曲线写入方向与数组索引相反，因此继续反转发送缓冲区位置。100 kHz谱线最终应位于画面左侧约20%，即显示横坐标约159。

## 文本更新

`Display_UpdateResultTexts()`统一更新：

```text
t_vpp
t_rms
t_freq
t_c1
t_c2
t_c3
```

函数接受峰峰值、真有效值、基频及最多三个`SpectrumPeak`，并始终发送包含名称、数值和单位的完整UTF-8字符串。

## 后续真实数据接入边界

后续与队友确认接口后，主要替换：

- `Demo_PrepareInputData()`的数据来源；
- 演示DFT结果来源；
- `SpectrumPeak`数组和测量值输入。

不得顺手修改：

- dashboard 6字节初始化帧；
- 动态曲线ID；
- 794×145曲线尺寸；
- 1T/3T按钮协议；
- `addt -> FE -> 数据 -> FD`握手；
- USART3、PC10/PC11与115200波特率；
- 已验证的频谱方向。

