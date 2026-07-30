# 淘晶驰HMI通信协议

更新时间：2026-07-30  
适用版本：V1.3 dashboard单页面

## 串口参数

```text
USART3
PC10 = TX
PC11 = RX
115200 bit/s
8N1
```

普通淘晶驰字符串指令由MCU自动追加：

```text
FF FF FF
```

## HMI到MCU的自定义帧

### dashboard初始化帧

```text
A5 20 01 time_id spec_id 5A
```

共6字节。`time_id`与`spec_id`分别是当前HMI工程中`s_time`和`s_spec`的实际数字ID。

MCU收到并校验完整帧后：

1. 保存两个曲线ID；
2. 标记dashboard有效；
3. 绘制当前1T或3T时域曲线；
4. 绘制频谱；
5. 更新六项测量文本。

### 周期按钮帧

| 帧 | 含义 |
|---|---|
| `A5 01 01 5A` | 显示1个完整周期 |
| `A5 01 03 5A` | 显示3个完整周期 |

按钮只刷新`s_time`，不重复计算或发送`s_spec`。

## 曲线透传

每条曲线保持原有可靠握手：

```text
MCU发送 cle id,0
MCU发送 addt id,0,794
HMI返回 FE FF FF FF
MCU发送794字节曲线数据
HMI返回 FD FF FF FF
```

- `FE`：透传就绪；
- `FD`：透传完成。

程序同时记录空闲阶段收到的四字节状态帧，并在同步等待透传时将非FE/FD状态帧作为错误返回。

禁止出现：

```text
12 FF FF FF
1A FF FF FF
1C FF FF FF
24 FF FF FF
```

常见含义：

- `0x12`：曲线ID或通道无效；
- `0x1A`：控件或变量名称无效；
- `0x1C`：文本赋值语法错误；
- `0x24`：串口缓冲区溢出。

## MCU到HMI的文本命令

六项结果均使用objname和完整字符串，例如：

```text
t_vpp.txt="Upp: 200.0 mV"
t_rms.txt="Urms: 70.71 mV"
t_freq.txt="f1: 100.0 kHz"
t_c1.txt="基波: 100.0 kHz / 100.0 mVpk"
t_c2.txt="谐波1: --.- kHz / --.- mVpk"
t_c3.txt="谐波2: --.- kHz / --.- mVpk"
```

每条命令末尾均追加三个`0xFF`。HMI工程与C源码必须统一使用UTF-8。

