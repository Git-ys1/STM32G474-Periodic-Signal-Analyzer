#include "display.h"

#include "analyzer_bridge.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* 淘晶驰自定义串口协议。 */
#define TJC_FRAME_HEAD               0xA5U
#define TJC_FRAME_END                0x5AU
#define TJC_DASHBOARD_CODE           0x01U
#define TJC_CURVE_CHANNEL            0U
#define TJC_TRIGGER_MODE_CODE        0x07U
#define TJC_HUBER_MODE_CODE          0x08U

/* dashboard页面内两块曲线控件的固定显示尺寸。 */
#define DISPLAY_TIME_CURVE_WIDTH     512U
#define DISPLAY_SPEC_CURVE_WIDTH     256U
#define DISPLAY_CURVE_BUFFER_WIDTH   DISPLAY_TIME_CURVE_WIDTH
#define DISPLAY_CURVE_HEIGHT         256U
#define DISPLAY_CURVE_Y_MAX          (DISPLAY_CURVE_HEIGHT - 1U)
#define DISPLAY_TIME_MARGIN          0U
#define DISPLAY_SPEC_MARGIN          0U
#define DISPLAY_SPEC_X_MARGIN        0U
#define DISPLAY_SPECTRUM_MAX_HZ      500000.0f
#define DISPLAY_REFRESH_INTERVAL_MS  3000U
#define DISPLAY_TRIGGER_HYST_RATIO   0.02f
#define DISPLAY_TRIGGER_HYST_MIN_MV  0.50f
#define DISPLAY_TRIGGER_EPSILON_MV   0.0001f

typedef enum
{
    UI_ACTION_NONE = 0,
    UI_ACTION_DRAW_DASHBOARD,
    UI_ACTION_DRAW_TIME_1,
    UI_ACTION_DRAW_TIME_3,
    UI_ACTION_START_REAL,
    UI_ACTION_START_TEST,
    UI_ACTION_CLEAR_DASHBOARD,
    UI_ACTION_STOP
} UI_Action;

typedef enum
{
    DISPLAY_RUN_STOPPED = 0,
    DISPLAY_RUN_REAL_AUTO,
    DISPLAY_RUN_TEST_AUTO
} DisplayRunMode;

typedef struct
{
    uint8_t time_curve_id;
    uint8_t spectrum_curve_id;
    bool valid;
} TJC_DashboardInfo;

static UART_HandleTypeDef *s_uart = NULL;
static volatile TJC_DashboardInfo s_dashboard = {0U, 0U, false};
static volatile uint8_t s_visible_periods = 1U;
static volatile DisplayTriggerMode s_trigger_mode =
    DISPLAY_TRIGGER_RISING_ZERO;
static volatile uint8_t s_requested_huber_enabled = 1U;
static volatile bool s_huber_mode_update_pending = false;
static volatile bool s_period_switch_sync_pending = false;
static volatile UI_Action s_pending_action = UI_ACTION_NONE;
static volatile DisplayRunMode s_run_mode = DISPLAY_RUN_STOPPED;
static uint32_t s_next_refresh_tick = 0U;
/*
 * AnalyzerResult包含256点float波形，单个对象超过1 KB。
 * 显示任务会继续调用HAL UART发送函数，不能在仅1 KB的主栈上保存
 * 该对象，否则会覆盖s_uart并在HAL_UART_Transmit()中触发BusFault。
 */
static AnalyzerResult s_display_result;

/*
 * dashboard初始化帧为6字节，按钮帧为4字节。
 * 状态帧保持“状态码 FF FF FF”格式。
 */
static uint8_t s_rx_frame[6];
static uint8_t s_rx_frame_index = 0U;
static uint8_t s_rx_frame_expected = 0U;
static uint8_t s_status_frame[4];
static uint8_t s_status_frame_index = 0U;
static volatile uint8_t s_last_hmi_status = 0U;
static uint8_t s_rx_it_byte = 0U;

static uint8_t s_curve_buffer[DISPLAY_CURVE_BUFFER_WIDTH];

/**
 * @brief 向淘晶驰屏发送字符串指令，并自动追加三个0xFF。
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
 * @brief 修改数字、虚拟浮点或状态开关控件的val属性。
 */
static HAL_StatusTypeDef TJC_SetValue(const char *object_name,
                                      int32_t value)
{
    char command[48];
    int length;

    if (object_name == NULL)
    {
        return HAL_ERROR;
    }

    length = snprintf(
        command,
        sizeof(command),
        "%s.val=%ld",
        object_name,
        (long)value
    );

    if ((length <= 0) || ((size_t)length >= sizeof(command)))
    {
        return HAL_ERROR;
    }

    return TJC_SendCommand(command);
}

/**
 * @brief 清空指定曲线控件的指定通道。
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
 * @brief 等待addt透传的FE或FD四字节应答。
 */
static HAL_StatusTypeDef TJC_WaitSpecialReply(uint8_t reply_code,
                                              uint32_t timeout_ms)
{
    uint32_t start_tick;
    uint8_t status;

    if (s_uart == NULL)
    {
        return HAL_ERROR;
    }

    start_tick = HAL_GetTick();

    while ((uint32_t)(HAL_GetTick() - start_tick) < timeout_ms)
    {
        status = s_last_hmi_status;

        if (status == reply_code)
        {
            return HAL_OK;
        }

        /*
         * 0x01是普通指令执行成功，可与上一条cle命令的返回交错。
         * 其他非FE/FD状态码代表HMI明确报告了命令错误。
         */
        if ((status != 0U) &&
            (status != 0x01U) &&
            (status != 0xFEU) &&
            (status != 0xFDU))
        {
            return HAL_ERROR;
        }
    }

    return HAL_TIMEOUT;
}

