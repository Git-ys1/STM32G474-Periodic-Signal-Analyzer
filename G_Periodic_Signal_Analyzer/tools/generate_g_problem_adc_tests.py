#!/usr/bin/env python3
"""
Generate deterministic, native-format ADC test vectors for G problem items 1-3.

The generated C header is consumed only by analyzer_bridge.c test mode.  Each
case contains the same uint16_t[2048] format as teammate adc_b, plus the ideal
analysis metadata that the teammate module would publish after processing.

The script also reproduces both the legacy "first-period stretch" waveform
builder and the full-buffer phase-fold builder, then writes quantitative
validation results for regression review.
"""

from __future__ import annotations

import csv
import json
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


REPO_ROOT = Path(__file__).resolve().parents[1]
PROJECT_ROOT = REPO_ROOT / "projects" / "g474_full_integration_test"
HEADER_PATH = PROJECT_ROOT / "Core" / "Inc" / "generated_adc_tests.h"
OUTPUT_DIR = REPO_ROOT / "tests" / "generated_adc"
MANIFEST_PATH = OUTPUT_DIR / "manifest.json"
VALIDATION_PATH = OUTPUT_DIR / "validation.csv"
WORST_CASE_SVG_PATH = OUTPUT_DIR / "q2_upper_phase_fold_validation.svg"
GALLERY_SVG_PATH = OUTPUT_DIR / "phase_fold_gallery.svg"

SAMPLE_COUNT = 2048
SAMPLE_RATE_HZ = 1_024_000.0
ADC_VREF_V = 3.3
ADC_CODE_COUNT = 4096.0
VOLTS_PER_CODE = ADC_VREF_V / ADC_CODE_COUNT
ADC_BIAS_V = 1.65
DISPLAY_POINT_COUNT = 256
DISPLAY_WIDTH = 794
FREQUENCY_RESOLUTION_HZ = 500.0
FREQUENCY_SEARCH_HALF_HZ = 500.0
FREQUENCY_SEARCH_STEP_HZ = 10.0

Q3_INTERFERENCE_INPUT_VPP_MV = 200.0
Q3_INTERFERENCE_ATTENUATION_DB = 40.0
Q3_INTERFERENCE_RESIDUAL_VPP_MV = (
    Q3_INTERFERENCE_INPUT_VPP_MV
    * 10.0 ** (-Q3_INTERFERENCE_ATTENUATION_DB / 20.0)
)


def write_utf8_lf(path: Path, content: str) -> None:
    """Write deterministic UTF-8 text with LF endings on all supported Python versions."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as output_file:
        output_file.write(content)


@dataclass(frozen=True)
class CaseDefinition:
    name: str
    requirement: int
    fundamental_hz: float
    relative_tones: tuple[tuple[int, float], ...]
    target_vpp_mv: float
    interference_hz: float = 0.0


@dataclass
class GeneratedCase:
    definition: CaseDefinition
    tones_mvpk: list[tuple[int, float]]
    expected_vrms_mv: float
    adc_codes: list[int]


CASES: tuple[CaseDefinition, ...] = (
    CaseDefinition(
        "q1_min_10k_100mVpp_1_3",
        1,
        10_000.0,
        ((1, 1.00), (3, 0.35)),
        100.0,
    ),
    CaseDefinition(
        "q1_official_10p5k_180mVpp_1_3_4",
        1,
        10_500.0,
        ((1, 1.00), (3, 0.50), (4, 0.30)),
        180.0,
    ),
    CaseDefinition(
        "q1_upper_49p75k_250mVpp_1_2_4",
        1,
        49_750.0,
        ((1, 1.00), (2, 0.35), (4, 0.20)),
        250.0,
    ),
    CaseDefinition(
        "q2_min_10k_50mVpp_1_4",
        2,
        10_000.0,
        ((1, 1.00), (4, 0.30)),
        50.0,
    ),
    CaseDefinition(
        "q2_mid_120p25k_180mVpp_1_2_4",
        2,
        120_250.0,
        ((1, 1.00), (2, 0.33), (4, 0.17)),
        180.0,
    ),
    CaseDefinition(
        "q2_upper_249p75k_250mVpp_1_2",
        2,
        249_750.0,
        ((1, 1.00), (2, 0.27)),
        250.0,
    ),
    CaseDefinition(
        "q3_low_20k_50mVpp_1_3_4_j1p1m",
        3,
        20_000.0,
        ((1, 1.00), (3, 0.40), (4, 0.20)),
        50.0,
        1_100_000.0,
    ),
    CaseDefinition(
        "q3_mid_80p25k_180mVpp_1_3_5_j1p25m",
        3,
        80_250.0,
        ((1, 1.00), (3, 0.40), (5, 0.18)),
        180.0,
        1_250_000.0,
    ),
    CaseDefinition(
        "q3_upper_124p75k_250mVpp_1_2_4_j1p3m",
        3,
        124_750.0,
        ((1, 1.00), (2, 0.32), (4, 0.16)),
        250.0,
        1_300_000.0,
    ),
)


def waveform_mv(
    phase_cycles: float,
    tones_mvpk: Sequence[tuple[int, float]],
) -> float:
    return sum(
        amplitude_mv
        * math.sin(2.0 * math.pi * harmonic * phase_cycles)
        for harmonic, amplitude_mv in tones_mvpk
    )


def quantize_frequency_hz(frequency_hz: float) -> float:
    """Model the teammate 2048-point FFT's 500 Hz output grid."""
    return (
        math.floor(frequency_hz / FREQUENCY_RESOLUTION_HZ + 0.5)
        * FREQUENCY_RESOLUTION_HZ
    )


