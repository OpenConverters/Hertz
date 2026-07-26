#pragma once
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "hertz/Lisn.hpp"

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
inline std::string filter_spice_deck(int stages, double lCmH, double cXF, double cYPerLineF,
                                     double lDmH, const Lisn& lisn, const std::string& mode,
                                     int nLines = 2) {
    if (stages < 1 || stages > 4) {
        throw std::invalid_argument("stages must be 1..4");
    }
    if (nLines < 2 || nLines > 4) {
        throw std::invalid_argument("nLines must be 2..4");
    }
    if (mode != "cm" && mode != "dm") {
        throw std::invalid_argument("netlist mode must be cm or dm");
    }
    double coupling = 1.0 - lDmH / (2.0 * lCmH);
    if (!(coupling > 0.0 && coupling < 1.0)) {
        throw std::invalid_argument(
            "leakage inductance is not consistent with the CM inductance (K out of (0,1))");
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
    std::snprintf(head, sizeof(head),
                  "* Hertz line filter — ANP015 design, %s\n"
                  "* CM choke per stage: %.6g H per winding, leakage (DM loop) %.6g H -> K=%.6f\n",
                  topo, lCmH, lDmH, coupling);
    std::string netlist = head;
    netlist += lisn.to_spice_subckt("LISN");
    char buffer[256];
    std::vector<std::string> node;
    for (int i = 0; i < nLines; ++i) {
        node.push_back(line[i] + "_src");
    }
    for (int s = 1; s <= stages; ++s) {
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
        std::snprintf(buffer, sizeof(buffer), "Xlisn_%s %s_out mains_%s meas_%s LISN\n",
                      line[i].c_str(), line[i].c_str(), line[i].c_str(), line[i].c_str());
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

}  // namespace Hertz
