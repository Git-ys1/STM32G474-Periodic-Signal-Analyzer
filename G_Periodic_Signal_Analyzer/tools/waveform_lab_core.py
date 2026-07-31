#!/usr/bin/env python3
"""
Core mathematics and deterministic export helpers for the custom waveform lab.

The module mirrors the firmware display path:

    harmonic model
    -> uint16_t[2048] ADC codes
    -> correlation frequency refinement
    -> 256-bin phase folding
    -> 794-column periodic linear interpolation

It intentionally does not import tkinter or matplotlib so it can also be used by
command-line regression scripts.
"""

from __future__ import annotations

import csv
import json
import math
import re
from dataclasses import asdict, dataclass
from fractions import Fraction
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

import numpy as np


REPO_ROOT = Path(__file__).resolve().parents[1]
FIRMWARE_PROJECT_ROOT = (
    REPO_ROOT / "projects" / "g474_full_integration_test"
)
CUSTOM_DATA_ROOT = REPO_ROOT / "tests" / "custom_waveforms"
CUSTOM_HEADER_PATH = (
    FIRMWARE_PROJECT_ROOT
    / "Core"
    / "Inc"
    / "generated_custom_adc_tests.h"
)
REGISTRY_PATH = CUSTOM_DATA_ROOT / "registry.json"

DEFAULT_SAMPLE_COUNT = 2048
DEFAULT_SAMPLE_RATE_HZ = 1_024_000.0
DEFAULT_ADC_VREF_V = 3.3
DEFAULT_ADC_BIAS_V = 1.65
DEFAULT_PHASE_BIN_COUNT = 256
DEFAULT_DISPLAY_WIDTH = 794
DEFAULT_DISPLAY_PERIODS = 1
MAX_SIGNAL_FREQUENCY_HZ = 500_000.0
FREQUENCY_SEARCH_HALF_HZ = 500.0
FREQUENCY_SEARCH_STEP_HZ = 10.0


@dataclass(frozen=True)
class ToneSpec:
    """One base-frequency harmonic component."""

    order: int
    amplitude_mvpk: float
    relative_phase_deg: float = 0.0


@dataclass(frozen=True)
class WaveformSpec:
    """A reproducible waveform group and acquisition configuration."""

    group_number: int
    name: str
    requirement: int
    fundamental_hz: float
    tones: Tuple[ToneSpec, ...]
    sample_rate_hz: float = DEFAULT_SAMPLE_RATE_HZ
    sample_count: int = DEFAULT_SAMPLE_COUNT
    adc_vref_v: float = DEFAULT_ADC_VREF_V
    adc_bias_v: float = DEFAULT_ADC_BIAS_V
    start_phase_deg: float = 0.0
    noise_rms_mv: float = 0.0
    random_seed: int = 1


@dataclass(frozen=True)
class CoverageMetrics:
    """Finite-record phase coverage before any missing-bin interpolation."""

    unique_phase_count: int
    occupied_hard_bins: int
    occupied_weighted_bins: int
    hard_bin_coverage: float
    weighted_bin_coverage: float
    minimum_hard_bin_hits: int
    maximum_hard_bin_hits: int
    maximum_phase_gap_cycles: float
    maximum_phase_gap_bins: float
    rational_numerator: int
    rational_denominator: int


@dataclass(frozen=True)
class HuberFoldDiagnostics:
    """Robust-scale and sample-weight diagnostics for a Huber fold."""

    robust_scale_mv: float
    huber_delta_mv: float
    downweighted_sample_count: int
    downweighted_fraction: float
    minimum_sample_weight: float


@dataclass(frozen=True)
class HarmonicFitMetrics:
    """Least-squares harmonic-model reconstruction quality."""

    rank: int
    parameter_count: int
    condition_number: float
    sample_rmse_mv: float
    display_rmse_mv: float
    display_max_error_mv: float


@dataclass
class SimulationResult:
    """All data needed by the GUI, records, plots and C export."""

    spec: WaveformSpec
    sample_time_s: np.ndarray
    sample_signal_mv: np.ndarray
    adc_codes: np.ndarray
    adc_signal_mv: np.ndarray
    refined_fundamental_hz: float
    phase_bin_count: int
    phase_cycles: np.ndarray
    phase_weights: np.ndarray
    phase_waveform_mv: np.ndarray
    ideal_phase_waveform_mv: np.ndarray
    display_waveform_mv: np.ndarray
    ideal_display_waveform_mv: np.ndarray
    harmonic_fit_display_waveform_mv: np.ndarray
    harmonic_fit: HarmonicFitMetrics
    coverage: CoverageMetrics
    expected_vpp_mv: float
    expected_vrms_mv: float
    phase_fold_rmse_mv: float
    phase_fold_max_error_mv: float
    display_rmse_mv: float
    display_max_error_mv: float