/**
 * @brief 使用addt发送一整帧8位曲线点。
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
        (point_count > DISPLAY_CURVE_BUFFER_WIDTH))
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

    /*
     * 接收由USART3逐字节中断完成。发送前清空旧状态，
     * 防止上一条命令的01/FE/FD被误认为本次addt应答。
     */
    s_last_hmi_status = 0U;
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

    s_last_hmi_status = 0U;
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
 * @brief 将非负浮点数转换为指定倍率的无符号定点整数。
 *
 * 当前工程继续避免依赖浮点printf，保证newlib/Keil配置变化时
 * 测量文本仍能稳定显示。
 */
static uint32_t Display_ToUnsignedFixed(float value,
                                        uint32_t scale)
{
    if ((value <= 0.0f) || (scale == 0U))
    {
        return 0U;
    }

    return (uint32_t)(value * (float)scale + 0.5f);
}

/**
 * @brief 把有符号mV数值格式化为一位小数，避免启用浮点printf。
 */
static void Display_FormatSignedTick(char *text,
                                     size_t text_size,
                                     float value)
{
    int32_t value_x10;
    uint32_t absolute_x10;

    if ((text == NULL) || (text_size == 0U))
    {
        return;
    }

    value_x10 = (value >= 0.0f)
        ? (int32_t)(value * 10.0f + 0.5f)
        : (int32_t)(value * 10.0f - 0.5f);

    if (value_x10 < 0)
    {
        absolute_x10 = (uint32_t)(-value_x10);
        (void)snprintf(
            text,
            text_size,
            "-%lu.%01lu",
            (unsigned long)(absolute_x10 / 10U),
            (unsigned long)(absolute_x10 % 10U)
        );
    }
    else
    {
        absolute_x10 = (uint32_t)value_x10;
        (void)snprintf(
            text,
            text_size,
            "%lu.%01lu",
            (unsigned long)(absolute_x10 / 10U),
            (unsigned long)(absolute_x10 % 10U)
        );
    }
}

/**
 * @brief 清空动态坐标文字；频谱横轴0~500 kHz由HMI固定显示。
 */
static void Display_ClearAxisLabels(void)
{
    uint8_t i;
    static const char *time_y_names[5] =
    {
        "n_ty4", "n_ty3", "n_ty2", "n_ty1", "n_ty0"
    };
    static const char *spectrum_y_names[5] =
    {
        "n_sy4", "n_sy3", "n_sy2", "n_sy1", "n_sy0"
    };
    static const char *time_x_names[5] =
    {
        "x_tx0", "x_tx1", "x_tx2", "x_tx3", "x_tx4"
    };

    for (i = 0U; i < 5U; ++i)
    {
        (void)TJC_SetText(time_y_names[i], "---");
        (void)TJC_SetText(spectrum_y_names[i], "---");
        (void)TJC_SetValue(time_x_names[i], 0);
    }
}

/**
 * @brief 更新dashboard的峰峰值、真有效值、基频和最多三个谱峰。
 */
static void Display_UpdateResultTexts(const AnalyzerResult *result)
{
    char text[64];
    uint32_t vpp_x10;
    uint32_t model_vpp_x10;
    uint32_t rms_x100;
    uint32_t fundamental_khz_x10;
    uint8_t i;
    static const char *object_names[ANALYZER_MAX_COMPONENTS] =
    {
        "t_c1", "t_c2", "t_c3"
    };
    static const char *labels[ANALYZER_MAX_COMPONENTS] =
    {
        "基波", "谐波1", "谐波2"
    };

    if ((result == NULL) || (result->valid == 0U))
    {
        return;
    }

    vpp_x10 = Display_ToUnsignedFixed(result->vpp_mv, 10U);
    model_vpp_x10 =
        Display_ToUnsignedFixed(result->model_vpp_mv, 10U);
    rms_x100 = Display_ToUnsignedFixed(result->vrms_mv, 100U);
    fundamental_khz_x10 =
        Display_ToUnsignedFixed(
            result->fundamental_hz / 1000.0f,
            10U
        );

    (void)snprintf(
        text,
        sizeof(text),
        "Upp: %lu.%01lu mV",
        (unsigned long)(vpp_x10 / 10U),
        (unsigned long)(vpp_x10 % 10U)
    );
    (void)TJC_SetText("t_vpp", text);

    if (result->model_vpp_valid != 0U)
    {
        (void)snprintf(
            text,
            sizeof(text),
            "Mpp: %lu.%01lu mV",
            (unsigned long)(model_vpp_x10 / 10U),
            (unsigned long)(model_vpp_x10 % 10U)
        );
    }
    else
    {
        (void)snprintf(text, sizeof(text), "Mpp: --.- mV");
    }
    (void)TJC_SetText("t_vpp2", text);

    (void)snprintf(
        text,
        sizeof(text),
        "Urms: %lu.%02lu mV",
        (unsigned long)(rms_x100 / 100U),
        (unsigned long)(rms_x100 % 100U)
    );
    (void)TJC_SetText("t_rms", text);

    (void)snprintf(
        text,
        sizeof(text),
        "f1: %lu.%01lu kHz",
        (unsigned long)(fundamental_khz_x10 / 10U),
        (unsigned long)(fundamental_khz_x10 % 10U)
    );
    (void)TJC_SetText("t_freq", text);

    for (i = 0U; i < ANALYZER_MAX_COMPONENTS; ++i)
    {
        if (i < result->component_count)
        {
            uint32_t frequency_khz_x10 =
                Display_ToUnsignedFixed(
                    result->components[i].frequency_hz /
                    1000.0f,
                    10U
                );
            uint32_t amplitude_mv_x10 =
                Display_ToUnsignedFixed(
                    result->components[i].amplitude_mv,
                    10U
                );

            (void)snprintf(
                text,
                sizeof(text),
                "%s: %lu.%01lu kHz / %lu.%01lu mVpk",
                labels[i],
                (unsigned long)(frequency_khz_x10 / 10U),
                (unsigned long)(frequency_khz_x10 % 10U),
                (unsigned long)(amplitude_mv_x10 / 10U),
                (unsigned long)(amplitude_mv_x10 % 10U)
            );
        }
        else
        {
            (void)snprintf(
                text,
                sizeof(text),
                "%s: --.- kHz / --.- mVpk",
                labels[i]
            );
        }

        /*
         * 测试编号放在“谐波2”文本最前面，避免较长分量文本把末尾编号
         * 裁出控件可视区域。原T1~T9和自定义T1~T255共用该字段；
         * 真实ADC结果test_case_number为0，不显示任何测试标记。
         */
        if ((i == (ANALYZER_MAX_COMPONENTS - 1U)) &&
            (result->source == ANALYZER_SOURCE_TEST) &&
            (result->test_case_number != 0U))
        {
            char numbered_text[64];

            (void)snprintf(
                numbered_text,
                sizeof(numbered_text),
                "[T%u] %s",
                (unsigned int)result->test_case_number,
                text
            );
            (void)snprintf(text, sizeof(text), "%s", numbered_text);
        }

        (void)TJC_SetText(object_names[i], text);
    }
}

