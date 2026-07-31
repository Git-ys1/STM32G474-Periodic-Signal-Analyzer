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

/*
 * 0：测试按钮使用原T1~T9固定回归集。
 * 1：测试按钮使用自定义波形实验室导出的generated_custom_adc_tests.h。
 *
 * 自定义工具会直接生成2048点uint16_t数组和用户填写的1~255组号，
 * 不需要手工粘贴数组。正式比赛可同时将ANALYZER_TEST_ENABLE设为0。
 */
#ifndef ANALYZER_CUSTOM_TEST_ENABLE
#define ANALYZER_CUSTOM_TEST_ENABLE   1U
#endif

#define ANALYZER_DISPLAY_POINT_COUNT  256U
#define ANALYZER_MAX_COMPONENTS       3U

/*
 * ADC引脚前的模拟链路总电压增益。真实输入的模型Vpp必须折算回信号源端，
 * 因此默认除以6；若队友后续重新标定，可在工程宏定义中覆盖本值。
 */
#ifndef ANALYZER_FRONTEND_VOLTAGE_GAIN
#define ANALYZER_FRONTEND_VOLTAGE_GAIN 6.0f
#endif

typedef enum
{
    ANALYZER_SOURCE_REAL = 0,
    ANALYZER_SOURCE_TEST
} AnalyzerSource;

/**
 * @brief 时域波形相位折叠算法。
 *
 * 普通模式保持V2.0.0现有行为；Huber模式执行两遍稳健相位折叠，
 * 再把显示波形投影到FFT已识别的整数谐波子空间。两种模式都不修改
 * 队友测得的Vpp、RMS、频率和频谱分量。
 */
typedef enum
{
    ANALYZER_WAVEFORM_FOLD_ORDINARY = 0,
    ANALYZER_WAVEFORM_FOLD_HUBER,
    ANALYZER_WAVEFORM_FOLD_MODE_COUNT
} AnalyzerWaveformFoldMode;

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
    float model_vpp_mv;
    float vrms_mv;
    uint8_t model_vpp_valid;

    uint8_t component_count;
    AnalyzerComponent components[ANALYZER_MAX_COMPONENTS];

    uint16_t waveform_count;
    AnalyzerWaveformFoldMode waveform_fold_mode;
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
 * @brief 直接用全部VO浮点电压样本做Huber谐波模型拟合并计算输入端峰峰值。
 *
 * 模型为DC项加FFT已识别整数次谐波的正弦/余弦项。函数先做普通最小二乘，
 * 再做三轮Huber IRLS；最终在4096个等相位点上合成模型并求最大值减最小值。
 * 它不读取256槽显示波形，也不修改队友的Vpp、RMS或FFT结果。
 *
 * @param frontend_gain ADC引脚电压相对信号源输入的增益；当前真实前端传6.0。
 * @param model_vpp_mv 返回折算到信号源输入端的模型峰峰值，单位mV。
 */
bool AnalyzerBridge_CalculateRobustModelVpp(
    const float *samples_v,
    uint16_t sample_count,
    float sample_rate_hz,
    float refined_fundamental_hz,
    float reported_fundamental_hz,
    const AnalyzerComponent components[ANALYZER_MAX_COMPONENTS],
    uint8_t component_count,
    float frontend_gain,
    float *model_vpp_mv);

/**
 * @brief 在队友Vpp_R()改写VO之前，准备一次真实波形结果。
 *
 * @param samples_v 队友已交错并换算为电压的VO浮点数组，单位V。桥接层不再
 *                  读取原始ADC码，也不再自行执行3.3/4096换算。
 * @param sample_count VO采样点数，当前为4096。
 * @param sample_rate_hz 实际或当前算法使用的采样率，单位Hz。
 * @param teammate_flag 队友最新版flag，当前2/3分别表示2/3个谱峰。
 * @param frequencies_hz 三个谱峰频率，单位Hz。
 * @param amplitudes_v 三个谱峰峰值幅度，单位V。
 *
 * 频谱结果会在桥接层按频率升序排列并统一转换为峰值mV；
 * 显示层不再直接读取队友的散乱全局变量。
 */
void AnalyzerBridge_PrepareReal(
    const float *samples_v,
    uint16_t sample_count,
    float sample_rate_hz,
    uint8_t teammate_flag,
    const float frequencies_hz[ANALYZER_MAX_COMPONENTS],
    const float amplitudes_v[ANALYZER_MAX_COMPONENTS]);

/**
 * @brief 在队友Vpp_R()完成后，把Vpp/RMS写入已准备结果并原子发布。
 *
 * 采用“先准备VO波形、后发布测量值”两阶段接口，是因为队友Vpp_R()会原地
 * 修改VO。显示层只会看到完整提交后的AnalyzerResult，不会读到半成品。
 */
void AnalyzerBridge_PublishPreparedReal(float vpp_v,
                                        float vrms_v);

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

/**
 * @brief 切换时域波形折叠算法。
 *
 * 真实VO不在桥接层重复缓存，真实模式会在下一帧到达时用新模式重建；测试
 * 数据仍可立即重建。这样既避免复制4096个float，也避免重新读取原始ADC码。
 */
bool AnalyzerBridge_SetWaveformFoldMode(
    AnalyzerWaveformFoldMode mode);

/**
 * @brief 获取当前时域波形折叠算法。
 */
AnalyzerWaveformFoldMode AnalyzerBridge_GetWaveformFoldMode(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_ANALYZER_BRIDGE_H_ */
