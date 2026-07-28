"""LISN (artificial network) models for conducted-emissions setups.

Single-line simplified network: the EUT sees the main inductor towards the mains
in parallel with the coupling capacitor into the measuring receiver. The 1 µF
mains-side capacitor and the CISPR 16 5 Ω/250 µH low-frequency branch are
omitted, so the model is valid for the 150 kHz - 108 MHz measurement bands, not
for CISPR band A (9-150 kHz).
"""

import math
from dataclasses import dataclass

import numpy as np


@dataclass(frozen=True)
class Lisn:
    name: str
    inductance_h: float
    coupling_capacitance_f: float
    measuring_impedance_ohm: float = 50.0

    def __post_init__(self):
        if self.inductance_h <= 0.0 or self.coupling_capacitance_f <= 0.0:
            raise ValueError("LISN inductance and coupling capacitance must be positive")
        if self.measuring_impedance_ohm <= 0.0:
            raise ValueError("measuring impedance must be positive")

    def measuring_branch_impedance(self, f_hz):
        w = 2.0 * math.pi * np.asarray(f_hz, dtype=float)
        if np.any(w <= 0.0):
            raise ValueError("frequency must be positive")
        return self.measuring_impedance_ohm + 1.0 / (1j * w * self.coupling_capacitance_f)

    def eut_impedance(self, f_hz):
        """Impedance the EUT sees between line and reference ground."""
        w = 2.0 * math.pi * np.asarray(f_hz, dtype=float)
        if np.any(w <= 0.0):
            raise ValueError("frequency must be positive")
        z_l = 1j * w * self.inductance_h
        z_m = self.measuring_branch_impedance(f_hz)
        return z_l * z_m / (z_l + z_m)

    def to_spice_subckt(self, name=None, terminated=True):
        """SPICE subcircuit, nodes: eut mains meas (meas is the receiver port)."""
        # a caller-supplied name is folded too: a space in a .subckt name makes
        # the following word a node, silently emitting a different circuit
        sub = (name if name is not None else self.name).replace(" ", "_")
        lines = [
            f".subckt {sub} eut mains meas",
            f"L1 eut mains {self.inductance_h:.6g}",
            f"C1 eut meas {self.coupling_capacitance_f:.6g}",
        ]
        if terminated:
            lines.append(f"R1 meas 0 {self.measuring_impedance_ohm:.6g}")
        lines.append(f".ends {sub}")
        return "\n".join(lines) + "\n"


def cispr16_lisn():
    """50 µH / 50 Ω V-network (CISPR 16-1-2), mains setups: CISPR 32/11, FCC 15."""
    return Lisn("CISPR16 50uH", 50e-6, 0.1e-6)


def cispr25_lisn():
    """5 µH / 50 Ω network (CISPR 25), automotive DC supplies."""
    return Lisn("CISPR25 5uH", 5e-6, 0.1e-6)
