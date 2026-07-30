# HMI布局基线

更新时间：2026-07-30  
适用版本：V1.3 dashboard单页面

## dashboard单页面布局

- 屏幕：淘晶驰X2系列 `TJC8048X270_011R`
- 分辨率：800×480
- 页面名：`dashboard`
- HMI工程与MCU源码编码：UTF-8

| 控件 | 用途 | 尺寸或属性 |
|---|---|---|
| `s_time` | 时域曲线 | 794×145，通道0 |
| `s_spec` | 正频率轴频谱 | 794×145，通道0 |
| `b0` | 显示1个周期 | 文本`1T` |
| `b1` | 显示3个周期 | 文本`3T` |
| `t_vpp` | 峰峰值 | type 116，id 2 |
| `t_rms` | 真有效值 | type 116，id 3 |
| `t_freq` | 基频 | type 116，id 4 |
| `t_c1` | 基波频率与峰值幅度 | type 116，id 8 |
| `t_c2` | 第一谐波频率与峰值幅度 | type 116，id 9 |
| `t_c3` | 第二谐波频率与峰值幅度 | type 116，id 10 |

## 曲线ID约束

`s_time`和`s_spec`的数字ID不在MCU代码中写死。页面完成初始化后，由HMI上报两个实际ID：

```text
A5 20 01 time_id spec_id 5A
```

MCU保存：

```text
time_curve_id     = time_id
spectrum_curve_id = spec_id
```

两条曲线均使用通道0，发送宽度均为794点。

## HMI页面事件

`dashboard`页面后初始化事件：

```text
printh A5 20 01
prints s_time.id,1
prints s_spec.id,1
printh 5A
```

按钮弹起事件：

```text
b0: printh A5 01 01 5A
b1: printh A5 01 03 5A
```

两个按钮均不勾选“发送键值”。

