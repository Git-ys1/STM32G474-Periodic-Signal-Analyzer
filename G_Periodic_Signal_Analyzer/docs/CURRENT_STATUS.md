# 当前开发状态

更新时间：2026-07-30  
版本：V1.3.1 dashboard单页面显示基线，HMI模拟器回归通过

## 已完成

- HMI由time/spectrum双页面改为`dashboard`单页面；
- MCU启动命令改为`page dashboard`；
- 解析6字节页面初始化帧；
- 动态保存`s_time`和`s_spec`数字ID；
- 曲线尺寸改为794×145，纵向数据范围改为0～144；
- dashboard就绪后依次绘制时域、频谱并更新六项文本；
- 1T/3T按钮只刷新时域曲线；
- 新增刷新按钮协议`A5 01 02 5A`，可重新加载dashboard并恢复丢失的首次握手；
- 文本数值改用定点整数格式化，规避newlib-nano未启用浮点printf导致的空白数字；
- 删除旧页面切换和`current_page`逻辑；
- 保留模拟正弦、256点演示DFT和FE/FD握手；
- 保留频谱横向方向修正；
- C源码已验证为UTF-8；
- STM32 Debug Clean Build通过，0 errors、0 warnings；
- 构建规模：text 23880 B、data 96 B、bss 4920 B，总计28896 B；
- USART HMI模拟器已验证1T、3T、频谱和六项文本完整显示；
- 当前配套HMI为最新单页面`testv2.HMI`，旧`testv1.HMI`不得混用；
- 完整故障复盘与发布证据见`DASHBOARD_V1.3.1_RELEASE.md`。

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

## 已完成的HMI模拟器回归

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

### 刷新

在HMI模拟器已经连接后点击`b2`，应收到：

```text
A5 01 02 5A
```

随后MCU发送`page dashboard`，页面重新上报：

```text
A5 20 01 <time_id> <spec_id> 5A
```

时域和频谱应各完成一次FE/FD握手，并显示完整六项数值。

### 禁止项

实机日志不得出现：

```text
12 FF FF FF
1A FF FF FF
1C FF FF FF
24 FF FF FF
```

## 尚未完成

- 尚未在最终TJC8048X270_011R实物屏上完成同版HMI与固件回归；
- 尚未接入队友的真实ADC波形、测量结果和FFT谱峰；
- 当前DFT仍为显示链路演示，不满足G题最终500 Hz频率分辨率。
