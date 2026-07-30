#!/usr/bin/env python3
"""
Interactive custom waveform laboratory for the STM32G474 G-problem project.

Run:
    python tools/custom_waveform_lab.py

The GUI lets a user define a base tone plus zero, one or two harmonics, inspect
all 2048 ADC samples, compare the ideal waveform with the firmware-equivalent
256-bin/794-column result, save numbered groups, and export a C header.
"""

from __future__ import annotations

import argparse
import math
import os
import random
import subprocess
import sys
import tkinter as tk
from pathlib import Path
from tkinter import messagebox, ttk
from typing import List, Optional

import matplotlib

matplotlib.use("TkAgg")
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg, NavigationToolbar2Tk
from matplotlib.figure import Figure
import numpy as np

from waveform_lab_core import (
    CUSTOM_DATA_ROOT,
    CUSTOM_HEADER_PATH,
    DEFAULT_ADC_BIAS_V,
    DEFAULT_ADC_VREF_V,
    DEFAULT_DISPLAY_WIDTH,
    DEFAULT_SAMPLE_COUNT,
    DEFAULT_SAMPLE_RATE_HZ,
    REPO_ROOT,
    SimulationResult,
    ToneSpec,
    WaveformSpec,
    export_custom_header,
    export_group_artifacts,
    load_registry,
    periodic_linear_display,
    phase_coverage,
    phase_fold_waveform,
    save_registry,
    simulate,
    synthesize_at_base_phase_mv,
    synthesize_at_times_mv,
    typical_specs,
    validate_spec,
    write_typical_registry_if_missing,
)


