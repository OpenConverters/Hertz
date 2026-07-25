#pragma once
#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>
#include <stdexcept>
#include <string>

namespace Hertz {

// Single-line simplified LISN: the EUT sees the main inductor towards the mains
// in parallel with the coupling capacitor into the measuring receiver. The 1 µF
// mains-side capacitor and the CISPR 16 5 Ohm/250 µH low-frequency branch are
// omitted, so the model is valid for the 150 kHz - 108 MHz measurement bands,
// not for CISPR band A (9-150 kHz).
struct Lisn {
    std::string name;
    double inductanceH;
    double couplingCapacitanceF;
    double measuringImpedanceOhm;

    Lisn(std::string lisnName, double inductance, double couplingCapacitance,
         double measuringImpedance = 50.0)
        : name(std::move(lisnName)),
          inductanceH(inductance),
          couplingCapacitanceF(couplingCapacitance),
          measuringImpedanceOhm(measuringImpedance) {
        if (inductanceH <= 0.0 || couplingCapacitanceF <= 0.0) {
            throw std::invalid_argument("LISN inductance and coupling capacitance must be positive");
        }
        if (measuringImpedanceOhm <= 0.0) {
            throw std::invalid_argument("measuring impedance must be positive");
        }
    }

    std::complex<double> measuring_branch_impedance(double fHz) const {
        if (fHz <= 0.0) {
            throw std::invalid_argument("frequency must be positive");
        }
        double w = 2.0 * std::numbers::pi * fHz;
        return measuringImpedanceOhm + 1.0 / (std::complex<double>(0.0, w * couplingCapacitanceF));
    }

    // Impedance the EUT sees between line and reference ground.
    std::complex<double> eut_impedance(double fHz) const {
        double w = 2.0 * std::numbers::pi * fHz;
        std::complex<double> zL(0.0, w * inductanceH);
        std::complex<double> zM = measuring_branch_impedance(fHz);
        return zL * zM / (zL + zM);
    }

    // SPICE subcircuit, nodes: eut mains meas (meas is the receiver port).
    std::string to_spice_subckt(const std::string& subcktName = "", bool terminated = true) const {
        std::string sub = subcktName.empty() ? name : subcktName;
        for (auto& c : sub) {
            if (c == ' ') c = '_';
        }
        char buffer[128];
        std::string text = ".subckt " + sub + " eut mains meas\n";
        std::snprintf(buffer, sizeof(buffer), "L1 eut mains %.6g\n", inductanceH);
        text += buffer;
        std::snprintf(buffer, sizeof(buffer), "C1 eut meas %.6g\n", couplingCapacitanceF);
        text += buffer;
        if (terminated) {
            std::snprintf(buffer, sizeof(buffer), "R1 meas 0 %.6g\n", measuringImpedanceOhm);
            text += buffer;
        }
        text += ".ends " + sub + "\n";
        return text;
    }
};

// 50 µH / 50 Ohm V-network (CISPR 16-1-2), mains setups: CISPR 32/11, FCC 15.
inline Lisn cispr16_lisn() {
    return Lisn("CISPR16 50uH", 50e-6, 0.1e-6);
}

// 5 µH / 50 Ohm network (CISPR 25), automotive DC supplies.
inline Lisn cispr25_lisn() {
    return Lisn("CISPR25 5uH", 5e-6, 0.1e-6);
}

}  // namespace Hertz
