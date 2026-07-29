#include "tjc_hmi.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define TJC_FRAME_HEAD             0xA5U
#define TJC_FRAME_END              0x5AU
#define TJC_FRAME_SIZE             4U
#define TJC_RX_BYTES_PER_POLL      16U
#define TJC_MAX_CURVE_POINTS       4096U

static UART_HandleTypeDef *s_tjc_uart = NULL;
static uint8_t s_rx_frame[TJC_FRAME_SIZE];
static uint8_t s_rx_frame_index = 0U;

/**
 * @brief 等待addt透传协议的四字节应答。
 * @param reply_code 0xFE表示准备接收，0xFD表示透传完成。
 * @param timeout_ms 最长等待时间，单位ms。
 * @return true表示收到完整应答，false表示未初始化或等待超时。
 */
static bool TJC_WaitSpecialReply(uint8_t reply_code,
                                 uint32_t timeout_ms)
{
    uint32_t start_tick;
    uint8_t state = 0U;
    uint8_t byte = 0U;

    if (s_tjc_uart == NULL)
    {
        return false;
    }

    start_tick = HAL_GetTick();

    while ((uint32_t)(HAL_GetTick() - start_tick) < timeout_ms)
    {
        if (HAL_UART_Receive(s_tjc_uart, &byte, 1U, 10U) != HAL_OK)
        {
            continue;
        }

        if (state == 0U)
        {
            state = (byte == reply_code) ? 1U : 0U;
        }
        else if (byte == 0xFFU)
        {
            state++;

            if (state == 4U)
            {
                return true;
            }
        }
        else
        {
            state = (byte == reply_code) ? 1U : 0U;
        }
    }

    return false;
}

/**
 * @brief 将一帧“A5 功能 数据 5A”转换成应用层事件。
 * @param frame 已接收完成的四字节自定义协议帧。
 * @return 对应的页面或按钮事件；帧无效或无需处理时返回TJC_EVENT_NONE。
 */
static TJC_Event TJC_DecodeFrame(const uint8_t *frame)
{
    if ((frame == NULL) ||
        (frame[0] != TJC_FRAME_HEAD) ||
        (frame[3] != TJC_FRAME_END))
    {
        return TJC_EVENT_NONE;
    }

    if (frame[1] == 0x20U)
    {
        if (frame[2] == 0x01U)
        {
            return TJC_EVENT_TIME_PAGE_READY;
        }

        if (frame[2] == 0x02U)
        {
            return TJC_EVENT_SPECTRUM_PAGE_READY;
        }
    }
    else if (frame[1] == 0x01U)
    {
        if (frame[2] == 0x01U)
        {
            return TJC_EVENT_SHOW_ONE_PERIOD;
        }

        if (frame[2] == 0x03U)
        {
            return TJC_EVENT_SHOW_THREE_PERIODS;
        }
    }

    /*
     * 功能码0x02只是页面切换通知。实际绘图等待目标页面发来的
     * 0x20就绪帧，因此这里不向应用层产生事件。
     */
    return TJC_EVENT_NONE;
}

/**
 * @brief 将一个接收字节送入固定四字节帧解析器。
 * @param byte 本次收到的一个字节。
 * @param event 用于返回解析得到的应用层事件。
 * @return true表示得到有效事件，false表示帧尚未完成或无需处理。
 */
static bool TJC_ParserPushByte(uint8_t byte,
                               TJC_Event *event)
{
    if (event == NULL)
    {
        return false;
    }

    if (s_rx_frame_index == 0U)
    {
        if (byte == TJC_FRAME_HEAD)
        {
            s_rx_frame[0] = byte;
            s_rx_frame_index = 1U;
        }

        return false;
    }

    s_rx_frame[s_rx_frame_index] = byte;
    s_rx_frame_index++;

    if (s_rx_frame_index < TJC_FRAME_SIZE)
    {
        return false;
    }

    s_rx_frame_index = 0U;
    *event = TJC_DecodeFrame(s_rx_frame);

    /*
     * 无效帧的最后一个字节若恰好是新帧头，则立即作为下一帧起点，
     * 提高串口丢字节后的重新同步能力。
     */
    if ((*event == TJC_EVENT_NONE) &&
        (byte == TJC_FRAME_HEAD))
    {
        s_rx_frame[0] = byte;
        s_rx_frame_index = 1U;
    }

    return (*event != TJC_EVENT_NONE);
}

/**
 * @brief 初始化淘晶驰串口通信模块。
 * @param huart 与串口屏连接的UART句柄。
 *
 * 保存UART句柄并清空接收状态。后续所有TJC发送和接收函数均使用
 * 此处传入的句柄，因此底层模块不直接依赖main.c中的huart3全局变量。
 */
void TJC_Init(UART_HandleTypeDef *huart)
{
    s_tjc_uart = huart;
    s_rx_frame_index = 0U;
}

/**
 * @brief 向淘晶驰屏发送一条标准字符串指令。
 * @param command 不含结束符的ASCII指令，例如"page time"。
 * @return HAL_OK表示指令和结束符均发送成功，其余值表示参数或串口错误。
 *
 * 淘晶驰字符串指令必须以三个0xFF结束，本函数统一追加结束字节。
 */
