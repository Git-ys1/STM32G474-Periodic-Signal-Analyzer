#!/usr/bin/env python3
"""Compare ordinary and two-pass Huber phase folding on T101--T108."""

from __future__ import annotations

import csv
import json
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Tuple

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

from waveform_lab_core import (
    DEFAULT_DISPLAY_WIDTH,
    DEFAULT_PHASE_BIN_COUNT,
    REPO_ROOT,
    SimulationResult,
    WaveformSpec,
    huber_phase_fold_waveform,
    load_registry,
    periodic_linear_display,
    phase_coverage,
    phase_fold_waveform,
    refine_fundamental_hz,
    simulate,
)


OUTPUT_ROOT = REPO_ROOT / "tests" / "huber_phase_fold"
RANDOM_SEED = 20260731
HUBER_K = 1.345
MINIMUM_DELTA_LSB = 1.5
MEDIUM_COVERAGE_THRESHOLD = 0.5


@dataclass(frozen=True)
class Contamination:
    name: str
    sample_count: int
    amplitude_mv: float
    is_burst: bool = False


@dataclass
class Evaluation:
    row: Dict[str, object]
    ideal_display_mv: np.ndarray
    ordinary_display_mv: np.ndarray
    huber_display_mv: np.ndarray


CONTAMINATIONS = (
    Contamination("isolated_4x20mv", 4, 20.0),
    Contamination("isolated_16x40mv", 16, 40.0),
    Contamination("isolated_32x80mv", 32, 80.0),
    Contamination("burst_8x40mv", 8, 40.0, is_burst=True),
)

CSV_FIELDS = (
    "test_number",
    "test_name",
    "contamination_type",
    "frequency_condition",
    "injected_outlier_count",
    "outlier_amplitude_mv",
    "injected_code_delta",
    "phase_bin_count",
    "fold_frequency_hz",
    "clean_refined_frequency_hz",
    "contaminated_refined_frequency_hz",
    "frequency_shift_hz",
    "hard_bin_coverage",
    "maximum_phase_gap_bins",
    "ordinary_phase_rmse_mv",
    "huber_phase_rmse_mv",
    "ordinary_phase_max_error_mv",
    "huber_phase_max_error_mv",
    "ordinary_display_rmse_mv",
    "huber_display_rmse_mv",
    "ordinary_display_max_error_mv",
    "huber_display_max_error_mv",
    "ordinary_vpp_error_mv",
    "huber_vpp_error_mv",
    "robust_scale_mv",
    "huber_delta_mv",
    "downweighted_sample_count",
    "downweighted_fraction",
    "minimum_sample_weight",
)


def _rmse(actual: np.ndarray, expected: np.ndarray) -> float:
    return float(np.sqrt(np.mean(np.square(actual - expected))))


def _maximum_error(
    actual: np.ndarray,
    expected: np.ndarray,
) -> float:
    return float(np.max(np.abs(actual - expected)))


def _improvement_percent(
    ordinary_error: float,
    huber_error: float,
) -> float:
    if ordinary_error <= 1.0e-12:
        return 0.0
    return 100.0 * (ordinary_error - huber_error) / ordinary_error


