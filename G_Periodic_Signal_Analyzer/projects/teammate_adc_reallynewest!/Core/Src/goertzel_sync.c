/*
 * Goertzel 同步 DFT
 * 用途：FFT 先估出信号频率 f0（误差<=10Hz），再用本模块在 f0 上做
 *       “单谱线 DFT”，并把分析窗取成 f0 的整数周期 -> 无频谱泄漏地
 *       提取该频率的幅值与相位。
 *
 * 原理：
 *   递推   s[n] = x[n] + 2cos(w0)*s[n-1] - s[n-2] ,  w0 = 2*pi*f0/fs
 *   馈前   X = s[N-1] - e^{-j w0} * s[N-2]
 *         I = Re(X) = s[N-1] - cos(w0)*s[N-2]
 *         Q = Im(X) = sin(w0)*s[N-2]
 *   幅值   A = 2*|X|/N   （输入为实正弦 A*cos(w0*n+phi) 时）
 *   相位   phi = atan2(Q, I)
 *
 * 每样本仅 1 次乘法 + 2 次加法；w0 可为任意实数，故可精确对准 FFT 估出的频率。
 */
#include "goertzel_sync.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

int32_t goertzel_choose_sync_N(float fs, float f0, int32_t periods)
{
    if (fs <= 0.0f || f0 <= 0.0f || periods <= 0)
        return 0;
    /* P 个 f0 周期对应的点数：N*Ts = P/f0  =>  N = P*fs/f0 */
    float Nf = (float)periods * fs / f0;
    return (int32_t)lroundf(Nf);   /* 四舍五入到整数周期 */
}

GoertzelResult goertzel_sync(const float *x, int32_t N, float fs, float f0)
{
    GoertzelResult r = {0, 0, 0, 0};
    if (N <= 0 || fs <= 0.0f || f0 <= 0.0f || x == 0)
        return r;

    float w0    = 2.0f * M_PI * f0 / fs;   /* w0 = 2*pi*f0/fs （可为任意实数） */
    float coeff = 2.0f * cosf(w0);         /* 2cos(w0) */
    float s1 = 0.0f, s2 = 0.0f;            /* s[n-1], s[n-2] */

    for (int32_t n = 0; n < N; ++n) {
        float s = x[n] + coeff * s1 - s2;  /* s[n] = x[n] + 2cos(w0)*s[n-1] - s[n-2] */
        s2 = s1;
        s1 = s;
    }

    /* 馈前：N 步后算一次，得到复数谱 X[f0] */
    r.real = s1 - cosf(w0) * s2;           /* I */
    r.imag = sinf(w0) * s2;                /* Q */
    float mag = sqrtf(r.real * r.real + r.imag * r.imag);
    r.magnitude = 2.0f * mag / (float)N;   /* 实正弦幅值 */
    r.phase     = atan2f(r.imag, r.real);  /* 相位 (rad) */
    return r;
}

/* ---- 用法示例（假设在别处调用）----
   float fs = 48000.0f;                 // 采样率
   float f0 = f0_from_fft;              // FFT 估出的频率，误差<=10Hz
   int   P  = 100;                      // 取 100 个周期，越大越准、越慢
   int   N  = goertzel_choose_sync_N(fs, f0, P);   // 同步窗点数
   // ...采集 N 点到 buf[]...
   GoertzelResult g = goertzel_sync(buf, N, fs, f0);
   // g.magnitude = 该频率分量幅值, g.phase = 相位
   // 频率细测：前后两段求相位差 dphi、时间差 dt -> f = f0 + dphi/(2*pi*dt)
*/