HAL_StatusTypeDef TJC_SendCommand(const char *command)
{
    static const uint8_t end_code[3] =
    {
        0xFFU, 0xFFU, 0xFFU
    };
    HAL_StatusTypeDef status;

    if ((s_tjc_uart == NULL) || (command == NULL))
    {
        return HAL_ERROR;
    }

    status = HAL_UART_Transmit(
        s_tjc_uart,
        (uint8_t *)command,
        (uint16_t)strlen(command),
        500U
    );

    if (status != HAL_OK)
    {
        return status;
    }

    return HAL_UART_Transmit(
        s_tjc_uart,
        (uint8_t *)end_code,
        sizeof(end_code),
        500U
    );
}

/**
 * @brief 修改淘晶驰文本控件的txt属性。
 * @param object_name 控件完整名称，例如"time.t_vpp"。
 * @param text 需要显示的文本。
 * @return HAL_OK表示发送成功，其余值表示格式化或串口发送失败。
 */
HAL_StatusTypeDef TJC_SetText(const char *object_name,
                              const char *text)
{
    char command[96];
    int length;

    if ((object_name == NULL) || (text == NULL))
    {
        return HAL_ERROR;
    }

    length = snprintf(
        command,
        sizeof(command),
        "%s.txt=\"%s\"",
        object_name,
        text
    );

    if ((length <= 0) || ((size_t)length >= sizeof(command)))
    {
        return HAL_ERROR;
    }

    return TJC_SendCommand(command);
}

/**
 * @brief 按printf格式生成文本并写入淘晶驰文本控件。
 * @param object_name 控件完整名称。
 * @param format printf格式字符串，后面可跟对应的可变参数。
 * @return HAL_OK表示发送成功，其余值表示参数、格式化或串口错误。
 */
HAL_StatusTypeDef TJC_SetTextFormat(const char *object_name,
                                    const char *format,
                                    ...)
{
    char text[64];
    va_list arguments;
    int length;

    if ((object_name == NULL) || (format == NULL))
    {
        return HAL_ERROR;
    }

    va_start(arguments, format);
    length = vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);

    if ((length < 0) || ((size_t)length >= sizeof(text)))
    {
        return HAL_ERROR;
    }

    return TJC_SetText(object_name, text);
}

/**
 * @brief 清空当前页面曲线控件的指定通道。
 * @param object_id 曲线控件ID。
 * @param channel 曲线通道编号。
 * @return HAL_OK表示cle指令发送成功，其余值表示格式化或串口错误。
 */
HAL_StatusTypeDef TJC_ClearCurve(uint8_t object_id,
                                 uint8_t channel)
{
    char command[24];
    int length;

    length = snprintf(
        command,
        sizeof(command),
        "cle %u,%u",
        (unsigned int)object_id,
        (unsigned int)channel
    );

    if ((length <= 0) || ((size_t)length >= sizeof(command)))
    {
        return HAL_ERROR;
    }

    return TJC_SendCommand(command);
}

/**
 * @brief 使用addt命令批量发送一整帧8位曲线点。
 * @param object_id 曲线控件ID。
 * @param channel 曲线通道编号。
 * @param data 已映射到0~255的曲线数据。
 * @param point_count 本次发送的曲线点数。
 * @return HAL_OK表示命令、数据和两次握手均成功，其余值表示错误或超时。
 *
 * 固定流程为：发送addt -> 等待FE -> 发送原始数据 -> 等待FD。
 */
HAL_StatusTypeDef TJC_SendCurve(uint8_t object_id,
                                uint8_t channel,
                                const uint8_t *data,
                                uint16_t point_count)
{
    char command[32];
    int length;
    HAL_StatusTypeDef status;

    if ((s_tjc_uart == NULL) ||
        (data == NULL) ||
        (point_count == 0U) ||
        (point_count > TJC_MAX_CURVE_POINTS))
    {
        return HAL_ERROR;
    }

    length = snprintf(
        command,
        sizeof(command),
        "addt %u,%u,%u",
        (unsigned int)object_id,
        (unsigned int)channel,
        (unsigned int)point_count
    );

    if ((length <= 0) || ((size_t)length >= sizeof(command)))
    {
        return HAL_ERROR;
    }

    status = TJC_SendCommand(command);

    if (status != HAL_OK)
    {
        return status;
    }

    if (!TJC_WaitSpecialReply(0xFEU, 500U))
    {
        return HAL_TIMEOUT;
    }

    status = HAL_UART_Transmit(
        s_tjc_uart,
        (uint8_t *)data,
        point_count,
        1000U
    );

    if (status != HAL_OK)
    {
        return status;
    }

    if (!TJC_WaitSpecialReply(0xFDU, 1000U))
    {
        return HAL_TIMEOUT;
    }

    return HAL_OK;
}

/**
 * @brief 在主循环中轮询接收HMI页面和按钮事件。
 * @param event 用于返回解析得到的页面或按钮事件。
 * @return true表示获得有效事件，false表示未初始化、参数无效或当前无事件。
 *
 * 每轮最多读取16字节，单字节超时1 ms，避免主循环长期阻塞。
 */
bool TJC_PollEvent(TJC_Event *event)
{
    uint8_t byte;
    uint8_t count;

    if ((s_tjc_uart == NULL) || (event == NULL))
    {
        return false;
    }

    *event = TJC_EVENT_NONE;

    for (count = 0U; count < TJC_RX_BYTES_PER_POLL; ++count)
    {
        if (HAL_UART_Receive(
                s_tjc_uart,
                &byte,
                1U,
                1U
            ) != HAL_OK)
        {
            break;
        }

        if (TJC_ParserPushByte(byte, event))
        {
            return true;
        }
    }

    return false;
}
