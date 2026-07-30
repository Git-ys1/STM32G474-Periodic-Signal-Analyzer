#!/usr/bin/env python3
"""Quantify exact-256 kHz reconstruction and frequency-estimate sensitivity."""

from __future__ import annotations

import csv
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

from waveform_lab_core import (
    DEFAULT_ADC_VREF_V,
    DEFAULT_DISPLAY_WIDTH,
    DEFAULT_PHASE_BIN_COUNT,
    REPO_ROOT,
    ToneSpec,
    WaveformSpec,
    generate_adc_codes,
    harmonic_least_squares_fit,
    periodic_linear_display,
    phase_coverage,
    phase_fold_waveform,
    synthesize_at_base_phase_mv,
)


OUTPUT_ROOT = REPO_ROOT / "tests" / "phase_coverage"
TRUE_FREQUENCY_HZ = 256_000.0
OFFSETS_HZ = np.arange(-500.0, 501.0, 1.0)


def rmse(actual: np.ndarray, expected: np.ndarray) -> float:
    return float(np.sqrt(np.mean(np.square(actual - expected))))


def waveform_spec(frequency_hz: float) -> WaveformSpec:
    return WaveformSpec(
        group_number=108,
        name="exact_256k_pure",
        requirement=2,
        fundamental_hz=frequency_hz,
        tones=(ToneSpec(1, 55.0),),
        start_phase_deg=23.0,
        noise_rms_mv=0.0,
        random_seed=108,
    )


def evaluate(
    source_spec: WaveformSpec,
    adc_codes: np.ndarray,
    assumed_frequency_hz: float,
) -> tuple[float, float, float, float]:
    volts_per_code = source_spec.adc_vref_v / 4096.0
    folded, _, _ = phase_fold_waveform(
        adc_codes,
        volts_per_code,
        source_spec.sample_rate_hz,
        assumed_frequency_hz,
        DEFAULT_PHASE_BIN_COUNT,
    )
    folded_display = periodic_linear_display(folded, 1)
    display_phase = (
        np.arange(DEFAULT_DISPLAY_WIDTH, dtype=np.float64)
        / (DEFAULT_DISPLAY_WIDTH - 1)
    )
    ideal_display = synthesize_at_base_phase_mv(
        source_spec,
        display_phase,
    )
    ideal_display -= float(np.mean(ideal_display))

    _, _, fit = harmonic_least_squares_fit(
        source_spec,
        adc_codes,
        assumed_frequency_hz,
        display_periods=1,
    )
    coverage = phase_coverage(
        assumed_frequency_hz,
        source_spec.sample_rate_hz,
        source_spec.sample_count,
        DEFAULT_PHASE_BIN_COUNT,
        source_spec.start_phase_deg,
    )
    return (
        coverage.hard_bin_coverage,
        rmse(folded_display, ideal_display),
        fit.sample_rmse_mv,
        fit.display_rmse_mv,
    )