def validate_spec(spec: WaveformSpec) -> None:
    """Raise ValueError when a group cannot represent a valid ADC test."""

    if not 1 <= spec.group_number <= 255:
        raise ValueError("组号必须位于1~255，以便写入uint8_t测试编号。")
    if not spec.name.strip():
        raise ValueError("组名不能为空。")
    if spec.requirement not in (1, 2, 3):
        raise ValueError("题目分组只能是1、2或3。")
    if spec.fundamental_hz <= 0.0:
        raise ValueError("基频必须大于0 Hz。")
    if spec.sample_rate_hz <= 0.0:
        raise ValueError("采样率必须大于0 Hz。")
    if spec.sample_count != DEFAULT_SAMPLE_COUNT:
        raise ValueError("当前固件接口固定要求2048个ADC点。")
    if spec.adc_vref_v <= 0.0:
        raise ValueError("ADC参考电压必须大于0 V。")
    if not 0.0 < spec.adc_bias_v < spec.adc_vref_v:
        raise ValueError("ADC偏置必须位于0 V和参考电压之间。")
    if spec.noise_rms_mv < 0.0:
        raise ValueError("噪声RMS不能为负数。")
    if not 1 <= len(spec.tones) <= 3:
        raise ValueError("必须包含基波，可再加入1个或2个谐波。")

    orders = [tone.order for tone in spec.tones]
    if orders[0] != 1 or 1 not in orders:
        raise ValueError("第一项必须是1次基波。")
    if len(set(orders)) != len(orders):
        raise ValueError("谐波次数不能重复。")

    for tone in spec.tones:
        if tone.order < 1:
            raise ValueError("谐波次数必须为正整数。")
        if tone.amplitude_mvpk < 0.0:
            raise ValueError("各分量峰值幅度不能为负。")
        if (
            spec.fundamental_hz * tone.order
            > MAX_SIGNAL_FREQUENCY_HZ + 1.0e-6
        ):
            raise ValueError(
                "{}次分量频率{:.1f} kHz超过G题500 kHz上限。".format(
                    tone.order,
                    spec.fundamental_hz * tone.order / 1000.0,
                )
            )


def _component_phase_rad(
    tone: ToneSpec,
    start_phase_deg: float,
) -> float:
    """
    Return the phase at ADC sample n=0.

    A global time offset rotates harmonic h by h times the base phase.  The
    optional relative phase is an additional stress-test phase.
    """

    return math.radians(
        tone.order * start_phase_deg + tone.relative_phase_deg
    )


def synthesize_at_times_mv(
    spec: WaveformSpec,
    time_s: np.ndarray,
) -> np.ndarray:
    """Evaluate the continuous harmonic model at arbitrary times."""

    signal_mv = np.zeros_like(time_s, dtype=np.float64)
    for tone in spec.tones:
        signal_mv += tone.amplitude_mvpk * np.sin(
            2.0
            * math.pi
            * tone.order
            * spec.fundamental_hz
            * time_s
            + _component_phase_rad(tone, spec.start_phase_deg)
        )
    return signal_mv


def synthesize_at_base_phase_mv(
    spec: WaveformSpec,
    phase_cycles: np.ndarray,
) -> np.ndarray:
    """Evaluate the waveform against normalized base-cycle phase."""

    signal_mv = np.zeros_like(phase_cycles, dtype=np.float64)
    for tone in spec.tones:
        signal_mv += tone.amplitude_mvpk * np.sin(
            2.0 * math.pi * tone.order * phase_cycles
            + _component_phase_rad(tone, spec.start_phase_deg)
        )
    return signal_mv


def generate_adc_codes(
    spec: WaveformSpec,
) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Generate deterministic time, analog signal and quantized ADC arrays."""

    validate_spec(spec)
    sample_time_s = (
        np.arange(spec.sample_count, dtype=np.float64)
        / spec.sample_rate_hz
    )
    sample_signal_mv = synthesize_at_times_mv(spec, sample_time_s)

    if spec.noise_rms_mv > 0.0:
        rng = np.random.RandomState(spec.random_seed)
        sample_signal_mv = sample_signal_mv + rng.normal(
            loc=0.0,
            scale=spec.noise_rms_mv,
            size=spec.sample_count,
        )

    adc_voltage_v = spec.adc_bias_v + sample_signal_mv / 1000.0
    if adc_voltage_v.min() < 0.0 or adc_voltage_v.max() > spec.adc_vref_v:
        raise ValueError(
            "波形加偏置后超出ADC范围：{:.4f}~{:.4f} V。".format(
                float(adc_voltage_v.min()),
                float(adc_voltage_v.max()),
            )
        )

    volts_per_code = spec.adc_vref_v / 4096.0
    adc_codes = np.rint(adc_voltage_v / volts_per_code)
    adc_codes = np.clip(adc_codes, 0, 4095).astype(np.uint16)
    adc_signal_mv = (
        adc_codes.astype(np.float64) - float(np.mean(adc_codes))
    ) * volts_per_code * 1000.0
    return sample_time_s, sample_signal_mv, adc_codes, adc_signal_mv


def _correlation_scores(
    centered_codes: np.ndarray,
    sample_rate_hz: float,
    frequencies_hz: np.ndarray,
) -> np.ndarray:
    """Vectorized equivalent of the firmware single-frequency correlation."""

    sample_indices = np.arange(centered_codes.size, dtype=np.float64)
    angles = (
        -2.0
        * math.pi
        * frequencies_hz[:, None]
        * sample_indices[None, :]
        / sample_rate_hz
    )
    correlations = np.exp(1j * angles).dot(centered_codes)
    return np.square(correlations.real) + np.square(correlations.imag)


def refine_fundamental_hz(
    spec: WaveformSpec,
    adc_codes: np.ndarray,
) -> float:
    """Mirror AnalyzerBridge's +/-500 Hz, 10 Hz correlation refinement."""

    reference_tone = max(
        spec.tones,
        key=lambda tone: tone.amplitude_mvpk * tone.order,
    )
    candidates = np.arange(
        spec.fundamental_hz - FREQUENCY_SEARCH_HALF_HZ,
        spec.fundamental_hz
        + FREQUENCY_SEARCH_HALF_HZ
        + FREQUENCY_SEARCH_STEP_HZ * 0.5,
        FREQUENCY_SEARCH_STEP_HZ,
        dtype=np.float64,
    )
    reference_frequencies = candidates * reference_tone.order
    valid = (
        (reference_frequencies > 0.0)
        & (reference_frequencies < spec.sample_rate_hz * 0.5)
    )
    scores = np.full(candidates.shape, -1.0, dtype=np.float64)
    centered_codes = (
        adc_codes.astype(np.float64) - float(np.mean(adc_codes))
    )
    scores[valid] = _correlation_scores(
        centered_codes,
        spec.sample_rate_hz,
        reference_frequencies[valid],
    )
    best_index = int(np.argmax(scores))
    fractional_offset = 0.0

    if 0 < best_index < scores.size - 1:
        left = float(scores[best_index - 1])
        center = float(scores[best_index])
        right = float(scores[best_index + 1])
        denominator = left - 2.0 * center + right
        if abs(denominator) > 1.0e-6:
            fractional_offset = 0.5 * (left - right) / denominator
            fractional_offset = max(-1.0, min(1.0, fractional_offset))

    return float(
        candidates[best_index]
        + fractional_offset * FREQUENCY_SEARCH_STEP_HZ
    )


