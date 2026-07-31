#!/usr/bin/env python3
"""
Analyze the three real 2048-point ADC captures for V2.2 display smoothing.

The script deliberately mirrors the firmware path:

    recovered uint16 ADC codes
    -> correlation frequency refinement
    -> ordinary / two-pass Huber 256-bin fold
    -> optional periodic smoothing candidate
    -> default rising-zero trigger
    -> 794-column linear display and 145-pixel quantization

The source workbook mixes hexadecimal strings with cells auto-converted by
Excel.  Digits-only cells are still hexadecimal tokens.  Scientific cells such
as the original ``1e3`` are recovered from their number-format style.
"""

from __future__ import annotations

import csv
import json
import math
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple
from xml.etree import ElementTree as ET
from zipfile import ZipFile

import matplotlib
import numpy as np
from scipy.signal import savgol_filter

matplotlib.use("Agg")
import matplotlib.pyplot as plt

from waveform_lab_core import (
    ToneSpec,
    harmonic_project_waveform,
    huber_phase_fold_waveform,
    phase_fold_waveform,
)


REPO_ROOT = Path(__file__).resolve().parents[1]
SOURCE_WORKBOOK = REPO_ROOT / "tests" / "三组实际ADC数据.xlsx"
OUTPUT_ROOT = REPO_ROOT / "tests" / "real_adc_smoothing_v22"
SAMPLE_RATE_HZ = 1_024_090.0
VOLTS_PER_CODE = 3.3 / 4096.0
PHASE_BIN_COUNT = 256
DISPLAY_WIDTH = 794
DISPLAY_Y_MAX = 144
DISPLAY_MARGIN = 5
TRIGGER_HYSTERESIS_RATIO = 0.02
TRIGGER_HYSTERESIS_MIN_MV = 0.50
XML_NAMESPACE = {
    "m": "http://schemas.openxmlformats.org/spreadsheetml/2006/main"
}


@dataclass(frozen=True)
class CaptureSpec:
    column: str
    name: str
    fundamental_hz: float
    tones: Tuple[ToneSpec, ...]
    note: str


@dataclass(frozen=True)
class RobustFitDiagnostics:
    residual_rms_mv: float
    robust_scale_mv: float
    minimum_weight: float
    downweighted_count: int
    interleave_stability_rmse_mv: float


CAPTURES = (
    CaptureSpec(
        column="A",
        name="A_58k_h8",
        fundamental_hz=58_000.0,
        tones=(ToneSpec(1, 75.0), ToneSpec(8, 25.0)),
        note="58 kHz基波150 mVpp；8次谐波50 mVpp",
    ),
    CaptureSpec(
        column="B",
        name="B_120k_h3_h4",
        fundamental_hz=120_000.0,
        tones=(
            ToneSpec(1, 75.0),
            ToneSpec(3, 25.0),
            ToneSpec(4, 25.0),
        ),
        note="120 kHz基波150 mVpp；3次、4次谐波各50 mVpp",
    ),
    CaptureSpec(
        column="C",
        name="C_60k_h2_h7",
        fundamental_hz=60_000.0,
        tones=(
            ToneSpec(1, 50.0),
            ToneSpec(2, 35.0),
            ToneSpec(7, 40.0),
        ),
        note="60 kHz基波100 mVpp；2次70 mVpp；7次80 mVpp",
    ),
)


def _shared_strings(root: ET.Element) -> List[str]:
    strings: List[str] = []
    for item in root.findall("m:si", XML_NAMESPACE):
        strings.append(
            "".join(
                node.text or ""
                for node in item.iterfind(".//m:t", XML_NAMESPACE)
            )
        )
    return strings


def _recover_scientific_hex_token(value: float) -> str:
    """
    Reverse Excel's conversion of tokens such as 1e3 into 1000.

    All style-1 cells in this workbook are exact powers of ten and belong to
    column C.  The original hexadecimal token therefore has the form 1eN.
    """

    if value <= 0.0:
        raise ValueError("科学计数法ADC单元格必须为正数。")
    exponent = int(round(math.log10(value)))
    if not math.isclose(
        value,
        10.0 ** exponent,
        rel_tol=0.0,
        abs_tol=1.0e-6,
    ):
        raise ValueError(
            "无法无歧义恢复科学计数法ADC值：{}".format(value)
        )
    return "1e{:x}".format(exponent)


