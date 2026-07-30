#include "analyzer_bridge.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define ANALYZER_PI                         3.14159265358979323846f
#define ANALYZER_TEST_CASE_COUNT            6U
#define ANALYZER_STATUS_TEAMMATE_FLAG_MASK  0x000000FFUL
#define ANALYZER_STATUS_TEST_OVERRIDE       0x00000100UL

typedef struct
{
    uint8_t harmonic;
    float amplitude_mv;
    float phase_rad;
} AnalyzerTestTone;

typedef struct
{
    float fundamental_hz;
    uint8_t component_count;
    AnalyzerTestTone tones[ANALYZER_MAX_COMPONENTS];
} AnalyzerTestCase;

static AnalyzerResult s_real_result;
static AnalyzerResult s_test_result;
/*
 * AnalyzerResult包含256点float波形，单个对象超过1 KB。
 * STM32启动文件仅预留1 KB主栈，因此发布真实结果时不能把它作为
 * AnalyzerBridge_PublishReal()的局部变量放到栈上。
 */
static AnalyzerResult s_publish_result;
static uint32_t s_next_sequence = 1U;
static uint32_t s_random_state = 0x6D2B79F5UL;
static uint8_t s_last_test_case = 0xFFU;
static bool s_test_override = false;

#if ANALYZER_TEST_ENABLE
/*
 * 场景表保存“分析完成后的最终结果定义”。频谱直接使用这些分量，
 * 时域波形由同一组分量合成，因此两种显示和Vpp/Vrms保持一致。
 */
static const AnalyzerTestCase s_test_cases[ANALYZER_TEST_CASE_COUNT] =
{
    {
        10500.0f, 3U,
        {{1U, 50.0f, 0.00f}, {3U, 25.0f, 0.42f}, {4U, 15.0f, -0.63f}}
    },
    {
        25000.0f, 3U,
        {{1U, 70.0f, 0.00f}, {2U, 20.0f, -0.35f}, {3U, 10.0f, 0.74f}}
    },
    {
        80000.0f, 2U,
        {{1U, 80.0f, 0.00f}, {2U, 30.0f, 0.58f}, {0U, 0.0f, 0.00f}}
    },
    {
        120000.0f, 3U,
        {{1U, 60.0f, 0.00f}, {2U, 20.0f, -0.48f}, {4U, 10.0f, 0.31f}}
    },
    {
        200000.0f, 2U,
        {{1U, 75.0f, 0.00f}, {2U, 25.0f, 0.67f}, {0U, 0.0f, 0.00f}}
    },
    {
        250000.0f, 2U,
        {{1U, 55.0f, 0.00f}, {2U, 15.0f, -0.52f}, {0U, 0.0f, 0.00f}}
    }
};
#endif

static void AnalyzerBridge_SortComponents(AnalyzerComponent *components,
                                           uint8_t count)
{
    uint8_t i;
    uint8_t j;

    for (i = 0U; i < count; ++i)
    {
        for (j = (uint8_t)(i + 1U); j < count; ++j)
        {
            if (components[j].frequency_hz <
                components[i].frequency_hz)
            {
                AnalyzerComponent temporary = components[i];
                components[i] = components[j];
                components[j] = temporary;
            }
        }
    }
}

