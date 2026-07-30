#ifndef INC_ANALYZER_BRIDGE_H_
#define INC_ANALYZER_BRIDGE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32g4xx_hal.h"

#include <stdbool.h>
#include <stdint.h>

#ifndef ANALYZER_TEST_ENABLE
#define ANALYZER_TEST_ENABLE          1U
#endif

#define ANALYZER_DISPLAY_POINT_COUNT  256U
#define ANALYZER_MAX_COMPONENTS       3U

typedef enum
{
    ANALYZER_SOURCE_REAL = 0,
    ANALYZER_SOURCE_TEST
} AnalyzerSource;

typedef struct
{
    float frequency_hz;
    float amplitude_mv;
} AnalyzerComponent;

typedef struct
{
    uint8_t valid;
    uint32_t sequence;
    AnalyzerSource source;

    float fundamental_hz;
    float vpp_mv;
    float vrms_mv;

    uint8_t component_count;
    AnalyzerComponent components[ANALYZER_MAX_COMPONENTS];

    uint16_t waveform_count;
    float waveform_mv[ANALYZER_DISPLAY_POINT_COUNT];

    /*
     * 测试模式下为1~GENERATED_ADC_TEST_CASE_COUNT，真实ADC模式为0。
     * 仅用于屏幕标识当前原生ADC测试向量，不参与任何测量计算。
     */
    uint8_t test_case_number;
    uint32_t status_flags;
} AnalyzerResult;

/**
 * @brief 初始化统一分析结果桥接层。
 */
void AnalyzerBridge_Init(void);

/**
 * @brief 发布一次队友算法已经完成的真实分析结果。
 *
 * @param adc_codes 原始ADC采样码。桥接层使用完整缓冲区按基频相位折叠，
 *                  生成一个周期的256点显示快照。
 * @param sample_count 原始采样点数。
 * @param volts_per_code 每个ADC码对应的电压，单位V。
 * @param sample_rate_hz 实际或当前算法使用的采样率，单位Hz。
 * @param vpp_v 队友算法计算的峰峰值，单位V。
 * @param vrms_v 队友算法计算的真有效值，单位V。
 * @param teammate_flag 队友最新版flag，当前2/3分别表示2/3个谱峰。
 * @param frequencies_hz 三个谱峰频率，单位Hz。
 * @param amplitudes_v 三个谱峰峰值幅度，单位V。
 *
 * 频谱结果会在桥接层按频率升序排列并统一转换为峰值mV；
 * 显示层不再直接读取队友的散乱全局变量。
 */
void AnalyzerBridge_PublishReal(
    const uint16_t *adc_codes,
    uint16_t sample_count,
    float volts_per_code,
    float sample_rate_hz,
    float vpp_v,
    float vrms_v,
    uint8_t teammate_flag,
    const float frequencies_hz[ANALYZER_MAX_COMPONENTS],
    const float amplitudes_v[ANALYZER_MAX_COMPONENTS]);

/**
 * @brief 获取当前显示应使用的最新稳定结果快照。
 * @return true表示返回了有效结果；false表示当前尚无有效结果。
 */
bool AnalyzerBridge_GetLatest(AnalyzerResult *result);

/**
 * @brief 随机选择一组完整测试场景并锁存为当前显示结果。
 *
 * 测试场景使用Python预生成的原生uint16_t[2048] ADC数组，并与真实输入
 * 共用波形提取链路；Vpp、RMS和谱峰元数据仍使用每个场景的理想期望值，
 * 因此本接口不重复验证队友的FFT和参数测量算法。
 */
void AnalyzerBridge_RunRandomTest(void);

/**
 * @brief 取消测试结果覆盖，恢复读取最新真实分析结果。
 *
 * 刷新按钮进入真实自动刷新模式时调用。真实结果在测试覆盖期间仍会持续发布，
 * 因此取消覆盖后无需重新计算即可读取最近一次有效真实快照。
 */
void AnalyzerBridge_UseRealResult(void);

/**
 * @brief 查询当前显示是否被测试结果覆盖。
 */
bool AnalyzerBridge_IsTestOverrideActive(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_ANALYZER_BRIDGE_H_ */
