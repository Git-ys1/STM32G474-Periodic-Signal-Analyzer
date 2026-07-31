#include "analyzer_bridge.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define ANALYZER_STATUS_TEAMMATE_FLAG_MASK  0x000000FFUL
#define ANALYZER_STATUS_TEST_OVERRIDE       0x00000100UL
#define ANALYZER_PHASE_WEIGHT_EPSILON       0.000001f
#define ANALYZER_TWO_PI                     6.28318530717958647692f
#define ANALYZER_FREQ_SEARCH_HALF_HZ        500.0f
#define ANALYZER_FREQ_SEARCH_STEP_HZ        10.0f
#define ANALYZER_FREQ_SEARCH_COUNT          101U
#define ANALYZER_MAX_SAMPLE_COUNT           2048U
#define ANALYZER_HUBER_K                    1.345f
#define ANALYZER_HUBER_SCALE_FACTOR         1.4826f
#define ANALYZER_HUBER_MINIMUM_DELTA_LSB    1.5f

#if ANALYZER_TEST_ENABLE
#include "generated_adc_tests.h"

#if ANALYZER_CUSTOM_TEST_ENABLE
#include "generated_custom_adc_tests.h"
#define ANALYZER_TEST_CASE_COUNT       GENERATED_CUSTOM_ADC_TEST_CASE_COUNT
#define ANALYZER_TEST_SAMPLE_COUNT     GENERATED_CUSTOM_ADC_SAMPLE_COUNT
#define ANALYZER_TEST_SAMPLE_RATE_HZ   GENERATED_CUSTOM_ADC_SAMPLE_RATE_HZ
#define ANALYZER_TEST_VOLTS_PER_CODE   GENERATED_CUSTOM_ADC_VOLTS_PER_CODE
#define ANALYZER_TEST_CASES            s_generated_custom_adc_test_cases
#else
#define ANALYZER_TEST_CASE_COUNT       GENERATED_ADC_TEST_CASE_COUNT
#define ANALYZER_TEST_SAMPLE_COUNT     GENERATED_ADC_SAMPLE_COUNT
#define ANALYZER_TEST_SAMPLE_RATE_HZ   GENERATED_ADC_SAMPLE_RATE_HZ
#define ANALYZER_TEST_VOLTS_PER_CODE   GENERATED_ADC_VOLTS_PER_CODE
#define ANALYZER_TEST_CASES            s_generated_adc_test_cases
#endif
#endif

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
static AnalyzerWaveformFoldMode s_waveform_fold_mode =
    ANALYZER_WAVEFORM_FOLD_ORDINARY;
/*
 * 完整2048点ADC缓冲区按基频相位折叠到256个显示相位槽。
 * 两个工作数组必须是静态存储，避免占用仅1 KB的主栈。
 */
static float s_phase_sum_mv[ANALYZER_DISPLAY_POINT_COUNT];
static float s_phase_weight[ANALYZER_DISPLAY_POINT_COUNT];
static float s_frequency_scores[ANALYZER_FREQ_SEARCH_COUNT];
/*
 * Huber需要保存2048个第一遍残差以计算中位数和MAD。
 * 最近一帧真实ADC也保留在静态区，保证状态开关切换后可用同一帧
 * 立即重建，而不是等待下一帧或拿不同测试组做视觉比较。
 */
static float s_huber_workspace[ANALYZER_MAX_SAMPLE_COUNT];
static uint16_t s_latest_real_adc[ANALYZER_MAX_SAMPLE_COUNT];
static uint16_t s_latest_real_sample_count = 0U;
static float s_latest_real_volts_per_code = 0.0f;
static float s_latest_real_sample_rate_hz = 0.0f;
static bool s_latest_real_input_valid = false;

/**
 * @brief 原地选择第k小浮点数，避免qsort递归和额外2048点副本。
 */
static float AnalyzerBridge_SelectKth(float *values,
                                      uint16_t count,
                                      uint16_t k)
{
    int32_t left = 0;
    int32_t right = (int32_t)count - 1;
    int32_t target = (int32_t)k;

    while (left < right)
    {
        int32_t i = left;
        int32_t j = right;
        float pivot =
            values[left + (right - left) / 2];

        while (i <= j)
        {
            while ((i <= right) && (values[i] < pivot))
            {
                ++i;
            }

            while ((j >= left) && (values[j] > pivot))
            {
                --j;
            }

            if (i <= j)
            {
                float temporary = values[i];
                values[i] = values[j];
                values[j] = temporary;
                ++i;
                --j;
            }
        }

        if (target <= j)
        {
            right = j;
        }
        else if (target >= i)
        {
            left = i;
        }
        else
        {
            break;
        }
    }

    return values[k];
}

