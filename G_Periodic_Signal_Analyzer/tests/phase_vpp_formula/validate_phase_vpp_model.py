#!/usr/bin/env python3
"""Validate the teammate Goertzel-phase Vpp reconstruction formula.

The test reproduces goertzel_sync.c's phase convention, removes a synthetic
front-end phase response, converts every harmonic to a phase relative to the
fundamental, and compares the 4096-point firmware reconstruction with a dense
reference waveform.
"""

from __future__ import annotations

import math
import random
from dataclasses import dataclass


FS_HZ = 2_048_193.0
ADC_POINT_COUNT = 4096
MODEL_POINT_COUNT = 4096
REFERENCE_POINT_COUNT = 131_072


def wrap_phase(value: float) -> float:
    return math.atan2(math.sin(value), math.cos(value))


def frontend_phase_rad(frequency_hz: float) -> float:
    """Synthetic nonlinear phase used only to prove correction algebra."""
    ratio = frequency_hz / 500_000.0
    return -0.18 * ratio - 0.42 * ratio * ratio


def goertzel_phase(samples: list[float], frequency_hz: float) -> float:
    omega = 2.0 * math.pi * frequency_hz / FS_HZ
    coefficient = 2.0 * math.cos(omega)
    state1 = 0.0
    state2 = 0.0
    for sample in samples:
        state = sample + coefficient * state1 - state2
        state2 = state1
        state1 = state
    real = state1 - math.cos(omega) * state2
    imag = math.sin(omega) * state2
    return math.atan2(imag, real)


@dataclass(frozen=True)
class Component:
    order: int
    amplitude_v: float
    phase_rad: float


def waveform_value(phase_rad: float, components: list[Component]) -> float:
    return sum(
        component.amplitude_v
        * math.sin(component.order * phase_rad + component.phase_rad)
        for component in components
    )


def peak_to_peak(components: list[Component], point_count: int) -> float:
    values = [
        waveform_value(2.0 * math.pi * index / point_count, components)
        for index in range(point_count)
    ]
    return max(values) - min(values)


def legacy_getup_vpp(
    fundamental_hz: float, components: list[Component]
) -> float:
    """Reproduce win/main.c getup() before the integration fix."""
    values: list[float] = []
    for index in range(MODEL_POINT_COUNT):
        if len(components) == 2:
            value_v = (
                components[0].amplitude_v
                * math.sin(2.0 * math.pi * index / 1000.0)
                + components[1].amplitude_v
                * math.sin(
                    2.0 * math.pi * index / 1000.0
                    + components[1].phase_rad
                )
            )
        else:
            value_v = sum(
                component.amplitude_v
                * math.sin(
                    2.0
                    * math.pi
                    * component.order
                    * fundamental_hz
                    * index
                    / MODEL_POINT_COUNT
                    + component.phase_rad
                )
                for component in components
            )
        values.append(value_v)
    return max(values) - min(values)


def reconstruct_case(
    fundamental_hz: float,
    source_components: list[Component],
) -> tuple[float, float, float]:
    samples: list[float] = []
    for index in range(ADC_POINT_COUNT):
        time_s = index / FS_HZ
        value_v = 0.0
        for component in source_components:
            frequency_hz = component.order * fundamental_hz
            measured_phase = (
                component.phase_rad + frontend_phase_rad(frequency_hz)
            )
            value_v += component.amplitude_v * math.sin(
                2.0 * math.pi * frequency_hz * time_s + measured_phase
            )
        samples.append(value_v)

    measured_phases = [
        goertzel_phase(samples, component.order * fundamental_hz)
        for component in source_components
    ]
    corrected_fundamental = (
        measured_phases[0] - frontend_phase_rad(fundamental_hz)
    )

    reconstructed = [
        Component(order=1, amplitude_v=source_components[0].amplitude_v,
                  phase_rad=0.0)
    ]
    for component, measured_phase in zip(
        source_components[1:], measured_phases[1:]
    ):
        corrected_phase = measured_phase - frontend_phase_rad(
            component.order * fundamental_hz
        )
        relative_phase = wrap_phase(
            corrected_phase
            - component.order * corrected_fundamental
            - (component.order - 1) * math.pi / 2.0
        )
        reconstructed.append(
            Component(
                order=component.order,
                amplitude_v=component.amplitude_v,
                phase_rad=relative_phase,
            )
        )

    reference_relative = [
        Component(
            order=component.order,
            amplitude_v=component.amplitude_v,
            phase_rad=wrap_phase(
                component.phase_rad
                - component.order * source_components[0].phase_rad
            ),
        )
        for component in source_components
    ]
    reference_relative[0] = Component(
        order=1,
        amplitude_v=source_components[0].amplitude_v,
        phase_rad=0.0,
    )

    return (
        peak_to_peak(reference_relative, REFERENCE_POINT_COUNT),
        peak_to_peak(reconstructed, MODEL_POINT_COUNT),
        legacy_getup_vpp(fundamental_hz, reconstructed),
    )


def main() -> None:
    random_generator = random.Random(20260801)
    errors_mv: list[float] = []
    legacy_errors_mv: list[float] = []

    for component_count in (2, 3):
        for _ in range(36):
            fundamental_hz = random_generator.choice(
                (10_000.0, 25_000.0, 50_000.0, 62_500.0, 100_000.0)
            )
            possible_orders = [
                order
                for order in range(2, 10)
                if order * fundamental_hz <= 500_000.0
            ]
            orders = [1] + sorted(
                random_generator.sample(
                    possible_orders, component_count - 1
                )
            )
            components = [
                Component(
                    order=order,
                    amplitude_v=random_generator.uniform(0.01, 0.12)
                    / order**0.35,
                    phase_rad=random_generator.uniform(-math.pi, math.pi),
                )
                for order in orders
            ]
            reference_vpp, reconstructed_vpp, legacy_vpp = reconstruct_case(
                fundamental_hz, components
            )
            errors_mv.append(
                abs(reference_vpp - reconstructed_vpp) * 1000.0
            )
            legacy_errors_mv.append(
                abs(reference_vpp - legacy_vpp) * 1000.0
            )

    maximum_error_mv = max(errors_mv)
    mean_error_mv = sum(errors_mv) / len(errors_mv)
    print(f"cases={len(errors_mv)}")
    print(f"mean_abs_error_mv={mean_error_mv:.6f}")
    print(f"max_abs_error_mv={maximum_error_mv:.6f}")
    print(
        "legacy_getup_max_abs_error_mv="
        f"{max(legacy_errors_mv):.6f}"
    )
    if maximum_error_mv > 0.05:
        raise SystemExit("phase/Vpp reconstruction error exceeds 0.05 mV")


if __name__ == "__main__":
    main()
