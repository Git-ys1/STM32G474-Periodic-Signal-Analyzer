# 当前开发状态

更新时间：2026-07-30  
版本：V1.3 dashboard单页面代码完成，等待实机回归

## 已完成

- HMI由time/spectrum双页面改为`dashboard`单页面；
- MCU启动命令改为`page dashboard`；
- 解析6字节页面初始化帧；
- 动态保存`s_time`和`s_spec`数字ID；
- 曲线尺寸改为794×145，纵向数据范围改为0～144；
- dashboard就绪后依次绘制时域、频谱并更新六项文本；
- 1T/3T按钮只刷新时域曲线；
- 删除旧页面切换和`current_page`逻辑；
- 保留模拟正弦、256点演示DFT和FE/FD握手；
- 保留频谱横向方向修正；
- C源码已验证为UTF-8；
- STM32 Debug增量构建通过。

## 当前模拟数据

```text
频率：100 kHz
峰值：100 mV
峰峰值：200 mVpp
真有效值：约70.71 mV
```

预期六项文本：

```text
Upp: 200.0 mV
Urms: 70.71 mV
f1: 100.0 kHz
基波: 100.0 kHz / 100.0 mVpk
谐波1: --.- kHz / --.- mVpk
谐波2: --.- kHz / --.- mVpk
```

## 实机回归清单

### 上电

应收到：

```text
A5 20 01 <time_id> <spec_id> 5A
```

随后时域和频谱各完成一次：

```text
FE FF FF FF
FD FF FF FF
```

### 1T

点击`b0`后收到：

```text
A5 01 01 5A
```

只刷新`s_time`，显示一个完整周期。

### 3T

点击`b1`后收到：

```text
A5 01 03 5A
```

只刷新`s_time`，显示三个完整周期。

### 禁止项

实机日志不得出现：

```text
12 FF FF FF
1A FF FF FF
1C FF FF FF
24 FF FF FF
```

## 尚未完成

- 尚未用实物屏或HMI串口联调完成本版本回归；
- 尚未接入队友的真实ADC波形、测量结果和FFT谱峰；
- 当前DFT仍为显示链路演示，不满足G题最终500 Hz频率分辨率。