def main() -> int:
    OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
    exact_spec = waveform_spec(TRUE_FREQUENCY_HZ)
    _, _, exact_adc, _ = generate_adc_codes(exact_spec)

    rows = []
    for offset_hz in OFFSETS_HZ:
        assumed_hz = TRUE_FREQUENCY_HZ + float(offset_hz)

        # A: 真信号始终是精确256 kHz，只把估计频率改错。
        (
            estimated_coverage,
            wrong_fold_rmse,
            wrong_fit_sample_rmse,
            wrong_fit_display_rmse,
        ) = evaluate(exact_spec, exact_adc, assumed_hz)

        # B: 真信号本身就在邻近频率，并用正确频率重建。
        nearby_spec = waveform_spec(assumed_hz)
        _, _, nearby_adc, _ = generate_adc_codes(nearby_spec)
        (
            true_coverage,
            nearby_fold_rmse,
            nearby_fit_sample_rmse,
            nearby_fit_display_rmse,
        ) = evaluate(nearby_spec, nearby_adc, assumed_hz)

        rows.append(
            {
                "frequency_offset_hz": float(offset_hz),
                "assumed_frequency_hz": assumed_hz,
                "estimated_hard_coverage": estimated_coverage,
                "exact_256k_wrong_frequency_fold_rmse_mv": wrong_fold_rmse,
                "exact_256k_wrong_frequency_ls_sample_rmse_mv": (
                    wrong_fit_sample_rmse
                ),
                "exact_256k_wrong_frequency_ls_display_rmse_mv": (
                    wrong_fit_display_rmse
                ),
                "nearby_true_frequency_hard_coverage": true_coverage,
                "nearby_true_frequency_fold_rmse_mv": nearby_fold_rmse,
                "nearby_true_frequency_ls_sample_rmse_mv": (
                    nearby_fit_sample_rmse
                ),
                "nearby_true_frequency_ls_display_rmse_mv": (
                    nearby_fit_display_rmse
                ),
            }
        )

    csv_path = OUTPUT_ROOT / "exact_256k_frequency_sensitivity.csv"
    with csv_path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=tuple(rows[0]))
        writer.writeheader()
        writer.writerows(rows)

    selected_offsets = (0, 1, 10, 50, 100, 250, 500)
    selected_rows = {
        str(offset): rows[int(offset + 500)]
        for offset in selected_offsets
    }
    false_confidence = [
        row
        for row in rows
        if (
            row["estimated_hard_coverage"] >= 0.8
            and row["exact_256k_wrong_frequency_fold_rmse_mv"] >= 5.0
        )
    ]
    summary = {
        "true_frequency_hz": TRUE_FREQUENCY_HZ,
        "sample_rate_hz": exact_spec.sample_rate_hz,
        "sample_count": exact_spec.sample_count,
        "phase_bin_count": DEFAULT_PHASE_BIN_COUNT,
        "selected_positive_offsets": selected_rows,
        "false_confidence_point_count": len(false_confidence),
        "conclusion": (
            "Coverage computed only from an assumed frequency can become high "
            "even when exact-256 kHz ADC values contain only four physical "
            "phases. Use fit residual and estimate stability as guardrails."
        ),
    }
    json_path = OUTPUT_ROOT / "exact_256k_frequency_sensitivity.json"
    json_path.write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )

    offsets = np.asarray(
        [row["frequency_offset_hz"] for row in rows],
        dtype=np.float64,
    )
    coverage = np.asarray(
        [row["estimated_hard_coverage"] for row in rows],
        dtype=np.float64,
    )
    wrong_fold = np.asarray(
        [
            row["exact_256k_wrong_frequency_fold_rmse_mv"]
            for row in rows
        ],
        dtype=np.float64,
    )
    nearby_fold = np.asarray(
        [row["nearby_true_frequency_fold_rmse_mv"] for row in rows],
        dtype=np.float64,
    )
    wrong_ls_sample = np.asarray(
        [
            row["exact_256k_wrong_frequency_ls_sample_rmse_mv"]
            for row in rows
        ],
        dtype=np.float64,
    )
    nearby_ls_sample = np.asarray(
        [
            row["nearby_true_frequency_ls_sample_rmse_mv"]
            for row in rows
        ],
        dtype=np.float64,
    )

    figure, axes = plt.subplots(3, 1, figsize=(12, 10), sharex=True)
    figure.subplots_adjust(
        left=0.085,
        right=0.98,
        top=0.91,
        bottom=0.08,
        hspace=0.34,
    )
    figure.suptitle(
        "Exact 256 kHz: phase coverage can lie when the fold frequency is wrong",
        fontsize=14,
        y=0.975,
    )

    axes[0].plot(offsets, coverage * 100.0, color="#2c7fb8")
    axes[0].set_ylabel("Hard coverage / %")
    axes[0].set_title(
        "Geometry reported from assumed frequency (same for both scenarios)"
    )

    axes[1].plot(
        offsets,
        wrong_fold,
        color="#de2d26",
        label="true 256 kHz, wrong fold frequency",
    )
    axes[1].plot(
        offsets,
        nearby_fold,
        color="#31a354",
        label="nearby true frequency, correct fold frequency",
    )
    axes[1].set_ylabel("Fold display RMSE / mV")
    axes[1].set_title("Phase-fold reconstruction")
    axes[1].legend(loc="upper left")

    axes[2].plot(
        offsets,
        wrong_ls_sample,
        color="#756bb1",
        label="true 256 kHz, wrong LS frequency",
    )
    axes[2].plot(
        offsets,
        nearby_ls_sample,
        color="#e6550d",
        label="nearby true frequency, correct LS frequency",
    )
    axes[2].set_ylabel("LS sample residual / mV")
    axes[2].set_xlabel("assumed/true frequency offset from 256 kHz / Hz")
    axes[2].set_title("Harmonic least-squares consistency check")
    axes[2].legend(loc="upper left")

    for axis in axes:
        axis.axvline(0.0, color="black", linewidth=0.8, linestyle="--")
        axis.grid(True, alpha=0.25)

    png_path = OUTPUT_ROOT / "exact_256k_frequency_sensitivity.png"
    svg_path = OUTPUT_ROOT / "exact_256k_frequency_sensitivity.svg"
    figure.savefig(png_path, dpi=170)
    figure.savefig(svg_path)
    plt.close(figure)

    print("CSV:", csv_path)
    print("JSON:", json_path)
    print("PNG:", png_path)
    print("SVG:", svg_path)
    print("false confidence points:", len(false_confidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
