#ifndef GOERTZEL_SYNC_H
#define GOERTZEL_SYNC_H
#include <stdint.h>

/* Goertzel 同步 DFT 结果 */
typedef struct {
    float real;       /* 复数谱实部 I = Re{X[f0]} */
    float imag;       /* 复数谱虚部 Q = Im{X[f0]} */
    float magnitude;  /* 该频率分量幅值 A = 2|X|/N （实正弦幅值） */
    float phase;      /* 该频率分量相位 phi (rad) = atan2(Q, I) */
} GoertzelResult;

/* 选取同步窗点数 N，使 N 点正好含 periods 个 f0 周期（消除频谱泄漏）。
   返回 N；若 f0<=0 或 periods<=0 返回 0 表示参数非法。 */
int32_t goertzel_choose_sync_N(float fs, float f0, int32_t periods);

/* Goertzel 同步 DFT：对 N 点缓冲区 x，在已知频率 f0 处求复数谱、幅值、相位。
   fs 采样率，f0 目标频率（可为任意实数，不限于 FFT 谱线），N 点数（建议为整周期）。 */
GoertzelResult goertzel_sync(const float *x, int32_t N, float fs, float f0);

#endif /* GOERTZEL_SYNC_H */