def reported_component_frequencies(
    case: GeneratedCase,
) -> list[float]:
    return [
        quantize_frequency_hz(case.definition.fundamental_hz * harmonic)
        for harmonic, _ in case.tones_mvpk
    ]


def dense_vpp_mv(tones_mvpk: Sequence[tuple[int, float]]) -> float:
    values = [
        waveform_mv(index / 131_072.0, tones_mvpk)
        for index in range(131_072)
    ]
    return max(values) - min(values)


def scale_tones(definition: CaseDefinition) -> list[tuple[int, float]]:
    unscaled = [
        (harmonic, relative_amplitude)
        for harmonic, relative_amplitude in definition.relative_tones
    ]
    scale = definition.target_vpp_mv / dense_vpp_mv(unscaled)
    return [
        (harmonic, relative_amplitude * scale)
        for harmonic, relative_amplitude in unscaled
    ]


def generate_adc_codes(
    definition: CaseDefinition,
    tones_mvpk: Sequence[tuple[int, float]],
    *,
    interference_residual_vpp_mv: float | None = None,
) -> list[int]:
    if interference_residual_vpp_mv is None:
        interference_residual_vpp_mv = (
            Q3_INTERFERENCE_RESIDUAL_VPP_MV
            if definition.requirement == 3
            else 0.0
        )

    interference_peak_v = interference_residual_vpp_mv / 2000.0
    codes: list[int] = []

    for sample_index in range(SAMPLE_COUNT):
        time_s = sample_index / SAMPLE_RATE_HZ
        phase_cycles = definition.fundamental_hz * time_s
        signal_v = waveform_mv(phase_cycles, tones_mvpk) / 1000.0

        if definition.interference_hz > 0.0:
            signal_v += interference_peak_v * math.sin(
                2.0
                * math.pi
                * definition.interference_hz
                * time_s
            )

        adc_voltage_v = ADC_BIAS_V + signal_v
        code = round(adc_voltage_v / VOLTS_PER_CODE)
        codes.append(max(0, min(4095, code)))

    return codes


def ideal_waveform(
    tones_mvpk: Sequence[tuple[int, float]],
    point_count: int = DISPLAY_POINT_COUNT,
) -> list[float]:
    return [
        waveform_mv(index / point_count, tones_mvpk)
        for index in range(point_count)
    ]


def legacy_first_period_waveform(
    adc_codes: Sequence[int],
    fundamental_hz: float,
) -> list[float]:
    samples_per_period = SAMPLE_RATE_HZ / fundamental_hz
    waveform: list[float] = []

    for index in range(DISPLAY_POINT_COUNT):
        source_position = (
            index * samples_per_period / DISPLAY_POINT_COUNT
        )
        index0 = int(source_position)
        index1 = min(index0 + 1, SAMPLE_COUNT - 1)
        fraction = source_position - index0
        code = (
            adc_codes[index0]
            + fraction * (adc_codes[index1] - adc_codes[index0])
        )
        waveform.append(code * VOLTS_PER_CODE * 1000.0)

    mean_mv = sum(waveform) / len(waveform)
    return [value - mean_mv for value in waveform]


