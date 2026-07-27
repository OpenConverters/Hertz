#pragma once
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "hertz/Lisn.hpp"
#include "hertz/Network.hpp"

namespace Hertz {

// SPICE deck of the designed filter between a noise port and one LISN per
// line, ready for Kirchhoff/ngspice/LTspice (.ac insertion-loss analysis).
//
// nLines: 2 = L/N (or a DC +/RTN pair), 3 = 3-phase 3-wire (delta X),
// 4 = 3-phase + neutral (star X). The CM choke is nLines identical coupled
// windings with the SAME pairwise coupling K = 1 - L_dm/(2·L_cm): with equal
// self-inductances and equal K, any line-to-line DM loop sees
// 2·L·(1-K) = L_dm regardless of the other windings, so the two-winding
// derivation carries over unchanged.
namespace detail {
inline std::string spice_deck_impl(int stages, double lCmH, double cXF, double cYPerLineF,
                                   double lDmH, const Lisn& lisn, const std::string& mode,
                                   int nLines, bool includeFilter) {
    if (stages < 1 || stages > 4) {
        throw std::invalid_argument("stages must be 1..4");
    }
    if (nLines < 2 || nLines > 4) {
        throw std::invalid_argument("nLines must be 2..4");
    }
    if (mode != "cm" && mode != "dm") {
        throw std::invalid_argument("netlist mode must be cm or dm");
    }
    double coupling = 1.0;
    if (includeFilter) {
        coupling = 1.0 - lDmH / (2.0 * lCmH);
        if (!(coupling > 0.0 && coupling < 1.0)) {
            throw std::invalid_argument(
                "leakage inductance is not consistent with the CM inductance (K out of (0,1))");
        }
    }
    std::vector<std::string> line;
    if (nLines == 2) {
        line = {"line", "neut"};
    } else if (nLines == 3) {
        line = {"l1", "l2", "l3"};
    } else {
        line = {"l1", "l2", "l3", "neut"};
    }
    const char* topo = nLines == 2 ? "single-phase pair" : nLines == 3 ? "3-phase 3-wire (delta X)"
                                                                       : "3-phase + neutral (star X)";
    char head[320];
    if (includeFilter) {
        std::snprintf(head, sizeof(head),
                      "* Hertz line filter — ANP015 design, %s\n"
                      "* CM choke per stage: %.6g H per winding, leakage (DM loop) %.6g H -> K=%.6f\n",
                      topo, lCmH, lDmH, coupling);
    } else {
        std::snprintf(head, sizeof(head),
                      "* Hertz REFERENCE bench, %s — same drive and LISNs, NO filter\n"
                      "* IL of the filter deck = vdb(meas_*) here minus vdb(meas_*) there\n",
                      topo);
    }
    std::string netlist = head;
    netlist += lisn.to_spice_subckt("LISN");
    char buffer[256];
    std::vector<std::string> node;
    for (int i = 0; i < nLines; ++i) {
        node.push_back(line[i] + "_src");
    }
    for (int s = 1; includeFilter && s <= stages; ++s) {
        std::vector<std::string> out;
        for (int i = 0; i < nLines; ++i) {
            out.push_back(s == stages ? line[i] + "_out" : line[i] + "_" + std::to_string(s));
        }
        for (int i = 0; i < nLines; ++i) {
            std::snprintf(buffer, sizeof(buffer), "Lcm_%s_%d %s %s %.6g\n", line[i].c_str(), s,
                          node[i].c_str(), out[i].c_str(), lCmH);
            netlist += buffer;
        }
        for (int i = 0; i < nLines; ++i) {
            for (int j = i + 1; j < nLines; ++j) {
                std::snprintf(buffer, sizeof(buffer), "Kcm%d_%d%d Lcm_%s_%d Lcm_%s_%d %.6f\n", s,
                              i + 1, j + 1, line[i].c_str(), s, line[j].c_str(), s, coupling);
                netlist += buffer;
            }
        }
        if (nLines == 2) {
            std::snprintf(buffer, sizeof(buffer), "Cx%d %s %s %.6g\n", s, out[0].c_str(),
                          out[1].c_str(), cXF);
            netlist += buffer;
        } else if (nLines == 3) {
            // delta: one X capacitor across every line pair
            for (int i = 0; i < 3; ++i) {
                int j = (i + 1) % 3;
                std::snprintf(buffer, sizeof(buffer), "Cx%d_%d%d %s %s %.6g\n", s, i + 1, j + 1,
                              out[i].c_str(), out[j].c_str(), cXF);
                netlist += buffer;
            }
        } else {
            // star: one X capacitor from every phase to the neutral rail
            for (int i = 0; i < 3; ++i) {
                std::snprintf(buffer, sizeof(buffer), "Cx%d_%dn %s %s %.6g\n", s, i + 1,
                              out[i].c_str(), out[3].c_str(), cXF);
                netlist += buffer;
            }
        }
        for (int i = 0; i < nLines; ++i) {
            std::snprintf(buffer, sizeof(buffer), "Cy_%s_%d %s 0 %.6g\n", line[i].c_str(), s,
                          out[i].c_str(), cYPerLineF);
            netlist += buffer;
        }
        node = out;
    }
    for (int i = 0; i < nLines; ++i) {
        // reference bench: the LISNs sit straight on the source nodes
        std::snprintf(buffer, sizeof(buffer), "Xlisn_%s %s mains_%s meas_%s LISN\n",
                      line[i].c_str(), node[i].c_str(), line[i].c_str(), line[i].c_str());
        netlist += buffer;
    }
    std::snprintf(buffer, sizeof(buffer), "Vmains mains_%s 0 DC 0\n", line[0].c_str());
    netlist += buffer;
    for (int i = 1; i < nLines; ++i) {
        std::snprintf(buffer, sizeof(buffer), "Rmains_%s mains_%s 0 1m\n", line[i].c_str(),
                      line[i].c_str());
        netlist += buffer;
    }
    netlist += "* LISN model omissions (see LISN screen): band-A branch, mains 1uF, 1k bleed\n";
    netlist +=
        "* capacitor ESL/ESR not modeled in this deck — the IL curves carry the parasitic-aware "
        "prediction\n";
    if (mode == "cm") {
        netlist += "* COMMON-MODE drive: all lines together against PE (exercises choke + Y caps)\n";
        netlist += "Vnoise cm_src 0 AC 1\n";
        for (int i = 0; i < nLines; ++i) {
            std::snprintf(buffer, sizeof(buffer), "Rsrc_%s cm_src %s 1m\n", line[i].c_str(),
                          (line[i] + "_src").c_str());
            netlist += buffer;
        }
    } else if (nLines == 4) {
        // star X sits phase-to-neutral, so that is the DM loop the design sized
        netlist += "* DIFFERENTIAL-MODE drive: phase 1 against neutral\n";
        netlist += "Vnoise l1_src neut_src AC 1\n";
    } else if (nLines == 3) {
        netlist += "* DIFFERENTIAL-MODE drive: line 1 against line 2\n";
        netlist += "Vnoise l1_src l2_src AC 1\n";
    } else {
        netlist += "* DIFFERENTIAL-MODE drive: line against neutral\n";
        netlist += "Vnoise line_src neut_src AC 1\n";
    }
    netlist += ".ac dec 100 150k 30meg\n.print ac";
    for (int i = 0; i < nLines; ++i) {
        netlist += " vdb(meas_" + line[i] + ")";
    }
    netlist += "\n.end\n";
    return netlist;
}
}  // namespace detail

inline std::string filter_spice_deck(int stages, double lCmH, double cXF, double cYPerLineF,
                                     double lDmH, const Lisn& lisn, const std::string& mode,
                                     int nLines = 2) {
    return detail::spice_deck_impl(stages, lCmH, cXF, cYPerLineF, lDmH, lisn, mode, nLines, true);
}

// The reference bench for the ngspice round-trip (ABT #299): identical drive
// and one LISN per line, NO filter. The filter's insertion loss is the
// measurement-port ratio between this deck's run and the filter deck's run —
// the LISN's own eut->receiver divider cancels exactly in the ratio, so no
// analytic LISN correction is needed.
inline std::string lisn_reference_deck(const Lisn& lisn, const std::string& mode, int nLines = 2) {
    return detail::spice_deck_impl(1, 1.0, 1.0, 1.0, 1.0, lisn, mode, nLines, false);
}

// ABCD insertion loss under the DECK's actual terminations — the analytic twin
// of the ngspice round-trip. Same elements the deck nets, ideal (the deck
// states ESL/ESR are not modeled):
//  - CM: series inductance is the coupled stack's common-mode value
//    L·(1+(n−1)K)/n with K = 1 − L_dm/(2L) (exact for equal self-L, equal
//    all-pairs K, equal drive); shunt = n·C_Y per stage. Source: the deck's n
//    1 mΩ drive resistors in parallel; load: n LISN EUT ports in parallel.
//  - DM: series = L_dm (the pairwise loop 2L(1−K), independent of the other
//    windings by symmetric cancellation); shunt = effective X (delta 1.5·C)
//    plus the two Y capacitors in series across the loop (C_Y/2). Source: the
//    deck's ideal voltage drive (0 Ω); load: out one LISN arm, back another
//    (2·Z_LISN).
// Frequencies are the caller's — pass the ngspice sweep so the two engines
// are compared at identical points.
inline std::vector<double> deck_abcd_il(const Lisn& lisn, const std::string& mode, int nLines,
                                        int stages, double lCmH, double cYPerLineF, double lDmH,
                                        double cXF, double cXDmFactor,
                                        const std::vector<double>& freqsHz) {
    if (mode != "cm" && mode != "dm") {
        throw std::invalid_argument("mode must be cm or dm");
    }
    if (nLines < 2 || nLines > 4) {
        throw std::invalid_argument("nLines must be 2..4");
    }
    double coupling = 1.0 - lDmH / (2.0 * lCmH);
    if (!(coupling > 0.0 && coupling < 1.0)) {
        throw std::invalid_argument(
            "leakage inductance is not consistent with the CM inductance (K out of (0,1))");
    }
    std::vector<double> out;
    out.reserve(freqsHz.size());
    for (double f : freqsHz) {
        std::complex<double> zLisn = lisn.eut_impedance(f);
        Abcd network;
        Complex zSource, zLoad;
        if (mode == "cm") {
            double lCmEffective = lCmH * (1.0 + (nLines - 1) * coupling) / nLines;
            network = lc_filter_abcd(f, lCmEffective, nLines * cYPerLineF, stages);
            zSource = Complex(1e-3 / nLines, 0.0);
            zLoad = zLisn / static_cast<double>(nLines);
        } else {
            network = lc_filter_abcd(f, lDmH, cXF * cXDmFactor + cYPerLineF / 2.0, stages);
            zSource = Complex(0.0, 0.0);
            zLoad = 2.0 * zLisn;
        }
        out.push_back(insertion_loss_db(network, zSource, zLoad));
    }
    return out;
}

}  // namespace Hertz
