#ifndef INC_DISPLAY_H_
#define INC_DISPLAY_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32g4xx_hal.h"

/**
 * @brief 初始化显示模块。
 *
 * 初始化淘晶驰通信、准备当前演示数据，等待屏幕启动后进入dashboard页面。
 * 页面上报两个曲线真实ID后才开始绘图。
 */
void Display_Init(UART_HandleTypeDef *huart);

/**
 * @brief 显示模块周期任务。
 *
 * 轮询dashboard页面及1T/3T按钮事件；页面首次就绪时绘制时域、
 * 频谱并更新六项文本，按钮事件只刷新时域曲线。
 * 应在main()的while(1)中持续调用。
 */
void Display_Task(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_DISPLAY_H_ */