def phase_fold_waveform(
    adc_codes: Sequence[int],
    fundamental_hz: float,
) -> list[float]:
    sums_mv = [0.0] * DISPLAY_POINT_COUNT
    weights = [0.0] * DISPLAY_POINT_COUNT
    mean_code = sum(adc_codes) / len(adc_codes)
    phase_cycles = 0.0
    phase_step = fundamental_hz / SAMPLE_RATE_HZ

    for code in adc_codes:
        phase_position = phase_cycles * DISPLAY_POINT_COUNT
        index0 = int(phase_position) % DISPLAY_POINT_COUNT
        fraction = phase_position - int(phase_position)
        index1 = (index0 + 1) % DISPLAY_POINT_COUNT
        sample_mv = (code - mean_code) * VOLTS_PER_CODE * 1000.0

        weight0 = 1.0 - fraction
        sums_mv[index0] += sample_mv * weight0
        weights[index0] += weight0

        if fraction > 1.0e-6:
            sums_mv[index1] += sample_mv * fraction
            weights[index1] += fraction

        phase_cycles += phase_step
        if phase_cycles >= 1.0:
            phase_cycles -= math.floor(phase_cycles)

    waveform: list[float | None] = [
        (sample_sum / weight) if weight > 1.0e-6 else None
        for sample_sum, weight in zip(sums_mv, weights)
    ]
    valid_indices = [
        index for index, value in enumerate(waveform) if value is not None
    ]
    if not valid_indices:
        return [0.0] * DISPLAY_POINT_COUNT

    for index, value in enumerate(waveform):
        if value is not None:
            continue

        left = max(
            (valid for valid in valid_indices if valid < index),
            default=valid_indices[-1] - DISPLAY_POINT_COUNT,
        )
        right = min(
            (valid for valid in valid_indices if valid > index),
            default=valid_indices[0] + DISPLAY_POINT_COUNT,
        )
        left_value = float(waveform[left % DISPLAY_POINT_COUNT])
        right_value = float(waveform[right % DISPLAY_POINT_COUNT])
        ratio = (index - left) / (right - left)
        waveform[index] = left_value + ratio * (right_value - left_value)

    result = [float(value) for value in waveform]
    mean_mv = sum(result) / len(result)
    return [value - mean_mv for value in result]


def correlation_score(
    adc_codes: Sequence[int],
    mean_code: float,
    frequency_hz: float,
) -> float:
    if frequency_hz <= 0.0 or frequency_hz >= SAMPLE_RATE_HZ * 0.5:
        return -1.0

    angle_step = 2.0 * math.pi * frequency_hz / SAMPLE_RATE_HZ
    step_real = math.cos(angle_step)
    step_imag = math.sin(angle_step)
    oscillator_real = 1.0
    oscillator_imag = 0.0
    accumulator_real = 0.0
    accumulator_imag = 0.0

    for code in adc_codes:
        sample = code - mean_code
        accumulator_real += sample * oscillator_real
        accumulator_imag -= sample * oscillator_imag
        oscillator_real, oscillator_imag = (
            oscillator_real * step_real
            - oscillator_imag * step_imag,
            oscillator_imag * step_real
            + oscillator_real * step_imag,
        )

    return (
        accumulator_real * accumulator_real
        + accumulator_imag * accumulator_imag
    )