def _inject_contamination(
    adc_codes: np.ndarray,
    volts_per_code: float,
    contamination: Contamination,
    seed: int,
) -> Tuple[np.ndarray, np.ndarray, int]:
    """Inject deterministic, sign-balanced outliers directly in ADC codes."""

    rng = np.random.RandomState(seed)
    code_delta = max(
        1,
        int(
            np.rint(
                contamination.amplitude_mv
                / (volts_per_code * 1000.0)
            )
        ),
    )
    if contamination.is_burst:
        start = int(
            rng.randint(0, adc_codes.size - contamination.sample_count + 1)
        )
        indices = np.arange(
            start,
            start + contamination.sample_count,
            dtype=np.int64,
        )
        signs = np.ones(contamination.sample_count, dtype=np.int64)
        signs[contamination.sample_count // 2 :] = -1
        if int(rng.randint(0, 2)):
            signs *= -1
    else:
        indices = np.sort(
            rng.choice(
                adc_codes.size,
                size=contamination.sample_count,
                replace=False,
            )
        )
        signs = np.ones(contamination.sample_count, dtype=np.int64)
        signs[1::2] = -1
        rng.shuffle(signs)

    contaminated = adc_codes.astype(np.int64)
    contaminated[indices] += signs * code_delta
    contaminated = np.clip(contaminated, 0, 4095).astype(np.uint16)
    return contaminated, indices, code_delta


def _evaluate(
    spec: WaveformSpec,
    adc_codes: np.ndarray,
    clean_result: SimulationResult,
    contamination_name: str,
    injected_count: int,
    outlier_amplitude_mv: float,
    injected_code_delta: int,
    frequency_condition: str,
    fold_frequency_hz: float,
    contaminated_refined_hz: float,
) -> Evaluation:
    volts_per_code = spec.adc_vref_v / 4096.0
    ordinary_phase, _, _ = phase_fold_waveform(
        adc_codes,
        volts_per_code,
        spec.sample_rate_hz,
        fold_frequency_hz,
        DEFAULT_PHASE_BIN_COUNT,
    )
    (
        huber_phase,
        _,
        _,
        diagnostics,
    ) = huber_phase_fold_waveform(
        adc_codes,
        volts_per_code,
        spec.sample_rate_hz,
        fold_frequency_hz,
        DEFAULT_PHASE_BIN_COUNT,
        HUBER_K,
        MINIMUM_DELTA_LSB,
    )
    ordinary_display = periodic_linear_display(ordinary_phase, 1)
    huber_display = periodic_linear_display(huber_phase, 1)
    coverage = phase_coverage(
        fold_frequency_hz,
        spec.sample_rate_hz,
        spec.sample_count,
        DEFAULT_PHASE_BIN_COUNT,
        spec.start_phase_deg,
    )

    ideal_phase = clean_result.ideal_phase_waveform_mv
    ideal_display = clean_result.ideal_display_waveform_mv
    ordinary_vpp = float(np.ptp(ordinary_phase))
    huber_vpp = float(np.ptp(huber_phase))
    row: Dict[str, object] = {
        "test_number": spec.group_number,
        "test_name": spec.name,
        "contamination_type": contamination_name,
        "frequency_condition": frequency_condition,
        "injected_outlier_count": injected_count,
        "outlier_amplitude_mv": outlier_amplitude_mv,
        "injected_code_delta": injected_code_delta,
        "phase_bin_count": DEFAULT_PHASE_BIN_COUNT,
        "fold_frequency_hz": fold_frequency_hz,
        "clean_refined_frequency_hz": (
            clean_result.refined_fundamental_hz
        ),
        "contaminated_refined_frequency_hz": contaminated_refined_hz,
        "frequency_shift_hz": (
            contaminated_refined_hz
            - clean_result.refined_fundamental_hz
        ),
        "hard_bin_coverage": coverage.hard_bin_coverage,
        "maximum_phase_gap_bins": coverage.maximum_phase_gap_bins,
        "ordinary_phase_rmse_mv": _rmse(ordinary_phase, ideal_phase),
        "huber_phase_rmse_mv": _rmse(huber_phase, ideal_phase),
        "ordinary_phase_max_error_mv": _maximum_error(
            ordinary_phase,
            ideal_phase,
        ),
        "huber_phase_max_error_mv": _maximum_error(
            huber_phase,
            ideal_phase,
        ),
        "ordinary_display_rmse_mv": _rmse(
            ordinary_display,
            ideal_display,
        ),
        "huber_display_rmse_mv": _rmse(
            huber_display,
            ideal_display,
        ),
        "ordinary_display_max_error_mv": _maximum_error(
            ordinary_display,
            ideal_display,
        ),
        "huber_display_max_error_mv": _maximum_error(
            huber_display,
            ideal_display,
        ),
        "ordinary_vpp_error_mv": abs(
            ordinary_vpp - clean_result.expected_vpp_mv
        ),
        "huber_vpp_error_mv": abs(
            huber_vpp - clean_result.expected_vpp_mv
        ),
        **asdict(diagnostics),
    }
    return Evaluation(
        row=row,
        ideal_display_mv=ideal_display,
        ordinary_display_mv=ordinary_display,
        huber_display_mv=huber_display,
    )


def _clean_evaluation(
    spec: WaveformSpec,
    clean_result: SimulationResult,
) -> Evaluation:
    return _evaluate(
        spec=spec,
        adc_codes=clean_result.adc_codes,
        clean_result=clean_result,
        contamination_name="clean",
        injected_count=0,
        outlier_amplitude_mv=0.0,
        injected_code_delta=0,
        frequency_condition="clean_refined_frequency",
        fold_frequency_hz=clean_result.refined_fundamental_hz,
        contaminated_refined_hz=clean_result.refined_fundamental_hz,
    )


def _run_matrix() -> List[Evaluation]:
    evaluations: List[Evaluation] = []
    specs = sorted(load_registry(), key=lambda item: item.group_number)
    expected_numbers = list(range(101, 109))
    actual_numbers = [spec.group_number for spec in specs]
    if actual_numbers != expected_numbers:
        raise RuntimeError(
            "Huber回归要求注册表恰好包含T101~T108；当前为{}".format(
                actual_numbers
            )
        )

    for spec in specs:
        clean_result = simulate(
            spec,
            display_periods=1,
            phase_bin_count=DEFAULT_PHASE_BIN_COUNT,
        )
        evaluations.append(_clean_evaluation(spec, clean_result))
        volts_per_code = spec.adc_vref_v / 4096.0

        for scenario_index, contamination in enumerate(
            CONTAMINATIONS,
            start=1,
        ):
            seed = (
                RANDOM_SEED
                + spec.group_number * 100
                + scenario_index
            )
            (
                contaminated_codes,
                _,
                code_delta,
            ) = _inject_contamination(
                clean_result.adc_codes,
                volts_per_code,
                contamination,
                seed,
            )
            contaminated_refined_hz = refine_fundamental_hz(
                spec,
                contaminated_codes,
            )

            evaluations.append(
                _evaluate(
                    spec=spec,
                    adc_codes=contaminated_codes,
                    clean_result=clean_result,
                    contamination_name=contamination.name,
                    injected_count=contamination.sample_count,
                    outlier_amplitude_mv=contamination.amplitude_mv,
                    injected_code_delta=code_delta,
                    frequency_condition="fixed_clean_frequency",
                    fold_frequency_hz=(
                        clean_result.refined_fundamental_hz
                    ),
                    contaminated_refined_hz=contaminated_refined_hz,
                )
            )
            evaluations.append(
                _evaluate(
                    spec=spec,
                    adc_codes=contaminated_codes,
                    clean_result=clean_result,
                    contamination_name=contamination.name,
                    injected_count=contamination.sample_count,
                    outlier_amplitude_mv=contamination.amplitude_mv,
                    injected_code_delta=code_delta,
                    frequency_condition=(
                        "contaminated_refined_frequency"
                    ),
                    fold_frequency_hz=contaminated_refined_hz,
                    contaminated_refined_hz=contaminated_refined_hz,
                )
            )

    return evaluations


def _write_csv(evaluations: Iterable[Evaluation]) -> Path:
    path = OUTPUT_ROOT / "comparison.csv"
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=CSV_FIELDS)
        writer.writeheader()
        for evaluation in evaluations:
            writer.writerow(evaluation.row)
    return path