def phase_coverage(
    fundamental_hz: float,
    sample_rate_hz: float = DEFAULT_SAMPLE_RATE_HZ,
    sample_count: int = DEFAULT_SAMPLE_COUNT,
    phase_bin_count: int = DEFAULT_PHASE_BIN_COUNT,
    start_phase_deg: float = 0.0,
) -> CoverageMetrics:
    """Measure unique phases, occupied bins and the largest circular gap."""

    sample_indices = np.arange(sample_count, dtype=np.float64)
    start_cycles = start_phase_deg / 360.0
    phases = np.mod(
        sample_indices * fundamental_hz / sample_rate_hz + start_cycles,
        1.0,
    )

    # 浮点计算的1/3、1/7等比例可能把理论0相位表示成
    # 0.999999999999...。先四舍五入再取模，避免把周期端点误算成
    # 一个额外的独立相位。
    rounded_phases = np.mod(np.round(phases, 12), 1.0)
    unique_phase_count = int(np.unique(rounded_phases).size)

    # “硬分箱”只把每个真实ADC样本记到最近的一个槽中。它专门用于
    # 衡量真实相位覆盖，不能复用后续左右分权重的折叠结果，否则覆盖率
    # 会被一个样本同时点亮两个槽而虚高。
    hard_indices = np.floor(
        phases * phase_bin_count + 0.5
    ).astype(np.int64)
    hard_indices = np.mod(hard_indices, phase_bin_count)
    hard_hit_counts = np.bincount(
        hard_indices,
        minlength=phase_bin_count,
    )
    occupied_hit_counts = hard_hit_counts[hard_hit_counts > 0]
    occupied_hard_bins = int(occupied_hit_counts.size)
    minimum_hard_bin_hits = int(np.min(occupied_hit_counts))
    maximum_hard_bin_hits = int(np.max(occupied_hit_counts))

    positions = phases * phase_bin_count
    index0 = np.floor(positions).astype(np.int64) % phase_bin_count
    fractions = positions - np.floor(positions)
    weights = np.zeros(phase_bin_count, dtype=np.float64)
    np.add.at(weights, index0, 1.0 - fractions)
    index1 = (index0 + 1) % phase_bin_count
    nonzero = fractions > 1.0e-6
    np.add.at(weights, index1[nonzero], fractions[nonzero])
    occupied_weighted_bins = int(np.count_nonzero(weights > 1.0e-6))

    unique_phases = np.sort(np.unique(rounded_phases))
    if unique_phases.size <= 1:
        maximum_gap = 1.0
    else:
        circular = np.concatenate(
            (unique_phases, np.array([unique_phases[0] + 1.0]))
        )
        maximum_gap = float(np.max(np.diff(circular)))

    ratio = Fraction(
        int(round(fundamental_hz * 1000.0)),
        int(round(sample_rate_hz * 1000.0)),
    ).limit_denominator(max(sample_count * 16, 65536))

    return CoverageMetrics(
        unique_phase_count=unique_phase_count,
        occupied_hard_bins=occupied_hard_bins,
        occupied_weighted_bins=occupied_weighted_bins,
        hard_bin_coverage=occupied_hard_bins / phase_bin_count,
        weighted_bin_coverage=occupied_weighted_bins / phase_bin_count,
        minimum_hard_bin_hits=minimum_hard_bin_hits,
        maximum_hard_bin_hits=maximum_hard_bin_hits,
        maximum_phase_gap_cycles=maximum_gap,
        maximum_phase_gap_bins=maximum_gap * phase_bin_count,
        rational_numerator=ratio.numerator,
        rational_denominator=ratio.denominator,
    )