def refine_fundamental_for_waveform(
    adc_codes: Sequence[int],
    rough_fundamental_hz: float,
    component_frequencies_hz: Sequence[float],
    component_amplitudes_mv: Sequence[float],
) -> float:
    """Mirror the bridge's correlation search used before phase folding."""
    harmonic_orders = [
        max(1, round(frequency_hz / rough_fundamental_hz))
        for frequency_hz in component_frequencies_hz
    ]
    reference_index = max(
        range(len(component_amplitudes_mv)),
        key=lambda index: (
            component_amplitudes_mv[index]
            * harmonic_orders[index]
        ),
    )
    harmonic_order = harmonic_orders[reference_index]
    mean_code = sum(adc_codes) / len(adc_codes)
    search_count = (
        round(
            2.0
            * FREQUENCY_SEARCH_HALF_HZ
            / FREQUENCY_SEARCH_STEP_HZ
        )
        + 1
    )
    scores: list[float] = []

    for index in range(search_count):
        candidate_fundamental_hz = (
            rough_fundamental_hz
            - FREQUENCY_SEARCH_HALF_HZ
            + index * FREQUENCY_SEARCH_STEP_HZ
        )
        scores.append(
            correlation_score(
                adc_codes,
                mean_code,
                candidate_fundamental_hz * harmonic_order,
            )
        )

    best_index = max(range(search_count), key=scores.__getitem__)
    fractional_offset = 0.0
    if 0 < best_index < search_count - 1:
        left, center, right = scores[
            best_index - 1 : best_index + 2
        ]
        denominator = left - 2.0 * center + right
        if abs(denominator) > 1.0e-6:
            fractional_offset = max(
                -1.0,
                min(
                    1.0,
                    0.5 * (left - right) / denominator,
                ),
            )

    return (
        rough_fundamental_hz
        - FREQUENCY_SEARCH_HALF_HZ
        + (best_index + fractional_offset)
        * FREQUENCY_SEARCH_STEP_HZ
    )


def display_interpolate(
    waveform: Sequence[float],
    periods: int,
) -> list[float]:
    output: list[float] = []
    count = len(waveform)

    for index in range(DISPLAY_WIDTH):
        source_position = (
            index * periods * count / (DISPLAY_WIDTH - 1)
        )
        integer_position = int(source_position)
        fraction = source_position - integer_position
        index0 = integer_position % count
        index1 = (index0 + 1) % count
        output.append(
            waveform[index0]
            + fraction * (waveform[index1] - waveform[index0])
        )

    return output


def ideal_display(
    tones_mvpk: Sequence[tuple[int, float]],
    periods: int,
) -> list[float]:
    return [
        waveform_mv(
            periods * index / (DISPLAY_WIDTH - 1),
            tones_mvpk,
        )
        for index in range(DISPLAY_WIDTH)
    ]


def metrics(
    actual: Sequence[float],
    expected: Sequence[float],
) -> tuple[float, float, float]:
    errors = [
        actual_value - expected_value
        for actual_value, expected_value in zip(actual, expected)
    ]
    rmse_mv = math.sqrt(
        sum(error * error for error in errors) / len(errors)
    )
    max_error_mv = max(abs(error) for error in errors)
    vpp_error_mv = (
        (max(actual) - min(actual))
        - (max(expected) - min(expected))
    )
    return rmse_mv, max_error_mv, vpp_error_mv


def generate_cases() -> list[GeneratedCase]:
    generated: list[GeneratedCase] = []

    for definition in CASES:
        tones_mvpk = scale_tones(definition)
        expected_vrms_mv = math.sqrt(
            sum(amplitude * amplitude for _, amplitude in tones_mvpk) / 2.0
        )
        generated.append(
            GeneratedCase(
                definition=definition,
                tones_mvpk=tones_mvpk,
                expected_vrms_mv=expected_vrms_mv,
                adc_codes=generate_adc_codes(definition, tones_mvpk),
            )
        )

    return generated


def c_float(value: float) -> str:
    text = f"{value:.9g}"
    if "." not in text and "e" not in text.lower():
        text += ".0"
    return text + "f"


def c_array(values: Iterable[int], indent: str = "    ") -> str:
    values_list = list(values)
    lines: list[str] = []
    for start in range(0, len(values_list), 16):
        chunk = values_list[start : start + 16]
        suffix = "," if start + 16 < len(values_list) else ""
        lines.append(
            indent + ", ".join(str(value) for value in chunk) + suffix
        )
    return "\n".join(lines)