def _row_improvement(row: Dict[str, object]) -> float:
    return _improvement_percent(
        float(row["ordinary_display_rmse_mv"]),
        float(row["huber_display_rmse_mv"]),
    )


def _compact_case(row: Dict[str, object]) -> Dict[str, object]:
    return {
        "test_number": row["test_number"],
        "test_name": row["test_name"],
        "contamination_type": row["contamination_type"],
        "frequency_condition": row["frequency_condition"],
        "hard_bin_coverage": row["hard_bin_coverage"],
        "frequency_shift_hz": row["frequency_shift_hz"],
        "ordinary_display_rmse_mv": row[
            "ordinary_display_rmse_mv"
        ],
        "huber_display_rmse_mv": row["huber_display_rmse_mv"],
        "display_rmse_improvement_percent": _row_improvement(row),
        "ordinary_display_max_error_mv": row[
            "ordinary_display_max_error_mv"
        ],
        "huber_display_max_error_mv": row[
            "huber_display_max_error_mv"
        ],
        "downweighted_sample_count": row[
            "downweighted_sample_count"
        ],
    }


def _median(values: Iterable[float]) -> float:
    array = np.asarray(tuple(values), dtype=np.float64)
    return float(np.median(array)) if array.size else 0.0


def _build_summary(
    evaluations: List[Evaluation],
) -> Dict[str, object]:
    rows = [evaluation.row for evaluation in evaluations]
    clean_rows = [
        row for row in rows if row["contamination_type"] == "clean"
    ]
    contaminated_rows = [
        row for row in rows if row["contamination_type"] != "clean"
    ]
    isolated_rows = [
        row
        for row in contaminated_rows
        if str(row["contamination_type"]).startswith("isolated_")
    ]
    burst_rows = [
        row
        for row in contaminated_rows
        if str(row["contamination_type"]).startswith("burst_")
    ]
    medium_coverage_isolated = [
        row
        for row in isolated_rows
        if float(row["hard_bin_coverage"])
        >= MEDIUM_COVERAGE_THRESHOLD
    ]

    clean_details: List[Dict[str, object]] = []
    for row in clean_rows:
        ordinary_rmse = float(row["ordinary_display_rmse_mv"])
        huber_rmse = float(row["huber_display_rmse_mv"])
        allowed_increase = max(0.1, ordinary_rmse * 0.10)
        clean_details.append(
            {
                "test_number": row["test_number"],
                "hard_bin_coverage": row["hard_bin_coverage"],
                "ordinary_display_rmse_mv": ordinary_rmse,
                "huber_display_rmse_mv": huber_rmse,
                "display_rmse_increase_mv": (
                    huber_rmse - ordinary_rmse
                ),
                "allowed_display_rmse_increase_mv": allowed_increase,
                "display_regression_pass": (
                    huber_rmse - ordinary_rmse
                    <= allowed_increase + 1.0e-12
                ),
                "ordinary_vpp_error_mv": row["ordinary_vpp_error_mv"],
                "huber_vpp_error_mv": row["huber_vpp_error_mv"],
                "extra_vpp_error_mv": (
                    float(row["huber_vpp_error_mv"])
                    - float(row["ordinary_vpp_error_mv"])
                ),
                "downweighted_sample_count": row[
                    "downweighted_sample_count"
                ],
            }
        )

    def by_condition(
        source: Iterable[Dict[str, object]],
        condition: str,
    ) -> List[Dict[str, object]]:
        return [
            row
            for row in source
            if row["frequency_condition"] == condition
        ]

    fixed_isolated = by_condition(
        medium_coverage_isolated,
        "fixed_clean_frequency",
    )
    refined_isolated = by_condition(
        medium_coverage_isolated,
        "contaminated_refined_frequency",
    )
    fixed_burst = by_condition(burst_rows, "fixed_clean_frequency")
    refined_burst = by_condition(
        burst_rows,
        "contaminated_refined_frequency",
    )
    best_case = max(contaminated_rows, key=_row_improvement)
    worst_case = min(contaminated_rows, key=_row_improvement)
    largest_frequency_shift = max(
        contaminated_rows,
        key=lambda row: abs(float(row["frequency_shift_hz"])),
    )

    clean_pass = all(
        bool(detail["display_regression_pass"])
        for detail in clean_details
    )
    fixed_median = _median(
        _row_improvement(row) for row in fixed_isolated
    )
    refined_median = _median(
        _row_improvement(row) for row in refined_isolated
    )
    recommended_for_board_candidate = (
        clean_pass and fixed_median >= 30.0
    )
    conclusion = (
        "PC artificial-outlier validation supports a guarded STM32 trial, "
        "but real adc_b[2048] frames are still required."
        if recommended_for_board_candidate
        else "Do not port yet; the artificial-data acceptance target failed."
    )

    return {
        "schema_version": 1,
        "algorithm": {
            "name": "two-pass Huber phase fold",
            "huber_k": HUBER_K,
            "minimum_delta_lsb": MINIMUM_DELTA_LSB,
            "phase_bin_count": DEFAULT_PHASE_BIN_COUNT,
            "display_width": DEFAULT_DISPLAY_WIDTH,
            "global_smoothing": False,
            "interpolation": "periodic linear",
        },
        "test_matrix": {
            "test_numbers": list(range(101, 109)),
            "row_count": len(rows),
            "clean_row_count": len(clean_rows),
            "contaminated_row_count": len(contaminated_rows),
            "fixed_random_seed": RANDOM_SEED,
            "contaminations": [
                asdict(contamination)
                for contamination in CONTAMINATIONS
            ],
            "frequency_conditions": [
                "fixed_clean_frequency",
                "contaminated_refined_frequency",
            ],
        },
        "clean_regression": {
            "all_display_regression_pass": clean_pass,
            "maximum_display_rmse_increase_mv": max(
                float(detail["display_rmse_increase_mv"])
                for detail in clean_details
            ),
            "maximum_extra_vpp_error_mv": max(
                float(detail["extra_vpp_error_mv"])
                for detail in clean_details
            ),
            "details": clean_details,
        },
        "isolated_outliers_medium_or_high_coverage": {
            "coverage_threshold": MEDIUM_COVERAGE_THRESHOLD,
            "fixed_clean_frequency_case_count": len(fixed_isolated),
            "fixed_clean_frequency_median_display_rmse_improvement_percent": (
                fixed_median
            ),
            "contaminated_refined_frequency_case_count": len(
                refined_isolated
            ),
            "contaminated_refined_frequency_median_display_rmse_improvement_percent": (
                refined_median
            ),
            "fixed_clean_frequency_median_max_error_improvement_percent": (
                _median(
                    _improvement_percent(
                        float(row["ordinary_display_max_error_mv"]),
                        float(row["huber_display_max_error_mv"]),
                    )
                    for row in fixed_isolated
                )
            ),
            "contaminated_refined_frequency_median_max_error_improvement_percent": (
                _median(
                    _improvement_percent(
                        float(row["ordinary_display_max_error_mv"]),
                        float(row["huber_display_max_error_mv"]),
                    )
                    for row in refined_isolated
                )
            ),
        },
        "burst_outliers": {
            "fixed_clean_frequency_median_display_rmse_improvement_percent": (
                _median(_row_improvement(row) for row in fixed_burst)
            ),
            "contaminated_refined_frequency_median_display_rmse_improvement_percent": (
                _median(_row_improvement(row) for row in refined_burst)
            ),
            "known_boundary": (
                "A contiguous burst can influence adjacent phases and the "
                "first-pass predictor together; Huber is not a replacement "
                "for acquisition-quality checks."
            ),
        },
        "best_contaminated_case": _compact_case(best_case),
        "worst_contaminated_case": _compact_case(worst_case),
        "largest_frequency_shift_case": _compact_case(
            largest_frequency_shift
        ),
        "recommendation": {
            "artificial_data_supports_guarded_board_trial": (
                recommended_for_board_candidate
            ),
            "conclusion": conclusion,
            "real_adc_validation_pending": True,
            "estimated_stm32_cost": {
                "extra_ram_bytes": (
                    "about 8 KiB for one 2048-float residual workspace; "
                    "about 10 KiB if 256-bin sums/weights cannot reuse "
                    "existing buffers"
                ),
                "extra_flash_bytes": "roughly 1-3 KiB before measurement",
                "extra_compute": (
                    "one first-pass prediction/residual pass, two median "
                    "selections, and one additional weighted fold; roughly "
                    "2x the current folding work plus robust statistics"
                ),
            },
        },
    }


