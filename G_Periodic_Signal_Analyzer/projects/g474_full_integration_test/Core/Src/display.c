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

/* dashboard页面内两块曲线控件的固定显示尺寸。 */
#define DISPLAY_CURVE_WIDTH          794U
#define DISPLAY_CURVE_HEIGHT         145U
#define DISPLAY_CURVE_Y_MAX          (DISPLAY_CURVE_HEIGHT - 1U)
#define DISPLAY_TIME_MARGIN          5U
#define DISPLAY_SPEC_MARGIN          3U
#define DISPLAY_SPEC_X_MARGIN        32U
#define DISPLAY_SPECTRUM_MAX_HZ      500000.0f
#define DISPLAY_REFRESH_INTERVAL_MS  3000U

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

static uint8_t s_curve_buffer[DISPLAY_CURVE_WIDTH];

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
 * @brief 更新dashboard的峰峰值、真有效值、基频和最多三个谱峰。
 */
static void Display_UpdateResultTexts(const AnalyzerResult *result)
{
    char text[64];
    uint32_t vpp_x10;
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
 * @brief 将六项测量文本恢复为无数据占位符。
 */
static void Display_ClearResultTexts(void)
{
    (void)TJC_SetText("t_vpp", "Upp: --.- mV");
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
 * @brief 根据波形最大绝对值自动选择对称时域显示满量程。
 */
static float Display_GetTimeFullScale(const AnalyzerResult *result)
{
    float maximum_absolute = 0.0f;
    uint16_t i;

    for (i = 0U; i < result->waveform_count; ++i)
    {
        float value = fabsf(result->waveform_mv[i]);
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
 * @brief 使用统一结果中的一个周期波形绘制1T或3T时域图。
 */
static HAL_StatusTypeDef Display_DrawTimeResult(
    const AnalyzerResult *result,
    uint8_t periods)
{
    float full_scale_mv;
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

    for (i = 0U; i < DISPLAY_CURVE_WIDTH; ++i)
    {
        float source_position =
            ((float)i *
             (float)periods *
             (float)result->waveform_count) /
            (float)(DISPLAY_CURVE_WIDTH - 1U);
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
            result->waveform_mv[index0] +
            fraction *
            (result->waveform_mv[index1] -
             result->waveform_mv[index0]);

        /*
         * 淘晶驰addt整帧写入方向与逻辑横坐标相反。频谱路径已经
         * 使用相同补偿；时域也必须反向写入，否则一个周期会显示成
         * x -> 1-x 的左右镜像。这里只反转横轴，不改变电压纵轴。
         */
        s_curve_buffer[
            (DISPLAY_CURVE_WIDTH - 1U) - i
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
        DISPLAY_CURVE_WIDTH
    );
}

/**
 * @brief 直接使用统一结果中的频率/峰值分量绘制定性频谱。
 *
 * 不再执行显示侧DFT：真实模式使用队友FFT输出，测试模式使用
 * 场景表中的最终分量。频率轴固定为0~500 kHz，并在左右各保留
 * DISPLAY_SPEC_X_MARGIN个像素，避免0 Hz或500 kHz谱线贴住边框。
 */
static HAL_StatusTypeDef Display_DrawSpectrumResult(
    const AnalyzerResult *result)
{
    float full_scale_mv = 1.0f;
    uint16_t i;
    uint8_t component;
    HAL_StatusTypeDef status;

    if ((result == NULL) ||
        (result->valid == 0U) ||
        (!s_dashboard.valid))
    {
        return HAL_ERROR;
    }

    for (i = 0U; i < DISPLAY_CURVE_WIDTH; ++i)
    {
        s_curve_buffer[i] = DISPLAY_SPEC_MARGIN;
    }

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

    full_scale_mv *= 1.15f;

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
                    DISPLAY_CURVE_WIDTH -
                    1U -
                    2U * DISPLAY_SPEC_X_MARGIN
                ) +
                0.5f
            );

        if (calculated_position >= DISPLAY_CURVE_WIDTH)
        {
            calculated_position =
                DISPLAY_CURVE_WIDTH - 1U;
        }

        /*
         * 保留V1.3.1已验证方向：淘晶驰整帧写入方向与数组索引相反。
         */
        buffer_position =
            (DISPLAY_CURVE_WIDTH - 1U) -
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
        DISPLAY_CURVE_WIDTH
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
 * @brief 处理dashboard初始化帧或按钮帧。
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
        s_pending_action =
            (s_run_mode == DISPLAY_RUN_STOPPED)
            ? UI_ACTION_NONE
            : UI_ACTION_DRAW_DASHBOARD;
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
 * @note  使用中断接收是为了避免2048点FFT和排序运行期间发生ORE，
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

void Display_RequestTest(void)
{
    Display_ProcessButtonCommand(0x04U);
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
