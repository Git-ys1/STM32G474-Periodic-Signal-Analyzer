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
#define TJC_DASHBOARD_CODE           0x01U
#define TJC_CURVE_CHANNEL            0U

/* dashboard页面内两块曲线控件的固定显示尺寸。 */
#define DISPLAY_CURVE_WIDTH          794U
#define DISPLAY_CURVE_HEIGHT         145U
#define DISPLAY_CURVE_Y_MAX          (DISPLAY_CURVE_HEIGHT - 1U)
#define DISPLAY_TIME_MARGIN          5U
#define DISPLAY_SPEC_MARGIN          3U

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
    UI_ACTION_DRAW_DASHBOARD,
    UI_ACTION_DRAW_TIME_1,
    UI_ACTION_DRAW_TIME_3
} UI_Action;

/**
 * @brief dashboard页面上报的两个曲线控件数字ID。
 *
 * 数字ID由HMI页面后初始化事件动态发送，禁止再次在MCU中写死。
 */
typedef struct
{
    uint8_t time_curve_id;
    uint8_t spectrum_curve_id;
    bool valid;
} TJC_DashboardInfo;

/**
 * @brief 一个正频率轴频谱分量。
 */
typedef struct
{
    float frequency_hz;
    float amplitude_mv;
} SpectrumPeak;

/* 仅用于替代原main.c中的全局huart3，不改变串口行为。 */
static UART_HandleTypeDef *s_uart = NULL;

static TJC_DashboardInfo s_dashboard = {0U, 0U, false};
static uint8_t s_visible_periods = 1U;
static UI_Action s_pending_action = UI_ACTION_NONE;

/*
 * dashboard初始化帧为6字节，周期按钮帧为4字节。
 * 接收器依据功能码确定预期长度。
 */
static uint8_t s_rx_frame[6];
static uint8_t s_rx_frame_index = 0U;
static uint8_t s_rx_frame_expected = 0U;
static uint8_t s_status_frame[4];
static uint8_t s_status_frame_index = 0U;
static uint8_t s_last_hmi_status = 0U;

static uint8_t s_curve_buffer[DISPLAY_CURVE_WIDTH];
static float s_demo_one_period_mv[DEMO_PERIOD_SAMPLE_COUNT];
static float s_demo_fft_input_mv[DEMO_FFT_SAMPLE_COUNT];

static const SpectrumPeak s_demo_peaks[] =
{
    {DEMO_SIGNAL_FREQUENCY_HZ, DEMO_SIGNAL_PEAK_MV}
};

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
 * @param object_name dashboard页面中的控件objname，例如"t_vpp"。
 * @param text 要显示的UTF-8文本。
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
 * @brief 清空指定曲线控件的指定通道。
 * @param curve_id dashboard页面上报的真实数字ID。
 * @param channel 曲线通道，本项目固定使用0。
 * @return 指令格式化或串口发送状态。
 */
