#include "display.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* 淘晶驰自定义串口协议。 */
#define TJC_FRAME_HEAD               0xA5U
#define TJC_FRAME_END                0x5AU

#define TJC_PAGE_TIME                0x01U
#define TJC_PAGE_SPECTRUM            0x02U

/*
 * 两个页面的曲线控件虽然都叫s0，但数字ID不同。
 * time.s0=11，spectrum.s0=1；两者均为通道0、宽674。
 */
#define TIME_CURVE_ID                11U
#define SPECTRUM_CURVE_ID            1U
#define TJC_CURVE_CHANNEL            0U
#define TJC_CURVE_WIDTH              674U
#define TJC_CURVE_DATA_MAX           255U
#define TJC_CURVE_MARGIN             10U

/* 当前显示联调使用的模拟信号：100 kHz、峰值100 mV。 */
#define DEMO_SIGNAL_FREQUENCY_HZ     100000.0f
#define DEMO_SIGNAL_PEAK_MV          100.0f
#define DEMO_TIME_FULL_SCALE_MV      120.0f
#define DEMO_PERIOD_SAMPLE_COUNT     256U

/*
 * 演示频谱参数：Fs=1.024 MHz、N=256、频点间隔4 kHz。
 * 该DFT只验证显示链路，不代表G题最终500 Hz分辨率方案。
 */
#define DEMO_FFT_SAMPLE_COUNT        256U
#define DEMO_FFT_SAMPLE_RATE_HZ      1024000.0f
#define DEMO_SPECTRUM_MAX_HZ         500000.0f
#define DEMO_SPECTRUM_FULL_SCALE_MV  120.0f
#define TJC_PI                       3.14159265358979323846f

/**
 * @brief 主循环待执行的显示动作。
 *
 * 与已验证的单文件main.c保持一致：串口解析只设置动作，
 * 整帧绘图统一由Display_Task()执行。
 */
typedef enum
{
    UI_ACTION_NONE = 0,
    UI_ACTION_DRAW_TIME_1,
    UI_ACTION_DRAW_TIME_3,
    UI_ACTION_DRAW_SPECTRUM
} UI_Action;

/* 仅用于替代原main.c中的全局huart3，不改变串口行为。 */
static UART_HandleTypeDef *s_uart = NULL;

/* 以下状态初值与test/main绝对正确.c完全一致。 */
static uint8_t g_current_page = TJC_PAGE_TIME;
static uint8_t g_visible_periods = 1U;
static UI_Action g_pending_action = UI_ACTION_NONE;

/* HMI自定义帧固定为“A5 功能 数据 5A”。 */
static uint8_t g_rx_frame[4];
static uint8_t g_rx_frame_index = 0U;

static uint8_t g_curve_buffer[TJC_CURVE_WIDTH];
static float g_demo_one_period_mv[DEMO_PERIOD_SAMPLE_COUNT];
static float g_demo_fft_input_mv[DEMO_FFT_SAMPLE_COUNT];

/**
 * @brief 向淘晶驰屏发送字符串指令，并自动追加三个0xFF。
 * @param command 不含结束符的ASCII指令。
 * @return HAL发送状态。
 */
static HAL_StatusTypeDef TJC_SendCommand(const char *command)
{
    static const uint8_t end_code[3] = {0xFFU, 0xFFU, 0xFFU};
    HAL_StatusTypeDef status;

    if ((s_uart == NULL) || (command == NULL))
    {
        return HAL_ERROR;
    }

    status = HAL_UART_Transmit(
        s_uart,
        (uint8_t *)command,
        (uint16_t)strlen(command),
        500U
    );

    if (status != HAL_OK)
    {
        return status;
    }

    return HAL_UART_Transmit(
        s_uart,
        (uint8_t *)end_code,
        sizeof(end_code),
        500U
    );
}

/**
 * @brief 修改文本控件的txt属性。
 * @param object_name 控件完整名称，例如"time.t_vpp"。
 * @param text 要显示的ASCII文本。
 * @return 指令格式化或串口发送状态。
 */
