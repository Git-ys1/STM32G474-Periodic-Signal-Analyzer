#ifndef INC_DISPLAY_H_
#define INC_DISPLAY_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32g4xx_hal.h"

/**
 * @brief 初始化显示模块。
 *
 * 初始化淘晶驰通信、准备当前演示数据，等待屏幕启动后进入time页面。
 */
void Display_Init(UART_HandleTypeDef *huart);

/**
 * @brief 显示模块周期任务。
 *
 * 轮询页面及按钮事件，并执行1周期、3周期或频谱整帧绘制。
 * 应在main()的while(1)中持续调用。
 */
void Display_Task(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_DISPLAY_H_ */
