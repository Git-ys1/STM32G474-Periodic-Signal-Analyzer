#ifndef INC_DISPLAY_H_
#define INC_DISPLAY_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32g4xx_hal.h"

/**
 * @brief 初始化显示模块。
 *
 * 初始化淘晶驰通信，等待屏幕启动后进入dashboard页面。
 * 页面上报两个曲线真实ID后，显示AnalyzerBridge中的最新稳定结果。
 */
void Display_Init(UART_HandleTypeDef *huart);

/**
 * @brief 显示模块周期任务。
 *
 * 轮询dashboard页面、1T/3T、刷新和随机测试按钮事件；
 * 同时检测AnalyzerBridge的新结果并刷新dashboard。
 * 应在main()的while(1)中持续调用。
 */
void Display_Task(void);

/**
 * @brief 请求使用AnalyzerBridge的最新结果重绘当前dashboard。
 *
 * 该函数只安排主循环绘图，不在串口解析或ADC中断中执行耗时发送。
 */
void Display_RedrawCurrentPage(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_DISPLAY_H_ */