def load_real_adc_workbook(
    path: Path,
) -> Tuple[Dict[str, np.ndarray], List[Dict[str, object]]]:
    """Load all three hexadecimal ADC columns without modifying the workbook."""

    with ZipFile(path) as archive:
        shared_root = ET.fromstring(
            archive.read("xl/sharedStrings.xml")
        )
        strings = _shared_strings(shared_root)
        sheet_root = ET.fromstring(
            archive.read("xl/worksheets/sheet1.xml")
        )

    columns: Dict[str, List[int]] = {
        capture.column: [] for capture in CAPTURES
    }
    recoveries: List[Dict[str, object]] = []

    for cell in sheet_root.iterfind(".//m:c", XML_NAMESPACE):
        reference = cell.attrib["r"]
        column = reference[0]
        if column not in columns:
            continue

        value_node = cell.find("m:v", XML_NAMESPACE)
        if value_node is None or value_node.text is None:
            raise ValueError("ADC单元格{}为空。".format(reference))

        if cell.attrib.get("t") == "s":
            token = strings[int(value_node.text)]
            recovery_kind = "shared_hex_text"
        elif cell.attrib.get("s") == "1":
            numeric_value = float(value_node.text)
            token = _recover_scientific_hex_token(numeric_value)
            recovery_kind = "scientific_hex_recovery"
            recoveries.append(
                {
                    "cell": reference,
                    "stored_value": numeric_value,
                    "recovered_token": token,
                }
            )
        else:
            numeric_value = float(value_node.text)
            if not numeric_value.is_integer():
                raise ValueError(
                    "ADC单元格{}含非整数：{}".format(
                        reference,
                        numeric_value,
                    )
                )
            token = str(int(numeric_value))
            recovery_kind = "numeric_hex_token"

        try:
            code = int(token, 16)
        except ValueError as error:
            raise ValueError(
                "ADC单元格{}不是十六进制码：{}".format(
                    reference,
                    token,
                )
            ) from error

        if not 0 <= code <= 4095:
            raise ValueError(
                "ADC单元格{}恢复后越界：{} -> {}".format(
                    reference,
                    token,
                    code,
                )
            )

        columns[column].append(code)
        if recovery_kind == "scientific_hex_recovery":
            continue

    arrays = {
        column: np.asarray(values, dtype=np.uint16)
        for column, values in columns.items()
    }
    for column, values in arrays.items():
        if values.size != 2048:
            raise ValueError(
                "{}列应有2048点，实际{}点。".format(
                    column,
                    values.size,
                )
            )

    return arrays, recoveries


