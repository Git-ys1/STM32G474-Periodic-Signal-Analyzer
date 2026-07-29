#ifndef INC_TJC_HMI_H_
#define INC_TJC_HMI_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32g4xx_hal.h"

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief 淘晶驰屏幕上报给应用层的事件。
 *
 * 页面切换由HMI按钮自身执行；MCU只在收到目标页面就绪事件后绘图，
 * 避免向当前未显示的页面发送曲线数据。
 */
typedef enum
{
    TJC_EVENT_NONE = 0,
    TJC_EVENT_TIME_PAGE_READY,
    TJC_EVENT_SPECTRUM_PAGE_READY,
    TJC_EVENT_SHOW_ONE_PERIOD,
    TJC_EVENT_SHOW_THREE_PERIODS
} TJC_Event;

/**
 * @brief 初始化淘晶驰通信模块。
 * @param huart 与串口屏连接的UART句柄。
 */
void TJC_Init(UART_HandleTypeDef *huart);

/**
 * @brief 发送淘晶驰字符串指令，并自动追加三个0xFF结束字节。
 */
HAL_StatusTypeDef TJC_SendCommand(const char *command);

/**
 * @brief 修改文本控件的txt属性。
 * @param object_name 控件完整名称，例如"time.t_vpp"。
 * @param text 要显示的文本。
 */
HAL_StatusTypeDef TJC_SetText(const char *object_name,
                              const char *text);

/**
 * @brief 使用printf风格格式更新文本控件。
 */
HAL_StatusTypeDef TJC_SetTextFormat(const char *object_name,
                                    const char *format,
                                    ...);

/**
 * @brief 清空曲线控件的指定通道。
 */
HAL_StatusTypeDef TJC_ClearCurve(uint8_t object_id,
                                 uint8_t channel);

/**
 * @brief 使用addt批量发送一帧8位曲线数据。
 *
 * 内部保持“addt -> FE -> 原始数据 -> FD”的官方透传握手流程。
 */
HAL_StatusTypeDef TJC_SendCurve(uint8_t object_id,
                                uint8_t channel,
                                const uint8_t *data,
                                uint16_t point_count);

/**
 * @brief 轮询串口并返回一个页面或按钮事件。
 * @retval true表示获得有效事件，false表示当前没有完整有效事件。
 */
bool TJC_PollEvent(TJC_Event *event);

#ifdef __cplusplus
}
#endif

#endif /* INC_TJC_HMI_H_ */