def phase_fold_waveform(
    adc_codes: np.ndarray,
    volts_per_code: float,
    sample_rate_hz: float,
    fold_frequency_hz: float,
    phase_bin_count: int = DEFAULT_PHASE_BIN_COUNT,
) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Mirror the firmware's weighted folding and circular gap filling."""

    sample_indices = np.arange(adc_codes.size, dtype=np.float64)
    phases = np.mod(
        sample_indices * fold_frequency_hz / sample_rate_hz,
        1.0,
    )
    positions = phases * phase_bin_count
    index0 = np.floor(positions).astype(np.int64) % phase_bin_count
    fractions = positions - np.floor(positions)
    index1 = (index0 + 1) % phase_bin_count

    centered_mv = (
        adc_codes.astype(np.float64) - float(np.mean(adc_codes))
    ) * volts_per_code * 1000.0
    sums = np.zeros(phase_bin_count, dtype=np.float64)
    weights = np.zeros(phase_bin_count, dtype=np.float64)

    np.add.at(sums, index0, centered_mv * (1.0 - fractions))
    np.add.at(weights, index0, 1.0 - fractions)
    nonzero = fractions > 1.0e-6
    np.add.at(
        sums,
        index1[nonzero],
        centered_mv[nonzero] * fractions[nonzero],
    )
    np.add.at(weights, index1[nonzero], fractions[nonzero])

    waveform = np.full(phase_bin_count, np.nan, dtype=np.float64)
    valid = weights > 1.0e-6
    waveform[valid] = sums[valid] / weights[valid]
    valid_indices = np.flatnonzero(valid)
    if valid_indices.size == 0:
        return np.zeros(phase_bin_count), weights, phases

    # np.interp supports periodic circular interpolation directly.
    missing_indices = np.flatnonzero(~valid)
    if missing_indices.size:
        waveform[missing_indices] = np.interp(
            missing_indices.astype(np.float64),
            valid_indices.astype(np.float64),
            waveform[valid_indices],
            period=phase_bin_count,
        )

    waveform -= float(np.mean(waveform))
    return waveform, weights, phases


def huber_phase_fold_waveform(
    adc_codes: np.ndarray,
    volts_per_code: float,
    sample_rate_hz: float,
    fold_frequency_hz: float,
    phase_bin_count: int = DEFAULT_PHASE_BIN_COUNT,
    huber_k: float = 1.345,
    minimum_delta_lsb: float = 1.5,
) -> Tuple[
    np.ndarray,
    np.ndarray,
    np.ndarray,
    HuberFoldDiagnostics,
]:
    """
    Fold ADC samples twice, downweighting first-pass residual outliers.

    The first pass is the unchanged ordinary phase fold.  Its periodic linear
    prediction is used only to calculate Huber sample weights; the second pass
    always folds the original centered ADC samples.
    """

    if volts_per_code <= 0.0:
        raise ValueError("ADC每码电压必须大于0。")
    if adc_codes.size == 0:
        raise ValueError("ADC数组不能为空。")
    if sample_rate_hz <= 0.0:
        raise ValueError("采样率必须大于0。")
    if fold_frequency_hz <= 0.0:
        raise ValueError("折叠频率必须大于0。")
    if phase_bin_count < 1:
        raise ValueError("相位槽数必须大于0。")
    if huber_k <= 0.0:
        raise ValueError("Huber系数必须大于0。")
    if minimum_delta_lsb <= 0.0:
        raise ValueError("Huber阈值下限必须大于0 LSB。")

    initial_waveform, _, phases = phase_fold_waveform(
        adc_codes,
        volts_per_code,
        sample_rate_hz,
        fold_frequency_hz,
        phase_bin_count,
    )

    positions = phases * phase_bin_count
    integer_positions = np.floor(positions)
    index0 = integer_positions.astype(np.int64) % phase_bin_count
    fractions = positions - integer_positions
    index1 = (index0 + 1) % phase_bin_count
    predicted_mv = (
        initial_waveform[index0]
        + fractions
        * (initial_waveform[index1] - initial_waveform[index0])
    )

    centered_mv = (
        adc_codes.astype(np.float64) - float(np.mean(adc_codes))
    ) * volts_per_code * 1000.0
    residual = centered_mv - predicted_mv
    residual_center = float(np.median(residual))
    centered_residual = residual - residual_center
    absolute_deviation = np.abs(centered_residual)
    robust_scale_mv = 1.4826 * float(np.median(absolute_deviation))
    adc_lsb_mv = volts_per_code * 1000.0
    delta_mv = max(
        huber_k * robust_scale_mv,
        minimum_delta_lsb * adc_lsb_mv,
    )

    sample_weights = np.ones_like(residual)
    large = absolute_deviation > delta_mv
    sample_weights[large] = delta_mv / absolute_deviation[large]

    sums = np.zeros(phase_bin_count, dtype=np.float64)
    weights = np.zeros(phase_bin_count, dtype=np.float64)
    left_weights = (1.0 - fractions) * sample_weights
    right_weights = fractions * sample_weights
    np.add.at(sums, index0, centered_mv * left_weights)
    np.add.at(weights, index0, left_weights)
    nonzero = fractions > 1.0e-6
    np.add.at(
        sums,
        index1[nonzero],
        centered_mv[nonzero] * right_weights[nonzero],
    )
    np.add.at(weights, index1[nonzero], right_weights[nonzero])

    waveform = np.full(phase_bin_count, np.nan, dtype=np.float64)
    valid = weights > 1.0e-6
    waveform[valid] = sums[valid] / weights[valid]
    valid_indices = np.flatnonzero(valid)
    if valid_indices.size == 0:
        waveform = np.zeros(phase_bin_count, dtype=np.float64)
    else:
        missing_indices = np.flatnonzero(~valid)
        if missing_indices.size:
            waveform[missing_indices] = np.interp(
                missing_indices.astype(np.float64),
                valid_indices.astype(np.float64),
                waveform[valid_indices],
                period=phase_bin_count,
            )
        waveform -= float(np.mean(waveform))

    downweighted_count = int(np.count_nonzero(large))
    diagnostics = HuberFoldDiagnostics(
        robust_scale_mv=robust_scale_mv,
        huber_delta_mv=delta_mv,
        downweighted_sample_count=downweighted_count,
        downweighted_fraction=(
            downweighted_count / adc_codes.size
            if adc_codes.size
            else 0.0
        ),
        minimum_sample_weight=(
            float(np.min(sample_weights))
            if sample_weights.size
            else 1.0
        ),
    )
    return waveform, weights, phases, diagnostics