static HAL_StatusTypeDef TJC_SetText(const char *object_name,
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
 * @brief 使用printf风格格式更新文本控件。
 * @param object_name 文本控件完整名称。
 * @param format printf格式字符串。
 * @return 格式化或串口发送状态。
 */
static HAL_StatusTypeDef TJC_SetTextFormat(const char *object_name,
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
 * @brief 清空指定曲线控件的0号通道。
 * @param curve_id 当前页面s0的真实数字ID。
 * @return 指令格式化或串口发送状态。
 */
static HAL_StatusTypeDef TJC_ClearCurve(uint8_t curve_id)
{
    char command[24];
    int length;

    length = snprintf(
        command,
        sizeof(command),
        "cle %u,%u",
        (unsigned int)curve_id,
        (unsigned int)TJC_CURVE_CHANNEL
    );

    if ((length <= 0) || ((size_t)length >= sizeof(command)))
    {
        return HAL_ERROR;
    }

    return TJC_SendCommand(command);
}

/**
 * @brief 等待addt透传的“FE/FD FF FF FF”四字节应答。
 * @param reply_code 0xFE表示准备接收，0xFD表示透传完成。
 * @param timeout_ms 最长等待时间。
 * @return true表示收到完整应答，false表示超时或串口未初始化。
 */
static bool TJC_WaitSpecialReply(uint8_t reply_code,
                                 uint32_t timeout_ms)
{
    uint32_t start_tick;
    uint8_t state = 0U;
    uint8_t byte = 0U;

    if (s_uart == NULL)
    {
        return false;
    }

    start_tick = HAL_GetTick();

    while ((uint32_t)(HAL_GetTick() - start_tick) < timeout_ms)
    {
        if (HAL_UART_Receive(s_uart, &byte, 1U, 10U) != HAL_OK)
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
 * @brief 使用addt发送一整帧8位曲线点。
 * @param curve_id 当前页面s0的真实数字ID。
 * @param data 曲线数组。
 * @param point_count 曲线点数。
 * @return HAL_OK表示“addt -> FE -> 数据 -> FD”全部成功。
 */
static HAL_StatusTypeDef TJC_SendCurveBlock(uint8_t curve_id,
                                            const uint8_t *data,
                                            uint16_t point_count)
{
    char command[32];
    int length;
    HAL_StatusTypeDef status;

    if ((data == NULL) ||
        (point_count == 0U) ||
        (point_count > TJC_CURVE_WIDTH))
    {
        return HAL_ERROR;
    }

    length = snprintf(
        command,
        sizeof(command),
        "addt %u,%u,%u",
        (unsigned int)curve_id,
        (unsigned int)TJC_CURVE_CHANNEL,
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
        s_uart,
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
 * @brief 将任意数值区间线性映射到淘晶驰8位曲线坐标。
 * @param value 当前输入数值。
 * @param value_min 输入区间下限。
 * @param value_max 输入区间上限。
 * @return 映射到10~245范围内的8位曲线值。
 *
 * 时域输入区间为[-满量程,+满量程]，频谱输入区间为[0,满量程]。
 */
static uint8_t Display_MapToByte(float value,
                                 float value_min,
                                 float value_max)
{
    float normalized;
    float mapped;
    const float output_min = (float)TJC_CURVE_MARGIN;
    const float output_max =
        (float)(TJC_CURVE_DATA_MAX - TJC_CURVE_MARGIN);

    if (value_max <= value_min)
    {
        return TJC_CURVE_MARGIN;
    }

    if (value < value_min)
    {
        value = value_min;
    }
    else if (value > value_max)
    {
        value = value_max;
    }

    normalized = (value - value_min) / (value_max - value_min);
    mapped = output_min + normalized * (output_max - output_min);

    return (uint8_t)(mapped + 0.5f);
}

/**
 * @brief 生成演示用时域周期数组和DFT输入数组。
 * @return 无。
 *
 * 本轮保留模拟数据，后续与信号处理队友确定接口后再替换。
 */
static void Demo_PrepareInputData(void)
{
    uint32_t i;

    for (i = 0U; i < DEMO_PERIOD_SAMPLE_COUNT; ++i)
    {
        float phase =
            2.0f * TJC_PI *
            (float)i /
            (float)DEMO_PERIOD_SAMPLE_COUNT;

        g_demo_one_period_mv[i] =
            DEMO_SIGNAL_PEAK_MV * sinf(phase);
    }

    for (i = 0U; i < DEMO_FFT_SAMPLE_COUNT; ++i)
    {
        float time_s = (float)i / DEMO_FFT_SAMPLE_RATE_HZ;
        float phase =
            2.0f * TJC_PI *
            DEMO_SIGNAL_FREQUENCY_HZ *
            time_s;

        g_demo_fft_input_mv[i] =
            DEMO_SIGNAL_PEAK_MV * sinf(phase);
    }
}

/**
 * @brief 更新time页面的周期模式、峰峰值、真有效值和基频。
 * @param periods 当前显示周期数，只允许1或3。
 * @param frequency_hz 被测信号基频，单位Hz。
 * @param vpp_mv 峰峰值，单位mV。
 * @param rms_mv 真有效值，单位mV。
 * @return 无。
 */
static void Display_UpdateTimeLabels(uint8_t periods,
                                     float frequency_hz,
                                     float vpp_mv,
                                     float rms_mv)
{
    (void)TJC_SetTextFormat(
        "time.t_mode",
        "%u PERIOD%s",
        periods,
        (periods == 1U) ? "" : "S"
    );

    (void)TJC_SetTextFormat("time.t_vpp", "%.1f mV", vpp_mv);
    (void)TJC_SetTextFormat("time.t_rms", "%.2f mV", rms_mv);
    (void)TJC_SetTextFormat(
        "time.t_freq",
        "%.1f kHz",
        frequency_hz / 1000.0f
    );
}

/**
 * @brief 将一个完整周期的电压数组绘制成一屏1周期或3周期。
 * @param one_period_mv 一个完整周期的电压样本，单位mV。
 * @param source_count 输入周期数组点数。
 * @param periods 当前显示周期数，只允许1或3。
 * @param frequency_hz 被测信号基频，单位Hz。
 * @param vpp_mv 峰峰值，单位mV。
 * @param rms_mv 真有效值，单位mV。
 * @param full_scale_mv 时域正负满量程，单位mV。
 * @return HAL_OK表示曲线和文本均已发送，其余值表示参数、串口或握手错误。
 *
 * 线性插值只改变显示采样密度，不改变原始波形的物理幅值关系。
 */
static HAL_StatusTypeDef Display_DrawTimeFrame(
    const float *one_period_mv,
    uint16_t source_count,
    uint8_t periods,
    float frequency_hz,
    float vpp_mv,
    float rms_mv,
    float full_scale_mv)
{
    uint16_t i;
    HAL_StatusTypeDef status;

    if ((one_period_mv == NULL) ||
        (source_count < 2U) ||
        ((periods != 1U) && (periods != 3U)) ||
        (full_scale_mv <= 0.0f))
    {
        return HAL_ERROR;
    }

    for (i = 0U; i < TJC_CURVE_WIDTH; ++i)
    {
        float source_position =
            ((float)i *
             (float)periods *
             (float)source_count) /
            (float)(TJC_CURVE_WIDTH - 1U);
        uint32_t integer_position = (uint32_t)source_position;
        float fraction =
            source_position - (float)integer_position;
        uint16_t index0 =
            (uint16_t)(integer_position % source_count);
        uint16_t index1 =
            (uint16_t)((index0 + 1U) % source_count);
        float sample_mv =
            one_period_mv[index0] +
            fraction *
            (one_period_mv[index1] - one_period_mv[index0]);

        g_curve_buffer[i] = Display_MapToByte(
            sample_mv,
            -full_scale_mv,
            full_scale_mv
        );
    }

    status = TJC_ClearCurve(TIME_CURVE_ID);

    if (status != HAL_OK)
    {
        return status;
    }

    HAL_Delay(10U);

    status = TJC_SendCurveBlock(
        TIME_CURVE_ID,
        g_curve_buffer,
        TJC_CURVE_WIDTH
    );

    if (status == HAL_OK)
    {
        Display_UpdateTimeLabels(
            periods,
            frequency_hz,
            vpp_mv,
            rms_mv
        );
    }

    return status;
}

/**
 * @brief 更新spectrum页面的主要频谱分量文本。
 * @param f1_hz 第一根谱线频率，单位Hz。
 * @param u1_mv 第一根谱线峰值幅度，单位mV。
 * @return 无。
 *
 * 当前演示信号只有一个分量，因此第二、第三组显示为空。
 */
static void Display_UpdateSpectrumLabels(float f1_hz,
                                         float u1_mv)
{
    (void)TJC_SetTextFormat(
        "spectrum.t_f1",
        "f1: %.1f kHz U1: %.1f mV",
        f1_hz / 1000.0f,
        u1_mv
    );

    (void)TJC_SetText("spectrum.t_f2", "f2: -- U2: --");
    (void)TJC_SetText("spectrum.t_f3", "f3: -- U3: --");
}

/**
 * @brief 计算演示正弦的单边DFT并绘制正频率轴离散频谱。
 * @return HAL_OK表示曲线和文本均已发送，其余值表示串口或握手错误。
 *
 * 当前参数N=256、频点间隔4 kHz，仅用于验证频谱显示链路。
 */
static HAL_StatusTypeDef Display_DrawDemoSpectrum(void)
{
    uint16_t i;
    uint16_t k;
    HAL_StatusTypeDef status;

    for (i = 0U; i < TJC_CURVE_WIDTH; ++i)
    {
        g_curve_buffer[i] = TJC_CURVE_MARGIN;
    }

    for (k = 1U;
         k <= (DEMO_FFT_SAMPLE_COUNT / 2U);
         ++k)
    {
        float frequency_hz =
            ((float)k * DEMO_FFT_SAMPLE_RATE_HZ) /
            (float)DEMO_FFT_SAMPLE_COUNT;
        float real_part = 0.0f;
        float imag_part = 0.0f;
        float amplitude_mv;
        uint32_t n;
        uint16_t calculated_position;
        uint16_t buffer_position;

        if (frequency_hz > DEMO_SPECTRUM_MAX_HZ)
        {
            break;
        }

        for (n = 0U; n < DEMO_FFT_SAMPLE_COUNT; ++n)
        {
            float angle =
                2.0f * TJC_PI *
                (float)k *
                (float)n /
                (float)DEMO_FFT_SAMPLE_COUNT;

            real_part += g_demo_fft_input_mv[n] * cosf(angle);
            imag_part -= g_demo_fft_input_mv[n] * sinf(angle);
        }

        amplitude_mv =
            2.0f *
            sqrtf(
                real_part * real_part +
                imag_part * imag_part
            ) /
            (float)DEMO_FFT_SAMPLE_COUNT;

        if (amplitude_mv < 1.0f)
        {
            continue;
        }

        calculated_position =
            (uint16_t)(
                frequency_hz /
                DEMO_SPECTRUM_MAX_HZ *
                (float)(TJC_CURVE_WIDTH - 1U) +
                0.5f
            );

        if (calculated_position >= TJC_CURVE_WIDTH)
        {
            calculated_position = TJC_CURVE_WIDTH - 1U;
        }

        /*
         * 淘晶驰整帧曲线的写入方向与数组索引方向相反。
         * 反转后100 kHz位于0~500 kHz横轴从左侧起约20%处。
         */
        buffer_position =
            (TJC_CURVE_WIDTH - 1U) - calculated_position;

        g_curve_buffer[buffer_position] = Display_MapToByte(
            amplitude_mv,
            0.0f,
            DEMO_SPECTRUM_FULL_SCALE_MV
        );
    }

    status = TJC_ClearCurve(SPECTRUM_CURVE_ID);

    if (status != HAL_OK)
    {
        return status;
    }

    HAL_Delay(10U);

    status = TJC_SendCurveBlock(
        SPECTRUM_CURVE_ID,
        g_curve_buffer,
        TJC_CURVE_WIDTH
    );

    if (status == HAL_OK)
    {
        Display_UpdateSpectrumLabels(
            DEMO_SIGNAL_FREQUENCY_HZ,
            DEMO_SIGNAL_PEAK_MV
        );
    }

    return status;
}

/**
 * @brief 处理HMI发送的固定4字节帧“A5 功能 数据 5A”。
 * @param frame 完整4字节帧。
 *
 * 本函数逐句迁移自test/main绝对正确.c：
 * 0x20处理页面就绪，0x01处理1/3周期，0x02仅作页面切换通知。
 */
static void TJC_ProcessCustomFrame(const uint8_t *frame)
{
    if ((frame == NULL) ||
        (frame[0] != TJC_FRAME_HEAD) ||
        (frame[3] != TJC_FRAME_END))
    {
        return;
    }

    if (frame[1] == 0x20U)
    {
        if (frame[2] == TJC_PAGE_TIME)
        {
            g_current_page = TJC_PAGE_TIME;
            g_pending_action =
                (g_visible_periods == 3U)
                ? UI_ACTION_DRAW_TIME_3
                : UI_ACTION_DRAW_TIME_1;
        }
        else if (frame[2] == TJC_PAGE_SPECTRUM)
        {
            g_current_page = TJC_PAGE_SPECTRUM;
            g_pending_action = UI_ACTION_DRAW_SPECTRUM;
        }

        return;
    }

    if (frame[1] == 0x01U)
    {
        if (frame[2] == 0x01U)
        {
            g_visible_periods = 1U;
        }
        else if (frame[2] == 0x03U)
        {
            g_visible_periods = 3U;
        }
        else
        {
            return;
        }

        if (g_current_page == TJC_PAGE_TIME)
        {
            g_pending_action =
                (g_visible_periods == 3U)
                ? UI_ACTION_DRAW_TIME_3
                : UI_ACTION_DRAW_TIME_1;
        }

        return;
    }

    /* 0x02页面切换帧只作通知，绘图等待目标页面的0x20就绪帧。 */
}

/**
 * @brief 将串口收到的单字节送入固定4字节帧解析器。
 * @param byte 新收到的一个字节。
 */
static void TJC_ParserPushByte(uint8_t byte)
{
    if (g_rx_frame_index == 0U)
    {
        if (byte == TJC_FRAME_HEAD)
        {
            g_rx_frame[0] = byte;
            g_rx_frame_index = 1U;
        }

        return;
    }

    g_rx_frame[g_rx_frame_index] = byte;
    g_rx_frame_index++;

    if (g_rx_frame_index == sizeof(g_rx_frame))
    {
        TJC_ProcessCustomFrame(g_rx_frame);
        g_rx_frame_index = 0U;
    }
}

/**
 * @brief 在主循环中轮询接收HMI页面和按钮事件。
 *
 * 每轮最多读取16字节，单字节超时1 ms；参数与原始正确版本一致。
 */
static void TJC_PollReceive(void)
{
    uint8_t byte;
    uint8_t count;

    if (s_uart == NULL)
    {
        return;
    }

    for (count = 0U; count < 16U; ++count)
    {
        if (HAL_UART_Receive(s_uart, &byte, 1U, 1U) != HAL_OK)
        {
            break;
        }

        TJC_ParserPushByte(byte);
    }
}

/**
 * @brief 初始化显示模块。
 * @param huart 与淘晶驰串口屏连接的UART句柄。
 *
 * 除把原全局huart3保存为模块指针外，执行顺序与正确版本完全一致：
 * 生成内部测试信号，等待1秒，然后发送“page time”。
 */
void Display_Init(UART_HandleTypeDef *huart)
{
    if (huart == NULL)
    {
        return;
    }

    s_uart = huart;
    g_current_page = TJC_PAGE_TIME;
    g_visible_periods = 1U;
    g_pending_action = UI_ACTION_NONE;
    g_rx_frame_index = 0U;

    Demo_PrepareInputData();
    HAL_Delay(1000U);
    (void)TJC_SendCommand("page time");
}

/**
 * @brief 执行一次显示模块任务。
 *
 * 本函数是原main.c中while(1)用户代码的等价封装：
 * 先收帧，再取出并清空待执行动作，最后完成对应整帧绘图。
 */
void Display_Task(void)
{
    UI_Action action;

    if (s_uart == NULL)
    {
        return;
    }

    TJC_PollReceive();

    action = g_pending_action;
    g_pending_action = UI_ACTION_NONE;

    switch (action)
    {
        case UI_ACTION_DRAW_TIME_1:
            (void)Display_DrawTimeFrame(
                g_demo_one_period_mv,
                DEMO_PERIOD_SAMPLE_COUNT,
                1U,
                DEMO_SIGNAL_FREQUENCY_HZ,
                2.0f * DEMO_SIGNAL_PEAK_MV,
                DEMO_SIGNAL_PEAK_MV / 1.41421356f,
                DEMO_TIME_FULL_SCALE_MV
            );
            break;

        case UI_ACTION_DRAW_TIME_3:
            (void)Display_DrawTimeFrame(
                g_demo_one_period_mv,
                DEMO_PERIOD_SAMPLE_COUNT,
                3U,
                DEMO_SIGNAL_FREQUENCY_HZ,
                2.0f * DEMO_SIGNAL_PEAK_MV,
                DEMO_SIGNAL_PEAK_MV / 1.41421356f,
                DEMO_TIME_FULL_SCALE_MV
            );
            break;

        case UI_ACTION_DRAW_SPECTRUM:
            (void)Display_DrawDemoSpectrum();
            break;

        case UI_ACTION_NONE:
        default:
            break;
    }
}