class WaveformLabApp:
    """Tkinter front-end; all numerical work lives in waveform_lab_core.py."""

    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("G题 2048点自定义波形实验室")
        self.root.geometry("1550x930")
        self.root.minsize(1220, 760)

        self.current_result: Optional[SimulationResult] = None
        self.records: List[WaveformSpec] = []
        self._build_variables()
        self._configure_style()
        self._build_layout()
        self._load_registry()
        self._load_spec(typical_specs()[1])
        self.generate()

    def _build_variables(self) -> None:
        self.group_number = tk.StringVar(value="110")
        self.group_name = tk.StringVar(value="custom_group")
        self.requirement = tk.StringVar(value="2")
        self.fundamental_hz = tk.StringVar(value="10500")
        self.base_amplitude_mv = tk.StringVar(value="50")
        self.base_phase_deg = tk.StringVar(value="0")

        self.h1_enabled = tk.BooleanVar(value=True)
        self.h1_frequency_hz = tk.StringVar(value="31500")
        self.h1_amplitude_mv = tk.StringVar(value="25")
        self.h1_phase_deg = tk.StringVar(value="0")

        self.h2_enabled = tk.BooleanVar(value=True)
        self.h2_frequency_hz = tk.StringVar(value="42000")
        self.h2_amplitude_mv = tk.StringVar(value="15")
        self.h2_phase_deg = tk.StringVar(value="0")

        self.sample_rate_hz = tk.StringVar(
            value=str(int(DEFAULT_SAMPLE_RATE_HZ))
        )
        self.start_phase_deg = tk.StringVar(value="0")
        self.noise_rms_mv = tk.StringVar(value="0")
        self.random_seed = tk.StringVar(value="1")
        self.random_start_phase = tk.BooleanVar(value=False)
        self.random_relative_phases = tk.BooleanVar(value=False)
        self.display_periods = tk.StringVar(value="1")
        self.phase_bin_count = tk.StringVar(value="256")
        self.preset_name = tk.StringVar()
        self.status_text = tk.StringVar(value="就绪")
        self.metrics_text = tk.StringVar(value="")

    def _configure_style(self) -> None:
        style = ttk.Style()
        if "vista" in style.theme_names():
            style.theme_use("vista")
        style.configure("Header.TLabel", font=("Microsoft YaHei", 11, "bold"))
        style.configure("Metric.TLabel", font=("Consolas", 10))
        matplotlib.rcParams["font.sans-serif"] = [
            "Microsoft YaHei",
            "SimHei",
            "DejaVu Sans",
        ]
        matplotlib.rcParams["axes.unicode_minus"] = False

    def _build_layout(self) -> None:
        self.root.columnconfigure(0, weight=0)
        self.root.columnconfigure(1, weight=1)
        self.root.rowconfigure(0, weight=1)

        left_outer = ttk.Frame(self.root, padding=(8, 8, 4, 8))
        left_outer.grid(row=0, column=0, sticky="nsw")
        right = ttk.Frame(self.root, padding=(4, 8, 8, 8))
        right.grid(row=0, column=1, sticky="nsew")
        right.columnconfigure(0, weight=1)
        right.rowconfigure(0, weight=1)

        canvas = tk.Canvas(
            left_outer,
            width=395,
            highlightthickness=0,
        )
        scrollbar = ttk.Scrollbar(
            left_outer,
            orient="vertical",
            command=canvas.yview,
        )
        self.form = ttk.Frame(canvas)
        self.form.bind(
            "<Configure>",
            lambda event: canvas.configure(scrollregion=canvas.bbox("all")),
        )
        canvas.create_window((0, 0), window=self.form, anchor="nw")
        canvas.configure(yscrollcommand=scrollbar.set)
        canvas.pack(side="left", fill="y", expand=False)
        scrollbar.pack(side="right", fill="y")

        self._build_form()
        self._build_figure(right)

    def _add_entry(
        self,
        parent: ttk.Frame,
        row: int,
        label: str,
        variable: tk.StringVar,
        *,
        width: int = 15,
    ) -> ttk.Entry:
        ttk.Label(parent, text=label).grid(
            row=row,
            column=0,
            sticky="w",
            padx=(0, 6),
            pady=2,
        )
        entry = ttk.Entry(parent, textvariable=variable, width=width)
        entry.grid(row=row, column=1, sticky="ew", pady=2)
        return entry

    def _build_form(self) -> None:
        form = self.form
        form.columnconfigure(0, weight=1)
        row = 0

        ttk.Label(
            form,
            text="2048点自定义波形实验室",
            style="Header.TLabel",
        ).grid(row=row, column=0, sticky="w", pady=(0, 7))
        row += 1

        preset_box = ttk.LabelFrame(form, text="典型数据 / 已保存组", padding=7)
        preset_box.grid(row=row, column=0, sticky="ew", pady=4)
        preset_box.columnconfigure(0, weight=1)
        self.preset_combo = ttk.Combobox(
            preset_box,
            textvariable=self.preset_name,
            state="readonly",
            width=36,
        )
        self.preset_combo["values"] = tuple(
            "T{:03d} {}".format(spec.group_number, spec.name)
            for spec in typical_specs()
        )
        self.preset_combo.grid(row=0, column=0, sticky="ew")
        ttk.Button(
            preset_box,
            text="载入典型",
            command=self._load_selected_preset,
        ).grid(row=0, column=1, padx=(5, 0))

        self.record_list = tk.Listbox(
            preset_box,
            height=6,
            exportselection=False,
            font=("Consolas", 9),
        )
        self.record_list.grid(
            row=1,
            column=0,
            columnspan=2,
            sticky="ew",
            pady=(6, 3),
        )
        record_buttons = ttk.Frame(preset_box)
        record_buttons.grid(row=2, column=0, columnspan=2, sticky="ew")
        ttk.Button(
            record_buttons,
            text="载入记录",
            command=self._load_selected_record,
        ).pack(side="left")
        ttk.Button(
            record_buttons,
            text="保存/覆盖当前组",
            command=self._save_current_record,
        ).pack(side="left", padx=4)
        ttk.Button(
            record_buttons,
            text="删除记录",
            command=self._delete_selected_record,
        ).pack(side="left")
        row += 1

        identity = ttk.LabelFrame(form, text="组信息", padding=7)
        identity.grid(row=row, column=0, sticky="ew", pady=4)
        identity.columnconfigure(1, weight=1)
        self._add_entry(identity, 0, "组号 1~255", self.group_number)
        self._add_entry(identity, 1, "组名", self.group_name)
        ttk.Label(identity, text="对应题目").grid(
            row=2,
            column=0,
            sticky="w",
            pady=2,
        )
        ttk.Combobox(
            identity,
            textvariable=self.requirement,
            values=("1", "2", "3"),
            state="readonly",
            width=13,
        ).grid(row=2, column=1, sticky="w")
        row += 1

        signal = ttk.LabelFrame(form, text="基波与谐波（峰值mV）", padding=7)
        signal.grid(row=row, column=0, sticky="ew", pady=4)
        signal.columnconfigure(1, weight=1)
        self._add_entry(signal, 0, "基频 / Hz", self.fundamental_hz)
        self._add_entry(signal, 1, "基波幅值 / mVpk", self.base_amplitude_mv)
        self._add_entry(signal, 2, "基波相对相位 / °", self.base_phase_deg)

        ttk.Separator(signal).grid(
            row=3,
            column=0,
            columnspan=2,
            sticky="ew",
            pady=5,
        )
        ttk.Checkbutton(
            signal,
            text="启用谐波1",
            variable=self.h1_enabled,
        ).grid(row=4, column=0, columnspan=2, sticky="w")
        self._add_entry(signal, 5, "谐波1频率 / Hz", self.h1_frequency_hz)
        self._add_entry(signal, 6, "谐波1幅值 / mVpk", self.h1_amplitude_mv)
        self._add_entry(signal, 7, "谐波1相对相位 / °", self.h1_phase_deg)

        ttk.Separator(signal).grid(
            row=8,
            column=0,
            columnspan=2,
            sticky="ew",
            pady=5,
        )
        ttk.Checkbutton(
            signal,
            text="启用谐波2",
            variable=self.h2_enabled,
        ).grid(row=9, column=0, columnspan=2, sticky="w")
        self._add_entry(signal, 10, "谐波2频率 / Hz", self.h2_frequency_hz)
        self._add_entry(signal, 11, "谐波2幅值 / mVpk", self.h2_amplitude_mv)
        self._add_entry(signal, 12, "谐波2相对相位 / °", self.h2_phase_deg)
        row += 1

        acquisition = ttk.LabelFrame(
            form,
            text="采样、相位与噪声",
            padding=7,
        )
        acquisition.grid(row=row, column=0, sticky="ew", pady=4)
        acquisition.columnconfigure(1, weight=1)
        self._add_entry(
            acquisition,
            0,
            "采样率 / Hz",
            self.sample_rate_hz,
        )
        ttk.Label(acquisition, text="采样点数").grid(
            row=1,
            column=0,
            sticky="w",
            pady=2,
        )
        ttk.Label(acquisition, text="2048（固件固定）").grid(
            row=1,
            column=1,
            sticky="w",
            pady=2,
        )
        self._add_entry(
            acquisition,
            2,
            "整体起始相位 / °",
            self.start_phase_deg,
        )
        self._add_entry(
            acquisition,
            3,
            "高斯噪声RMS / mV",
            self.noise_rms_mv,
        )
        self._add_entry(acquisition, 4, "随机种子", self.random_seed)
        ttk.Checkbutton(
            acquisition,
            text="随机整体起始相位（正式题允许）",
            variable=self.random_start_phase,
        ).grid(row=5, column=0, columnspan=2, sticky="w")
        ttk.Checkbutton(
            acquisition,
            text="随机独立谐波相位（扩展压力测试）",
            variable=self.random_relative_phases,
        ).grid(row=6, column=0, columnspan=2, sticky="w")
        ttk.Button(
            acquisition,
            text="更换随机种子",
            command=self._new_random_seed,
        ).grid(row=7, column=0, columnspan=2, sticky="ew", pady=(4, 0))
        ttk.Label(acquisition, text="折叠相位槽数").grid(
            row=8,
            column=0,
            sticky="w",
            pady=2,
        )
        ttk.Combobox(
            acquisition,
            textvariable=self.phase_bin_count,
            values=("64", "128", "256"),
            state="readonly",
            width=13,
        ).grid(row=8, column=1, sticky="w")
        row += 1

        actions = ttk.LabelFrame(form, text="生成与导出", padding=7)
        actions.grid(row=row, column=0, sticky="ew", pady=4)
        actions.columnconfigure(1, weight=1)
        ttk.Label(actions, text="比较周期数").grid(
            row=0,
            column=0,
            sticky="w",
        )
        ttk.Combobox(
            actions,
            textvariable=self.display_periods,
            values=("1", "3"),
            state="readonly",
            width=8,
        ).grid(row=0, column=1, sticky="w")
        ttk.Button(
            actions,
            text="生成并绘图",
            command=self.generate,
        ).grid(row=1, column=0, columnspan=2, sticky="ew", pady=(5, 2))
        ttk.Button(
            actions,
            text="导出当前组（JSON/CSV/PNG/SVG）",
            command=self._export_current,
        ).grid(row=2, column=0, columnspan=2, sticky="ew", pady=2)
        ttk.Button(
            actions,
            text="导出全部记录到固件C头文件",
            command=self._export_firmware_header,
        ).grid(row=3, column=0, columnspan=2, sticky="ew", pady=2)
        ttk.Button(
            actions,
            text="执行混合相位覆盖扫描",
            command=self._run_coverage_sweep,
        ).grid(row=4, column=0, columnspan=2, sticky="ew", pady=2)
        ttk.Button(
            actions,
            text="打开导出目录",
            command=self._open_output_folder,
        ).grid(row=5, column=0, columnspan=2, sticky="ew", pady=2)
        row += 1

        ttk.Label(
            form,
            textvariable=self.metrics_text,
            justify="left",
            style="Metric.TLabel",
        ).grid(row=row, column=0, sticky="ew", pady=(6, 2))
        row += 1
        ttk.Label(
            form,
            textvariable=self.status_text,
            wraplength=380,
            foreground="#245a8d",
        ).grid(row=row, column=0, sticky="ew", pady=(2, 8))

    def _build_figure(self, parent: ttk.Frame) -> None:
        self.figure = Figure(figsize=(11, 8), dpi=100)
        self.axes = self.figure.subplots(3, 1)
        self.figure.subplots_adjust(
            left=0.075,
            right=0.985,
            top=0.885,
            bottom=0.065,
            hspace=0.58,
        )
        self.canvas = FigureCanvasTkAgg(self.figure, master=parent)
        self.canvas.get_tk_widget().grid(row=0, column=0, sticky="nsew")
        toolbar_frame = ttk.Frame(parent)
        toolbar_frame.grid(row=1, column=0, sticky="ew")
        NavigationToolbar2Tk(self.canvas, toolbar_frame).update()

    @staticmethod
    def _parse_float(variable: tk.StringVar, label: str) -> float:
        try:
            return float(variable.get().strip())
        except ValueError:
            raise ValueError("{}必须是数字。".format(label))

    @staticmethod
    def _parse_int(variable: tk.StringVar, label: str) -> int:
        try:
            return int(variable.get().strip())
        except ValueError:
            raise ValueError("{}必须是整数。".format(label))

    def _harmonic_tone(
        self,
        enabled: bool,
        frequency_variable: tk.StringVar,
        amplitude_variable: tk.StringVar,
        phase_variable: tk.StringVar,
        fundamental_hz: float,
        label: str,
        randomized_phase: Optional[float],
    ) -> Optional[ToneSpec]:
        if not enabled:
            return None
        frequency_hz = self._parse_float(
            frequency_variable,
            label + "频率",
        )
        ratio = frequency_hz / fundamental_hz
        order = int(round(ratio))
        if order < 2 or not math.isclose(
            frequency_hz,
            fundamental_hz * order,
            rel_tol=0.0,
            abs_tol=0.5,
        ):
            raise ValueError(
                "{}必须是基频的整数倍；当前{:.3f}倍。".format(
                    label,
                    ratio,
                )
            )
        phase_deg = (
            randomized_phase
            if randomized_phase is not None
            else self._parse_float(phase_variable, label + "相对相位")
        )
        return ToneSpec(
            order=order,
            amplitude_mvpk=self._parse_float(
                amplitude_variable,
                label + "幅值",
            ),
            relative_phase_deg=phase_deg,
        )

    def _build_spec(self) -> WaveformSpec:
        seed = self._parse_int(self.random_seed, "随机种子")
        rng = random.Random(seed)
        fundamental_hz = self._parse_float(
            self.fundamental_hz,
            "基频",
        )
        start_phase = (
            rng.uniform(0.0, 360.0)
            if self.random_start_phase.get()
            else self._parse_float(self.start_phase_deg, "整体起始相位")
        )
        base_relative_phase = self._parse_float(
            self.base_phase_deg,
            "基波相对相位",
        )
        random_h1 = (
            rng.uniform(-180.0, 180.0)
            if self.random_relative_phases.get()
            else None
        )
        random_h2 = (
            rng.uniform(-180.0, 180.0)
            if self.random_relative_phases.get()
            else None
        )

        tones: List[ToneSpec] = [
            ToneSpec(
                order=1,
                amplitude_mvpk=self._parse_float(
                    self.base_amplitude_mv,
                    "基波幅值",
                ),
                relative_phase_deg=base_relative_phase,
            )
        ]
        h1 = self._harmonic_tone(
            self.h1_enabled.get(),
            self.h1_frequency_hz,
            self.h1_amplitude_mv,
            self.h1_phase_deg,
            fundamental_hz,
            "谐波1",
            random_h1,
        )
        h2 = self._harmonic_tone(
            self.h2_enabled.get(),
            self.h2_frequency_hz,
            self.h2_amplitude_mv,
            self.h2_phase_deg,
            fundamental_hz,
            "谐波2",
            random_h2,
        )
        if h1 is not None:
            tones.append(h1)
        if h2 is not None:
            tones.append(h2)
        tones.sort(key=lambda tone: tone.order)

        spec = WaveformSpec(
            group_number=self._parse_int(self.group_number, "组号"),
            name=self.group_name.get().strip(),
            requirement=int(self.requirement.get()),
            fundamental_hz=fundamental_hz,
            tones=tuple(tones),
            sample_rate_hz=self._parse_float(
                self.sample_rate_hz,
                "采样率",
            ),
            sample_count=DEFAULT_SAMPLE_COUNT,
            adc_vref_v=DEFAULT_ADC_VREF_V,
            adc_bias_v=DEFAULT_ADC_BIAS_V,
            start_phase_deg=start_phase,
            noise_rms_mv=self._parse_float(
                self.noise_rms_mv,
                "噪声RMS",
            ),
            random_seed=seed,
        )
        validate_spec(spec)
        return spec

    def generate(self) -> None:
        try:
            spec = self._build_spec()
            periods = int(self.display_periods.get())
            phase_bin_count = self._parse_int(
                self.phase_bin_count,
                "折叠相位槽数",
            )
            self.current_result = simulate(
                spec,
                display_periods=periods,
                phase_bin_count=phase_bin_count,
            )
            self._plot_result(self.current_result, periods)
            self._update_metrics(self.current_result)
            self.status_text.set(
                "生成完成：实际使用起始相位{:.2f}°；相对相位{}。".format(
                    spec.start_phase_deg,
                    ", ".join(
                        "h{}={:.2f}°".format(
                            tone.order,
                            tone.relative_phase_deg,
                        )
                        for tone in spec.tones
                    ),
                )
            )
        except Exception as error:
            self.current_result = None
            self.status_text.set("生成失败：{}".format(error))
            messagebox.showerror("生成失败", str(error))

    def _plot_result(
        self,
        result: SimulationResult,
        periods: int,
    ) -> None:
        ax_full, ax_compare, ax_coverage = self.axes
        for axis in self.axes:
            axis.clear()

        time_us = result.sample_time_s * 1.0e6
        dense_time_s = np.linspace(
            0.0,
            result.sample_time_s[-1],
            8192,
            endpoint=True,
        )
        ax_full.plot(
            dense_time_s * 1.0e6,
            synthesize_at_times_mv(result.spec, dense_time_s),
            color="#2c7fb8",
            linewidth=1.0,
            alpha=0.85,
            label="蓝线：真正模拟的连续输入模型",
        )
        ax_full.scatter(
            time_us,
            result.adc_signal_mv,
            s=5,
            color="#d95f0e",
            alpha=0.55,
            label="量化后的2048个ADC点",
        )
        ax_full.set_title(
            "① 完整采集窗口：蓝线=连续输入，橙点=量化后的2048个ADC样本；"
            "{:.3f} ms，约{:.2f}T".format(
                time_us[-1] / 1000.0,
                result.spec.fundamental_hz
                * result.spec.sample_count
                / result.spec.sample_rate_hz,
            )
        )
        ax_full.set_xlabel("采样时间 / μs")
        ax_full.set_ylabel("去偏置电压 / mV")
        ax_full.grid(True, alpha=0.22)
        ax_full.legend(loc="upper right", fontsize=8, ncol=2)

        dense_axis = np.linspace(
            0.0,
            float(periods),
            4096,
            endpoint=True,
        )
        dense_ideal = synthesize_at_base_phase_mv(
            result.spec,
            dense_axis,
        )
        dense_ideal -= float(np.mean(dense_ideal))
        display_axis = np.linspace(
            0.0,
            float(periods),
            DEFAULT_DISPLAY_WIDTH,
            endpoint=True,
        )
        ax_compare.plot(
            dense_axis,
            dense_ideal,
            color="#2ca25f",
            linewidth=2.0,
            label="理想连续曲线",
        )
        ax_compare.plot(
            display_axis,
            result.display_waveform_mv,
            color="#756bb1",
            linewidth=1.1,
            label="{}槽折叠→794列线性插值".format(
                result.phase_bin_count
            ),
        )
        ax_compare.plot(
            display_axis,
            result.harmonic_fit_display_waveform_mv,
            color="#de2d26",
            linewidth=1.2,
            linestyle="--",
            label="ADC谐波最小二乘拟合",
        )
        raw_phase = result.phase_cycles
        raw_mv = result.adc_signal_mv
        for period_index in range(periods):
            ax_compare.scatter(
                raw_phase + period_index,
                raw_mv,
                s=4,
                color="#e6550d",
                alpha=0.16,
                label=(
                    "2048点按相位折叠位置"
                    if period_index == 0
                    else None
                ),
            )
        ax_compare.set_xlim(0.0, float(periods))
        ax_compare.set_title(
            "② {}T重建：绿=理想，紫=相位折叠，红虚线=谐波拟合；"
            "折叠显示RMSE {:.3f} mV，拟合RMSE {:.3f} mV".format(
                periods,
                result.display_rmse_mv,
                result.harmonic_fit.display_rmse_mv,
            )
        )
        ax_compare.set_xlabel("基波周期相位 / T")
        ax_compare.set_ylabel("电压 / mV")
        ax_compare.grid(True, alpha=0.22)
        ax_compare.legend(loc="upper right", fontsize=8, ncol=2)

        normalized_weights = result.phase_weights.copy()
        if float(normalized_weights.max()) > 0.0:
            normalized_weights /= float(normalized_weights.max())
        ax_coverage.bar(
            np.arange(normalized_weights.size),
            normalized_weights,
            width=1.0,
            color="#3182bd",
            alpha=0.85,
        )
        ax_coverage.set_xlim(0, normalized_weights.size - 1)
        ax_coverage.set_ylim(0.0, 1.05)
        ax_coverage.set_title(
            "③ {}个相位槽的采样证据权重：硬覆盖{}槽，加权写入{}槽，"
            "最大相位空洞{:.2f}槽".format(
                result.phase_bin_count,
                result.coverage.occupied_hard_bins,
                result.coverage.occupied_weighted_bins,
                result.coverage.maximum_phase_gap_bins,
            )
        )
        ax_coverage.set_xlabel("相位槽编号")
        ax_coverage.set_ylabel("归一化采样权重")
        ax_coverage.grid(True, axis="y", alpha=0.22)

        self.figure.suptitle(
            "T{:03d} {} | f1={:.3f} kHz | B={} | Upp={:.2f} mV | "
            "Urms={:.2f} mV".format(
                result.spec.group_number,
                result.spec.name,
                result.spec.fundamental_hz / 1000.0,
                result.phase_bin_count,
                result.expected_vpp_mv,
                result.expected_vrms_mv,
            ),
            fontsize=13,
            y=0.985,
        )
        self.canvas.draw_idle()

    def _update_metrics(self, result: SimulationResult) -> None:
        coverage = result.coverage
        volts_per_code = result.spec.adc_vref_v / 4096.0
        bin_comparisons = []
        for candidate_bins in (64, 128, 256):
            candidate_waveform, _, _ = phase_fold_waveform(
                result.adc_codes,
                volts_per_code,
                result.spec.sample_rate_hz,
                result.refined_fundamental_hz,
                candidate_bins,
            )
            candidate_display = periodic_linear_display(
                candidate_waveform,
                int(self.display_periods.get()),
            )
            candidate_coverage = phase_coverage(
                result.spec.fundamental_hz,
                result.spec.sample_rate_hz,
                result.spec.sample_count,
                candidate_bins,
                result.spec.start_phase_deg,
            )
            candidate_rmse = float(
                np.sqrt(
                    np.mean(
                        np.square(
                            candidate_display
                            - result.ideal_display_waveform_mv
                        )
                    )
                )
            )
            bin_comparisons.append(
                "B{}:{:.0f}%/{:.3f}mV".format(
                    candidate_bins,
                    candidate_coverage.hard_bin_coverage * 100.0,
                    candidate_rmse,
                )
            )
        self.metrics_text.set(
            "\n".join(
                (
                    "采样率: {:.3f} kS/s".format(
                        result.spec.sample_rate_hz / 1000.0
                    ),
                    "频率细化: {:.3f} Hz (误差 {:+.3f})".format(
                        result.refined_fundamental_hz,
                        result.refined_fundamental_hz
                        - result.spec.fundamental_hz,
                    ),
                    "独立相位: {} / 2048".format(
                        coverage.unique_phase_count
                    ),
                    "折叠槽数: B={}".format(result.phase_bin_count),
                    "硬槽覆盖: {} / {} ({:.1f}%)".format(
                        coverage.occupied_hard_bins,
                        result.phase_bin_count,
                        coverage.hard_bin_coverage * 100.0,
                    ),
                    "加权覆盖: {} / {} ({:.1f}%)".format(
                        coverage.occupied_weighted_bins,
                        result.phase_bin_count,
                        coverage.weighted_bin_coverage * 100.0,
                    ),
                    "硬槽命中: min {} / max {}".format(
                        coverage.minimum_hard_bin_hits,
                        coverage.maximum_hard_bin_hits,
                    ),
                    "最大空洞: {:.2f} 槽".format(
                        coverage.maximum_phase_gap_bins
                    ),
                    "频率比约分: {}/{}".format(
                        coverage.rational_numerator,
                        coverage.rational_denominator,
                    ),
                    "折叠误差: RMSE {:.3f}, Max {:.3f} mV".format(
                        result.phase_fold_rmse_mv,
                        result.phase_fold_max_error_mv,
                    ),
                    "显示误差: RMSE {:.3f}, Max {:.3f} mV".format(
                        result.display_rmse_mv,
                        result.display_max_error_mv,
                    ),
                    "谐波LS: rank {}/{}, cond {:.2e}".format(
                        result.harmonic_fit.rank,
                        result.harmonic_fit.parameter_count,
                        result.harmonic_fit.condition_number,
                    ),
                    "谐波LS误差: 样本残差 {:.3f}, 显示RMSE {:.3f} mV".format(
                        result.harmonic_fit.sample_rmse_mv,
                        result.harmonic_fit.display_rmse_mv,
                    ),
                    "离线槽数对比(硬覆盖/显示RMSE): {}".format(
                        "  ".join(bin_comparisons)
                    ),
                )
            )
        )

    def _load_registry(self) -> None:
        write_typical_registry_if_missing()
        self.records = load_registry()
        self._refresh_record_list()

    def _refresh_record_list(self) -> None:
        self.record_list.delete(0, tk.END)
        for spec in sorted(self.records, key=lambda item: item.group_number):
            self.record_list.insert(
                tk.END,
                "T{:03d} {:<26} f1={:8.2f}k".format(
                    spec.group_number,
                    spec.name[:26],
                    spec.fundamental_hz / 1000.0,
                ),
            )

    def _selected_record(self) -> Optional[WaveformSpec]:
        selection = self.record_list.curselection()
        if not selection:
            return None
        ordered = sorted(self.records, key=lambda item: item.group_number)
        return ordered[int(selection[0])]

    def _load_spec(self, spec: WaveformSpec) -> None:
        self.group_number.set(str(spec.group_number))
        self.group_name.set(spec.name)
        self.requirement.set(str(spec.requirement))
        self.fundamental_hz.set("{:g}".format(spec.fundamental_hz))
        self.base_amplitude_mv.set("{:g}".format(spec.tones[0].amplitude_mvpk))
        self.base_phase_deg.set(
            "{:g}".format(spec.tones[0].relative_phase_deg)
        )
        harmonics = list(spec.tones[1:])

        if len(harmonics) >= 1:
            tone = harmonics[0]
            self.h1_enabled.set(True)
            self.h1_frequency_hz.set(
                "{:g}".format(spec.fundamental_hz * tone.order)
            )
            self.h1_amplitude_mv.set("{:g}".format(tone.amplitude_mvpk))
            self.h1_phase_deg.set("{:g}".format(tone.relative_phase_deg))
        else:
            self.h1_enabled.set(False)

        if len(harmonics) >= 2:
            tone = harmonics[1]
            self.h2_enabled.set(True)
            self.h2_frequency_hz.set(
                "{:g}".format(spec.fundamental_hz * tone.order)
            )
            self.h2_amplitude_mv.set("{:g}".format(tone.amplitude_mvpk))
            self.h2_phase_deg.set("{:g}".format(tone.relative_phase_deg))
        else:
            self.h2_enabled.set(False)

        self.sample_rate_hz.set("{:g}".format(spec.sample_rate_hz))
        self.start_phase_deg.set("{:g}".format(spec.start_phase_deg))
        self.noise_rms_mv.set("{:g}".format(spec.noise_rms_mv))
        self.random_seed.set(str(spec.random_seed))
        self.random_start_phase.set(False)
        self.random_relative_phases.set(False)

    def _load_selected_preset(self) -> None:
        index = self.preset_combo.current()
        if index < 0:
            index = 0
        self._load_spec(typical_specs()[index])
        self.generate()

    def _load_selected_record(self) -> None:
        spec = self._selected_record()
        if spec is None:
            messagebox.showinfo("载入记录", "请先选择一条记录。")
            return
        self._load_spec(spec)
        self.generate()

    def _save_current_record(self) -> None:
        try:
            if self.current_result is None:
                self.generate()
            if self.current_result is None:
                return
            spec = self.current_result.spec
            existing = next(
                (
                    item
                    for item in self.records
                    if item.group_number == spec.group_number
                ),
                None,
            )
            if existing is not None and not messagebox.askyesno(
                "覆盖记录",
                "T{:03d}已经存在，是否覆盖？".format(
                    spec.group_number
                ),
            ):
                return
            self.records = [
                item
                for item in self.records
                if item.group_number != spec.group_number
            ]
            self.records.append(spec)
            save_registry(self.records)
            self._refresh_record_list()
            self.status_text.set(
                "已保存T{:03d}；随机相位已固化为当前实际值。".format(
                    spec.group_number
                )
            )
        except Exception as error:
            messagebox.showerror("保存失败", str(error))

    def _delete_selected_record(self) -> None:
        spec = self._selected_record()
        if spec is None:
            messagebox.showinfo("删除记录", "请先选择一条记录。")
            return
        if not messagebox.askyesno(
            "删除记录",
            "确认删除T{:03d} {}？".format(
                spec.group_number,
                spec.name,
            ),
        ):
            return
        self.records = [
            item
            for item in self.records
            if item.group_number != spec.group_number
        ]
        save_registry(self.records)
        self._refresh_record_list()

    def _export_current(self) -> None:
        if self.current_result is None:
            self.generate()
        if self.current_result is None:
            return
        try:
            folder = export_group_artifacts(self.current_result)
            self.figure.savefig(
                folder / "comparison.png",
                dpi=160,
                bbox_inches="tight",
            )
            self.figure.savefig(
                folder / "comparison.svg",
                format="svg",
                bbox_inches="tight",
            )
            self.status_text.set("当前组已导出到：{}".format(folder))
        except Exception as error:
            messagebox.showerror("导出失败", str(error))

    def _export_firmware_header(self) -> None:
        try:
            if not self.records:
                raise ValueError("当前没有已保存记录。")
            path = export_custom_header(self.records)
            self.status_text.set(
                "已生成固件头文件：{}；将"
                "ANALYZER_CUSTOM_TEST_ENABLE设为1即可启用。".format(path)
            )
            messagebox.showinfo(
                "固件头文件已生成",
                "{}\n\n在analyzer_bridge.h中设置：\n"
                "#define ANALYZER_CUSTOM_TEST_ENABLE 1U".format(path),
            )
        except Exception as error:
            messagebox.showerror("固件导出失败", str(error))

    def _run_coverage_sweep(self) -> None:
        script = REPO_ROOT / "tools" / "analyze_phase_coverage.py"
        try:
            subprocess.check_call([sys.executable, str(script)])
            plot = (
                REPO_ROOT
                / "tests"
                / "phase_coverage"
                / "phase_coverage_sweep.svg"
            )
            self.status_text.set(
                "10 Hz均匀 + 有理共振混合覆盖扫描完成：{}".format(plot)
            )
            if os.name == "nt":
                os.startfile(str(plot))
        except Exception as error:
            messagebox.showerror("覆盖度扫描失败", str(error))

    def _open_output_folder(self) -> None:
        CUSTOM_DATA_ROOT.mkdir(parents=True, exist_ok=True)
        if os.name == "nt":
            os.startfile(str(CUSTOM_DATA_ROOT))

    def _new_random_seed(self) -> None:
        self.random_seed.set(
            str(random.SystemRandom().randint(1, 2_147_483_647))
        )
        self.generate()