def periodic_linear_display(
    waveform_mv: np.ndarray,
    periods: int,
    display_width: int = DEFAULT_DISPLAY_WIDTH,
) -> np.ndarray:
    """Mirror Display_BuildTimeCurve's periodic linear interpolation."""

    if periods not in (1, 2, 3):
        raise ValueError("显示周期数必须是1、2或3。")
    output_indices = np.arange(display_width, dtype=np.float64)
    positions = (
        output_indices
        * periods
        * waveform_mv.size
        / (display_width - 1)
    )
    integer_positions = np.floor(positions).astype(np.int64)
    fractions = positions - integer_positions
    index0 = integer_positions % waveform_mv.size
    index1 = (index0 + 1) % waveform_mv.size
    return (
        waveform_mv[index0]
        + fractions * (waveform_mv[index1] - waveform_mv[index0])
    )


def harmonic_least_squares_fit(
    spec: WaveformSpec,
    adc_codes: np.ndarray,
    fundamental_hz: float,
    *,
    display_periods: int = DEFAULT_DISPLAY_PERIODS,
    display_width: int = DEFAULT_DISPLAY_WIDTH,
) -> Tuple[np.ndarray, np.ndarray, HarmonicFitMetrics]:
    """
    Reconstruct a known harmonic-order model directly from ADC samples.

    The fit estimates one DC term plus sine/cosine coefficients for every
    harmonic order present in ``spec``.  It does not use the configured
    amplitudes or phases as answers; those are only used to calculate the
    independent ideal-error metric after fitting.
    """

    if fundamental_hz <= 0.0:
        raise ValueError("拟合基频必须大于0 Hz。")
    if display_periods not in (1, 2, 3):
        raise ValueError("显示周期数必须是1、2或3。")

    sample_indices = np.arange(adc_codes.size, dtype=np.float64)
    sample_time_s = sample_indices / spec.sample_rate_hz
    volts_per_code = spec.adc_vref_v / 4096.0
    centered_mv = (
        adc_codes.astype(np.float64) - float(np.mean(adc_codes))
    ) * volts_per_code * 1000.0

    sample_columns = [np.ones(adc_codes.size, dtype=np.float64)]
    for tone in spec.tones:
        angle = (
            2.0
            * math.pi
            * tone.order
            * fundamental_hz
            * sample_time_s
        )
        sample_columns.extend((np.sin(angle), np.cos(angle)))
    sample_matrix = np.column_stack(sample_columns)

    coefficients, _, rank, singular_values = np.linalg.lstsq(
        sample_matrix,
        centered_mv,
        rcond=None,
    )
    fitted_samples = sample_matrix @ coefficients

    display_phase = (
        np.arange(display_width, dtype=np.float64)
        * display_periods
        / (display_width - 1)
    )
    display_columns = [np.ones(display_width, dtype=np.float64)]
    for tone in spec.tones:
        angle = 2.0 * math.pi * tone.order * display_phase
        display_columns.extend((np.sin(angle), np.cos(angle)))
    display_matrix = np.column_stack(display_columns)
    fitted_display = display_matrix @ coefficients
    fitted_display -= float(np.mean(fitted_display))

    ideal_display = synthesize_at_base_phase_mv(spec, display_phase)
    ideal_display -= float(np.mean(ideal_display))

    if singular_values.size == 0 or singular_values[-1] <= 0.0:
        condition_number = math.inf
    else:
        condition_number = float(
            singular_values[0] / singular_values[-1]
        )

    metrics = HarmonicFitMetrics(
        rank=int(rank),
        parameter_count=int(sample_matrix.shape[1]),
        condition_number=condition_number,
        sample_rmse_mv=_rmse(fitted_samples, centered_mv),
        display_rmse_mv=_rmse(fitted_display, ideal_display),
        display_max_error_mv=_max_abs_error(
            fitted_display,
            ideal_display,
        ),
    )
    return fitted_samples, fitted_display, metrics