static HAL_StatusTypeDef TJC_ClearCurve(uint8_t curve_id,
                                        uint8_t channel)
{
    char command[24];
    int length;

    length = snprintf(
        command,
        sizeof(command),
        "cle %u,%u",
        (unsigned int)curve_id,
        (unsigned int)channel
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
 * @return HAL_OK表示收到目标应答；HAL_ERROR表示收到屏幕错误帧；
 *         HAL_TIMEOUT表示超时。
 *
 * @note 淘晶驰状态帧均为“状态码 FF FF FF”。等待透传期间若收到
 *       0x12、0x1A、0x1C或0x24等错误码，会保存到s_last_hmi_status
 *       并立即返回错误，避免把错误误判为普通超时。
 */
static HAL_StatusTypeDef TJC_WaitSpecialReply(uint8_t reply_code,
                                              uint32_t timeout_ms)
{
    uint32_t start_tick;
    uint8_t reply[4];
    uint8_t index = 0U;
    uint8_t byte = 0U;

    if (s_uart == NULL)
    {
        return HAL_ERROR;
    }

    start_tick = HAL_GetTick();

    while ((uint32_t)(HAL_GetTick() - start_tick) < timeout_ms)
    {
        if (HAL_UART_Receive(s_uart, &byte, 1U, 10U) != HAL_OK)
        {
            continue;
        }

        if (index == 0U)
        {
            reply[0] = byte;
            index = 1U;
        }
        else if (byte == 0xFFU)
        {
            reply[index++] = byte;

            if (index == sizeof(reply))
            {
                if ((reply[1] == 0xFFU) &&
                    (reply[2] == 0xFFU) &&
                    (reply[3] == 0xFFU))
                {
                    s_last_hmi_status = reply[0];

                    if (reply[0] == reply_code)
                    {
                        return HAL_OK;
                    }

                    if ((reply[0] != 0xFEU) &&
                        (reply[0] != 0xFDU))
                    {
                        return HAL_ERROR;
                    }
                }

                index = 0U;
            }
        }
        else
        {
            /*
             * 非状态帧或交错到来的自定义帧不会满足“三个0xFF”，
             * 将当前字节作为下一候选状态码继续搜索。
             */
            reply[0] = byte;
            index = 1U;
        }
    }

    return HAL_TIMEOUT;
}

/**
 * @brief 使用addt发送一整帧8位曲线点。
 * @param curve_id dashboard页面上报的真实数字ID。
 * @param channel 曲线通道，本项目固定使用0。
 * @param data 曲线数组。
 * @param point_count 曲线点数。
 * @return HAL_OK表示“addt -> FE -> 数据 -> FD”全部成功。
 */
static HAL_StatusTypeDef TJC_SendCurve(uint8_t curve_id,
                                       uint8_t channel,
                                       const uint8_t *data,
                                       uint16_t point_count)
{
    char command[32];
    int length;
    HAL_StatusTypeDef status;

    if ((data == NULL) ||
        (point_count == 0U) ||
        (point_count > DISPLAY_CURVE_WIDTH))
    {
        return HAL_ERROR;
    }

    length = snprintf(
        command,
        sizeof(command),
        "addt %u,%u,%u",
        (unsigned int)curve_id,
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

    status = TJC_WaitSpecialReply(0xFEU, 500U);

    if (status != HAL_OK)
    {
        return status;
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

    return TJC_WaitSpecialReply(0xFDU, 1000U);
}

/**
 * @brief 将任意数值区间线性映射到dashboard曲线纵坐标。
 * @param value 当前输入数值。
 * @param value_min 输入区间下限。
 * @param value_max 输入区间上限。
 * @param margin 曲线上、下端保留的纵向余量。
 * @return 映射并限制到0~144范围内的曲线值。
 *
 * 时域输入区间为[-满量程,+满量程]，频谱输入区间为[0,满量程]。
 */
static uint8_t Display_MapToByte(float value,
                                 float value_min,
                                 float value_max,
                                 uint8_t margin)
{
    float normalized;
    float mapped;
    float output_min;
    float output_max;

    if ((value_max <= value_min) ||
        ((uint16_t)(2U * margin) >= DISPLAY_CURVE_Y_MAX))
    {
        return 0U;
    }

    output_min = (float)margin;
    output_max = (float)(DISPLAY_CURVE_Y_MAX - margin);

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

    if (mapped < 0.0f)
    {
        mapped = 0.0f;
    }
    else if (mapped > (float)DISPLAY_CURVE_Y_MAX)
    {
        mapped = (float)DISPLAY_CURVE_Y_MAX;
    }

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

        s_demo_one_period_mv[i] =
            DEMO_SIGNAL_PEAK_MV * sinf(phase);
    }

    for (i = 0U; i < DEMO_FFT_SAMPLE_COUNT; ++i)
    {
        float time_s = (float)i / DEMO_FFT_SAMPLE_RATE_HZ;
        float phase =
            2.0f * TJC_PI *
            DEMO_SIGNAL_FREQUENCY_HZ *
            time_s;

        s_demo_fft_input_mv[i] =
            DEMO_SIGNAL_PEAK_MV * sinf(phase);
    }
}

/**
 * @brief 更新dashboard页面中的全部六项测量结果。
 * @param vpp_mv 峰峰值，单位mV。
 * @param rms_mv 真有效值，单位mV。
 * @param fundamental_hz 基频，单位Hz。
 * @param peaks 正频率轴主要频谱分量数组。
 * @param peak_count 有效频谱分量数量，最多使用前三项。
 *
 * @note 每次都向txt属性写入包含名称、数值和单位的完整UTF-8字符串，
 *       避免只写数字覆盖HMI中的前缀与单位。
 */
static void Display_UpdateResultTexts(
    float vpp_mv,
    float rms_mv,
    float fundamental_hz,
    const SpectrumPeak *peaks,
    uint8_t peak_count)
{
    (void)TJC_SetTextFormat("t_vpp", "Upp: %.1f mV", vpp_mv);
    (void)TJC_SetTextFormat("t_rms", "Urms: %.2f mV", rms_mv);
    (void)TJC_SetTextFormat(
        "t_freq",
        "f1: %.1f kHz",
        fundamental_hz / 1000.0f
    );

    if ((peaks != NULL) && (peak_count >= 1U))
    {
        (void)TJC_SetTextFormat(
            "t_c1",
            "基波: %.1f kHz / %.1f mVpk",
            peaks[0].frequency_hz / 1000.0f,
            peaks[0].amplitude_mv
        );
    }
    else
    {
        (void)TJC_SetText(
            "t_c1",
            "基波: --.- kHz / --.- mVpk"
        );
    }

    if ((peaks != NULL) && (peak_count >= 2U))
    {
        (void)TJC_SetTextFormat(
            "t_c2",
            "谐波1: %.1f kHz / %.1f mVpk",
            peaks[1].frequency_hz / 1000.0f,
            peaks[1].amplitude_mv
        );
    }
    else
    {
        (void)TJC_SetText(
            "t_c2",
            "谐波1: --.- kHz / --.- mVpk"
        );
    }

    if ((peaks != NULL) && (peak_count >= 3U))
    {
        (void)TJC_SetTextFormat(
            "t_c3",
            "谐波2: %.1f kHz / %.1f mVpk",
            peaks[2].frequency_hz / 1000.0f,
            peaks[2].amplitude_mv
        );
    }
    else
    {
        (void)TJC_SetText(
            "t_c3",
            "谐波2: --.- kHz / --.- mVpk"
        );
    }
}

/**
 * @brief 将一个完整周期的电压数组绘制成一屏1周期或3周期。
 * @param one_period_mv 一个完整周期的电压样本，单位mV。
 * @param source_count 输入周期数组点数。
 * @param periods 当前显示周期数，只允许1或3。
 * @param full_scale_mv 时域正负满量程，单位mV。
 * @return HAL_OK表示时域曲线已发送，其余值表示参数、串口或握手错误。
 *
 * 线性插值只改变显示采样密度，不改变原始波形的物理幅值关系。
 */
static HAL_StatusTypeDef Display_DrawTimeFrame(
    const float *one_period_mv,
    uint16_t source_count,
    uint8_t periods,
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

    if (!s_dashboard.valid)
    {
        return HAL_ERROR;
    }

    for (i = 0U; i < DISPLAY_CURVE_WIDTH; ++i)
    {
        float source_position =
            ((float)i *
             (float)periods *
             (float)source_count) /
            (float)(DISPLAY_CURVE_WIDTH - 1U);
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

        s_curve_buffer[i] = Display_MapToByte(
            sample_mv,
            -full_scale_mv,
            full_scale_mv,
            DISPLAY_TIME_MARGIN
        );
    }

    status = TJC_ClearCurve(
        s_dashboard.time_curve_id,
        TJC_CURVE_CHANNEL
    );

    if (status != HAL_OK)
    {
        return status;
    }

    HAL_Delay(10U);

    return TJC_SendCurve(
        s_dashboard.time_curve_id,
        TJC_CURVE_CHANNEL,
        s_curve_buffer,
        DISPLAY_CURVE_WIDTH
    );
}

/**
 * @brief 计算演示正弦的单边DFT并绘制正频率轴离散频谱。
 * @return HAL_OK表示频谱曲线已发送，其余值表示串口或握手错误。
 *
 * 当前参数N=256、频点间隔4 kHz，仅用于验证频谱显示链路。
 */
static HAL_StatusTypeDef Display_DrawDemoSpectrum(void)
{
    uint16_t i;
    uint16_t k;
    HAL_StatusTypeDef status;

    if (!s_dashboard.valid)
    {
        return HAL_ERROR;
    }

    for (i = 0U; i < DISPLAY_CURVE_WIDTH; ++i)
    {
        s_curve_buffer[i] = DISPLAY_SPEC_MARGIN;
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

            real_part += s_demo_fft_input_mv[n] * cosf(angle);
            imag_part -= s_demo_fft_input_mv[n] * sinf(angle);
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
                (float)(DISPLAY_CURVE_WIDTH - 1U) +
                0.5f
            );

        if (calculated_position >= DISPLAY_CURVE_WIDTH)
        {
            calculated_position = DISPLAY_CURVE_WIDTH - 1U;
        }

        /*
         * 淘晶驰整帧曲线的写入方向与数组索引方向相反。
         * 反转后100 kHz位于0~500 kHz横轴从左侧起约20%处。
         */
        buffer_position =
            (DISPLAY_CURVE_WIDTH - 1U) - calculated_position;

        s_curve_buffer[buffer_position] = Display_MapToByte(
            amplitude_mv,
            0.0f,
            DEMO_SPECTRUM_FULL_SCALE_MV,
            DISPLAY_SPEC_MARGIN
        );
    }

    status = TJC_ClearCurve(
        s_dashboard.spectrum_curve_id,
        TJC_CURVE_CHANNEL
    );

    if (status != HAL_OK)
    {
        return status;
    }

    HAL_Delay(10U);

    return TJC_SendCurve(
        s_dashboard.spectrum_curve_id,
        TJC_CURVE_CHANNEL,
        s_curve_buffer,
        DISPLAY_CURVE_WIDTH
    );
}

/**
 * @brief 在dashboard页面首次就绪后完成整页初始化显示。
 *
 * 严格按“时域曲线 -> 频谱曲线 -> 六项文本”的顺序执行。
 * 任一曲线发送失败时停止后续步骤，避免屏幕显示半套不一致数据。
 */
static HAL_StatusTypeDef Display_DrawDashboard(void)
{
    HAL_StatusTypeDef status;

    status = Display_DrawTimeFrame(
        s_demo_one_period_mv,
        DEMO_PERIOD_SAMPLE_COUNT,
        s_visible_periods,
        DEMO_TIME_FULL_SCALE_MV
    );

    if (status != HAL_OK)
    {
        return status;
    }

    status = Display_DrawDemoSpectrum();

    if (status != HAL_OK)
    {
        return status;
    }

    Display_UpdateResultTexts(
        2.0f * DEMO_SIGNAL_PEAK_MV,
        DEMO_SIGNAL_PEAK_MV / 1.41421356f,
        DEMO_SIGNAL_FREQUENCY_HZ,
        s_demo_peaks,
        (uint8_t)(sizeof(s_demo_peaks) / sizeof(s_demo_peaks[0]))
    );

    return HAL_OK;
}

/**
 * @brief 处理HMI发送的dashboard初始化帧或周期按钮帧。
 * @param frame 完整帧。
 * @param length 帧长度：dashboard初始化为6，按钮为4。
 */
static void TJC_ProcessCustomFrame(const uint8_t *frame,
                                   uint8_t length)
{
    if ((frame == NULL) ||
        (length < 4U) ||
        (frame[0] != TJC_FRAME_HEAD) ||
        (frame[length - 1U] != TJC_FRAME_END))
    {
        return;
    }

    /*
     * 页面初始化：
     * A5 20 01 time_id spec_id 5A
     */
    if ((length == 6U) &&
        (frame[1] == 0x20U) &&
        (frame[2] == TJC_DASHBOARD_CODE))
    {
        s_dashboard.time_curve_id = frame[3];
        s_dashboard.spectrum_curve_id = frame[4];
        s_dashboard.valid = true;
        s_pending_action = UI_ACTION_DRAW_DASHBOARD;
        return;
    }

    /*
     * 周期按钮：
     * A5 01 01 5A -> 1T
     * A5 01 03 5A -> 3T
     */
    if ((length == 4U) && (frame[1] == 0x01U))
    {
        if (frame[2] == 0x01U)
        {
            s_visible_periods = 1U;
            s_pending_action =
                s_dashboard.valid
                ? UI_ACTION_DRAW_TIME_1
                : UI_ACTION_NONE;
        }
        else if (frame[2] == 0x03U)
        {
            s_visible_periods = 3U;
            s_pending_action =
                s_dashboard.valid
                ? UI_ACTION_DRAW_TIME_3
                : UI_ACTION_NONE;
        }
    }
}

/**
 * @brief 记录空闲阶段收到的淘晶驰四字节状态帧。
 * @param frame 形如“状态码 FF FF FF”的完整状态帧。
 *
 * FE/FD通常由同步透传握手函数消费；0x12、0x1A、0x1C、0x24等
 * 错误若在主循环轮询阶段到达，则保存在s_last_hmi_status中供调试。
 */
static void TJC_ProcessStatusFrame(const uint8_t *frame)
{
    if ((frame != NULL) &&
        (frame[1] == 0xFFU) &&
        (frame[2] == 0xFFU) &&
        (frame[3] == 0xFFU))
    {
        s_last_hmi_status = frame[0];
    }
}

/**
 * @brief 将串口收到的单字节送入变长自定义帧和状态帧解析器。
 * @param byte 新收到的一个字节。
 */
static void TJC_ParserPushByte(uint8_t byte)
{
    if (s_status_frame_index > 0U)
    {
        if (byte == 0xFFU)
        {
            s_status_frame[s_status_frame_index++] = byte;

            if (s_status_frame_index == sizeof(s_status_frame))
            {
                TJC_ProcessStatusFrame(s_status_frame);
                s_status_frame_index = 0U;
            }

            return;
        }

        if (byte == TJC_FRAME_HEAD)
        {
            s_rx_frame[0] = byte;
            s_rx_frame_index = 1U;
            s_rx_frame_expected = 0U;
            s_status_frame_index = 0U;
            return;
        }

        s_status_frame[0] = byte;
        s_status_frame_index = 1U;
        return;
    }

    if (s_rx_frame_index == 0U)
    {
        if (byte == TJC_FRAME_HEAD)
        {
            s_rx_frame[0] = byte;
            s_rx_frame_index = 1U;
            s_rx_frame_expected = 0U;
            s_status_frame_index = 0U;
        }
        else
        {
            s_status_frame[0] = byte;
            s_status_frame_index = 1U;
        }

        return;
    }

    if (s_rx_frame_index > 0U)
    {
        if (s_rx_frame_index >= sizeof(s_rx_frame))
        {
            s_rx_frame_index = 0U;
            s_rx_frame_expected = 0U;
            return;
        }

        s_rx_frame[s_rx_frame_index++] = byte;

        if (s_rx_frame_index == 2U)
        {
            if (s_rx_frame[1] == 0x20U)
            {
                s_rx_frame_expected = 6U;
            }
            else if (s_rx_frame[1] == 0x01U)
            {
                s_rx_frame_expected = 4U;
            }
            else
            {
                s_rx_frame_index = 0U;
                s_rx_frame_expected = 0U;
                return;
            }
        }

        if ((s_rx_frame_expected != 0U) &&
            (s_rx_frame_index == s_rx_frame_expected))
        {
            TJC_ProcessCustomFrame(
                s_rx_frame,
                s_rx_frame_expected
            );
            s_rx_frame_index = 0U;
            s_rx_frame_expected = 0U;
        }

        return;
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
 * 生成内部测试信号，等待屏幕启动，然后进入dashboard页面。
 * 本函数不直接发送曲线；收到页面上报的两个真实曲线ID后才绘图。
 */
void Display_Init(UART_HandleTypeDef *huart)
{
    if (huart == NULL)
    {
        return;
    }

    s_uart = huart;
    s_dashboard.time_curve_id = 0U;
    s_dashboard.spectrum_curve_id = 0U;
    s_dashboard.valid = false;
    s_visible_periods = 1U;
    s_pending_action = UI_ACTION_NONE;
    s_rx_frame_index = 0U;
    s_rx_frame_expected = 0U;
    s_status_frame_index = 0U;
    s_last_hmi_status = 0U;

    Demo_PrepareInputData();
    HAL_Delay(1000U);
    (void)TJC_SendCommand("page dashboard");
}

/**
 * @brief 执行一次显示模块任务。
 *
 * 本函数是原main.c中while(1)用户代码的等价封装：
 * 先收帧，再取出并清空待执行动作，最后完成dashboard整页初始化
 * 或仅刷新1T/3T时域曲线。
 */
void Display_Task(void)
{
    UI_Action action;

    if (s_uart == NULL)
    {
        return;
    }

    TJC_PollReceive();

    action = s_pending_action;
    s_pending_action = UI_ACTION_NONE;

    switch (action)
    {
        case UI_ACTION_DRAW_DASHBOARD:
            (void)Display_DrawDashboard();
            break;

        case UI_ACTION_DRAW_TIME_1:
            (void)Display_DrawTimeFrame(
                s_demo_one_period_mv,
                DEMO_PERIOD_SAMPLE_COUNT,
                1U,
                DEMO_TIME_FULL_SCALE_MV
            );
            break;

        case UI_ACTION_DRAW_TIME_3:
            (void)Display_DrawTimeFrame(
                s_demo_one_period_mv,
                DEMO_PERIOD_SAMPLE_COUNT,
                3U,
                DEMO_TIME_FULL_SCALE_MV
            );
            break;

        case UI_ACTION_NONE:
        default:
            break;
    }
}