static bool AnalyzerBridge_BuildRealWaveform(
    AnalyzerResult *result,
    const uint16_t *adc_codes,
    uint16_t sample_count,
    float volts_per_code,
    float sample_rate_hz)
{
    float samples_per_period;
    float mean_mv = 0.0f;
    uint16_t i;

    if ((result == NULL) ||
        (adc_codes == NULL) ||
        (sample_count < 2U) ||
        (volts_per_code <= 0.0f) ||
        (sample_rate_hz <= 0.0f) ||
        (result->fundamental_hz <= 0.0f))
    {
        return false;
    }

    samples_per_period =
        sample_rate_hz / result->fundamental_hz;

    if ((samples_per_period < 2.0f) ||
        (samples_per_period > (float)sample_count))
    {
        return false;
    }

    for (i = 0U; i < ANALYZER_DISPLAY_POINT_COUNT; ++i)
    {
        float source_position =
            (float)i *
            samples_per_period /
            (float)ANALYZER_DISPLAY_POINT_COUNT;
        uint16_t index0 = (uint16_t)source_position;
        uint16_t index1;
        float fraction;
        float code;
        float sample_mv;

        if (index0 >= sample_count)
        {
            index0 = (uint16_t)(sample_count - 1U);
        }

        index1 = (uint16_t)(index0 + 1U);
        if (index1 >= sample_count)
        {
            index1 = index0;
        }

        fraction = source_position - (float)index0;
        code =
            (float)adc_codes[index0] +
            fraction *
            ((float)adc_codes[index1] -
             (float)adc_codes[index0]);
        sample_mv = code * volts_per_code * 1000.0f;

        result->waveform_mv[i] = sample_mv;
        mean_mv += sample_mv;
    }

    mean_mv /= (float)ANALYZER_DISPLAY_POINT_COUNT;

    for (i = 0U; i < ANALYZER_DISPLAY_POINT_COUNT; ++i)
    {
        result->waveform_mv[i] -= mean_mv;
    }

    result->waveform_count = ANALYZER_DISPLAY_POINT_COUNT;
    return true;
}

#if ANALYZER_TEST_ENABLE
static uint32_t AnalyzerBridge_XorShift32(void)
{
    uint32_t value = s_random_state;

    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;

    if (value == 0U)
    {
        value = 0xA341316CUL;
    }

    s_random_state = value;
    return value;
}

static void AnalyzerBridge_BuildTestResult(
    const AnalyzerTestCase *test_case,
    AnalyzerResult *result)
{
    float maximum_mv = -3.402823466e+38F;
    float minimum_mv = 3.402823466e+38F;
    float square_sum = 0.0f;
    uint16_t i;
    uint8_t component;

    memset(result, 0, sizeof(*result));
    result->source = ANALYZER_SOURCE_TEST;
    result->fundamental_hz = test_case->fundamental_hz;
    result->component_count = test_case->component_count;
    result->waveform_count = ANALYZER_DISPLAY_POINT_COUNT;
    result->status_flags = ANALYZER_STATUS_TEST_OVERRIDE;

    for (component = 0U;
         component < test_case->component_count;
         ++component)
    {
        result->components[component].frequency_hz =
            test_case->fundamental_hz *
            (float)test_case->tones[component].harmonic;
        result->components[component].amplitude_mv =
            test_case->tones[component].amplitude_mv;
    }

    for (i = 0U; i < ANALYZER_DISPLAY_POINT_COUNT; ++i)
    {
        float base_phase =
            2.0f * ANALYZER_PI *
            (float)i /
            (float)ANALYZER_DISPLAY_POINT_COUNT;
        float sample_mv = 0.0f;

        for (component = 0U;
             component < test_case->component_count;
             ++component)
        {
            sample_mv +=
                test_case->tones[component].amplitude_mv *
                sinf(
                    (float)test_case->tones[component].harmonic *
                    base_phase +
                    test_case->tones[component].phase_rad
                );
        }

        result->waveform_mv[i] = sample_mv;
        square_sum += sample_mv * sample_mv;

        if (sample_mv > maximum_mv)
        {
            maximum_mv = sample_mv;
        }

        if (sample_mv < minimum_mv)
        {
            minimum_mv = sample_mv;
        }
    }

    result->vpp_mv = maximum_mv - minimum_mv;
    result->vrms_mv =
        sqrtf(
            square_sum /
            (float)ANALYZER_DISPLAY_POINT_COUNT
        );
    result->sequence = s_next_sequence++;
    result->valid = 1U;
}
#endif