/**
 * @brief 将测量文本恢复为无数据占位符。
 */
static void Display_ClearResultTexts(void)
{
    (void)TJC_SetText("t_vpp", "Upp: --.- mV");
    (void)TJC_SetText("t_vpp2", "Mpp: --.- mV");
    (void)TJC_SetText("t_rms", "Urms: --.-- mV");
    (void)TJC_SetText("t_freq", "f1: --.- kHz");
    (void)TJC_SetText("t_c1", "基波: --.- kHz / --.- mVpk");
    (void)TJC_SetText("t_c2", "谐波1: --.- kHz / --.- mVpk");
    (void)TJC_SetText("t_c3", "谐波2: --.- kHz / --.- mVpk");
}

/**
 * @brief 立即清除两条曲线和全部测量文本，但不改变运行模式。
 *
 * 运行模式下调用后会在下一个3秒周期重新显示；停止状态下保持空白。
 */
static HAL_StatusTypeDef Display_ClearDashboard(void)
{
    HAL_StatusTypeDef time_status = HAL_ERROR;
    HAL_StatusTypeDef spectrum_status = HAL_ERROR;

    if (s_dashboard.valid)
    {
        time_status = TJC_ClearCurve(
            s_dashboard.time_curve_id,
            TJC_CURVE_CHANNEL
        );
        spectrum_status = TJC_ClearCurve(
            s_dashboard.spectrum_curve_id,
            TJC_CURVE_CHANNEL
        );
    }

    Display_ClearResultTexts();
    Display_ClearAxisLabels();

    if (time_status != HAL_OK)
    {
        return time_status;
    }

    return spectrum_status;
}

/**
 * @brief 从当前时刻重新计算下一次3秒刷新期限。
 */
static void Display_ArmNextRefresh(void)
{
    s_next_refresh_tick =
        HAL_GetTick() + DISPLAY_REFRESH_INTERVAL_MS;
}

/**
 * @brief 使用有符号差值判断HAL毫秒计数是否到达期限，兼容计数回绕。
 */
static bool Display_IsRefreshDue(uint32_t now)
{
    return ((int32_t)(now - s_next_refresh_tick) >= 0);
}

/**
 * @brief 频谱发送失败时在t_c3显示曲线ID和错误状态。
 */
static void Display_ShowSpectrumError(HAL_StatusTypeDef hal_status)
{
    char text[64];

    (void)snprintf(
        text,
        sizeof(text),
        "频谱错误: ID=%u HAL=%u HMI=%02X",
        (unsigned int)s_dashboard.spectrum_curve_id,
        (unsigned int)hal_status,
        (unsigned int)s_last_hmi_status
    );
    (void)TJC_SetText("t_c3", text);
}

/**
 * @brief 把真实ADC相位折叠波形折算回信号源输入端。
 */
static float Display_GetWaveformInputScale(
    const AnalyzerResult *result)
{
    if ((result != NULL) &&
        (result->source == ANALYZER_SOURCE_REAL) &&
        (ANALYZER_FRONTEND_VOLTAGE_GAIN > 0.0f))
    {
        return 1.0f / ANALYZER_FRONTEND_VOLTAGE_GAIN;
    }

    return 1.0f;
}

/**
 * @brief 用独立Mpp模型确定时域对称满量程；无Mpp时才回退到波形峰值。
 */
static float Display_GetTimeFullScale(const AnalyzerResult *result)
{
    float maximum_absolute = 0.0f;
    float input_scale;
    uint16_t i;

    if ((result->model_vpp_valid != 0U) &&
        (result->model_vpp_mv > 0.0f))
    {
        maximum_absolute = result->model_vpp_mv * 0.5f;

        if (maximum_absolute < 1.0f)
        {
            maximum_absolute = 1.0f;
        }

        return maximum_absolute;
    }

    input_scale = Display_GetWaveformInputScale(result);

    for (i = 0U; i < result->waveform_count; ++i)
    {
        float value =
            fabsf(result->waveform_mv[i] * input_scale);
        if (value > maximum_absolute)
        {
            maximum_absolute = value;
        }
    }

    if (maximum_absolute < 1.0f)
    {
        maximum_absolute = 1.0f;
    }

    return maximum_absolute * 1.10f;
}

/**
 * @brief 使用队友FFT分量中的最大幅度确定频谱纵轴满量程。
 */