def write_header(cases: Sequence[GeneratedCase]) -> None:
    HEADER_PATH.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "/*",
        " * AUTO-GENERATED by tools/generate_g_problem_adc_tests.py.",
        " * Do not hand-edit; rerun the generator instead.",
        " */",
        "#ifndef GENERATED_ADC_TESTS_H",
        "#define GENERATED_ADC_TESTS_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define GENERATED_ADC_SAMPLE_COUNT {SAMPLE_COUNT}U",
        (
            "#define GENERATED_ADC_SAMPLE_RATE_HZ "
            f"{c_float(SAMPLE_RATE_HZ)}"
        ),
        (
            "#define GENERATED_ADC_VOLTS_PER_CODE "
            f"({c_float(ADC_VREF_V)} / {c_float(ADC_CODE_COUNT)})"
        ),
        f"#define GENERATED_ADC_TEST_CASE_COUNT {len(cases)}U",
        "",
        "typedef struct",
        "{",
        "    const char *name;",
        "    uint8_t test_number;",
        "    uint8_t requirement;",
        "    float fundamental_hz;",
        "    float expected_vpp_mv;",
        "    float expected_vrms_mv;",
        "    uint8_t component_count;",
        "    float component_frequencies_hz[3];",
        "    float component_amplitudes_mv[3];",
        "    float interference_hz;",
        "    float input_interference_vpp_mv;",
        "    float residual_interference_vpp_mv;",
        "    const uint16_t *adc_codes;",
        "} GeneratedAdcTestCase;",
        "",
    ]

    for test_number, case in enumerate(cases, start=1):
        symbol = f"s_adc_{case.definition.name}"
        lines.extend(
            [
                f"static const uint16_t {symbol}[GENERATED_ADC_SAMPLE_COUNT] =",
                "{",
                c_array(case.adc_codes),
                "};",
                "",
            ]
        )

    lines.extend(
        [
            (
                "static const GeneratedAdcTestCase "
                "s_generated_adc_test_cases[GENERATED_ADC_TEST_CASE_COUNT] ="
            ),
            "{",
        ]
    )

    for case in cases:
        frequencies = reported_component_frequencies(case)
        amplitudes = [amplitude for _, amplitude in case.tones_mvpk]
        while len(frequencies) < 3:
            frequencies.append(0.0)
            amplitudes.append(0.0)

        interference_input = (
            Q3_INTERFERENCE_INPUT_VPP_MV
            if case.definition.requirement == 3
            else 0.0
        )
        interference_residual = (
            Q3_INTERFERENCE_RESIDUAL_VPP_MV
            if case.definition.requirement == 3
            else 0.0
        )

        lines.extend(
            [
                "    {",
                f'        "{case.definition.name}",',
                f"        {test_number}U,",
                f"        {case.definition.requirement}U,",
                f"        {c_float(frequencies[0])},",
                f"        {c_float(case.definition.target_vpp_mv)},",
                f"        {c_float(case.expected_vrms_mv)},",
                f"        {len(case.tones_mvpk)}U,",
                (
                    "        {"
                    + ", ".join(c_float(value) for value in frequencies)
                    + "},"
                ),
                (
                    "        {"
                    + ", ".join(c_float(value) for value in amplitudes)
                    + "},"
                ),
                f"        {c_float(case.definition.interference_hz)},",
                f"        {c_float(interference_input)},",
                f"        {c_float(interference_residual)},",
                f"        s_adc_{case.definition.name}",
                "    },",
            ]
        )

    lines.extend(["};", "", "#endif", ""])
    write_utf8_lf(HEADER_PATH, "\n".join(lines))