/**
 * @brief 计算浮点数组中位数；允许原地重排工作数组。
 */
static float AnalyzerBridge_Median(float *values,
                                   uint16_t count)
{
    uint16_t upper_index;
    float upper;

    if ((values == NULL) || (count == 0U))
    {
        return 0.0f;
    }

    upper_index = (uint16_t)(count / 2U);
    upper = AnalyzerBridge_SelectKth(
        values,
        count,
        upper_index
    );

    if ((count & 1U) != 0U)
    {
        return upper;
    }

    return
        0.5f *
        (upper +
         AnalyzerBridge_SelectKth(
             values,
             count,
             (uint16_t)(upper_index - 1U)
         ));
}

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

/**
 * @brief 计算指定频率处的单频相关能量。
 *
 * 使用正余弦递推避免在2048点循环中反复调用sinf/cosf。这里只比较候选
 * 频率之间的相对能量，因此无需换算为实际电压或执行幅值归一化。
 */
static float AnalyzerBridge_CorrelationScore(
    const uint16_t *adc_codes,
    uint16_t sample_count,
    float mean_code,
    float sample_rate_hz,
    float frequency_hz)
{
    float angle_step;
    float step_real;
    float step_imag;
    float oscillator_real = 1.0f;
    float oscillator_imag = 0.0f;
    float accumulator_real = 0.0f;
    float accumulator_imag = 0.0f;
    uint16_t i;

    if ((frequency_hz <= 0.0f) ||
        (frequency_hz >= (sample_rate_hz * 0.5f)))
    {
        return -1.0f;
    }

    angle_step =
        ANALYZER_TWO_PI *
        frequency_hz /
        sample_rate_hz;
    step_real = cosf(angle_step);
    step_imag = sinf(angle_step);

    for (i = 0U; i < sample_count; ++i)
    {
        float sample =
            (float)adc_codes[i] - mean_code;
        float next_real;

        accumulator_real += sample * oscillator_real;
        accumulator_imag -= sample * oscillator_imag;

        next_real =
            oscillator_real * step_real -
            oscillator_imag * step_imag;
        oscillator_imag =
            oscillator_imag * step_real +
            oscillator_real * step_imag;
        oscillator_real = next_real;
    }

    return
        accumulator_real * accumulator_real +
        accumulator_imag * accumulator_imag;
}

/**
 * @brief 为波形相位折叠细化基频，不修改对外显示的队友测量结果。
 *
 * 队友FFT频率分辨率为500 Hz。若直接用量化后的频率折叠完整2 ms缓冲区，
 * 约±250 Hz的误差会在多个周期上累计并把波形平均变小。这里选取幅度最大
 * 的已识别谱线中选择“幅度×谐波次数”最大的参考谱线，在粗基频±500 Hz
 * 范围内按10 Hz步进搜索相关峰，再用三点抛物线插值得到更精细的折叠频率。
 */