static float Display_GetSpectrumFullScale(
    const AnalyzerResult *result)
{
    float full_scale_mv = 1.0f;
    uint8_t component;

    for (component = 0U;
         component < result->component_count;
         ++component)
    {
        if (result->components[component].amplitude_mv >
            full_scale_mv)
        {
            full_scale_mv =
                result->components[component].amplitude_mv;
        }
    }

    return full_scale_mv * 1.15f;
}

/**
 * @brief 写入时域实际mV纵轴和0~显示时长的us横轴。
 */
static void Display_UpdateTimeAxes(const AnalyzerResult *result,
                                   uint8_t periods,
                                   float full_scale_mv)
{
    char text[16];
    float duration_us = 0.0f;
    uint8_t i;
    static const char *time_y_names[5] =
    {
        "n_ty4", "n_ty3", "n_ty2", "n_ty1", "n_ty0"
    };
    static const char *time_x_names[5] =
    {
        "x_tx0", "x_tx1", "x_tx2", "x_tx3", "x_tx4"
    };

    for (i = 0U; i < 5U; ++i)
    {
        float y_value =
            full_scale_mv * (1.0f - 0.5f * (float)i);

        Display_FormatSignedTick(text, sizeof(text), y_value);
        (void)TJC_SetText(time_y_names[i], text);
    }

    if (result->fundamental_hz > 0.0f)
    {
        duration_us =
            (float)periods * 1000000.0f /
            result->fundamental_hz;
    }

    for (i = 0U; i < 5U; ++i)
    {
        uint32_t value_x10 = Display_ToUnsignedFixed(
            duration_us * (float)i / 4.0f,
            10U
        );

        if (value_x10 > 2147483647UL)
        {
            value_x10 = 2147483647UL;
        }

        (void)TJC_SetValue(
            time_x_names[i],
            (int32_t)value_x10
        );
    }
}

/**
 * @brief 写入频谱0~满量程的实际mV纵轴。
 */
static void Display_UpdateSpectrumAxis(float full_scale_mv)
{
    char text[16];
    uint8_t i;
    static const char *spectrum_y_names[5] =
    {
        "n_sy4", "n_sy3", "n_sy2", "n_sy1", "n_sy0"
    };

    for (i = 0U; i < 5U; ++i)
    {
        float y_value =
            full_scale_mv * (1.0f - 0.25f * (float)i);

        Display_FormatSignedTick(text, sizeof(text), y_value);
        (void)TJC_SetText(spectrum_y_names[i], text);
    }
}

/**
 * @brief 把任意浮点槽位置折回一个周期。
 */
static float Display_WrapWaveformPosition(float position,
                                          uint16_t point_count)
{
    float period = (float)point_count;

    while (position >= period)
    {
        position -= period;
    }

    while (position < 0.0f)
    {
        position += period;
    }

    return position;
}

/**
 * @brief 找到一个周期内的正峰，并用三点抛物线细化到亚槽位置。
 */
static float Display_FindPositivePeakPosition(
    const AnalyzerResult *result,
    uint16_t *peak_index_out)
{
    uint16_t peak_index = 0U;
    uint16_t previous_index;
    uint16_t next_index;
    uint16_t i;
    float previous_value;
    float peak_value;
    float next_value;
    float denominator;
    float fractional_offset = 0.0f;

    for (i = 1U; i < result->waveform_count; ++i)
    {
        if (result->waveform_mv[i] >
            result->waveform_mv[peak_index])
        {
            peak_index = i;
        }
    }

    previous_index =
        (uint16_t)(
            (peak_index +
             result->waveform_count -
             1U) %
            result->waveform_count
        );
    next_index =
        (uint16_t)(
            (peak_index + 1U) %
            result->waveform_count
        );
    previous_value = result->waveform_mv[previous_index];
    peak_value = result->waveform_mv[peak_index];
    next_value = result->waveform_mv[next_index];
    denominator =
        previous_value -
        2.0f * peak_value +
        next_value;

    if (fabsf(denominator) > DISPLAY_TRIGGER_EPSILON_MV)
    {
        fractional_offset =
            0.5f *
            (previous_value - next_value) /
            denominator;

        if (fractional_offset < -0.5f)
        {
            fractional_offset = -0.5f;
        }
        else if (fractional_offset > 0.5f)
        {
            fractional_offset = 0.5f;
        }
    }

    if (peak_index_out != NULL)
    {
        *peak_index_out = peak_index;
    }

    return Display_WrapWaveformPosition(
        (float)peak_index + fractional_offset,
        result->waveform_count
    );
}

/**
 * @brief 检查过零点前后是否真正越过滞回带，过滤零点附近的小毛刺。
 */