def _rmse(actual: np.ndarray, expected: np.ndarray) -> float:
    return float(np.sqrt(np.mean(np.square(actual - expected))))


def _max_abs_error(actual: np.ndarray, expected: np.ndarray) -> float:
    return float(np.max(np.abs(actual - expected)))


def simulate(
    spec: WaveformSpec,
    *,
    display_periods: int = DEFAULT_DISPLAY_PERIODS,
    phase_bin_count: int = DEFAULT_PHASE_BIN_COUNT,
) -> SimulationResult:
    """Run the complete custom waveform simulation and comparison."""

    if phase_bin_count < 4 or phase_bin_count > 2048:
        raise ValueError("折叠相位槽数必须位于4~2048。")

    sample_time_s, sample_signal_mv, adc_codes, adc_signal_mv = (
        generate_adc_codes(spec)
    )
    refined_hz = refine_fundamental_hz(spec, adc_codes)
    volts_per_code = spec.adc_vref_v / 4096.0
    phase_waveform, phase_weights, phase_cycles = phase_fold_waveform(
        adc_codes,
        volts_per_code,
        spec.sample_rate_hz,
        refined_hz,
        phase_bin_count,
    )

    phase_axis = (
        np.arange(phase_bin_count, dtype=np.float64)
        / phase_bin_count
    )
    ideal_phase = synthesize_at_base_phase_mv(spec, phase_axis)
    ideal_phase -= float(np.mean(ideal_phase))

    display_waveform = periodic_linear_display(
        phase_waveform,
        display_periods,
    )
    display_axis = (
        np.arange(DEFAULT_DISPLAY_WIDTH, dtype=np.float64)
        * display_periods
        / (DEFAULT_DISPLAY_WIDTH - 1)
    )
    ideal_display = synthesize_at_base_phase_mv(spec, display_axis)
    ideal_display -= float(np.mean(ideal_display))

    dense_phase = np.arange(131072, dtype=np.float64) / 131072.0
    dense_waveform = synthesize_at_base_phase_mv(spec, dense_phase)
    expected_vpp = float(dense_waveform.max() - dense_waveform.min())
    expected_vrms = float(
        np.sqrt(np.mean(np.square(dense_waveform)))
    )
    coverage = phase_coverage(
        spec.fundamental_hz,
        spec.sample_rate_hz,
        spec.sample_count,
        phase_bin_count,
        spec.start_phase_deg,
    )
    _, harmonic_fit_display, harmonic_fit_metrics = (
        harmonic_least_squares_fit(
            spec,
            adc_codes,
            refined_hz,
            display_periods=display_periods,
        )
    )

    return SimulationResult(
        spec=spec,
        sample_time_s=sample_time_s,
        sample_signal_mv=sample_signal_mv,
        adc_codes=adc_codes,
        adc_signal_mv=adc_signal_mv,
        refined_fundamental_hz=refined_hz,
        phase_bin_count=phase_bin_count,
        phase_cycles=phase_cycles,
        phase_weights=phase_weights,
        phase_waveform_mv=phase_waveform,
        ideal_phase_waveform_mv=ideal_phase,
        display_waveform_mv=display_waveform,
        ideal_display_waveform_mv=ideal_display,
        harmonic_fit_display_waveform_mv=harmonic_fit_display,
        harmonic_fit=harmonic_fit_metrics,
        coverage=coverage,
        expected_vpp_mv=expected_vpp,
        expected_vrms_mv=expected_vrms,
        phase_fold_rmse_mv=_rmse(phase_waveform, ideal_phase),
        phase_fold_max_error_mv=_max_abs_error(
            phase_waveform,
            ideal_phase,
        ),
        display_rmse_mv=_rmse(display_waveform, ideal_display),
        display_max_error_mv=_max_abs_error(
            display_waveform,
            ideal_display,
        ),
    )


def spec_to_record(spec: WaveformSpec) -> Dict[str, object]:
    """Convert immutable dataclasses to a stable JSON-compatible record."""

    record = asdict(spec)
    record["tones"] = [asdict(tone) for tone in spec.tones]
    return record


def spec_from_record(record: Dict[str, object]) -> WaveformSpec:
    """Restore a WaveformSpec from registry JSON."""

    tones = tuple(
        ToneSpec(
            order=int(tone["order"]),
            amplitude_mvpk=float(tone["amplitude_mvpk"]),
            relative_phase_deg=float(
                tone.get("relative_phase_deg", 0.0)
            ),
        )
        for tone in record["tones"]
    )
    return WaveformSpec(
        group_number=int(record["group_number"]),
        name=str(record["name"]),
        requirement=int(record.get("requirement", 2)),
        fundamental_hz=float(record["fundamental_hz"]),
        tones=tones,
        sample_rate_hz=float(
            record.get("sample_rate_hz", DEFAULT_SAMPLE_RATE_HZ)
        ),
        sample_count=int(
            record.get("sample_count", DEFAULT_SAMPLE_COUNT)
        ),
        adc_vref_v=float(
            record.get("adc_vref_v", DEFAULT_ADC_VREF_V)
        ),
        adc_bias_v=float(
            record.get("adc_bias_v", DEFAULT_ADC_BIAS_V)
        ),
        start_phase_deg=float(record.get("start_phase_deg", 0.0)),
        noise_rms_mv=float(record.get("noise_rms_mv", 0.0)),
        random_seed=int(record.get("random_seed", 1)),
    )