def _write_summary(summary: Dict[str, object]) -> Path:
    path = OUTPUT_ROOT / "summary.json"
    path.write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    return path


def _find_evaluation(
    evaluations: Iterable[Evaluation],
    test_number: int,
    contamination_type: str,
    frequency_condition: str,
) -> Evaluation:
    for evaluation in evaluations:
        row = evaluation.row
        if (
            row["test_number"] == test_number
            and row["contamination_type"] == contamination_type
            and row["frequency_condition"] == frequency_condition
        ):
            return evaluation
    raise KeyError(
        (test_number, contamination_type, frequency_condition)
    )


def _write_gallery(evaluations: List[Evaluation]) -> Path:
    burst_evaluations = [
        evaluation
        for evaluation in evaluations
        if str(evaluation.row["contamination_type"]).startswith(
            "burst_"
        )
    ]
    worst_burst = min(
        burst_evaluations,
        key=lambda evaluation: _row_improvement(evaluation.row),
    )
    panels = [
        (
            "Low frequency, high coverage",
            _find_evaluation(
                evaluations,
                101,
                "clean",
                "clean_refined_frequency",
            ),
        ),
        (
            "Composite harmonic waveform",
            _find_evaluation(
                evaluations,
                102,
                "clean",
                "clean_refined_frequency",
            ),
        ),
        (
            "High frequency, low coverage",
            _find_evaluation(
                evaluations,
                108,
                "clean",
                "clean_refined_frequency",
            ),
        ),
        (
            "Representative isolated outliers",
            _find_evaluation(
                evaluations,
                102,
                "isolated_16x40mv",
                "fixed_clean_frequency",
            ),
        ),
        (
            "Worst burst case in the full matrix",
            worst_burst,
        ),
    ]

    figure, axes = plt.subplots(3, 2, figsize=(15, 13))
    flat_axes = axes.ravel()
    display_phase = (
        np.arange(DEFAULT_DISPLAY_WIDTH, dtype=np.float64)
        / (DEFAULT_DISPLAY_WIDTH - 1)
    )
    for axis, (title, evaluation) in zip(flat_axes, panels):
        row = evaluation.row
        axis.plot(
            display_phase,
            evaluation.ideal_display_mv,
            color="#202020",
            linewidth=1.6,
            label="ideal",
        )
        axis.plot(
            display_phase,
            evaluation.ordinary_display_mv,
            color="#d95f02",
            linewidth=1.1,
            label="ordinary",
        )
        axis.plot(
            display_phase,
            evaluation.huber_display_mv,
            color="#1b9e77",
            linewidth=1.1,
            label="Huber",
        )
        axis.set_title(
            "{}\nT{} {}, {} / {}".format(
                title,
                row["test_number"],
                row["test_name"],
                row["contamination_type"],
                row["frequency_condition"],
            ),
            fontsize=10,
        )
        axis.text(
            0.01,
            0.02,
            (
                "coverage={:.1f}%  ordinary={:.3f} mV  "
                "Huber={:.3f} mV\n"
                "max: {:.3f}->{:.3f} mV  downweighted={}"
            ).format(
                float(row["hard_bin_coverage"]) * 100.0,
                float(row["ordinary_display_rmse_mv"]),
                float(row["huber_display_rmse_mv"]),
                float(row["ordinary_display_max_error_mv"]),
                float(row["huber_display_max_error_mv"]),
                row["downweighted_sample_count"],
            ),
            transform=axis.transAxes,
            fontsize=8,
            va="bottom",
            bbox={
                "facecolor": "white",
                "edgecolor": "none",
                "alpha": 0.78,
            },
        )
        axis.set_xlabel("base-cycle phase")
        axis.set_ylabel("centered voltage / mV")
        axis.grid(True, alpha=0.24)
        axis.legend(loc="upper right", fontsize=8)

    flat_axes[-1].axis("off")
    figure.suptitle(
        "Two-pass Huber phase fold: clean regression and outlier limits",
        fontsize=15,
        y=0.995,
    )
    figure.tight_layout(rect=(0.0, 0.0, 1.0, 0.98))
    path = OUTPUT_ROOT / "comparison_gallery.png"
    figure.savefig(path, dpi=170)
    plt.close(figure)
    return path


def main() -> int:
    OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
    evaluations = _run_matrix()
    csv_path = _write_csv(evaluations)
    summary = _build_summary(evaluations)
    summary_path = _write_summary(summary)
    gallery_path = _write_gallery(evaluations)

    isolated = summary[
        "isolated_outliers_medium_or_high_coverage"
    ]
    clean = summary["clean_regression"]
    print("rows:", summary["test_matrix"]["row_count"])
    print("clean regression pass:", clean["all_display_regression_pass"])
    print(
        "clean max RMSE increase / mV:",
        clean["maximum_display_rmse_increase_mv"],
    )
    print(
        "isolated median improvement, fixed clean frequency / %:",
        isolated[
            "fixed_clean_frequency_median_display_rmse_improvement_percent"
        ],
    )
    print(
        "isolated median improvement, contaminated refinement / %:",
        isolated[
            "contaminated_refined_frequency_median_display_rmse_improvement_percent"
        ],
    )
    print("CSV:", csv_path)
    print("JSON:", summary_path)
    print("PNG:", gallery_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
