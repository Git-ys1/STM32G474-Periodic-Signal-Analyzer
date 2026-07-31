#ifndef INC_DISPLAY_H_
#define INC_DISPLAY_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32g4xx_hal.h"

/**
 * @brief 时域显示触发模式。
 *
 * 触发只决定一个周期波形从哪个相位开始显示，不修改ADC采样、Vpp、
 * RMS、频率或频谱结果。
 */
typedef enum
{
    DISPLAY_TRIGGER_OFF = 0,
    DISPLAY_TRIGGER_RISING_ZERO,
    DISPLAY_TRIGGER_FALLING_ZERO,
    DISPLAY_TRIGGER_POSITIVE_PEAK,
    DISPLAY_TRIGGER_MODE_COUNT
} DisplayTriggerMode;

/**
 * @brief 初始化显示模块。
 *
 * 初始化淘晶驰通信，等待屏幕启动后进入dashboard页面。
 * 页面上报两个曲线真实ID后保持停止状态，直到收到刷新或测试命令。
 */
void Display_Init(UART_HandleTypeDef *huart);

/**
 * @brief 显示模块周期任务。
 *
 * 处理dashboard页面、1T/3T、普通/Huber折叠、刷新、测试、清除和停止事件；
 * 运行状态下每3秒刷新一次真实结果或随机测试结果。
 * 应在main()的while(1)中持续调用。
 */
void Display_Task(void);

/**
 * @brief 请求在当前运行模式下立即重绘dashboard。
 *
 * 停止状态下不绘图；该函数只安排主循环动作，不执行耗时UART发送。
 */
void Display_RedrawCurrentPage(void);

/**
 * @brief 通过原HMI按钮命令处理入口，在1T和3T之间切换。
 */
void Display_TogglePeriods(void);

/**
 * @brief 通过原HMI按钮命令处理入口，恢复真实ADC自动刷新。
 */
void Display_RequestRefresh(void);

/**
 * @brief 通过原HMI按钮命令处理入口，启动随机测试自动刷新。
 */
void Display_RequestTest(void);

/**
 * @brief 设置时域显示触发模式并在运行状态下安排一次时域重绘。
 */
void Display_SetTriggerMode(DisplayTriggerMode mode);

/**
 * @brief 获取当前时域显示触发模式。
 */
DisplayTriggerMode Display_GetTriggerMode(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_DISPLAY_H_ */
