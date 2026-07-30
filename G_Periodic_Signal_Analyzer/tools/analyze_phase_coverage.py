#!/usr/bin/env python3
"""Fine-grained phase-coverage sweep for the 2048 -> 256 folding path."""

from __future__ import annotations

import argparse
import csv
import json
import math
from dataclasses import asdict
from pathlib import Path
from typing import Dict, List

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

from waveform_lab_core import (
    DEFAULT_PHASE_BIN_COUNT,
    DEFAULT_SAMPLE_COUNT,
    DEFAULT_SAMPLE_RATE_HZ,
    REPO_ROOT,
    phase_coverage,
)


DEFAULT_OUTPUT_DIR = REPO_ROOT / "tests" / "phase_coverage"


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "按有限2048点记录计算基频相位覆盖。默认把10 Hz均匀扫描"
            "与Fs*p/q精确有理共振扫描结合，避免十进制步长漏掉"
            "分母为3、7、9、11等的锁相点。"
        )
    )
    parser.add_argument(
        "--mode",
        choices=("uniform", "rational", "hybrid"),
        default="hybrid",
        help="uniform=仅均匀扫描；rational=仅有理共振；hybrid=两者并集。",
    )
    parser.add_argument("--start-hz", type=float, default=10_000.0)
    parser.add_argument("--end-hz", type=float, default=500_000.0)
    parser.add_argument(
        "--step-hz",
        type=float,
        default=10.0,
        help="均匀扫描步进。十进制步进本身不能替代有理共振枚举。",
    )
    parser.add_argument(
        "--sample-rate-hz",
        type=float,
        default=DEFAULT_SAMPLE_RATE_HZ,
    )
    parser.add_argument(
        "--sample-count",
        type=int,
        default=DEFAULT_SAMPLE_COUNT,
    )
    parser.add_argument(
        "--phase-bins",
        type=int,
        default=DEFAULT_PHASE_BIN_COUNT,
    )
    parser.add_argument(
        "--max-denominator",
        type=int,
        default=256,
        help="枚举互质比p/q时的最大q；256足以覆盖所有低于256槽的精确锁相点。",
    )
    parser.add_argument(
        "--resonance-radius-hz",
        type=float,
        default=1.0,
        help="每个精确有理共振点两侧继续扫描的半径。",
    )
    parser.add_argument(
        "--resonance-step-hz",
        type=float,
        default=1.0,
        help="有理共振邻域的偏移步进。",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=DEFAULT_OUTPUT_DIR,
    )
    return parser.parse_args()


def _frequency_key(frequency_hz: float) -> int:
    """Deduplicate candidates at 1 microhertz resolution."""

    return int(round(frequency_hz * 1_000_000.0))


def _add_candidate(
    candidates: Dict[int, Dict[str, object]],
    frequency_hz: float,
    source: str,
    resonance_numerator: int = 0,
    resonance_denominator: int = 0,
    resonance_offset_hz: float = 0.0,
) -> None:
    if frequency_hz <= 0.0:
        return
    key = _frequency_key(frequency_hz)
    candidate = candidates.get(key)
    if candidate is None:
        candidates[key] = {
            "frequency_hz": float(frequency_hz),
            "sources": {source},
            "resonance_numerator": int(resonance_numerator),
            "resonance_denominator": int(resonance_denominator),
            "resonance_offset_hz": float(resonance_offset_hz),
        }
        return

    candidate["sources"].add(source)
    old_denominator = int(candidate["resonance_denominator"])
    if (
        resonance_denominator > 0
        and (
            old_denominator == 0
            or resonance_denominator < old_denominator
            or (
                resonance_denominator == old_denominator
                and abs(resonance_offset_hz)
                < abs(float(candidate["resonance_offset_hz"]))
            )
        )
    ):
        candidate["resonance_numerator"] = int(resonance_numerator)
        candidate["resonance_denominator"] = int(resonance_denominator)
        candidate["resonance_offset_hz"] = float(resonance_offset_hz)


