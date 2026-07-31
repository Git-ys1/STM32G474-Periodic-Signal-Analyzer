# HMI布局基线

更新时间：2026-07-31
适用版本：V2.0基线 + V2.2普通/增强Huber折叠切换

配套源工程：
`archive/firmware/tjc_display_demo/HMI/TJC8048X270_dashboard_v1.3.1.HMI`，
来源为最新单页面`testv2.HMI`。`testv1.HMI`是旧双页面工程，不得与当前固件混用。

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
| `b2` | 进入真实结果自动刷新 | 文本`刷新` |
| `b3` | 进入随机测试自动刷新 | 文本`测试` |
| `b4` | 单次清除曲线和参数 | type 98，id 15，文本`清除` |
| `b5` | 停止绘画并冻结画面 | type 98，id 16，文本`停止` |
| `t_trigger` | 下拉框标题 | 文本`触发方式` |
| `cb0` | 选择时域触发方式 | 下拉框，默认`val=1` |
| `t_fold` | 状态开关标题 | 文本`折叠方式` |
| `sw_huber` | 普通/Huber折叠切换 | 状态开关，V2.2定版默认`val=1` |
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
printh A5 02 07
prints cb0.val,1
printh 5A
printh A5 02 08
prints sw_huber.val,1
printh 5A
```

第一帧上报两条曲线ID，第二帧同步当前触发方式，第三帧同步普通/Huber折叠
方式。三帧必须保持完整顺序，不要把参数塞进原6字节曲线ID帧。

按钮弹起事件：

```text
b0: printh A5 01 01 5A
b1: printh A5 01 03 5A
b2: printh A5 01 02 5A
b3: printh A5 01 04 5A
b4: printh A5 01 05 5A
b5: printh A5 01 06 5A
```

六个按钮均不勾选“发送键值”。

V2.0新增时域触发下拉框。`触发方式`是旁边文本控件`t_trigger`的标题；
下拉框对象名使用`cb0`，便于与官方Wiki示例一致。属性设置：

```text
objname = cb0
vscope  = 全局
val     = 1
dir     = 1
qty     = 4
path_m  = 64
```

`path`属性点开“多行输入”，逐行填写，不要把`\r`当普通字符输入：

```text
无触发
上升过零
下降过零
正峰值
```

在`cb0`的弹起事件中写：

```text
if(cb0.down==0)
{
  printh A5 02 07
  prints cb0.val,1
  printh 5A
}
```

选择四项后分别发送：

```text
无触发:     A5 02 07 00 5A
上升过零:   A5 02 07 01 5A
下降过零:   A5 02 07 02 5A
正峰值:     A5 02 07 03 5A
```

`if(cb0.down==0)`保证只在下拉框收回、选项确定后发送；`prints cb0.val,1`
按照官方指令发送数值属性的低1字节。固件默认使用上升过零，所以HMI默认
`val`也设为1。`vscope`设为全局，使固件执行`page dashboard`刷新页面时仍
保留用户的选择；页面后初始化再把保留下来的`val`同步给MCU。下拉框不修改
曲线ID或`addt`透传逻辑。

官方参考：

- [下拉框控件](http://wiki2.tjc1688.com/widgets/ComboBox.html)
- [prints发送变量或常量](http://wiki2.tjc1688.com/commands/prints.html)

## V2.2普通/增强Huber状态开关

在`dashboard`新增淘晶驰“状态开关”控件。X2系列原生支持该控件，建议属性：

```text
objname = sw_huber
vscope  = 全局
val     = 1
txt     = 普通/Huber
dez     = 0
```

旁边可增加文本控件：

```text
objname = t_fold
txt     = 折叠方式
```

官方说明要求状态开关的`txt`要么为空，要么用斜杠分隔两种状态文字。因此不能
写成`普通 Huber`，应使用`普通/Huber`。`val=0`是V2.0普通折叠，
`val=1`是两遍Huber折叠后再投影到FFT已识别的整数谐波；用户完成实屏验收后，
V2.2定版默认改为1。现有界面文字不必改。

在`sw_huber`的弹起事件中写：

```text
printh A5 02 08
prints sw_huber.val,1
printh 5A
```

分别产生：

```text
普通:  A5 02 08 00 5A
Huber增强: A5 02 08 01 5A
```

`vscope`设为全局，使MCU执行`page dashboard`后保留用户选择；页面后初始化
必须再发送一次当前`sw_huber.val`。运行中切换会用当前最新一帧ADC或当前测试组
立即重建并只重画时域曲线，不改变频谱、`Upp`、`Urms`和频率文本。状态开关
不勾选“发送键值”。

官方参考：

- [状态开关控件](http://wiki2.tjc1688.com/widgets/Switch.html)