def write_manifest(cases: Sequence[GeneratedCase]) -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    manifest = {
        "format": {
            "c_type": "uint16_t",
            "sample_count": SAMPLE_COUNT,
            "sample_rate_hz": SAMPLE_RATE_HZ,
            "adc_vref_v": ADC_VREF_V,
            "adc_code_count": int(ADC_CODE_COUNT),
            "adc_bias_v": ADC_BIAS_V,
            "volts_per_code": VOLTS_PER_CODE,
            "teammate_frequency_resolution_hz": (
                FREQUENCY_RESOLUTION_HZ
            ),
        },
        "phase_policy": (
            "All measured components use zero relative phase, matching the "
            "official Q&A. Sampling starts at common phase zero."
        ),
        "q3_interference_model": {
            "input_vpp_mv": Q3_INTERFERENCE_INPUT_VPP_MV,
            "analog_attenuation_db": Q3_INTERFERENCE_ATTENUATION_DB,
            "adc_residual_vpp_mv": Q3_INTERFERENCE_RESIDUAL_VPP_MV,
            "note": (
                "Compiled Q3 vectors model the ADC-side signal after ideal "
                "40 dB analog suppression. Unfiltered 200 mVpp interference "
                "is evaluated offline because it aliases below Nyquist."
            ),
        },
        "cases": [],
    }

    for test_number, case in enumerate(cases, start=1):
        manifest["cases"].append(
            {
                "test_number": test_number,
                "name": case.definition.name,
                "requirement": case.definition.requirement,
                "true_fundamental_hz": (
                    case.definition.fundamental_hz
                ),
                "reported_fundamental_hz": (
                    reported_component_frequencies(case)[0]
                ),
                "target_vpp_mv": case.definition.target_vpp_mv,
                "expected_vrms_mv": case.expected_vrms_mv,
                "components": [
                    {
                        "harmonic": harmonic,
                        "true_frequency_hz": (
                            case.definition.fundamental_hz * harmonic
                        ),
                        "reported_frequency_hz": (
                            reported_component_frequencies(case)[index]
                        ),
                        "amplitude_mvpk": amplitude,
                        "relative_phase_rad": 0.0,
                    }
                    for index, (harmonic, amplitude)
                    in enumerate(case.tones_mvpk)
                ],
                "interference_hz": case.definition.interference_hz,
                "adc_code_min": min(case.adc_codes),
                "adc_code_max": max(case.adc_codes),
            }
        )

    write_utf8_lf(
        MANIFEST_PATH,
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
    )


def validation_rows(cases: Sequence[GeneratedCase]) -> list[dict[str, float | str | int]]:
    rows: list[dict[str, float | str | int]] = []

    for test_number, case in enumerate(cases, start=1):
        component_frequencies_hz = reported_component_frequencies(case)
        component_amplitudes_mv = [
            amplitude for _, amplitude in case.tones_mvpk
        ]
        rough_fundamental_hz = component_frequencies_hz[0]
        refined_fundamental_hz = (
            refine_fundamental_for_waveform(
                case.adc_codes,
                rough_fundamental_hz,
                component_frequencies_hz,
                component_amplitudes_mv,
            )
        )
        reference = ideal_waveform(case.tones_mvpk)
        legacy = legacy_first_period_waveform(
            case.adc_codes,
            rough_fundamental_hz,
        )
        folded = phase_fold_waveform(
            case.adc_codes,
            refined_fundamental_hz,
        )
        legacy_metrics = metrics(legacy, reference)
        folded_metrics = metrics(folded, reference)
        display_1t_metrics = metrics(
            display_interpolate(folded, 1),
            ideal_display(case.tones_mvpk, 1),
        )
        display_3t_metrics = metrics(
            display_interpolate(folded, 3),
            ideal_display(case.tones_mvpk, 3),
        )

        prefilter_alias_rmse_mv = 0.0
        if case.definition.requirement == 3:
            unfiltered_codes = generate_adc_codes(
                case.definition,
                case.tones_mvpk,
                interference_residual_vpp_mv=(
                    Q3_INTERFERENCE_INPUT_VPP_MV
                ),
            )
            unfiltered_folded = phase_fold_waveform(
                unfiltered_codes,
                refine_fundamental_for_waveform(
                    unfiltered_codes,
                    rough_fundamental_hz,
                    component_frequencies_hz,
                    component_amplitudes_mv,
                ),
            )
            prefilter_alias_rmse_mv = metrics(
                unfiltered_folded,
                reference,
            )[0]

        rows.append(
            {
                "test_number": test_number,
                "name": case.definition.name,
                "requirement": case.definition.requirement,
                "true_fundamental_hz": (
                    case.definition.fundamental_hz
                ),
                "reported_fundamental_hz": rough_fundamental_hz,
                "refined_fundamental_hz": refined_fundamental_hz,
                "refined_frequency_error_hz": (
                    refined_fundamental_hz
                    - case.definition.fundamental_hz
                ),
                "samples_per_period": (
                    SAMPLE_RATE_HZ / case.definition.fundamental_hz
                ),
                "legacy_rmse_mv": legacy_metrics[0],
                "legacy_max_error_mv": legacy_metrics[1],
                "legacy_vpp_error_mv": legacy_metrics[2],
                "phase_fold_rmse_mv": folded_metrics[0],
                "phase_fold_max_error_mv": folded_metrics[1],
                "phase_fold_vpp_error_mv": folded_metrics[2],
                "display_1t_rmse_mv": display_1t_metrics[0],
                "display_3t_rmse_mv": display_3t_metrics[0],
                "q3_unfiltered_alias_rmse_mv": prefilter_alias_rmse_mv,
            }
        )

    return rows