def build_candidates(
    args: argparse.Namespace,
) -> List[Dict[str, object]]:
    """Build a union of decimal-grid points and exact Fs*p/q resonances."""

    candidates: Dict[int, Dict[str, object]] = {}

    if args.mode in ("uniform", "hybrid"):
        frequencies = np.arange(
            args.start_hz,
            args.end_hz + args.step_hz * 0.5,
            args.step_hz,
            dtype=np.float64,
        )
        for frequency_hz in frequencies:
            _add_candidate(
                candidates,
                float(frequency_hz),
                "uniform",
            )

    if args.mode in ("rational", "hybrid"):
        if args.resonance_radius_hz == 0.0:
            offsets = np.array([0.0], dtype=np.float64)
        else:
            offsets = np.arange(
                -args.resonance_radius_hz,
                args.resonance_radius_hz
                + args.resonance_step_hz * 0.5,
                args.resonance_step_hz,
                dtype=np.float64,
            )
            if not np.any(np.isclose(offsets, 0.0)):
                offsets = np.append(offsets, 0.0)
            offsets = np.unique(np.round(offsets, 12))

        for denominator in range(2, args.max_denominator + 1):
            numerator_start = max(
                1,
                int(
                    math.ceil(
                        args.start_hz
                        * denominator
                        / args.sample_rate_hz
                    )
                ),
            )
            numerator_end = int(
                math.floor(
                    args.end_hz
                    * denominator
                    / args.sample_rate_hz
                )
            )
            for numerator in range(numerator_start, numerator_end + 1):
                if math.gcd(numerator, denominator) != 1:
                    continue
                exact_frequency = (
                    args.sample_rate_hz
                    * numerator
                    / denominator
                )
                for offset_hz in offsets:
                    frequency_hz = exact_frequency + float(offset_hz)
                    if (
                        frequency_hz < args.start_hz
                        or frequency_hz > args.end_hz
                    ):
                        continue
                    _add_candidate(
                        candidates,
                        frequency_hz,
                        (
                            "rational_exact"
                            if abs(float(offset_hz)) < 1.0e-12
                            else "rational_neighbor"
                        ),
                        numerator,
                        denominator,
                        float(offset_hz),
                    )

    return sorted(
        candidates.values(),
        key=lambda item: float(item["frequency_hz"]),
    )


def scan(args: argparse.Namespace) -> List[Dict[str, object]]:
    candidates = build_candidates(args)
    rows: List[Dict[str, object]] = []
    for candidate in candidates:
        frequency_hz = float(candidate["frequency_hz"])
        metrics = phase_coverage(
            frequency_hz,
            args.sample_rate_hz,
            args.sample_count,
            args.phase_bins,
        )
        row: Dict[str, object] = {
            "frequency_hz": frequency_hz,
            "frequency_khz": float(frequency_hz / 1000.0),
            "scan_source": "+".join(sorted(candidate["sources"])),
            "resonance_numerator": int(
                candidate["resonance_numerator"]
            ),
            "resonance_denominator": int(
                candidate["resonance_denominator"]
            ),
            "resonance_offset_hz": float(
                candidate["resonance_offset_hz"]
            ),
        }
        row.update(asdict(metrics))
        rows.append(row)
    return rows


def write_csv(rows: List[Dict[str, object]], path: Path) -> None:
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(
            output,
            fieldnames=list(rows[0].keys()),
            lineterminator="\n",
        )
        writer.writeheader()
        writer.writerows(rows)