static float AnalyzerBridge_RefineFundamentalForWaveform(
    const AnalyzerResult *result,
    const uint16_t *adc_codes,
    uint16_t sample_count,
    float mean_code,
    float sample_rate_hz)
{
    uint8_t reference_index = 0U;
    uint16_t harmonic_order;
    uint16_t best_index = 0U;
    uint16_t i;
    float best_information_score = -1.0f;
    float ratio;
    float best_score = -1.0f;
    float fractional_offset = 0.0f;

    if ((result == NULL) ||
        (adc_codes == NULL) ||
        (result->component_count == 0U) ||
        (result->fundamental_hz <= 0.0f))
    {
        return 0.0f;
    }

    for (i = 0U; i < result->component_count; ++i)
    {
        uint16_t candidate_order =
            (uint16_t)(
                result->components[i].frequency_hz /
                result->fundamental_hz +
                0.5f
            );
        float information_score;

        if (candidate_order == 0U)
        {
            candidate_order = 1U;
        }

        information_score =
            result->components[i].amplitude_mv *
            (float)candidate_order;
        if (information_score > best_information_score)
        {
            best_information_score = information_score;
            reference_index = (uint8_t)i;
        }
    }

    ratio =
        result->components[reference_index].frequency_hz /
        result->fundamental_hz;
    harmonic_order = (uint16_t)(ratio + 0.5f);
    if (harmonic_order == 0U)
    {
        harmonic_order = 1U;
    }

    for (i = 0U; i < ANALYZER_FREQ_SEARCH_COUNT; ++i)
    {
        float candidate_fundamental =
            result->fundamental_hz -
            ANALYZER_FREQ_SEARCH_HALF_HZ +
            (float)i * ANALYZER_FREQ_SEARCH_STEP_HZ;
        float candidate_tone =
            candidate_fundamental *
            (float)harmonic_order;

        s_frequency_scores[i] =
            AnalyzerBridge_CorrelationScore(
                adc_codes,
                sample_count,
                mean_code,
                sample_rate_hz,
                candidate_tone
            );

        if (s_frequency_scores[i] > best_score)
        {
            best_score = s_frequency_scores[i];
            best_index = i;
        }
    }

    if ((best_index > 0U) &&
        (best_index + 1U < ANALYZER_FREQ_SEARCH_COUNT))
    {
        float left = s_frequency_scores[best_index - 1U];
        float center = s_frequency_scores[best_index];
        float right = s_frequency_scores[best_index + 1U];
        float denominator =
            left - 2.0f * center + right;

        if (fabsf(denominator) > ANALYZER_PHASE_WEIGHT_EPSILON)
        {
            fractional_offset =
                0.5f * (left - right) / denominator;
            if (fractional_offset < -1.0f)
            {
                fractional_offset = -1.0f;
            }
            else if (fractional_offset > 1.0f)
            {
                fractional_offset = 1.0f;
            }
        }
    }

    return
        result->fundamental_hz -
        ANALYZER_FREQ_SEARCH_HALF_HZ +
        ((float)best_index + fractional_offset) *
        ANALYZER_FREQ_SEARCH_STEP_HZ;
}

/**
 * @brief 在普通折叠结果上计算Huber权重并对原ADC样本执行第二遍折叠。
 *
 * 第一遍波形只用于预测每个样本的相位期望值；第二遍仍折叠原始中心化
 * ADC样本。阈值为max(1.345×1.4826×MAD, 1.5×ADC_LSB)，
 * 不增加全局平滑或高阶插值。
 */