def load_registry(path: Path = REGISTRY_PATH) -> List[WaveformSpec]:
    """Load user records; a missing registry is treated as an empty set."""

    if not path.exists():
        return []
    payload = json.loads(path.read_text(encoding="utf-8"))
    return [spec_from_record(item) for item in payload.get("groups", [])]


def save_registry(
    specs: Sequence[WaveformSpec],
    path: Path = REGISTRY_PATH,
) -> None:
    """Persist records in stable group-number order."""

    unique: Dict[int, WaveformSpec] = {}
    for spec in specs:
        validate_spec(spec)
        unique[spec.group_number] = spec
    ordered = [unique[key] for key in sorted(unique)]
    payload = {
        "schema_version": 1,
        "groups": [spec_to_record(spec) for spec in ordered],
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def _identifier(text: str) -> str:
    identifier = re.sub(r"[^0-9A-Za-z_]+", "_", text).strip("_")
    if not identifier:
        identifier = "custom"
    if identifier[0].isdigit():
        identifier = "g_" + identifier
    return identifier.lower()


def _format_c_array(
    name: str,
    values: Sequence[int],
    values_per_line: int = 16,
) -> List[str]:
    lines = [
        "static const uint16_t {}"
        "[GENERATED_CUSTOM_ADC_SAMPLE_COUNT] =".format(name),
        "{",
    ]
    for start in range(0, len(values), values_per_line):
        chunk = values[start : start + values_per_line]
        suffix = "," if start + values_per_line < len(values) else ""
        lines.append(
            "    " + ", ".join(str(int(value)) for value in chunk) + suffix
        )
    lines.extend(("};", ""))
    return lines


def export_custom_header(
    specs: Sequence[WaveformSpec],
    path: Path = CUSTOM_HEADER_PATH,
) -> Path:
    """Generate a consolidated firmware header from saved custom groups."""

    if not specs:
        raise ValueError("至少需要保存一组曲线后才能导出C头文件。")
    ordered = sorted(specs, key=lambda item: item.group_number)
    if len({item.group_number for item in ordered}) != len(ordered):
        raise ValueError("导出组号不能重复。")

    reference = ordered[0]
    for spec in ordered:
        validate_spec(spec)
        if spec.sample_count != reference.sample_count:
            raise ValueError("同一C头文件中的采样点数必须一致。")
        if not math.isclose(
            spec.sample_rate_hz,
            reference.sample_rate_hz,
            rel_tol=0.0,
            abs_tol=1.0e-6,
        ):
            raise ValueError("同一C头文件中的采样率必须一致。")
        if not math.isclose(
            spec.adc_vref_v,
            reference.adc_vref_v,
            rel_tol=0.0,
            abs_tol=1.0e-9,
        ):
            raise ValueError("同一C头文件中的ADC参考电压必须一致。")

    simulations = [simulate(spec) for spec in ordered]
    array_names = [
        "s_custom_adc_t{:03d}_{}".format(
            spec.group_number,
            _identifier(spec.name),
        )
        for spec in ordered
    ]
    lines = [
        "/*",
        " * AUTO-GENERATED by tools/custom_waveform_lab.py.",
        " * Edit the lab records and export again; do not hand-edit arrays.",
        " */",
        "#ifndef GENERATED_CUSTOM_ADC_TESTS_H",
        "#define GENERATED_CUSTOM_ADC_TESTS_H",
        "",
        "#define GENERATED_CUSTOM_ADC_SAMPLE_COUNT {}U".format(
            reference.sample_count
        ),
        "#define GENERATED_CUSTOM_ADC_SAMPLE_RATE_HZ {:.1f}f".format(
            reference.sample_rate_hz
        ),
        (
            "#define GENERATED_CUSTOM_ADC_VOLTS_PER_CODE "
            "({:.9g}f / 4096.0f)"
        ).format(reference.adc_vref_v),
        "#define GENERATED_CUSTOM_ADC_TEST_CASE_COUNT {}U".format(
            len(ordered)
        ),
        "",
    ]

    for array_name, simulation in zip(array_names, simulations):
        lines.extend(
            _format_c_array(array_name, simulation.adc_codes.tolist())
        )

    lines.extend(
        (
            "static const GeneratedAdcTestCase",
            (
                "s_generated_custom_adc_test_cases"
                "[GENERATED_CUSTOM_ADC_TEST_CASE_COUNT] ="
            ),
            "{",
        )
    )

    for index, (spec, simulation, array_name) in enumerate(
        zip(ordered, simulations, array_names)
    ):
        frequencies = [
            spec.fundamental_hz * tone.order for tone in spec.tones
        ]
        amplitudes = [tone.amplitude_mvpk for tone in spec.tones]
        frequencies.extend([0.0] * (3 - len(frequencies)))
        amplitudes.extend([0.0] * (3 - len(amplitudes)))
        comma = "," if index + 1 < len(ordered) else ""
        lines.extend(
            (
                "    {",
                '        "{}",'.format(spec.name.replace('"', "'")),
                "        {}U,".format(spec.group_number),
                "        {}U,".format(spec.requirement),
                "        {:.6f}f,".format(spec.fundamental_hz),
                "        {:.6f}f,".format(simulation.expected_vpp_mv),
                "        {:.6f}f,".format(simulation.expected_vrms_mv),
                "        {}U,".format(len(spec.tones)),
                "        {{{}}},".format(
                    ", ".join(
                        "{:.6f}f".format(value) for value in frequencies
                    )
                ),
                "        {{{}}},".format(
                    ", ".join(
                        "{:.6f}f".format(value) for value in amplitudes
                    )
                ),
                "        0.0f,",
                "        0.0f,",
                "        0.0f,",
                "        {}".format(array_name),
                "    }" + comma,
            )
        )

    lines.extend(("};", "", "#endif /* GENERATED_CUSTOM_ADC_TESTS_H */", ""))
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as output_file:
        output_file.write("\n".join(lines))
    return path


def export_group_artifacts(
    simulation: SimulationResult,
    output_root: Path = CUSTOM_DATA_ROOT,
) -> Path:
    """Export JSON metadata and a readable 2048-point CSV for one group."""

    spec = simulation.spec
    folder = output_root / "T{:03d}_{}".format(
        spec.group_number,
        _identifier(spec.name),
    )
    folder.mkdir(parents=True, exist_ok=True)

    metadata = {
        "schema_version": 1,
        "spec": spec_to_record(spec),
        "result": {
            "refined_fundamental_hz": simulation.refined_fundamental_hz,
            "phase_bin_count": simulation.phase_bin_count,
            "expected_vpp_mv": simulation.expected_vpp_mv,
            "expected_vrms_mv": simulation.expected_vrms_mv,
            "phase_fold_rmse_mv": simulation.phase_fold_rmse_mv,
            "phase_fold_max_error_mv": (
                simulation.phase_fold_max_error_mv
            ),
            "display_rmse_mv": simulation.display_rmse_mv,
            "display_max_error_mv": simulation.display_max_error_mv,
            "harmonic_fit": asdict(simulation.harmonic_fit),
            "coverage": asdict(simulation.coverage),
        },
    }
    (folder / "case.json").write_text(
        json.dumps(metadata, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )

    with (folder / "samples_2048.csv").open(
        "w",
        encoding="utf-8",
        newline="",
    ) as csv_file:
        writer = csv.writer(csv_file, lineterminator="\n")
        writer.writerow(
            (
                "sample_index",
                "time_us",
                "ideal_signal_mv",
                "adc_code",
                "quantized_centered_mv",
                "base_phase_cycles",
            )
        )
        for index in range(spec.sample_count):
            writer.writerow(
                (
                    index,
                    "{:.9f}".format(
                        simulation.sample_time_s[index] * 1.0e6
                    ),
                    "{:.9f}".format(
                        simulation.sample_signal_mv[index]
                    ),
                    int(simulation.adc_codes[index]),
                    "{:.9f}".format(
                        simulation.adc_signal_mv[index]
                    ),
                    "{:.12f}".format(
                        simulation.phase_cycles[index]
                    ),
                )
            )
    return folder


def typical_specs() -> Tuple[WaveformSpec, ...]:
    """Representative normal and coherent-coverage cases for quick loading."""

    return (
        WaveformSpec(
            101,
            "pure_10k",
            2,
            10_000.0,
            (ToneSpec(1, 50.0),),
        ),
        WaveformSpec(
            102,
            "official_10p5k_1_3_4",
            1,
            10_500.0,
            (
                ToneSpec(1, 50.0),
                ToneSpec(3, 25.0),
                ToneSpec(4, 15.0),
            ),
        ),
        WaveformSpec(
            103,
            "coherent_128k_1_2_3",
            2,
            128_000.0,
            (
                ToneSpec(1, 55.0),
                ToneSpec(2, 20.0),
                ToneSpec(3, 10.0),
            ),
        ),
        WaveformSpec(
            104,
            "coherent_160k_1_2_3",
            2,
            160_000.0,
            (
                ToneSpec(1, 55.0),
                ToneSpec(2, 20.0),
                ToneSpec(3, 10.0),
            ),
        ),
        WaveformSpec(
            105,
            "coherent_200k_1_2",
            2,
            200_000.0,
            (ToneSpec(1, 55.0), ToneSpec(2, 15.0)),
        ),
        WaveformSpec(
            106,
            "boundary_250k_1_2",
            2,
            250_000.0,
            (ToneSpec(1, 55.0), ToneSpec(2, 15.0)),
        ),
        WaveformSpec(
            107,
            "half_grid_249p75k_1_2",
            2,
            249_750.0,
            (ToneSpec(1, 55.0), ToneSpec(2, 15.0)),
            start_phase_deg=37.0,
        ),
        WaveformSpec(
            108,
            "exact_256k_pure",
            2,
            256_000.0,
            (ToneSpec(1, 55.0),),
            start_phase_deg=23.0,
        ),
    )


def write_typical_registry_if_missing(
    path: Path = REGISTRY_PATH,
) -> bool:
    """Create initial examples once without overwriting user records."""

    if path.exists():
        return False
    save_registry(typical_specs(), path)
    return True