def write_summary(
    args: argparse.Namespace,
    rows: List[Dict[str, object]],
    path: Path,
) -> None:
    worst = sorted(
        rows,
        key=lambda row: (
            int(row["occupied_hard_bins"]),
            int(row["unique_phase_count"]),
            float(row["frequency_hz"]),
        ),
    )[:30]

    checkpoints = []
    for target_hz in np.arange(50_000.0, 500_000.1, 50_000.0):
        nearest = min(
            rows,
            key=lambda row: abs(float(row["frequency_hz"]) - target_hz),
        )
        checkpoints.append(nearest)

    exact_rows = [
        row
        for row in rows
        if "rational_exact" in str(row["scan_source"])
    ]
    representative_denominators = []
    for denominator in (3, 4, 5, 7, 8, 9, 10, 11, 16, 32, 64, 128, 256):
        matching = [
            row
            for row in exact_rows
            if int(row["resonance_denominator"]) == denominator
        ]
        if matching:
            representative_denominators.append(
                min(
                    matching,
                    key=lambda row: float(row["frequency_hz"]),
                )
            )

    payload = {
        "schema_version": 1,
        "scan": {
            "mode": args.mode,
            "start_hz": args.start_hz,
            "end_hz": args.end_hz,
            "uniform_step_hz": args.step_hz,
            "sample_rate_hz": args.sample_rate_hz,
            "sample_count": args.sample_count,
            "phase_bins": args.phase_bins,
            "max_denominator": args.max_denominator,
            "resonance_radius_hz": args.resonance_radius_hz,
            "resonance_step_hz": args.resonance_step_hz,
            "row_count": len(rows),
            "exact_rational_row_count": len(exact_rows),
        },
        "decision": (
            "默认采用10 Hz均匀扫描 + q<=256的Fs*p/q精确共振点"
            " + 共振点±1 Hz邻域。任何十进制步长都只能精确命中"
            "分母由2和5组成的比例，不能替代有理共振枚举。"
        ),
        "worst_30_by_hard_bin_coverage": worst,
        "checkpoints_every_50khz": checkpoints,
        "representative_exact_denominators": (
            representative_denominators
        ),
    }
    path.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def write_plot(
    args: argparse.Namespace,
    rows: List[Dict[str, object]],
    svg_path: Path,
    png_path: Path,
) -> None:
    frequencies_khz = np.array(
        [float(row["frequency_khz"]) for row in rows]
    )
    hard = np.array(
        [float(row["hard_bin_coverage"]) * 100.0 for row in rows]
    )
    weighted = np.array(
        [float(row["weighted_bin_coverage"]) * 100.0 for row in rows]
    )
    unique = np.array(
        [
            min(int(row["unique_phase_count"]), args.phase_bins)
            for row in rows
        ],
        dtype=np.float64,
    )
    max_gap = np.array(
        [float(row["maximum_phase_gap_bins"]) for row in rows]
    )
    exact_mask = np.array(
        [
            "rational_exact" in str(row["scan_source"])
            for row in rows
        ],
        dtype=bool,
    )

    plt.rcParams["font.sans-serif"] = [
        "Microsoft YaHei",
        "SimHei",
        "DejaVu Sans",
    ]
    plt.rcParams["axes.unicode_minus"] = False
    figure, axes = plt.subplots(
        3,
        1,
        figsize=(15, 11),
        sharex=True,
        constrained_layout=True,
    )
    axes[0].plot(
        frequencies_khz,
        hard,
        linewidth=0.8,
        color="#1f77b4",
        label="硬分箱覆盖率",
        rasterized=True,
    )
    axes[0].plot(
        frequencies_khz,
        weighted,
        linewidth=0.8,
        color="#ff7f0e",
        alpha=0.8,
        label="线性加权覆盖率",
        rasterized=True,
    )
    axes[0].set_ylabel("256槽覆盖率 / %")
    axes[0].set_ylim(-2.0, 102.0)
    axes[0].grid(True, alpha=0.25)
    axes[0].legend(loc="lower left")

    axes[1].plot(
        frequencies_khz,
        unique,
        linewidth=0.8,
        color="#2ca02c",
        rasterized=True,
    )
    axes[1].set_ylabel("独立相位数（截断到256）")
    axes[1].set_ylim(-5.0, args.phase_bins + 5.0)
    axes[1].grid(True, alpha=0.25)

    axes[2].plot(
        frequencies_khz,
        max_gap,
        linewidth=0.8,
        color="#d62728",
        rasterized=True,
    )
    axes[2].set_ylabel("最大相位空洞 / 槽")
    axes[2].set_xlabel("基频 / kHz")
    axes[2].grid(True, alpha=0.25)

    if np.any(exact_mask):
        axes[0].scatter(
            frequencies_khz[exact_mask],
            hard[exact_mask],
            s=2.0,
            color="#8b0000",
            alpha=0.35,
            label="精确Fs·p/q",
            rasterized=True,
        )
        axes[0].legend(loc="lower left")

    for axis in axes:
        for checkpoint in range(50, 501, 50):
            axis.axvline(
                checkpoint,
                linewidth=0.45,
                color="#808080",
                alpha=0.25,
            )

    figure.suptitle(
        "2048点到256相位槽：{}扫描（Fs={:.3f} kHz，均匀步进{:.1f} Hz，q≤{}）".format(
            args.mode,
            args.sample_rate_hz / 1000.0,
            args.step_hz,
            args.max_denominator,
        ),
        fontsize=15,
    )
    figure.savefig(svg_path, format="svg")
    figure.savefig(png_path, format="png", dpi=170)
    plt.close(figure)


def main() -> int:
    args = parse_arguments()
    if args.step_hz <= 0.0:
        raise SystemExit("--step-hz必须大于0")
    if args.start_hz <= 0.0 or args.end_hz < args.start_hz:
        raise SystemExit("频率范围无效")
    if args.max_denominator < 2:
        raise SystemExit("--max-denominator必须不小于2")
    if args.resonance_radius_hz < 0.0:
        raise SystemExit("--resonance-radius-hz不能为负")
    if args.resonance_step_hz <= 0.0:
        raise SystemExit("--resonance-step-hz必须大于0")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    rows = scan(args)
    csv_path = args.output_dir / "phase_coverage_sweep.csv"
    summary_path = args.output_dir / "phase_coverage_summary.json"
    plot_path = args.output_dir / "phase_coverage_sweep.svg"
    plot_png_path = args.output_dir / "phase_coverage_sweep.png"
    write_csv(rows, csv_path)
    write_summary(args, rows, summary_path)
    write_plot(args, rows, plot_path, plot_png_path)

    worst = sorted(
        rows,
        key=lambda row: (
            int(row["occupied_hard_bins"]),
            int(row["unique_phase_count"]),
        ),
    )[:10]
    source_counts: Dict[str, int] = {}
    for row in rows:
        source = str(row["scan_source"])
        source_counts[source] = source_counts.get(source, 0) + 1
    print("扫描模式:", args.mode)
    print("扫描点数:", len(rows))
    print("来源统计:", source_counts)
    print("CSV:", csv_path)
    print("摘要:", summary_path)
    print("图:", plot_path)
    print("PNG:", plot_png_path)
    print("最差10个频点（kHz / 硬槽 / 独立相位 / 最大空洞槽）:")
    for row in worst:
        print(
            "{:8.1f}  {:3d}  {:4d}  {:8.3f}".format(
                float(row["frequency_khz"]),
                int(row["occupied_hard_bins"]),
                int(row["unique_phase_count"]),
                float(row["maximum_phase_gap_bins"]),
            )
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