static bool AnalyzerBridge_ApplyHuberSecondPass(
    AnalyzerResult *result,
    const uint16_t *adc_codes,
    uint16_t sample_count,
    float mean_code,
    float volts_per_code,
    float sample_rate_hz,
    float fold_frequency_hz)
{
    float phase_step;
    float phase_cycles = 0.0f;
    float residual_center;
    float robust_scale_mv;
    float delta_mv;
    float mean_mv = 0.0f;
    uint16_t i;

    if ((result == NULL) ||
        (adc_codes == NULL) ||
        (sample_count < 2U) ||
        (sample_count > ANALYZER_MAX_SAMPLE_COUNT) ||
        (volts_per_code <= 0.0f) ||
        (sample_rate_hz <= 0.0f) ||
        (fold_frequency_hz <= 0.0f))
    {
        return false;
    }

    phase_step = fold_frequency_hz / sample_rate_hz;

    /*
     * 第一遍残差。result->waveform_mv此时仍是普通折叠并去均值后的
     * 256点周期波形，预测值继续使用与显示一致的周期线性插值。
     */
    for (i = 0U; i < sample_count; ++i)
    {
        float phase_position =
            phase_cycles * (float)ANALYZER_DISPLAY_POINT_COUNT;
        uint16_t index0 = (uint16_t)phase_position;
        uint16_t index1;
        float fraction;
        float predicted_mv;
        float sample_mv;

        if (index0 >= ANALYZER_DISPLAY_POINT_COUNT)
        {
            index0 = 0U;
        }

        fraction = phase_position - (float)index0;
        index1 =
            (uint16_t)(
                (index0 + 1U) %
                ANALYZER_DISPLAY_POINT_COUNT
            );
        predicted_mv =
            result->waveform_mv[index0] +
            fraction *
            (result->waveform_mv[index1] -
             result->waveform_mv[index0]);
        sample_mv =
            ((float)adc_codes[i] - mean_code) *
            volts_per_code *
            1000.0f;
        s_huber_workspace[i] = sample_mv - predicted_mv;

        phase_cycles += phase_step;
        while (phase_cycles >= 1.0f)
        {
            phase_cycles -= 1.0f;
        }
    }

    residual_center = AnalyzerBridge_Median(
        s_huber_workspace,
        sample_count
    );

    for (i = 0U; i < sample_count; ++i)
    {
        s_huber_workspace[i] =
            fabsf(s_huber_workspace[i] - residual_center);
    }

    robust_scale_mv =
        ANALYZER_HUBER_SCALE_FACTOR *
        AnalyzerBridge_Median(
            s_huber_workspace,
            sample_count
        );
    delta_mv =
        ANALYZER_HUBER_K * robust_scale_mv;
    if (delta_mv <
        ANALYZER_HUBER_MINIMUM_DELTA_LSB *
        volts_per_code *
        1000.0f)
    {
        delta_mv =
            ANALYZER_HUBER_MINIMUM_DELTA_LSB *
            volts_per_code *
            1000.0f;
    }

    /*
     * 第二遍重新计算每个样本残差和Huber权重。工作数组已经被中位数
     * 选择原地重排，因此这里不依赖其样本顺序。
     */
    memset(s_phase_sum_mv, 0, sizeof(s_phase_sum_mv));
    memset(s_phase_weight, 0, sizeof(s_phase_weight));
    phase_cycles = 0.0f;

    for (i = 0U; i < sample_count; ++i)
    {
        float phase_position =
            phase_cycles * (float)ANALYZER_DISPLAY_POINT_COUNT;
        uint16_t index0 = (uint16_t)phase_position;
        uint16_t index1;
        float fraction;
        float predicted_mv;
        float sample_mv;
        float centered_residual;
        float sample_weight = 1.0f;
        float weight0;
        float weight1;

        if (index0 >= ANALYZER_DISPLAY_POINT_COUNT)
        {
            index0 = 0U;
        }

        fraction = phase_position - (float)index0;
        index1 =
            (uint16_t)(
                (index0 + 1U) %
                ANALYZER_DISPLAY_POINT_COUNT
            );
        predicted_mv =
            result->waveform_mv[index0] +
            fraction *
            (result->waveform_mv[index1] -
             result->waveform_mv[index0]);
        sample_mv =
            ((float)adc_codes[i] - mean_code) *
            volts_per_code *
            1000.0f;
        centered_residual =
            fabsf(
                sample_mv -
                predicted_mv -
                residual_center
            );

        if (centered_residual > delta_mv)
        {
            sample_weight = delta_mv / centered_residual;
        }

        weight0 = (1.0f - fraction) * sample_weight;
        weight1 = fraction * sample_weight;
        s_phase_sum_mv[index0] += sample_mv * weight0;
        s_phase_weight[index0] += weight0;

        if (fraction > ANALYZER_PHASE_WEIGHT_EPSILON)
        {
            s_phase_sum_mv[index1] += sample_mv * weight1;
            s_phase_weight[index1] += weight1;
        }

        phase_cycles += phase_step;
        while (phase_cycles >= 1.0f)
        {
            phase_cycles -= 1.0f;
        }
    }

    /*
     * 与普通折叠完全相同：有覆盖槽取加权平均，空槽做环形线性
     * 补齐，最后再次去除256点均值。
     */
    for (i = 0U; i < ANALYZER_DISPLAY_POINT_COUNT; ++i)
    {
        if (s_phase_weight[i] > ANALYZER_PHASE_WEIGHT_EPSILON)
        {
            result->waveform_mv[i] =
                s_phase_sum_mv[i] / s_phase_weight[i];
        }
        else
        {
            result->waveform_mv[i] = 0.0f;
        }
    }

    for (i = 0U; i < ANALYZER_DISPLAY_POINT_COUNT; ++i)
    {
        uint16_t left_distance;
        uint16_t right_distance;
        uint16_t left_index;
        uint16_t right_index;
        float ratio;

        if (s_phase_weight[i] > ANALYZER_PHASE_WEIGHT_EPSILON)
        {
            continue;
        }

        for (left_distance = 1U;
             left_distance <= ANALYZER_DISPLAY_POINT_COUNT;
             ++left_distance)
        {
            left_index =
                (uint16_t)(
                    (i +
                     ANALYZER_DISPLAY_POINT_COUNT -
                     left_distance) %
                    ANALYZER_DISPLAY_POINT_COUNT
                );
            if (s_phase_weight[left_index] >
                ANALYZER_PHASE_WEIGHT_EPSILON)
            {
                break;
            }
        }

        for (right_distance = 1U;
             right_distance <= ANALYZER_DISPLAY_POINT_COUNT;
             ++right_distance)
        {
            right_index =
                (uint16_t)(
                    (i + right_distance) %
                    ANALYZER_DISPLAY_POINT_COUNT
                );
            if (s_phase_weight[right_index] >
                ANALYZER_PHASE_WEIGHT_EPSILON)
            {
                break;
            }
        }

        if ((left_distance > ANALYZER_DISPLAY_POINT_COUNT) ||
            (right_distance > ANALYZER_DISPLAY_POINT_COUNT))
        {
            return false;
        }

        ratio =
            (float)left_distance /
            (float)(left_distance + right_distance);
        result->waveform_mv[i] =
            result->waveform_mv[left_index] +
            ratio *
            (result->waveform_mv[right_index] -
             result->waveform_mv[left_index]);
    }

    for (i = 0U; i < ANALYZER_DISPLAY_POINT_COUNT; ++i)
    {
        mean_mv += result->waveform_mv[i];
    }
    mean_mv /= (float)ANALYZER_DISPLAY_POINT_COUNT;

    for (i = 0U; i < ANALYZER_DISPLAY_POINT_COUNT; ++i)
    {
        result->waveform_mv[i] -= mean_mv;
    }

    result->waveform_count = ANALYZER_DISPLAY_POINT_COUNT;
    return true;
}