def run_self_test() -> int:
    """Headless-friendly regression used before launching the GUI."""

    write_typical_registry_if_missing()
    specs = load_registry()
    if not specs:
        raise RuntimeError("registry self-test did not create examples")
    for spec in specs:
        for phase_bin_count in (64, 128, 256):
            result = simulate(
                spec,
                phase_bin_count=phase_bin_count,
            )
            if result.adc_codes.size != DEFAULT_SAMPLE_COUNT:
                raise RuntimeError("unexpected ADC sample count")
            if result.display_waveform_mv.size != DEFAULT_DISPLAY_WIDTH:
                raise RuntimeError("unexpected display width")
            if (
                result.harmonic_fit.rank
                != result.harmonic_fit.parameter_count
            ):
                raise RuntimeError("harmonic fit is rank deficient")
            print(
                "T{:03d} {:<30} B={:3d} hit={:3d} "
                "fold={:.3f} LS={:.3f}".format(
                    spec.group_number,
                    spec.name,
                    phase_bin_count,
                    result.coverage.occupied_hard_bins,
                    result.display_rmse_mv,
                    result.harmonic_fit.display_rmse_mv,
                )
            )
    path = export_custom_header(specs)
    print("custom header:", path)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="不打开GUI，只运行典型数据和C头文件导出自检。",
    )
    args = parser.parse_args()
    if args.self_test:
        return run_self_test()

    root = tk.Tk()
    WaveformLabApp(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