static bool Display_CrossingPassesHysteresis(
    const AnalyzerResult *result,
    uint16_t crossing_index,
    bool rising,
    float hysteresis_mv)
{
    uint16_t search_count =
        (uint16_t)(result->waveform_count / 4U);
    uint16_t distance;
    bool before_ready = false;
    bool after_ready = false;

    if (search_count < 2U)
    {
        search_count = 2U;
    }

    for (distance = 0U; distance < search_count; ++distance)
    {
        uint16_t before_index =
            (uint16_t)(
                (crossing_index +
                 result->waveform_count -
                 distance) %
                result->waveform_count
            );
        uint16_t after_index =
            (uint16_t)(
                (crossing_index +
                 1U +
                 distance) %
                result->waveform_count
            );
        float before_value =
            result->waveform_mv[before_index];
        float after_value =
            result->waveform_mv[after_index];

        if (rising)
        {
            before_ready =
                before_ready ||
                (before_value <= -hysteresis_mv);
            after_ready =
                after_ready ||
                (after_value >= hysteresis_mv);
        }
        else
        {
            before_ready =
                before_ready ||
                (before_value >= hysteresis_mv);
            after_ready =
                after_ready ||
                (after_value <= -hysteresis_mv);
        }

        if (before_ready && after_ready)
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief 以主正峰为参照寻找确定的上升/下降过零点。
 *
 * 上升沿从主正峰向前回溯，下降沿从主正峰向后搜索。这样即使含有谐波并
 * 出现多个过零点，也会稳定选择与主正峰相邻的同一个交越。过零时再做
 * 线性插值，避免只对齐到整数样本槽产生可见抖动。
 */
static float Display_FindZeroCrossingPosition(
    const AnalyzerResult *result,
    bool rising)
{
    uint16_t peak_index;
    uint16_t distance;
    uint16_t fallback_index = 0U;
    bool fallback_valid = false;
    float peak_position;
    float minimum_value;
    float maximum_value;
    float hysteresis_mv;

    peak_position =
        Display_FindPositivePeakPosition(result, &peak_index);
    minimum_value = result->waveform_mv[0];
    maximum_value = result->waveform_mv[0];

    for (distance = 1U;
         distance < result->waveform_count;
         ++distance)
    {
        float value = result->waveform_mv[distance];

        if (value < minimum_value)
        {
            minimum_value = value;
        }
        if (value > maximum_value)
        {
            maximum_value = value;
        }
    }

    hysteresis_mv =
        (maximum_value - minimum_value) *
        DISPLAY_TRIGGER_HYST_RATIO;
    if (hysteresis_mv < DISPLAY_TRIGGER_HYST_MIN_MV)
    {
        hysteresis_mv = DISPLAY_TRIGGER_HYST_MIN_MV;
    }

    for (distance = 0U;
         distance < result->waveform_count;
         ++distance)
    {
        uint16_t index0;
        uint16_t index1;
        float value0;
        float value1;
        bool is_crossing;

        if (rising)
        {
            index0 =
                (uint16_t)(
                    (peak_index +
                     result->waveform_count -
                     1U -
                     distance) %
                    result->waveform_count
                );
        }
        else
        {
            index0 =
                (uint16_t)(
                    (peak_index + distance) %
                    result->waveform_count
                );
        }

        index1 =
            (uint16_t)(
                (index0 + 1U) %
                result->waveform_count
            );
        value0 = result->waveform_mv[index0];
        value1 = result->waveform_mv[index1];
        is_crossing =
            rising
            ? ((value0 <= 0.0f) && (value1 > 0.0f))
            : ((value0 >= 0.0f) && (value1 < 0.0f));

        if (!is_crossing)
        {
            continue;
        }

        if (!fallback_valid)
        {
            fallback_index = index0;
            fallback_valid = true;
        }

        if (Display_CrossingPassesHysteresis(
                result,
                index0,
                rising,
                hysteresis_mv))
        {
            float denominator = value1 - value0;
            float fraction = 0.0f;

            if (fabsf(denominator) >
                DISPLAY_TRIGGER_EPSILON_MV)
            {
                fraction = -value0 / denominator;
            }

            if (fraction < 0.0f)
            {
                fraction = 0.0f;
            }
            else if (fraction > 1.0f)
            {
                fraction = 1.0f;
            }

            return Display_WrapWaveformPosition(
                (float)index0 + fraction,
                result->waveform_count
            );
        }
    }

    if (fallback_valid)
    {
        uint16_t index1 =
            (uint16_t)(
                (fallback_index + 1U) %
                result->waveform_count
            );
        float value0 = result->waveform_mv[fallback_index];
        float value1 = result->waveform_mv[index1];
        float denominator = value1 - value0;
        float fraction = 0.0f;

        if (fabsf(denominator) > DISPLAY_TRIGGER_EPSILON_MV)
        {
            fraction = -value0 / denominator;
        }

        if (fraction < 0.0f)
        {
            fraction = 0.0f;
        }
        else if (fraction > 1.0f)
        {
            fraction = 1.0f;
        }

        return Display_WrapWaveformPosition(
            (float)fallback_index + fraction,
            result->waveform_count
        );
    }

    return peak_position;
}

/**
 * @brief 根据当前模式返回一个周期波形的逻辑起点。
 */
static float Display_GetTriggerPosition(
    const AnalyzerResult *result)
{
    switch (s_trigger_mode)
    {
        case DISPLAY_TRIGGER_RISING_ZERO:
            return Display_FindZeroCrossingPosition(
                result,
                true
            );

        case DISPLAY_TRIGGER_FALLING_ZERO:
            return Display_FindZeroCrossingPosition(
                result,
                false
            );

        case DISPLAY_TRIGGER_POSITIVE_PEAK:
            return Display_FindPositivePeakPosition(
                result,
                NULL
            );

        case DISPLAY_TRIGGER_OFF:
        default:
            return 0.0f;
    }
}

/**
 * @brief 使用统一结果中的一个周期波形绘制1T或3T时域图。
 */
static HAL_StatusTypeDef Display_DrawTimeResult(
    const AnalyzerResult *result,
    uint8_t periods)
{
    float full_scale_mv;
    float input_scale;
    float trigger_position;
    uint16_t i;
    HAL_StatusTypeDef status;

    if ((result == NULL) ||
        (result->valid == 0U) ||
        (result->waveform_count < 2U) ||
        (result->waveform_count >
         ANALYZER_DISPLAY_POINT_COUNT) ||
        ((periods != 1U) && (periods != 3U)) ||
        (!s_dashboard.valid))
    {
        return HAL_ERROR;
    }

    full_scale_mv = Display_GetTimeFullScale(result);
    input_scale = Display_GetWaveformInputScale(result);
    trigger_position = Display_GetTriggerPosition(result);
    Display_UpdateTimeAxes(result, periods, full_scale_mv);

    for (i = 0U; i < DISPLAY_TIME_CURVE_WIDTH; ++i)
    {
        float source_position =
            Display_WrapWaveformPosition(
                trigger_position +
                ((float)i *
                 (float)periods *
                 (float)result->waveform_count) /
                (float)(DISPLAY_TIME_CURVE_WIDTH - 1U),
                result->waveform_count
            );
        uint32_t integer_position =
            (uint32_t)source_position;
        float fraction =
            source_position -
            (float)integer_position;
        uint16_t index0 =
            (uint16_t)(
                integer_position %
                result->waveform_count
            );
        uint16_t index1 =
            (uint16_t)(
                (index0 + 1U) %
                result->waveform_count
            );
        float sample_mv =
            (result->waveform_mv[index0] +
             fraction *
             (result->waveform_mv[index1] -
              result->waveform_mv[index0])) *
            input_scale;

        /*
         * 淘晶驰addt整帧写入方向与逻辑横坐标相反。频谱路径已经
         * 使用相同补偿；时域也必须反向写入，否则一个周期会显示成
         * x -> 1-x 的左右镜像。这里只反转横轴，不改变电压纵轴。
         */
        s_curve_buffer[
            (DISPLAY_TIME_CURVE_WIDTH - 1U) - i
        ] = Display_MapToByte(
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
        DISPLAY_TIME_CURVE_WIDTH
    );
}

/**
 * @brief 直接使用统一结果中的频率/峰值分量绘制定性频谱。
 *
 * 不再执行显示侧DFT：真实模式使用队友FFT输出，测试模式使用
 * 场景表中的最终分量。频率轴固定为0~500 kHz，使用完整256像素宽度，
 * 使0 Hz和500 kHz分别与左右端刻度严格对应。
 */
static HAL_StatusTypeDef Display_DrawSpectrumResult(
    const AnalyzerResult *result)
{
    float full_scale_mv;
    uint16_t i;
    uint8_t component;
    HAL_StatusTypeDef status;

    if ((result == NULL) ||
        (result->valid == 0U) ||
        (!s_dashboard.valid))
    {
        return HAL_ERROR;
    }

    full_scale_mv = Display_GetSpectrumFullScale(result);
    Display_UpdateSpectrumAxis(full_scale_mv);

    for (i = 0U; i < DISPLAY_SPEC_CURVE_WIDTH; ++i)
    {
        s_curve_buffer[i] = DISPLAY_SPEC_MARGIN;
    }

    for (component = 0U;
         component < result->component_count;
         ++component)
    {
        float frequency_hz =
            result->components[component].frequency_hz;
        uint16_t calculated_position;
        uint16_t buffer_position;

        if ((frequency_hz < 0.0f) ||
            (frequency_hz > DISPLAY_SPECTRUM_MAX_HZ))
        {
            continue;
        }

        calculated_position =
            (uint16_t)(
                (float)DISPLAY_SPEC_X_MARGIN +
                frequency_hz /
                DISPLAY_SPECTRUM_MAX_HZ *
                (float)(
                    DISPLAY_SPEC_CURVE_WIDTH -
                    1U -
                    2U * DISPLAY_SPEC_X_MARGIN
                ) +
                0.5f
            );

        if (calculated_position >= DISPLAY_SPEC_CURVE_WIDTH)
        {
            calculated_position =
                DISPLAY_SPEC_CURVE_WIDTH - 1U;
        }

        /*
         * 保留V1.3.1已验证方向：淘晶驰整帧写入方向与数组索引相反。
         */
        buffer_position =
            (DISPLAY_SPEC_CURVE_WIDTH - 1U) -
            calculated_position;

        s_curve_buffer[buffer_position] =
            Display_MapToByte(
                result->components[component].amplitude_mv,
                0.0f,
                full_scale_mv,
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
        DISPLAY_SPEC_CURVE_WIDTH
    );
}

/**
 * @brief 使用AnalyzerBridge最新快照完成dashboard整页刷新。
 */
static HAL_StatusTypeDef Display_DrawDashboard(void)
{
    HAL_StatusTypeDef time_status;
    HAL_StatusTypeDef spectrum_status;

    if (!AnalyzerBridge_GetLatest(&s_display_result))
    {
        return HAL_ERROR;
    }

    time_status = Display_DrawTimeResult(
        &s_display_result,
        s_visible_periods
    );
    spectrum_status =
        Display_DrawSpectrumResult(&s_display_result);
    Display_UpdateResultTexts(&s_display_result);

    if (spectrum_status != HAL_OK)
    {
        Display_ShowSpectrumError(spectrum_status);
        return spectrum_status;
    }

    if (time_status != HAL_OK)
    {
        return time_status;
    }

    return HAL_OK;
}

/**
 * @brief 处理原dashboard按钮命令。
 *
 * HMI触摸按钮和KEY1实体按键都调用这一入口，保证两者行为完全一致。
 */
static void Display_ProcessButtonCommand(uint8_t command)
{
    if (command == 0x01U)
    {
        s_visible_periods = 1U;
        s_period_switch_sync_pending = true;
        s_pending_action =
            (s_dashboard.valid &&
             (s_run_mode != DISPLAY_RUN_STOPPED))
            ? UI_ACTION_DRAW_TIME_1
            : UI_ACTION_NONE;
    }
    else if (command == 0x02U)
    {
        /*
         * 刷新按钮恢复真实结果通路，并重新加载dashboard以完成页面握手。
         */
        s_pending_action = UI_ACTION_START_REAL;
    }
    else if (command == 0x03U)
    {
        s_visible_periods = 3U;
        s_period_switch_sync_pending = true;
        s_pending_action =
            (s_dashboard.valid &&
             (s_run_mode != DISPLAY_RUN_STOPPED))
            ? UI_ACTION_DRAW_TIME_3
            : UI_ACTION_NONE;
    }
    else if (command == 0x04U)
    {
        /*
         * 测试按钮启动随机测试自动刷新：立即生成一组结果，
         * 此后每3秒重新生成一组。
         */
        s_pending_action = UI_ACTION_START_TEST;
    }
    else if (command == 0x05U)
    {
        /*
         * 清除只清空当前曲线和文本，不改变运行模式。
         */
        s_pending_action = UI_ACTION_CLEAR_DASHBOARD;
    }
    else if (command == 0x06U)
    {
        /*
         * 停止冻结当前画面；只有刷新或测试才能重新开始绘制。
         */
        s_pending_action = UI_ACTION_STOP;
    }
}

/**
 * @brief 处理dashboard初始化帧、按钮帧或带参数的控件帧。
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

    if ((length == 6U) &&
        (frame[1] == 0x20U) &&
        (frame[2] == TJC_DASHBOARD_CODE))
    {
        s_dashboard.time_curve_id = frame[3];
        s_dashboard.spectrum_curve_id = frame[4];
        s_dashboard.valid = true;
        s_period_switch_sync_pending = true;
        s_pending_action =
            (s_run_mode == DISPLAY_RUN_STOPPED)
            ? UI_ACTION_NONE
            : UI_ACTION_DRAW_DASHBOARD;
        return;
    }

    if ((length == 5U) &&
        (frame[1] == 0x02U) &&
        (frame[2] == TJC_TRIGGER_MODE_CODE))
    {
        /*
         * 触发下拉框发送当前选项ID：
         * A5 02 07 mode 5A，mode=0~3。
         */
        Display_SetTriggerMode(
            (DisplayTriggerMode)frame[3]
        );
        return;
    }

    if ((length == 5U) &&
        (frame[1] == 0x02U) &&
        (frame[2] == TJC_HUBER_MODE_CODE) &&
        (frame[3] <= 1U))
    {
        /*
         * 状态开关发送：
         * A5 02 08 enabled 5A，enabled=0普通折叠，1为Huber。
         * UART中断只记录请求，MAD和第二遍折叠留给主循环执行。
         */
        s_requested_huber_enabled = frame[3];
        s_huber_mode_update_pending = true;
        return;
    }

    if ((length == 4U) && (frame[1] == 0x01U))
    {
        Display_ProcessButtonCommand(frame[2]);
    }
}

/**
 * @brief 记录空闲阶段收到的淘晶驰四字节状态帧。
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
 * @brief 将单字节送入变长自定义帧和状态帧解析器。
 */
static void TJC_ParserPushByte(uint8_t byte)
{
    if (s_status_frame_index > 0U)
    {
        if (byte == 0xFFU)
        {
            s_status_frame[s_status_frame_index++] = byte;

            if (s_status_frame_index ==
                sizeof(s_status_frame))
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
        else if (s_rx_frame[1] == 0x02U)
        {
            s_rx_frame_expected = 5U;
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
}

/**
 * @brief USART3逐字节接收完成回调。
 *
 * @note  使用中断接收是为了避免4096点FFT和排序运行期间发生ORE，
 *        每收到一个字节立即送入原有HMI帧解析器，然后重新挂起下一字节。
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if ((huart == NULL) || (huart != s_uart))
    {
        return;
    }

    TJC_ParserPushByte(s_rx_it_byte);
    (void)HAL_UART_Receive_IT(s_uart, &s_rx_it_byte, 1U);
}

/**
 * @brief USART3接收错误回调。
 *
 * @note  若现场出现噪声或接收溢出，清除ORE并丢弃残缺帧，
 *        随后立即恢复逐字节中断接收，避免UART永久失去响应。
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if ((huart == NULL) || (huart != s_uart))
    {
        return;
    }

    __HAL_UART_CLEAR_OREFLAG(huart);
    __HAL_UART_FLUSH_DRREGISTER(huart);
    s_rx_frame_index = 0U;
    s_rx_frame_expected = 0U;
    s_status_frame_index = 0U;
    (void)HAL_UART_Receive_IT(s_uart, &s_rx_it_byte, 1U);
}

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
    s_trigger_mode = DISPLAY_TRIGGER_RISING_ZERO;
    s_requested_huber_enabled = 1U;
    s_huber_mode_update_pending = false;
    s_period_switch_sync_pending = false;
    s_pending_action = UI_ACTION_NONE;
    s_run_mode = DISPLAY_RUN_STOPPED;
    s_next_refresh_tick = 0U;
    s_rx_frame_index = 0U;
    s_rx_frame_expected = 0U;
    s_status_frame_index = 0U;
    s_last_hmi_status = 0U;
    memset(&s_display_result, 0, sizeof(s_display_result));

    /*
     * 先清理可能遗留的RX数据和ORE，再启动逐字节中断接收。
     * 这样dashboard初始化帧和六个按钮帧不会被FFT长任务饿死。
     */
    __HAL_UART_CLEAR_OREFLAG(s_uart);
    __HAL_UART_FLUSH_DRREGISTER(s_uart);
    (void)HAL_UART_Receive_IT(s_uart, &s_rx_it_byte, 1U);

    HAL_Delay(1000U);
    (void)TJC_SendCommand("page dashboard");
}

void Display_RedrawCurrentPage(void)
{
    if (s_dashboard.valid &&
        (s_run_mode != DISPLAY_RUN_STOPPED))
    {
        s_pending_action = UI_ACTION_DRAW_DASHBOARD;
    }
}

void Display_TogglePeriods(void)
{
    if (s_visible_periods == 1U)
    {
        Display_ProcessButtonCommand(0x03U);
    }
    else
    {
        Display_ProcessButtonCommand(0x01U);
    }
}

void Display_RequestRefresh(void)
{
    Display_ProcessButtonCommand(0x02U);
}

void Display_RequestTest(void)
{
    Display_ProcessButtonCommand(0x04U);
}

void Display_SetTriggerMode(DisplayTriggerMode mode)
{
    if (mode >= DISPLAY_TRIGGER_MODE_COUNT)
    {
        return;
    }

    s_trigger_mode = mode;

    /*
     * 触发只影响时域横向起点；运行状态下立即安排一次时域重绘，
     * 不重复发送频谱和六项测量文本。若已有整页刷新、停止等更高优先级
     * 动作则不覆盖；停止状态只保存选择。
     */
    if (s_dashboard.valid &&
        (s_run_mode != DISPLAY_RUN_STOPPED) &&
        ((s_pending_action == UI_ACTION_NONE) ||
         (s_pending_action == UI_ACTION_DRAW_TIME_1) ||
         (s_pending_action == UI_ACTION_DRAW_TIME_3)))
    {
        s_pending_action =
            (s_visible_periods == 3U)
            ? UI_ACTION_DRAW_TIME_3
            : UI_ACTION_DRAW_TIME_1;
    }
}

DisplayTriggerMode Display_GetTriggerMode(void)
{
    return s_trigger_mode;
}

void Display_Task(void)
{
    UI_Action action;
    uint32_t now;

    if (s_uart == NULL)
    {
        return;
    }

    /*
     * 状态开关请求来自USART中断；测试源可在主循环重建同一帧，真实源
     * 则从下一帧VO开始应用，避免在中断内执行4096点MAD和第二遍折叠。
     */
    if (s_huber_mode_update_pending)
    {
        uint8_t requested_huber_enabled;
        AnalyzerWaveformFoldMode requested_mode =
            ANALYZER_WAVEFORM_FOLD_ORDINARY;

        s_huber_mode_update_pending = false;
        requested_huber_enabled = s_requested_huber_enabled;
        requested_mode =
            (requested_huber_enabled != 0U)
            ? ANALYZER_WAVEFORM_FOLD_HUBER
            : ANALYZER_WAVEFORM_FOLD_ORDINARY;

        if (AnalyzerBridge_SetWaveformFoldMode(requested_mode) &&
            s_dashboard.valid &&
            (s_run_mode != DISPLAY_RUN_STOPPED) &&
            ((s_pending_action == UI_ACTION_NONE) ||
             (s_pending_action == UI_ACTION_DRAW_TIME_1) ||
             (s_pending_action == UI_ACTION_DRAW_TIME_3)))
        {
            /*
             * 折叠算法只改变时域波形；立即重画时域，不重复发送
             * 频谱和六项测量文本。
             */
            s_pending_action =
                (s_visible_periods == 3U)
                ? UI_ACTION_DRAW_TIME_3
                : UI_ACTION_DRAW_TIME_1;
        }
    }

    /*
     * KEY1也能改变1T/3T，但不可触摸屏不会自行更新开关外观。
     * UART中断只登记同步请求，主循环在dashboard就绪后回写sw_period。
     */
    if (s_period_switch_sync_pending && s_dashboard.valid)
    {
        uint8_t periods_to_sync = s_visible_periods;

        s_period_switch_sync_pending = false;
        (void)TJC_SetValue(
            "sw_period",
            (periods_to_sync == 3U) ? 1 : 0
        );

        if (s_visible_periods != periods_to_sync)
        {
            s_period_switch_sync_pending = true;
        }
    }

    /*
     * 运行状态下以固定3秒节拍刷新。真实模式重复显示桥接层最新快照；
     * 测试模式每次到期先生成新的随机一致结果，再走同一绘图路径。
     */
    now = HAL_GetTick();
    if ((s_pending_action == UI_ACTION_NONE) &&
        s_dashboard.valid &&
        (s_run_mode != DISPLAY_RUN_STOPPED) &&
        Display_IsRefreshDue(now))
    {
        if (s_run_mode == DISPLAY_RUN_TEST_AUTO)
        {
            AnalyzerBridge_RunRandomTest();
        }

        s_pending_action = UI_ACTION_DRAW_DASHBOARD;
    }

    action = s_pending_action;
    s_pending_action = UI_ACTION_NONE;

    switch (action)
    {
        case UI_ACTION_DRAW_DASHBOARD:
            (void)Display_DrawDashboard();
            Display_ArmNextRefresh();
            break;

        case UI_ACTION_DRAW_TIME_1:
            if (AnalyzerBridge_GetLatest(&s_display_result))
            {
                (void)Display_DrawTimeResult(
                    &s_display_result,
                    1U
                );
            }
            break;

        case UI_ACTION_DRAW_TIME_3:
            if (AnalyzerBridge_GetLatest(&s_display_result))
            {
                (void)Display_DrawTimeResult(
                    &s_display_result,
                    3U
                );
            }
            break;

        case UI_ACTION_START_REAL:
            s_run_mode = DISPLAY_RUN_REAL_AUTO;
            AnalyzerBridge_UseRealResult();
            s_dashboard.valid = false;
            Display_ArmNextRefresh();
            (void)TJC_SendCommand("page dashboard");
            break;

        case UI_ACTION_START_TEST:
            s_run_mode = DISPLAY_RUN_TEST_AUTO;
            AnalyzerBridge_RunRandomTest();

            if (s_dashboard.valid)
            {
                s_pending_action = UI_ACTION_DRAW_DASHBOARD;
            }
            else
            {
                (void)TJC_SendCommand("page dashboard");
            }

            Display_ArmNextRefresh();
            break;

        case UI_ACTION_CLEAR_DASHBOARD:
            (void)Display_ClearDashboard();

            if (s_run_mode != DISPLAY_RUN_STOPPED)
            {
                Display_ArmNextRefresh();
            }
            break;

        case UI_ACTION_STOP:
            s_run_mode = DISPLAY_RUN_STOPPED;
            break;

        case UI_ACTION_NONE:
        default:
            break;
    }
}