static bool AnalyzerBridge_BuildRealWaveform(
    AnalyzerResult *result,
    const uint16_t *adc_codes,
    uint16_t sample_count,
    float volts_per_code,
    float sample_rate_hz)
{
    float samples_per_period;
    float mean_code = 0.0f;
    float fold_frequency_hz;
    float phase_cycles = 0.0f;
    float phase_step;
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

    /*
     * 旧实现只取第一个周期的Fs/f1个点并拉伸到256点。
     * 250 kHz时每周期仅4.096个采样点，无法保持复合波形形状。
     * 这里使用完整ADC缓冲区，把所有周期按基频相位折叠到256槽。
     */
    for (i = 0U; i < sample_count; ++i)
    {
        mean_code += (float)adc_codes[i];
    }
    mean_code /= (float)sample_count;

    fold_frequency_hz =
        AnalyzerBridge_RefineFundamentalForWaveform(
            result,
            adc_codes,
            sample_count,
            mean_code,
            sample_rate_hz
        );

    if (fold_frequency_hz <= 0.0f)
    {
        return false;
    }

    samples_per_period =
        sample_rate_hz / fold_frequency_hz;

    if ((samples_per_period < 2.0f) ||
        (samples_per_period > (float)sample_count))
    {
        return false;
    }

    memset(s_phase_sum_mv, 0, sizeof(s_phase_sum_mv));
    memset(s_phase_weight, 0, sizeof(s_phase_weight));
    phase_step = fold_frequency_hz / sample_rate_hz;

    for (i = 0U; i < sample_count; ++i)
    {
        float phase_position =
            phase_cycles * (float)ANALYZER_DISPLAY_POINT_COUNT;
        uint16_t index0 = (uint16_t)phase_position;
        uint16_t index1;
        float fraction;
        float sample_mv;
        float weight0;

        if (index0 >= ANALYZER_DISPLAY_POINT_COUNT)
        {
            index0 = 0U;
        }

        fraction = phase_position - (float)index0;
        index1 =
            (uint16_t)(
                (index0 + 1U) %
                ANALYZER_DISPLAY_POINT_COUNT
            );
        sample_mv =
            ((float)adc_codes[i] - mean_code) *
            volts_per_code *
            1000.0f;
        weight0 = 1.0f - fraction;

        s_phase_sum_mv[index0] += sample_mv * weight0;
        s_phase_weight[index0] += weight0;

        if (fraction > ANALYZER_PHASE_WEIGHT_EPSILON)
        {
            s_phase_sum_mv[index1] += sample_mv * fraction;
            s_phase_weight[index1] += fraction;
        }

        phase_cycles += phase_step;
        while (phase_cycles >= 1.0f)
        {
            phase_cycles -= 1.0f;
        }
    }

    /*
     * 先归一化有采样覆盖的相位槽；当频率与采样率形成较短有理比时，
     * 少数槽可能为空，随后在相邻有效槽之间做环形线性插值。
     */
    for (i = 0U; i < ANALYZER_DISPLAY_POINT_COUNT; ++i)
    {
        if (s_phase_weight[i] > ANALYZER_PHASE_WEIGHT_EPSILON)
        {
            result->waveform_mv[i] =
                s_phase_sum_mv[i] / s_phase_weight[i];
        }
        else
        {
            result->waveform_mv[i] = 0.0f;
        }
    }

    for (i = 0U; i < ANALYZER_DISPLAY_POINT_COUNT; ++i)
    {
        uint16_t left_distance;
        uint16_t right_distance;
        uint16_t left_index;
        uint16_t right_index;
        float ratio;

        if (s_phase_weight[i] > ANALYZER_PHASE_WEIGHT_EPSILON)
        {
            continue;
        }

        for (left_distance = 1U;
             left_distance <= ANALYZER_DISPLAY_POINT_COUNT;
             ++left_distance)
        {
            left_index =
                (uint16_t)(
                    (i +
                     ANALYZER_DISPLAY_POINT_COUNT -
                     left_distance) %
                    ANALYZER_DISPLAY_POINT_COUNT
                );
            if (s_phase_weight[left_index] >
                ANALYZER_PHASE_WEIGHT_EPSILON)
            {
                break;
            }
        }

        for (right_distance = 1U;
             right_distance <= ANALYZER_DISPLAY_POINT_COUNT;
             ++right_distance)
        {
            right_index =
                (uint16_t)(
                    (i + right_distance) %
                    ANALYZER_DISPLAY_POINT_COUNT
                );
            if (s_phase_weight[right_index] >
                ANALYZER_PHASE_WEIGHT_EPSILON)
            {
                break;
            }
        }

        if ((left_distance > ANALYZER_DISPLAY_POINT_COUNT) ||
            (right_distance > ANALYZER_DISPLAY_POINT_COUNT))
        {
            return false;
        }

        ratio =
            (float)left_distance /
            (float)(left_distance + right_distance);
        result->waveform_mv[i] =
            result->waveform_mv[left_index] +
            ratio *
            (result->waveform_mv[right_index] -
             result->waveform_mv[left_index]);
    }

    /*
     * ADC前端会把无直流偏置的输入信号抬到中点电压。
     * 相位折叠后再去均值，只保留题目要求显示的交流波形。
     */
    for (i = 0U; i < ANALYZER_DISPLAY_POINT_COUNT; ++i)
    {
        mean_mv += result->waveform_mv[i];
    }
    mean_mv /= (float)ANALYZER_DISPLAY_POINT_COUNT;

    for (i = 0U; i < ANALYZER_DISPLAY_POINT_COUNT; ++i)
    {
        result->waveform_mv[i] -= mean_mv;
    }

    result->waveform_count = ANALYZER_DISPLAY_POINT_COUNT;

    if ((s_waveform_fold_mode ==
         ANALYZER_WAVEFORM_FOLD_HUBER) &&
        !AnalyzerBridge_ApplyHuberSecondPass(
            result,
            adc_codes,
            sample_count,
            mean_code,
            volts_per_code,
            sample_rate_hz,
            fold_frequency_hz))
    {
        return false;
    }

    result->waveform_fold_mode = s_waveform_fold_mode;
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

static bool AnalyzerBridge_BuildTestResult(
    const GeneratedAdcTestCase *test_case,
    AnalyzerResult *result)
{
    uint8_t component;

    if ((test_case == NULL) || (result == NULL))
    {
        return false;
    }

    memset(result, 0, sizeof(*result));
    result->source = ANALYZER_SOURCE_TEST;
    result->fundamental_hz = test_case->fundamental_hz;
    result->vpp_mv = test_case->expected_vpp_mv;
    result->vrms_mv = test_case->expected_vrms_mv;
    result->component_count = test_case->component_count;
    result->test_case_number = test_case->test_number;
    result->status_flags = ANALYZER_STATUS_TEST_OVERRIDE;

    for (component = 0U;
         component < test_case->component_count;
         ++component)
    {
        result->components[component].frequency_hz =
            test_case->component_frequencies_hz[component];
        result->components[component].amplitude_mv =
            test_case->component_amplitudes_mv[component];
    }

    /*
     * 测试数据从与队友adc_b完全相同的uint16_t[2048]入口进入，
     * 与真实结果共用AnalyzerBridge_BuildRealWaveform()，不再直接
     * 在分析结果后合成256点理想波形。
     */
    if (!AnalyzerBridge_BuildRealWaveform(
            result,
            test_case->adc_codes,
            ANALYZER_TEST_SAMPLE_COUNT,
            ANALYZER_TEST_VOLTS_PER_CODE,
            ANALYZER_TEST_SAMPLE_RATE_HZ))
    {
        return false;
    }

    result->sequence = s_next_sequence++;
    result->valid = 1U;
    return true;
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
    s_waveform_fold_mode = ANALYZER_WAVEFORM_FOLD_ORDINARY;
    memset(s_latest_real_adc, 0, sizeof(s_latest_real_adc));
    s_latest_real_sample_count = 0U;
    s_latest_real_volts_per_code = 0.0f;
    s_latest_real_sample_rate_hz = 0.0f;
    s_latest_real_input_valid = false;
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
        (amplitudes_v == NULL) ||
        (sample_count > ANALYZER_MAX_SAMPLE_COUNT))
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
    memcpy(
        s_latest_real_adc,
        adc_codes,
        (size_t)sample_count * sizeof(adc_codes[0])
    );
    s_latest_real_sample_count = sample_count;
    s_latest_real_volts_per_code = volts_per_code;
    s_latest_real_sample_rate_hz = sample_rate_hz;
    s_latest_real_input_valid = true;
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

    if (AnalyzerBridge_BuildTestResult(
            &ANALYZER_TEST_CASES[selected_case],
            &s_test_result))
    {
        s_last_test_case = selected_case;
        s_test_override = true;
    }
#endif
}