void AnalyzerBridge_Init(void)
{
    memset(&s_real_result, 0, sizeof(s_real_result));
    memset(&s_test_result, 0, sizeof(s_test_result));
    memset(&s_publish_result, 0, sizeof(s_publish_result));
    s_next_sequence = 1U;
    s_random_state =
        0x6D2B79F5UL ^
        HAL_GetTick() ^
        SysTick->VAL;
    s_last_test_case = 0xFFU;
    s_test_override = false;
}

void AnalyzerBridge_PublishReal(
    const uint16_t *adc_codes,
    uint16_t sample_count,
    float volts_per_code,
    float sample_rate_hz,
    float vpp_v,
    float vrms_v,
    uint8_t teammate_flag,
    const float frequencies_hz[ANALYZER_MAX_COMPONENTS],
    const float amplitudes_v[ANALYZER_MAX_COMPONENTS])
{
    uint8_t requested_count;
    uint8_t i;

    if ((adc_codes == NULL) ||
        (frequencies_hz == NULL) ||
        (amplitudes_v == NULL))
    {
        return;
    }

    if (teammate_flag == 2U)
    {
        requested_count = 2U;
    }
    else if (teammate_flag == 3U)
    {
        requested_count = 3U;
    }
    else
    {
        return;
    }

    memset(&s_publish_result, 0, sizeof(s_publish_result));
    s_publish_result.source = ANALYZER_SOURCE_REAL;
    s_publish_result.vpp_mv = vpp_v * 1000.0f;
    s_publish_result.vrms_mv = vrms_v * 1000.0f;
    s_publish_result.status_flags =
        ((uint32_t)teammate_flag &
         ANALYZER_STATUS_TEAMMATE_FLAG_MASK);

    for (i = 0U; i < requested_count; ++i)
    {
        if ((frequencies_hz[i] > 0.0f) &&
            (amplitudes_v[i] >= 0.0f))
        {
            uint8_t output_index =
                s_publish_result.component_count;

            s_publish_result.components[output_index].frequency_hz =
                frequencies_hz[i];
            s_publish_result.components[output_index].amplitude_mv =
                amplitudes_v[i] * 1000.0f;
            s_publish_result.component_count++;
        }
    }

    if (s_publish_result.component_count == 0U)
    {
        return;
    }

    AnalyzerBridge_SortComponents(
        s_publish_result.components,
        s_publish_result.component_count
    );
    s_publish_result.fundamental_hz =
        s_publish_result.components[0].frequency_hz;

    if (!AnalyzerBridge_BuildRealWaveform(
            &s_publish_result,
            adc_codes,
            sample_count,
            volts_per_code,
            sample_rate_hz))
    {
        return;
    }

    s_publish_result.sequence = s_next_sequence++;
    s_publish_result.valid = 1U;
    s_real_result = s_publish_result;
}

bool AnalyzerBridge_GetLatest(AnalyzerResult *result)
{
    const AnalyzerResult *selected;

    if (result == NULL)
    {
        return false;
    }

    selected =
        s_test_override
        ? &s_test_result
        : &s_real_result;

    if (selected->valid == 0U)
    {
        memset(result, 0, sizeof(*result));
        return false;
    }

    *result = *selected;
    return true;
}

void AnalyzerBridge_RunRandomTest(void)
{
#if ANALYZER_TEST_ENABLE
    uint32_t entropy =
        HAL_GetTick() ^
        SysTick->VAL ^
        s_next_sequence;
    uint8_t selected_case;

    s_random_state ^= entropy;
    selected_case =
        (uint8_t)(
            AnalyzerBridge_XorShift32() %
            ANALYZER_TEST_CASE_COUNT
        );

    if (selected_case == s_last_test_case)
    {
        selected_case =
            (uint8_t)(
                (selected_case + 1U) %
                ANALYZER_TEST_CASE_COUNT
            );
    }

    AnalyzerBridge_BuildTestResult(
        &s_test_cases[selected_case],
        &s_test_result
    );
    s_last_test_case = selected_case;
    s_test_override = true;
#endif
}

bool AnalyzerBridge_IsTestOverrideActive(void)
{
#if ANALYZER_TEST_ENABLE
    return s_test_override;
#else
    return false;
#endif
}
