#include "display.h"

#include "tjc_hmi.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/* 两个页面中的曲线控件均为s0、ID=1、通道0、宽674。 */
#define TJC_CURVE_ID                 1U
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

typedef enum
{
    DISPLAY_PAGE_NONE = 0,
    DISPLAY_PAGE_TIME,
    DISPLAY_PAGE_SPECTRUM
} Display_Page;

static bool s_display_initialized = false;
static Display_Page s_current_page = DISPLAY_PAGE_NONE;
static uint8_t s_visible_periods = 1U;
static uint8_t s_curve_buffer[TJC_CURVE_WIDTH];
static float s_demo_one_period_mv[DEMO_PERIOD_SAMPLE_COUNT];
static float s_demo_fft_input_mv[DEMO_FFT_SAMPLE_COUNT];

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
 * @brief 更新time页面的周期模式、峰峰值、真有效值和基频。
 * @param periods 当前显示周期数，只允许1或3。
 * @param frequency_hz 被测信号基频，单位Hz。
 * @param vpp_mv 峰峰值，单位mV。
 * @param rms_mv 真有效值，单位mV。
 * @return 无。
 */
static void Display_UpdateTimeValues(uint8_t periods,
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

        s_curve_buffer[i] = Display_MapToByte(
            sample_mv,
            -full_scale_mv,
            full_scale_mv
        );
    }

    status = TJC_ClearCurve(TJC_CURVE_ID, TJC_CURVE_CHANNEL);

    if (status != HAL_OK)
    {
        return status;
    }

    HAL_Delay(10U);

    status = TJC_SendCurve(
        TJC_CURVE_ID,
        TJC_CURVE_CHANNEL,
        s_curve_buffer,
        TJC_CURVE_WIDTH
    );

    if (status == HAL_OK)
    {
        Display_UpdateTimeValues(
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
static void Display_UpdateSpectrumValues(float f1_hz,
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
        s_curve_buffer[i] = TJC_CURVE_MARGIN;
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

        s_curve_buffer[buffer_position] = Display_MapToByte(
            amplitude_mv,
            0.0f,
            DEMO_SPECTRUM_FULL_SCALE_MV
        );
    }

    status = TJC_ClearCurve(TJC_CURVE_ID, TJC_CURVE_CHANNEL);

    if (status != HAL_OK)
    {
        return status;
    }

    HAL_Delay(10U);

    status = TJC_SendCurve(
        TJC_CURVE_ID,
        TJC_CURVE_CHANNEL,
        s_curve_buffer,
        TJC_CURVE_WIDTH
    );

    if (status == HAL_OK)
    {
        Display_UpdateSpectrumValues(
            DEMO_SIGNAL_FREQUENCY_HZ,
            DEMO_SIGNAL_PEAK_MV
        );
    }

    return status;
}

/**
 * @brief 根据页面和按钮事件执行对应显示动作。
 * @param event 由淘晶驰通信模块解析出的页面或按钮事件。
 * @return 无。
 */
static void Display_HandleEvent(TJC_Event event)
{
    switch (event)
    {
        case TJC_EVENT_TIME_PAGE_READY:
            s_current_page = DISPLAY_PAGE_TIME;
            (void)Display_DrawTimeFrame(
                s_demo_one_period_mv,
                DEMO_PERIOD_SAMPLE_COUNT,
                s_visible_periods,
                DEMO_SIGNAL_FREQUENCY_HZ,
                2.0f * DEMO_SIGNAL_PEAK_MV,
                DEMO_SIGNAL_PEAK_MV / 1.41421356f,
                DEMO_TIME_FULL_SCALE_MV
            );
            break;

        case TJC_EVENT_SPECTRUM_PAGE_READY:
            s_current_page = DISPLAY_PAGE_SPECTRUM;
            (void)Display_DrawDemoSpectrum();
            break;

        case TJC_EVENT_SHOW_ONE_PERIOD:
            s_visible_periods = 1U;

            if (s_current_page == DISPLAY_PAGE_TIME)
            {
                (void)Display_DrawTimeFrame(
                    s_demo_one_period_mv,
                    DEMO_PERIOD_SAMPLE_COUNT,
                    1U,
                    DEMO_SIGNAL_FREQUENCY_HZ,
                    2.0f * DEMO_SIGNAL_PEAK_MV,
                    DEMO_SIGNAL_PEAK_MV / 1.41421356f,
                    DEMO_TIME_FULL_SCALE_MV
                );
            }
            break;

        case TJC_EVENT_SHOW_THREE_PERIODS:
            s_visible_periods = 3U;

            if (s_current_page == DISPLAY_PAGE_TIME)
            {
                (void)Display_DrawTimeFrame(
                    s_demo_one_period_mv,
                    DEMO_PERIOD_SAMPLE_COUNT,
                    3U,
                    DEMO_SIGNAL_FREQUENCY_HZ,
                    2.0f * DEMO_SIGNAL_PEAK_MV,
                    DEMO_SIGNAL_PEAK_MV / 1.41421356f,
                    DEMO_TIME_FULL_SCALE_MV
                );
            }
            break;

        case TJC_EVENT_NONE:
        default:
            break;
    }
}

/**
 * @brief 初始化显示模块并进入时域页面。
 * @param huart 与淘晶驰串口屏连接的UART句柄。
 * @return 无。
 *
 * 初始化底层通信、生成当前演示数据，等待屏幕启动后发送page time。
 * 第一帧波形仍需等待time页面返回就绪事件后才会绘制。
 */
void Display_Init(UART_HandleTypeDef *huart)
{
    if (huart == NULL)
    {
        return;
    }

    TJC_Init(huart);
    Demo_PrepareInputData();

    s_current_page = DISPLAY_PAGE_NONE;
    s_visible_periods = 1U;
    s_display_initialized = true;

    /* 等待HMI模拟器或实物串口屏启动。 */
    HAL_Delay(1000U);

    /*
     * time页面完成加载后会发送A5 20 01 5A；
     * 收到该就绪帧后才执行第一帧曲线绘制。
     */
    (void)TJC_SendCommand("page time");
}

/**
 * @brief 执行一次显示模块轮询任务。
 * @return 无。
 *
 * 本函数从淘晶驰通信模块取得一个有效事件，再调用对应的时域或
 * 频谱绘制函数。应在main()的while(1)中持续调用。
 */
void Display_Task(void)
{
    TJC_Event event;

    if (!s_display_initialized)
    {
        return;
    }

    if (TJC_PollEvent(&event))
    {
        Display_HandleEvent(event);
    }
}