void AnalyzerBridge_UseRealResult(void)
{
    if ((s_real_result.valid != 0U) &&
        (s_real_result.waveform_fold_mode !=
         s_waveform_fold_mode) &&
        s_latest_real_input_valid)
    {
        s_publish_result = s_real_result;
        if (AnalyzerBridge_BuildRealWaveform(
                &s_publish_result,
                s_latest_real_adc,
                s_latest_real_sample_count,
                s_latest_real_volts_per_code,
                s_latest_real_sample_rate_hz))
        {
            s_publish_result.sequence = s_next_sequence++;
            s_publish_result.valid = 1U;
            s_real_result = s_publish_result;
        }
    }

    s_test_override = false;
}

bool AnalyzerBridge_IsTestOverrideActive(void)
{
#if ANALYZER_TEST_ENABLE
    return s_test_override;
#else
    return false;
#endif
}

bool AnalyzerBridge_SetWaveformFoldMode(
    AnalyzerWaveformFoldMode mode)
{
    AnalyzerWaveformFoldMode previous_mode;

    if (mode >= ANALYZER_WAVEFORM_FOLD_MODE_COUNT)
    {
        return false;
    }

    if (mode == s_waveform_fold_mode)
    {
        return true;
    }

    previous_mode = s_waveform_fold_mode;
    s_waveform_fold_mode = mode;

    /*
     * 只重建当前显示源，确保切换前后比较同一帧。非活动源会在下一次
     * 正常发布或进入测试时自动使用新模式，避免一次开关动作重复执行
     * 两次完整Huber计算。
     */
    if (s_test_override)
    {
#if ANALYZER_TEST_ENABLE
        if ((s_test_result.valid != 0U) &&
            (s_last_test_case < ANALYZER_TEST_CASE_COUNT))
        {
            if (!AnalyzerBridge_BuildTestResult(
                    &ANALYZER_TEST_CASES[s_last_test_case],
                    &s_publish_result))
            {
                s_waveform_fold_mode = previous_mode;
                return false;
            }

            s_test_result = s_publish_result;
        }
#endif
    }
    else if (s_real_result.valid != 0U)
    {
        if (!s_latest_real_input_valid)
        {
            s_waveform_fold_mode = previous_mode;
            return false;
        }

        s_publish_result = s_real_result;
        if (!AnalyzerBridge_BuildRealWaveform(
                &s_publish_result,
                s_latest_real_adc,
                s_latest_real_sample_count,
                s_latest_real_volts_per_code,
                s_latest_real_sample_rate_hz))
        {
            s_waveform_fold_mode = previous_mode;
            return false;
        }

        s_publish_result.sequence = s_next_sequence++;
        s_publish_result.valid = 1U;
        s_real_result = s_publish_result;
    }

    return true;
}

AnalyzerWaveformFoldMode AnalyzerBridge_GetWaveformFoldMode(void)
{
    return s_waveform_fold_mode;
}