def write_validation(
    rows: Sequence[dict[str, float | str | int]],
) -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    fieldnames = list(rows[0].keys())
    with VALIDATION_PATH.open("w", encoding="utf-8", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(
                {
                    key: (
                        f"{value:.6f}"
                        if isinstance(value, float)
                        else value
                    )
                    for key, value in row.items()
                }
            )


def polyline_points(
    values: Sequence[float],
    x0: float,
    y0: float,
    width: float,
    height: float,
    y_abs_max: float,
) -> str:
    points = []
    for index, value in enumerate(values):
        x = x0 + width * index / (len(values) - 1)
        y = y0 + height * (0.5 - 0.45 * value / y_abs_max)
        points.append(f"{x:.2f},{y:.2f}")
    return " ".join(points)


def write_worst_case_svg(cases: Sequence[GeneratedCase]) -> None:
    case = next(
        item
        for item in cases
        if item.definition.name == "q2_upper_249p75k_250mVpp_1_2"
    )
    reported_frequencies = reported_component_frequencies(case)
    refined_fundamental_hz = refine_fundamental_for_waveform(
        case.adc_codes,
        reported_frequencies[0],
        reported_frequencies,
        [amplitude for _, amplitude in case.tones_mvpk],
    )
    ideal = ideal_waveform(case.tones_mvpk)
    legacy = legacy_first_period_waveform(
        case.adc_codes,
        reported_frequencies[0],
    )
    folded = phase_fold_waveform(
        case.adc_codes,
        refined_fundamental_hz,
    )
    y_abs_max = max(abs(value) for value in ideal) * 1.10
    x0, y0, width, height = 70.0, 70.0, 850.0, 360.0

    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" width="1000" height="520" viewBox="0 0 1000 520">
  <rect width="1000" height="520" fill="#ffffff"/>
  <text x="70" y="35" font-family="Arial, sans-serif" font-size="22">Q2 upper edge: true 249.75 kHz, reported 250.0 kHz</text>
  <rect x="{x0}" y="{y0}" width="{width}" height="{height}" fill="#101418" stroke="#708090"/>
  <line x1="{x0}" y1="{y0 + height / 2}" x2="{x0 + width}" y2="{y0 + height / 2}" stroke="#607080"/>
  <polyline points="{polyline_points(ideal, x0, y0, width, height, y_abs_max)}" fill="none" stroke="#5ee27a" stroke-width="3"/>
  <polyline points="{polyline_points(legacy, x0, y0, width, height, y_abs_max)}" fill="none" stroke="#ff6b6b" stroke-width="2"/>
  <polyline points="{polyline_points(folded, x0, y0, width, height, y_abs_max)}" fill="none" stroke="#5ba9ff" stroke-width="2"/>
  <line x1="90" y1="465" x2="130" y2="465" stroke="#5ee27a" stroke-width="4"/>
  <text x="140" y="472" font-family="Arial, sans-serif" font-size="18">ideal</text>
  <line x1="250" y1="465" x2="290" y2="465" stroke="#ff6b6b" stroke-width="4"/>
  <text x="300" y="472" font-family="Arial, sans-serif" font-size="18">legacy first-period stretch</text>
  <line x1="590" y1="465" x2="630" y2="465" stroke="#5ba9ff" stroke-width="4"/>
  <text x="640" y="472" font-family="Arial, sans-serif" font-size="18">full-buffer phase fold</text>
</svg>
"""
    write_utf8_lf(WORST_CASE_SVG_PATH, svg)


def write_phase_fold_gallery(cases: Sequence[GeneratedCase]) -> None:
    """Write one visual comparison panel per native ADC test vector."""
    panel_width = 460.0
    panel_height = 240.0
    columns = 2
    rows = math.ceil(len(cases) / columns)
    svg_width = int(panel_width * columns + 60.0)
    svg_height = int(panel_height * rows + 90.0)
    panels: list[str] = []

    for test_number, case in enumerate(cases, start=1):
        column = (test_number - 1) % columns
        row = (test_number - 1) // columns
        panel_x = 30.0 + column * panel_width
        panel_y = 55.0 + row * panel_height
        plot_x = panel_x + 15.0
        plot_y = panel_y + 55.0
        plot_width = panel_width - 30.0
        plot_height = 145.0
        frequencies = reported_component_frequencies(case)
        refined_fundamental_hz = refine_fundamental_for_waveform(
            case.adc_codes,
            frequencies[0],
            frequencies,
            [amplitude for _, amplitude in case.tones_mvpk],
        )
        ideal = ideal_waveform(case.tones_mvpk)
        folded = phase_fold_waveform(
            case.adc_codes,
            refined_fundamental_hz,
        )
        rmse_mv, max_error_mv, _ = metrics(folded, ideal)
        y_abs_max = max(
            max(abs(value) for value in ideal),
            max(abs(value) for value in folded),
            1.0,
        ) * 1.10

        panels.append(
            f"""
  <g>
    <text x="{panel_x:.1f}" y="{panel_y + 18.0:.1f}" font-family="Arial, sans-serif" font-size="16" font-weight="bold">T{test_number}  {case.definition.name}</text>
    <text x="{panel_x:.1f}" y="{panel_y + 40.0:.1f}" font-family="Arial, sans-serif" font-size="13">RMSE {rmse_mv:.3f} mV, max error {max_error_mv:.3f} mV</text>
    <rect x="{plot_x:.1f}" y="{plot_y:.1f}" width="{plot_width:.1f}" height="{plot_height:.1f}" fill="#101418" stroke="#708090"/>
    <line x1="{plot_x:.1f}" y1="{plot_y + plot_height / 2.0:.1f}" x2="{plot_x + plot_width:.1f}" y2="{plot_y + plot_height / 2.0:.1f}" stroke="#607080"/>
    <polyline points="{polyline_points(ideal, plot_x, plot_y, plot_width, plot_height, y_abs_max)}" fill="none" stroke="#5ee27a" stroke-width="2.5"/>
    <polyline points="{polyline_points(folded, plot_x, plot_y, plot_width, plot_height, y_abs_max)}" fill="none" stroke="#5ba9ff" stroke-width="1.5"/>
  </g>"""
        )

    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" width="{svg_width}" height="{svg_height}" viewBox="0 0 {svg_width} {svg_height}">
  <rect width="{svg_width}" height="{svg_height}" fill="#ffffff"/>
  <text x="30" y="30" font-family="Arial, sans-serif" font-size="22">Native ADC phase-fold validation gallery</text>
  <line x1="{svg_width - 250}" y1="24" x2="{svg_width - 215}" y2="24" stroke="#5ee27a" stroke-width="4"/>
  <text x="{svg_width - 205}" y="30" font-family="Arial, sans-serif" font-size="14">ideal</text>
  <line x1="{svg_width - 135}" y1="24" x2="{svg_width - 100}" y2="24" stroke="#5ba9ff" stroke-width="4"/>
  <text x="{svg_width - 90}" y="30" font-family="Arial, sans-serif" font-size="14">folded</text>
{''.join(panels)}
</svg>
"""
    write_utf8_lf(GALLERY_SVG_PATH, svg)


def main() -> None:
    cases = generate_cases()
    rows = validation_rows(cases)
    write_header(cases)
    write_manifest(cases)
    write_validation(rows)
    write_worst_case_svg(cases)
    write_phase_fold_gallery(cases)

    worst_legacy = max(float(row["legacy_rmse_mv"]) for row in rows)
    worst_folded = max(float(row["phase_fold_rmse_mv"]) for row in rows)
    worst_display = max(
        max(
            float(row["display_1t_rmse_mv"]),
            float(row["display_3t_rmse_mv"]),
        )
        for row in rows
    )
    print(f"generated {len(cases)} native ADC cases")
    print(f"header: {HEADER_PATH}")
    print(f"manifest: {MANIFEST_PATH}")
    print(f"validation: {VALIDATION_PATH}")
    print(f"gallery: {GALLERY_SVG_PATH}")
    print(f"worst legacy waveform RMSE: {worst_legacy:.3f} mV")
    print(f"worst phase-fold waveform RMSE: {worst_folded:.3f} mV")
    print(f"worst display interpolation RMSE: {worst_display:.3f} mV")


if __name__ == "__main__":
    main()