def emulate_teammate_fft(
    adc_codes: np.ndarray,
) -> List[Dict[str, float]]:
    """
    Mirror the current main.c three-peak FFT selection.

    The teammate path uses 1,024,000 Hz for 500 Hz bins, sums ±8 bins around
    each selected peak, zeros those bins, and reports at least two peaks.
    """

    adc_volts = adc_codes.astype(np.float64) * VOLTS_PER_CODE
    magnitudes = np.abs(np.fft.fft(adc_volts))[: adc_codes.size // 2]
    components: List[Dict[str, float]] = []

    for _ in range(3):
        peak_index = int(np.argmax(magnitudes[1:])) + 1
        lower = max(0, peak_index - 8)
        upper = min(magnitudes.size, peak_index + 9)
        amplitude_v = (
            2.0 *
            float(
                np.sqrt(
                    np.sum(np.square(magnitudes[lower:upper]))
                )
            ) /
            adc_codes.size
        )
        components.append(
            {
                "frequency_hz": peak_index * 500.0,
                "amplitude_mvpk": amplitude_v * 1000.0,
            }
        )
        magnitudes[lower:upper] = 0.0

    component_count = 2 if components[2]["amplitude_mvpk"] < 4.0 else 3
    return sorted(
        components[:component_count],
        key=lambda component: component["frequency_hz"],
    )


def harmonic_orders_from_components(
    components: Sequence[Dict[str, float]],
    fundamental_hz: float,
) -> Tuple[int, ...]:
    """Apply the planned MCU ratio check and de-duplicate harmonic orders."""

    orders = {1}
    for component in components:
        ratio = float(component["frequency_hz"]) / fundamental_hz
        order = max(1, int(ratio + 0.5))
        if (
            order < PHASE_BIN_COUNT // 2
            and abs(ratio - order) <= 0.15
        ):
            orders.add(order)
    return tuple(sorted(orders))


def firmware_float32_harmonic_projection(
    waveform_mv: np.ndarray,
    harmonic_orders: Sequence[int],
) -> np.ndarray:
    """Mirror the Cortex-M float32 oscillator implementation exactly."""

    waveform = np.asarray(waveform_mv, dtype=np.float32)
    count = waveform.size
    input_mean = np.float32(0.0)
    for value in waveform:
        input_mean = np.float32(input_mean + value)
    input_mean = np.float32(input_mean / np.float32(count))
    projected = np.zeros(count, dtype=np.float32)

    for order in harmonic_orders:
        angle_step = np.float32(
            2.0 * math.pi * int(order) / count
        )
        step_real = np.float32(math.cos(float(angle_step)))
        step_imag = np.float32(math.sin(float(angle_step)))
        oscillator_real = np.float32(1.0)
        oscillator_imag = np.float32(0.0)
        cosine_sum = np.float32(0.0)
        sine_sum = np.float32(0.0)

        for value in waveform:
            centered = np.float32(value - input_mean)
            cosine_sum = np.float32(
                cosine_sum + centered * oscillator_real
            )
            sine_sum = np.float32(
                sine_sum + centered * oscillator_imag
            )
            next_real = np.float32(
                oscillator_real * step_real -
                oscillator_imag * step_imag
            )
            oscillator_imag = np.float32(
                oscillator_real * step_imag +
                oscillator_imag * step_real
            )
            oscillator_real = next_real

        scale = np.float32(2.0 / count)
        cosine_coefficient = np.float32(scale * cosine_sum)
        sine_coefficient = np.float32(scale * sine_sum)
        oscillator_real = np.float32(1.0)
        oscillator_imag = np.float32(0.0)

        for index in range(count):
            projected[index] = np.float32(
                projected[index] +
                cosine_coefficient * oscillator_real +
                sine_coefficient * oscillator_imag
            )
            next_real = np.float32(
                oscillator_real * step_real -
                oscillator_imag * step_imag
            )
            oscillator_imag = np.float32(
                oscillator_real * step_imag +
                oscillator_imag * step_real
            )
            oscillator_real = next_real

    output_mean = np.float32(0.0)
    for value in projected:
        output_mean = np.float32(output_mean + value)
    output_mean = np.float32(output_mean / np.float32(count))
    return np.asarray(projected - output_mean, dtype=np.float64)


def refine_fundamental_from_components(
    adc_codes: np.ndarray,
    components: Sequence[Dict[str, float]],
) -> float:
    """Mirror AnalyzerBridge's ±500 Hz correlation refinement."""

    fundamental_hz = float(components[0]["frequency_hz"])
    reference = max(
        components,
        key=lambda component: (
            float(component["amplitude_mvpk"]) *
            max(
                1,
                int(
                    float(component["frequency_hz"]) /
                    fundamental_hz +
                    0.5
                ),
            )
        ),
    )
    reference_order = max(
        1,
        int(
            float(reference["frequency_hz"]) /
            fundamental_hz +
            0.5
        ),
    )
    candidates = np.arange(
        fundamental_hz - 500.0,
        fundamental_hz + 500.0 + 5.0,
        10.0,
        dtype=np.float64,
    )
    centered_codes = (
        adc_codes.astype(np.float64) -
        float(np.mean(adc_codes))
    )
    sample_indices = np.arange(adc_codes.size, dtype=np.float64)
    scores = np.empty(candidates.size, dtype=np.float64)

    for index, candidate in enumerate(candidates):
        tone_frequency = candidate * reference_order
        angle = (
            -2.0 *
            math.pi *
            tone_frequency *
            sample_indices /
            SAMPLE_RATE_HZ
        )
        correlation = np.dot(
            centered_codes,
            np.exp(1j * angle),
        )
        scores[index] = (
            correlation.real * correlation.real +
            correlation.imag * correlation.imag
        )

    best_index = int(np.argmax(scores))
    fractional_offset = 0.0
    if 0 < best_index < scores.size - 1:
        left = float(scores[best_index - 1])
        center = float(scores[best_index])
        right = float(scores[best_index + 1])
        denominator = left - 2.0 * center + right
        if abs(denominator) > 1.0e-6:
            fractional_offset = (
                0.5 * (left - right) / denominator
            )
            fractional_offset = max(
                -1.0,
                min(1.0, fractional_offset),
            )

    return float(
        candidates[best_index] + fractional_offset * 10.0
    )


def robust_harmonic_fit(
    adc_codes: np.ndarray,
    fundamental_hz: float,
    harmonic_orders: Sequence[int],
    sample_indices: np.ndarray | None = None,
    maximum_iterations: int = 12,
) -> Tuple[np.ndarray, np.ndarray, RobustFitDiagnostics]:
    """
    Fit DC plus selected sine/cosine terms using Huber IRLS.

    This expensive PC fit is an independent smooth reference and confidence
    check.  The firmware candidate instead projects the already-Huber-folded
    256-bin waveform, which is much cheaper.
    """

    full_indices = np.arange(adc_codes.size, dtype=np.float64)
    centered_mv = (
        adc_codes.astype(np.float64) -
        float(np.mean(adc_codes))
    ) * VOLTS_PER_CODE * 1000.0
    if sample_indices is None:
        indices = full_indices
        values = centered_mv
    else:
        indices = np.asarray(sample_indices, dtype=np.int64)
        values = centered_mv[indices]
        indices = indices.astype(np.float64)

    columns = [np.ones(indices.size, dtype=np.float64)]
    for order in harmonic_orders:
        angle = (
            2.0 *
            math.pi *
            order *
            fundamental_hz *
            indices /
            SAMPLE_RATE_HZ
        )
        columns.extend((np.sin(angle), np.cos(angle)))
    design = np.column_stack(columns)
    weights = np.ones(values.size, dtype=np.float64)
    coefficients = np.linalg.lstsq(
        design,
        values,
        rcond=None,
    )[0]

    for _ in range(maximum_iterations):
        residual = values - design.dot(coefficients)
        residual_center = float(np.median(residual))
        absolute_deviation = np.abs(residual - residual_center)
        robust_scale_mv = (
            1.4826 * float(np.median(absolute_deviation))
        )
        delta_mv = max(
            1.345 * robust_scale_mv,
            1.5 * VOLTS_PER_CODE * 1000.0,
        )
        weights = np.ones_like(absolute_deviation)
        large = absolute_deviation > delta_mv
        weights[large] = delta_mv / absolute_deviation[large]
        square_root_weights = np.sqrt(weights)
        next_coefficients = np.linalg.lstsq(
            design * square_root_weights[:, None],
            values * square_root_weights,
            rcond=None,
        )[0]
        if float(
            np.max(np.abs(next_coefficients - coefficients))
        ) < 1.0e-7:
            coefficients = next_coefficients
            break
        coefficients = next_coefficients

    residual = values - design.dot(coefficients)
    residual_center = float(np.median(residual))
    robust_scale_mv = (
        1.4826 *
        float(np.median(np.abs(residual - residual_center)))
    )
    phase_cycles = (
        np.arange(PHASE_BIN_COUNT, dtype=np.float64) /
        PHASE_BIN_COUNT
    )
    phase_columns = [
        np.ones(PHASE_BIN_COUNT, dtype=np.float64)
    ]
    for order in harmonic_orders:
        phase_angle = 2.0 * math.pi * order * phase_cycles
        phase_columns.extend(
            (np.sin(phase_angle), np.cos(phase_angle))
        )
    reference = np.column_stack(phase_columns).dot(coefficients)
    reference -= float(np.mean(reference))

    diagnostics = RobustFitDiagnostics(
        residual_rms_mv=float(
            np.sqrt(np.mean(np.square(residual)))
        ),
        robust_scale_mv=robust_scale_mv,
        minimum_weight=float(np.min(weights)),
        downweighted_count=int(
            np.count_nonzero(weights < 0.999999)
        ),
        interleave_stability_rmse_mv=0.0,
    )
    return reference, coefficients, diagnostics


def robust_fit_with_stability(
    adc_codes: np.ndarray,
    fundamental_hz: float,
    harmonic_orders: Sequence[int],
) -> Tuple[np.ndarray, np.ndarray, RobustFitDiagnostics]:
    """Fit all points and measure agreement among four interleaved subsets."""

    reference, coefficients, diagnostics = robust_harmonic_fit(
        adc_codes,
        fundamental_hz,
        harmonic_orders,
    )
    subset_waveforms: List[np.ndarray] = []
    all_indices = np.arange(adc_codes.size)
    for remainder in range(4):
        subset = all_indices[(all_indices % 4) == remainder]
        subset_waveform, _, _ = robust_harmonic_fit(
            adc_codes,
            fundamental_hz,
            harmonic_orders,
            subset,
        )
        subset_waveforms.append(subset_waveform)

    maximum_pair_rmse = 0.0
    for first in range(len(subset_waveforms)):
        for second in range(first):
            pair_rmse = float(
                np.sqrt(
                    np.mean(
                        np.square(
                            subset_waveforms[first] -
                            subset_waveforms[second]
                        )
                    )
                )
            )
            maximum_pair_rmse = max(
                maximum_pair_rmse,
                pair_rmse,
            )

    return (
        reference,
        coefficients,
        RobustFitDiagnostics(
            residual_rms_mv=diagnostics.residual_rms_mv,
            robust_scale_mv=diagnostics.robust_scale_mv,
            minimum_weight=diagnostics.minimum_weight,
            downweighted_count=diagnostics.downweighted_count,
            interleave_stability_rmse_mv=maximum_pair_rmse,
        ),
    )


def _positive_peak_position(waveform: np.ndarray) -> float:
    peak_index = int(np.argmax(waveform))
    previous_value = float(waveform[(peak_index - 1) % waveform.size])
    peak_value = float(waveform[peak_index])
    next_value = float(waveform[(peak_index + 1) % waveform.size])
    denominator = previous_value - 2.0 * peak_value + next_value
    offset = 0.0
    if abs(denominator) > 1.0e-6:
        offset = (
            0.5 *
            (previous_value - next_value) /
            denominator
        )
        offset = max(-0.5, min(0.5, offset))
    return float((peak_index + offset) % waveform.size)


def _crossing_passes_hysteresis(
    waveform: np.ndarray,
    crossing_index: int,
    hysteresis_mv: float,
) -> bool:
    search_count = max(2, waveform.size // 4)
    before_ready = False
    after_ready = False
    for distance in range(search_count):
        before_value = float(
            waveform[(crossing_index - distance) % waveform.size]
        )
        after_value = float(
            waveform[
                (crossing_index + 1 + distance) % waveform.size
            ]
        )
        before_ready = before_ready or (
            before_value <= -hysteresis_mv
        )
        after_ready = after_ready or (
            after_value >= hysteresis_mv
        )
        if before_ready and after_ready:
            return True
    return False


def rising_zero_trigger_position(waveform: np.ndarray) -> float:
    """Mirror the firmware's default rising-zero trigger."""

    peak_index = int(np.argmax(waveform))
    peak_position = _positive_peak_position(waveform)
    hysteresis_mv = max(
        TRIGGER_HYSTERESIS_MIN_MV,
        float(np.ptp(waveform)) * TRIGGER_HYSTERESIS_RATIO,
    )
    fallback_index = None

    for distance in range(waveform.size):
        index0 = (
            peak_index - 1 - distance
        ) % waveform.size
        index1 = (index0 + 1) % waveform.size
        value0 = float(waveform[index0])
        value1 = float(waveform[index1])
        if not (value0 <= 0.0 < value1):
            continue
        if fallback_index is None:
            fallback_index = index0
        if _crossing_passes_hysteresis(
            waveform,
            index0,
            hysteresis_mv,
        ):
            denominator = value1 - value0
            fraction = (
                -value0 / denominator
                if abs(denominator) > 1.0e-6
                else 0.0
            )
            return float(
                (index0 + max(0.0, min(1.0, fraction))) %
                waveform.size
            )

    if fallback_index is not None:
        index1 = (fallback_index + 1) % waveform.size
        value0 = float(waveform[fallback_index])
        value1 = float(waveform[index1])
        denominator = value1 - value0
        fraction = (
            -value0 / denominator
            if abs(denominator) > 1.0e-6
            else 0.0
        )
        return float(
            (
                fallback_index +
                max(0.0, min(1.0, fraction))
            ) %
            waveform.size
        )

    return peak_position


def triggered_display(waveform: np.ndarray) -> np.ndarray:
    """Build the same 794-point one-period display with trigger offset."""

    trigger_position = rising_zero_trigger_position(waveform)
    output_indices = np.arange(DISPLAY_WIDTH, dtype=np.float64)
    positions = (
        trigger_position +
        output_indices *
        waveform.size /
        (DISPLAY_WIDTH - 1)
    )
    integer_positions = np.floor(positions).astype(np.int64)
    fractions = positions - integer_positions
    index0 = integer_positions % waveform.size
    index1 = (index0 + 1) % waveform.size
    return (
        waveform[index0] +
        fractions * (waveform[index1] - waveform[index0])
    )


def display_bytes(display_mv: np.ndarray) -> np.ndarray:
    """Map a float display curve to the firmware's 145-pixel byte range."""

    full_scale_mv = max(1.0, float(np.max(np.abs(display_mv)))) * 1.10
    clipped = np.clip(
        display_mv,
        -full_scale_mv,
        full_scale_mv,
    )
    output_min = float(DISPLAY_MARGIN)
    output_max = float(DISPLAY_Y_MAX - DISPLAY_MARGIN)
    normalized = (
        clipped + full_scale_mv
    ) / (2.0 * full_scale_mv)
    mapped = output_min + normalized * (output_max - output_min)
    return np.rint(mapped).astype(np.uint8)[::-1]


def extrema_count(waveform: np.ndarray) -> int:
    """Count circular slope reversals; ideal h-th harmonic has 2h."""

    differences = np.diff(
        np.concatenate((waveform, waveform[:1]))
    )
    signs = np.sign(differences)
    signs[signs == 0.0] = 1.0
    return int(np.count_nonzero(signs != np.roll(signs, 1)))


def waveform_metrics(
    candidate: np.ndarray,
    reference: np.ndarray,
) -> Dict[str, float]:
    residual = candidate - reference
    circular_residual = np.concatenate(
        (residual, residual[:2])
    )
    second_difference = np.diff(
        circular_residual,
        n=2,
    )
    return {
        "phase_rmse_mv": float(
            np.sqrt(np.mean(np.square(residual)))
        ),
        "phase_max_error_mv": float(np.max(np.abs(residual))),
        "vpp_mv": float(np.ptp(candidate)),
        "reference_vpp_mv": float(np.ptp(reference)),
        "vpp_error_mv": float(
            np.ptp(candidate) - np.ptp(reference)
        ),
        "extrema_count": float(extrema_count(candidate)),
        "residual_second_difference_rms_mv": float(
            np.sqrt(np.mean(np.square(second_difference)))
        ),
    }


def coefficient_amplitudes(
    coefficients: np.ndarray,
    harmonic_orders: Sequence[int],
) -> Dict[int, float]:
    return {
        order: float(
            math.hypot(
                coefficients[1 + 2 * index],
                coefficients[2 + 2 * index],
            )
        )
        for index, order in enumerate(harmonic_orders)
    }


def build_candidates(
    ordinary: np.ndarray,
    huber: np.ndarray,
    harmonic_orders: Sequence[int],
) -> Dict[str, np.ndarray]:
    projected, _ = harmonic_project_waveform(
        huber,
        harmonic_orders,
    )
    candidates = {
        "ordinary": ordinary,
        "huber": huber,
        "huber_savgol_9": savgol_filter(
            huber,
            9,
            3,
            mode="wrap",
        ),
        "huber_savgol_15": savgol_filter(
            huber,
            15,
            3,
            mode="wrap",
        ),
        "huber_savgol_21": savgol_filter(
            huber,
            21,
            3,
            mode="wrap",
        ),
        "huber_harmonic_projection": projected,
    }
    for name, waveform in candidates.items():
        candidates[name] = waveform - float(np.mean(waveform))
    return candidates


def analyze_capture(
    capture: CaptureSpec,
    adc_codes: np.ndarray,
) -> Tuple[
    Dict[str, object],
    List[Dict[str, object]],
    Dict[str, np.ndarray],
]:
    expected_orders = tuple(tone.order for tone in capture.tones)
    components = emulate_teammate_fft(adc_codes)
    coarse_fundamental_hz = float(components[0]["frequency_hz"])
    harmonic_orders = harmonic_orders_from_components(
        components,
        coarse_fundamental_hz,
    )
    if harmonic_orders != expected_orders:
        raise AssertionError(
            "{}上游FFT推导次数{}与备注{}不一致。".format(
                capture.column,
                harmonic_orders,
                expected_orders,
            )
        )
    refined_frequency_hz = refine_fundamental_from_components(
        adc_codes,
        components,
    )
    ordinary, _, _ = phase_fold_waveform(
        adc_codes,
        VOLTS_PER_CODE,
        SAMPLE_RATE_HZ,
        refined_frequency_hz,
        PHASE_BIN_COUNT,
    )
    huber, _, _, huber_diagnostics = huber_phase_fold_waveform(
        adc_codes,
        VOLTS_PER_CODE,
        SAMPLE_RATE_HZ,
        refined_frequency_hz,
        PHASE_BIN_COUNT,
    )
    reference, coefficients, fit_diagnostics = (
        robust_fit_with_stability(
            adc_codes,
            refined_frequency_hz,
            harmonic_orders,
        )
    )
    candidates = build_candidates(
        ordinary,
        huber,
        harmonic_orders,
    )
    candidates["robust_raw_reference"] = reference

    rows: List[Dict[str, object]] = []
    for algorithm, waveform in candidates.items():
        metrics = waveform_metrics(waveform, reference)
        display = triggered_display(waveform)
        reference_display = triggered_display(reference)
        display_residual = display - reference_display
        row: Dict[str, object] = {
            "capture": capture.column,
            "capture_name": capture.name,
            "algorithm": algorithm,
            "nominal_fundamental_hz": capture.fundamental_hz,
            "refined_fundamental_hz": refined_frequency_hz,
            "adc_min_code": int(np.min(adc_codes)),
            "adc_max_code": int(np.max(adc_codes)),
            "adc_mean_code": float(np.mean(adc_codes)),
            "display_rmse_mv": float(
                np.sqrt(np.mean(np.square(display_residual)))
            ),
            "display_max_error_mv": float(
                np.max(np.abs(display_residual))
            ),
            "screen_unique_y_count": int(
                np.unique(display_bytes(display)).size
            ),
            **metrics,
        }
        rows.append(row)

    projected = candidates["huber_harmonic_projection"]
    firmware_projected = firmware_float32_harmonic_projection(
        huber,
        harmonic_orders,
    )
    firmware_projection_max_error_mv = float(
        np.max(np.abs(firmware_projected - projected))
    )
    _, projection_diagnostics = harmonic_project_waveform(
        huber,
        harmonic_orders,
    )
    summary = {
        "capture": capture.column,
        "capture_name": capture.name,
        "note": capture.note,
        "sample_count": int(adc_codes.size),
        "adc_min_code": int(np.min(adc_codes)),
        "adc_max_code": int(np.max(adc_codes)),
        "adc_mean_code": float(np.mean(adc_codes)),
        "adc_peak_to_peak_codes": int(np.ptp(adc_codes)),
        "nominal_fundamental_hz": capture.fundamental_hz,
        "teammate_fft_components": components,
        "refined_fundamental_hz": refined_frequency_hz,
        "harmonic_orders": list(harmonic_orders),
        "nominal_amplitudes_mvpk": [
            tone.amplitude_mvpk for tone in capture.tones
        ],
        "robust_fit_amplitudes_mvpk": coefficient_amplitudes(
            coefficients,
            harmonic_orders,
        ),
        "raw_fit": asdict(fit_diagnostics),
        "huber": asdict(huber_diagnostics),
        "projection": asdict(projection_diagnostics),
        "firmware_float32_projection_max_error_mv": (
            firmware_projection_max_error_mv
        ),
        "ordinary_extrema_count": extrema_count(ordinary),
        "huber_extrema_count": extrema_count(huber),
        "projected_extrema_count": extrema_count(projected),
        "reference_extrema_count": extrema_count(reference),
    }
    return summary, rows, candidates


def write_recovered_csv(
    arrays: Dict[str, np.ndarray],
    output_path: Path,
) -> None:
    with output_path.open("w", newline="", encoding="utf-8-sig") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            ["sample_index", "A_adc_code", "B_adc_code", "C_adc_code"]
        )
        for index in range(2048):
            writer.writerow(
                [
                    index,
                    int(arrays["A"][index]),
                    int(arrays["B"][index]),
                    int(arrays["C"][index]),
                ]
            )


def write_metrics_csv(
    rows: Iterable[Dict[str, object]],
    output_path: Path,
) -> None:
    rows = list(rows)
    if not rows:
        raise ValueError("没有可写入的算法指标。")
    with output_path.open("w", newline="", encoding="utf-8-sig") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def plot_gallery(
    analyses: Sequence[
        Tuple[CaptureSpec, np.ndarray, Dict[str, np.ndarray]]
    ],
    output_path: Path,
) -> None:
    figure, axes = plt.subplots(
        len(analyses),
        2,
        figsize=(15.5, 11.5),
        constrained_layout=True,
    )
    figure.suptitle(
        "V2.2 real ADC: current fold versus model-constrained smoothing",
        fontsize=16,
    )

    for row_index, (capture, adc_codes, candidates) in enumerate(
        analyses
    ):
        phase_axis = np.arange(PHASE_BIN_COUNT) / PHASE_BIN_COUNT
        left = axes[row_index, 0]
        left.plot(
            phase_axis,
            candidates["robust_raw_reference"],
            color="#111827",
            linewidth=2.4,
            label="robust harmonic reference",
        )
        left.plot(
            phase_axis,
            candidates["ordinary"],
            color="#DC6B19",
            linewidth=0.9,
            alpha=0.8,
            label="ordinary",
        )
        left.plot(
            phase_axis,
            candidates["huber"],
            color="#0F766E",
            linewidth=1.0,
            alpha=0.9,
            label="Huber",
        )
        left.plot(
            phase_axis,
            candidates["huber_harmonic_projection"],
            color="#2563EB",
            linewidth=2.0,
            linestyle="--",
            label="Huber + harmonic projection",
        )
        left.set_title(
            "{}: {}".format(
                capture.column,
                capture.name.replace("_", " "),
            )
        )
        left.set_xlabel("base-cycle phase")
        left.set_ylabel("centered voltage / mV")
        left.grid(alpha=0.22)
        left.legend(fontsize=8, ncol=2)

        right = axes[row_index, 1]
        reference_display = triggered_display(
            candidates["robust_raw_reference"]
        )
        ordinary_display = triggered_display(candidates["ordinary"])
        huber_display = triggered_display(candidates["huber"])
        projected_display = triggered_display(
            candidates["huber_harmonic_projection"]
        )
        screen_x = np.arange(DISPLAY_WIDTH)
        right.plot(
            screen_x,
            reference_display,
            color="#111827",
            linewidth=2.2,
            label="reference",
        )
        right.plot(
            screen_x,
            ordinary_display,
            color="#DC6B19",
            linewidth=0.8,
            alpha=0.75,
            label="ordinary display",
        )
        right.plot(
            screen_x,
            huber_display,
            color="#0F766E",
            linewidth=0.9,
            alpha=0.85,
            label="Huber display",
        )
        right.plot(
            screen_x,
            projected_display,
            color="#2563EB",
            linewidth=1.8,
            linestyle="--",
            label="projected display",
        )
        right.set_title(
            "MCU-equivalent 794-point display; ADC {}..{}".format(
                int(np.min(adc_codes)),
                int(np.max(adc_codes)),
            )
        )
        right.set_xlabel("logical screen x")
        right.set_ylabel("centered voltage / mV")
        right.grid(alpha=0.22)
        right.legend(fontsize=8, ncol=2)

    figure.savefig(output_path, dpi=180)
    plt.close(figure)


def plot_candidate_matrix(
    analyses: Sequence[
        Tuple[CaptureSpec, np.ndarray, Dict[str, np.ndarray]]
    ],
    output_path: Path,
) -> None:
    algorithms = (
        "ordinary",
        "huber",
        "huber_savgol_9",
        "huber_savgol_15",
        "huber_savgol_21",
        "huber_harmonic_projection",
    )
    figure, axes = plt.subplots(
        len(analyses),
        1,
        figsize=(15.5, 10.5),
        constrained_layout=True,
    )
    colors = (
        "#DC6B19",
        "#0F766E",
        "#A855F7",
        "#7C3AED",
        "#4F46E5",
        "#2563EB",
    )
    for axis, (capture, _, candidates) in zip(axes, analyses):
        reference = candidates["robust_raw_reference"]
        phase = np.arange(PHASE_BIN_COUNT) / PHASE_BIN_COUNT
        axis.plot(
            phase,
            reference,
            color="#111827",
            linewidth=2.5,
            label="robust reference",
        )
        for algorithm, color in zip(algorithms, colors):
            axis.plot(
                phase,
                candidates[algorithm],
                linewidth=1.0,
                alpha=0.85,
                color=color,
                label=algorithm,
            )
        axis.set_title(
            "{} candidate sweep: {}".format(
                capture.column,
                capture.name.replace("_", " "),
            )
        )
        axis.set_xlabel("base-cycle phase")
        axis.set_ylabel("mV")
        axis.grid(alpha=0.22)
        axis.legend(fontsize=7, ncol=4)
    figure.savefig(output_path, dpi=180)
    plt.close(figure)


def main() -> int:
    arrays, recoveries = load_real_adc_workbook(SOURCE_WORKBOOK)
    OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)

    summaries: List[Dict[str, object]] = []
    metric_rows: List[Dict[str, object]] = []
    plot_inputs: List[
        Tuple[CaptureSpec, np.ndarray, Dict[str, np.ndarray]]
    ] = []

    for capture in CAPTURES:
        adc_codes = arrays[capture.column]
        summary, rows, candidates = analyze_capture(
            capture,
            adc_codes,
        )
        summaries.append(summary)
        metric_rows.extend(rows)
        plot_inputs.append((capture, adc_codes, candidates))

    write_recovered_csv(
        arrays,
        OUTPUT_ROOT / "recovered_adc_codes.csv",
    )
    write_metrics_csv(
        metric_rows,
        OUTPUT_ROOT / "algorithm_metrics.csv",
    )
    plot_gallery(
        plot_inputs,
        OUTPUT_ROOT / "comparison_gallery.png",
    )
    plot_candidate_matrix(
        plot_inputs,
        OUTPUT_ROOT / "candidate_matrix.png",
    )

    payload = {
        "schema_version": 1,
        "source_workbook": SOURCE_WORKBOOK.relative_to(
            REPO_ROOT
        ).as_posix(),
        "source_sheet": "Sheet1",
        "sample_rate_hz": SAMPLE_RATE_HZ,
        "volts_per_code": VOLTS_PER_CODE,
        "sample_count_per_capture": 2048,
        "hex_recovery_rule": {
            "digits_only_numeric_cells": (
                "interpret the displayed integer token as hexadecimal"
            ),
            "scientific_style_cells": (
                "reverse exact power-of-ten values to original 1eN "
                "hexadecimal tokens"
            ),
            "scientific_cell_count": len(recoveries),
            "scientific_cells": recoveries,
        },
        "captures": summaries,
        "acceptance": {
            "projection_max_reference_rmse_mv": 0.25,
            "projection_max_extra_extrema": 0,
            "maximum_interleave_stability_rmse_mv": 2.5,
            "firmware_float32_projection_max_error_mv": 0.01,
        },
        "recommendation": {
            "algorithm": "Huber + selected harmonic projection",
            "reason": (
                "It removes off-model phase-bin jitter while preserving "
                "the fitted coefficients of the fundamental and reported "
                "integer harmonics. It is cheaper and safer on the MCU "
                "than a full 2048-point robust least-squares solve."
            ),
            "default_mode_change": False,
            "measurement_values_unchanged": True,
        },
    }

    projected_rows = [
        row
        for row in metric_rows
        if row["algorithm"] == "huber_harmonic_projection"
    ]
    for row in projected_rows:
        if float(row["phase_rmse_mv"]) > 0.25:
            raise AssertionError(
                "{}投影RMSE超限：{}".format(
                    row["capture"],
                    row["phase_rmse_mv"],
                )
            )
    for summary in summaries:
        extra_extrema = (
            int(summary["projected_extrema_count"]) -
            int(summary["reference_extrema_count"])
        )
        if extra_extrema > 0:
            raise AssertionError(
                "{}投影仍有{}个额外折返点。".format(
                    summary["capture"],
                    extra_extrema,
                )
            )
        if (
            float(
                summary["raw_fit"][
                    "interleave_stability_rmse_mv"
                ]
            )
            > 2.5
        ):
            raise AssertionError(
                "{}四路交错拟合稳定性不足。".format(
                    summary["capture"]
                )
            )
        if (
            float(
                summary[
                    "firmware_float32_projection_max_error_mv"
                ]
            )
            > 0.01
        ):
            raise AssertionError(
                "{}固件float32投影与PC参考不一致。".format(
                    summary["capture"]
                )
            )

    with (OUTPUT_ROOT / "summary.json").open(
        "w",
        encoding="utf-8",
    ) as handle:
        json.dump(payload, handle, ensure_ascii=False, indent=2)

    print("captures:", len(summaries))
    print("scientific cells recovered:", len(recoveries))
    for summary in summaries:
        projection_row = next(
            row
            for row in projected_rows
            if row["capture"] == summary["capture"]
        )
        print(
            "{}: f={:.3f} Hz, extrema {} -> {} -> {}, "
            "projection RMSE={:.3f} mV, split stability={:.3f} mV".format(
                summary["capture"],
                summary["refined_fundamental_hz"],
                summary["ordinary_extrema_count"],
                summary["huber_extrema_count"],
                summary["projected_extrema_count"],
                projection_row["phase_rmse_mv"],
                summary["raw_fit"][
                    "interleave_stability_rmse_mv"
                ],
            )
        )
    print("output:", OUTPUT_ROOT)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
