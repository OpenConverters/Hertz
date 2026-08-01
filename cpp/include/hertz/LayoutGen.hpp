#pragma once
// LayoutGen — the filter's PCB, generated (filter-block plan S1, ABT #442).
//
// A line filter is not a general layout problem: it is a canonical chain —
// terminals → X (+ discharge R) → CM choke → [X → choke] → Y pair →
// terminals, with a protective-earth spine — so this is a parametric
// TEMPLATE, not an auto-router. Hertz emits both the footprints and the
// netlist, so pin consistency holds by construction; the physical package of
// the actually-ordered part must still be reviewed before fabrication, and
// the board carries that warning on silk.
//
// Layout scheme (single-phase / DC pair, the nLines == 2 template):
//
//   PE ──────────────────────────────────────────────  (top spine)
//   │            Y_L
//   L ──[J]──┬──────[CMC╞═╡CMC]──┬────────[J]── L      (top rail)
//            X1,Rd              [X2,CMC2]  │
//   N ──[J]──┴──────[CMC╞═╡CMC]──┴────────[J]── N      (bottom rail)
//   │            Y_N
//   PE ──────────────────────────────────────────────  (bottom spine)
//
// The two PE spines join at the LEFT edge, before any L/N copper — a
// three-sided earth ring, exactly how chassis-mount filter bricks are built,
// and it keeps the whole board single-layer with no crossings.
//
// Electrical sizing is built in, not decorative:
//   * trace widths from the rated current (IPC-2221 external-layer
//     approximation, 35 um Cu, 30 K rise);
//   * L–N and line–PE spacing floors shaped by IEC 60664-1 for 250 VAC,
//     pollution degree 2 (conservative defaults, overridable) — these are
//     engineering floors, NEVER a compliance claim;
//   * the generator MEASURES its own emitted copper and throws if any floor
//     is violated — a generated board is never silently out of spec.

#include <cmath>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "hertz/FilterDesign.hpp"

namespace Hertz::layout {

// ---------------------------------------------------------------------------
// Packages — conservative defaults by VALUE, all overridable. mm everywhere.
// ---------------------------------------------------------------------------

struct Package {
    double bodyW = 0;   // x span of the body outline
    double bodyH = 0;   // y span
    double pitch = 0;   // pad pitch (2-pad parts: across; choke: along x)
    double pitchY = 0;  // choke only: pad pitch across the rails
    double drill = 0;   // THT drill
    double pad = 0;     // pad (annular) diameter
};

// X2 film box: pitch follows capacitance the way every vendor's series does.
inline Package default_x2_package(double cF) {
    if (cF <= 0.0) throw std::invalid_argument("X capacitance must be positive");
    if (cF <= 0.10e-6) return {13.0, 6.0, 10.0, 0, 1.0, 2.2};
    if (cF <= 0.47e-6) return {18.0, 8.5, 15.0, 0, 1.0, 2.2};
    if (cF <= 1.50e-6) return {26.5, 10.5, 22.5, 0, 1.2, 2.6};
    return {31.5, 13.0, 27.5, 0, 1.2, 2.6};
}

// Y2 ceramic disc.
inline Package default_y2_package(double cF) {
    if (cF <= 0.0) throw std::invalid_argument("Y capacitance must be positive");
    if (cF <= 2.2e-9) return {8.0, 5.0, 7.5, 0, 0.9, 2.0};
    return {10.0, 6.0, 10.0, 0, 1.0, 2.2};
}

// CM choke, 4 THT pins on a rectangle: 1(in-L) 2(out-L) on the L side,
// 3(in-N) 4(out-N) on the N side. Body size follows inductance the way
// toroidal CMC families grow.
inline Package default_choke_package(double lH) {
    if (lH <= 0.0) throw std::invalid_argument("CM inductance must be positive");
    if (lH <= 1.0e-3) return {14.0, 14.0, 9.0, 9.0, 1.1, 2.4};
    if (lH <= 10e-3) return {19.0, 19.0, 12.5, 12.5, 1.1, 2.4};
    if (lH <= 40e-3) return {27.0, 27.0, 17.5, 17.5, 1.3, 2.8};
    return {33.0, 33.0, 22.5, 22.5, 1.3, 2.8};
}

// Ring-lug terminal (M3) — one per line per side, the chassis-filter way;
// fixed-pitch terminal blocks would fight the rail separation.
inline Package lug_package() { return {8.0, 8.0, 0, 0, 3.2, 6.0}; }

// Discharge resistor, axial, stood vertical-in-plan across the rails.
inline Package discharge_r_package() { return {4.0, 11.0, 10.0, 0, 0.9, 2.0}; }

// ---------------------------------------------------------------------------
// Electrical sizing
// ---------------------------------------------------------------------------

// IPC-2221 external layers: I = 0.048 · dT^0.44 · A^0.725 (A in mil^2),
// inverted for width at 35 um (1.378 mil) copper. Floored at 0.5 mm.
inline double trace_width_ipc2221_mm(double currentA, double tempRiseK = 30.0) {
    if (currentA <= 0.0 || tempRiseK <= 0.0) {
        throw std::invalid_argument("current and temperature rise must be positive");
    }
    const double areaMil2 =
        std::pow(currentA / (0.048 * std::pow(tempRiseK, 0.44)), 1.0 / 0.725);
    const double wMm = areaMil2 / 1.378 * 0.0254;
    return std::max(wMm, 0.5);
}

struct LayoutParams {
    double traceWmm = 0;       // 0 = derive from ratedCurrentA
    // Spacing floors shaped by IEC 60664-1 @ 250 VAC, overvoltage cat II,
    // pollution degree 2: functional L-N creepage ~2.5 mm, basic line-PE
    // creepage ~3.2 mm — held with margin. Floors, not a compliance claim.
    double clearLNmm = 3.2;
    double clearPEmm = 4.0;
    double partGapmm = 3.2;    // body-to-body along the chain (>= clearLN)
    double edgeMarginmm = 3.0;
};

inline LayoutParams layout_params_for(double ratedCurrentA) {
    LayoutParams p;
    p.traceWmm = trace_width_ipc2221_mm(ratedCurrentA);
    p.partGapmm = std::max(p.partGapmm, p.clearLNmm);
    return p;
}

// ---------------------------------------------------------------------------
// Generation
// ---------------------------------------------------------------------------

// one shunt part's connection into the rails: total stub length and width —
// the series inductance the part's own ESL does not include
struct StubRun {
    std::string ref;
    double lenMm = 0, wMm = 0;
};

struct GeneratedBoard {
    std::string kicadPcb;
    double boardWmm = 0, boardHmm = 0;
    double minLNGapmm = 0;   // measured on the emitted copper, not assumed
    double minPEGapmm = 0;
    int parts = 0;
    // geometry summary for the layout-parasitic model (S2): the dirty-side
    // and clean-side rail runs, the rail separation, and every shunt
    // part's connection stubs — all in mm, all from the same numbers the
    // copper was emitted with
    double railSepMm = 0, traceWMm = 0;
    double inX1 = 0, inX2 = 0;     // input rail pair span (J_IN .. choke 1)
    double outX1 = 0, outX2 = 0;   // output rail pair span (last choke .. J_OUT)
    double peSpineLenMm = 0;       // shared earth run, input lug to output lug
    std::vector<StubRun> stubs;
};

namespace detail {

struct Pad {
    double x, y, drill, pad;
    int net;
    std::string pin;
};
struct Part {
    std::string ref, value;
    double cx, cy, bodyW, bodyH;
    std::vector<Pad> pads;
};
struct Seg {
    double x1, y1, x2, y2, w;
    int net;
};

inline std::string fmt(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.3f", v);
    return b;
}

// gap between two parallel copper runs given center distance and widths
inline double edge_gap(double centerDist, double wA, double wB) {
    return centerDist - wA / 2.0 - wB / 2.0;
}

}  // namespace detail

// Generate the filter board for a completed design. Single-phase / DC-pair
// template (nLines == 2); the 3-phase templates follow the same scheme and
// are recorded in the plan, not silently absent. cXF / cYF / lCmH are the
// SELECTED values from the design; ratedCurrentA sizes the copper.
inline GeneratedBoard generate_filter_board(
    const LineFilterDesign& d, double ratedCurrentA,
    std::optional<LayoutParams> paramsOpt = std::nullopt,
    std::optional<Package> xPkgOpt = std::nullopt,
    std::optional<Package> yPkgOpt = std::nullopt,
    std::optional<Package> chokePkgOpt = std::nullopt) {
    if (d.nLines != 2) {
        throw std::invalid_argument(
            "layout template S1 covers the single-phase/DC pair (nLines == 2); "
            "the 3-phase templates are planned, not silently skipped "
            "(docs/filter-block-plan.md)");
    }
    if (ratedCurrentA <= 0.0) {
        throw std::invalid_argument("rated current must be positive");
    }
    const LayoutParams p = paramsOpt ? *paramsOpt : layout_params_for(ratedCurrentA);
    const double tw = p.traceWmm > 0 ? p.traceWmm : trace_width_ipc2221_mm(ratedCurrentA);
    const Package xPkg = xPkgOpt ? *xPkgOpt : default_x2_package(d.cXSelectedF);
    const Package yPkg = yPkgOpt ? *yPkgOpt : default_y2_package(d.cYPerLineF);
    const Package cPkg = chokePkgOpt ? *chokePkgOpt : default_choke_package(d.lCmSelectedH);
    const Package lug = lug_package();
    const Package rPkg = discharge_r_package();

    // Rail separation. Every across-the-rails structure must fit INSIDE the
    // rail band, or its pads would invade the L-N gap or the PE zone
    // unmeasured: the X pitch, the discharge R pitch, the choke's pin span
    // AND the widest copper the rails carry (the M3 lugs beat the trace
    // width up to ~16 A) all raise the floor.
    const double widest = std::max(tw, lug.pad);
    const double railHalf =
        std::max({(p.clearLNmm + widest) / 2.0 + 0.25, xPkg.pitch / 2.0,
                  rPkg.pitch / 2.0, cPkg.pitchY / 2.0});
    const double yc = 0.0;                    // rails centered on y = 0
    const double yL = yc - railHalf, yN = yc + railHalf;
    // Spine offset: the PE lugs sit ON the spine and the Y pitch must span
    // rail-to-spine without poking past either.
    const double peGap = std::max(
        {p.clearPEmm + widest + 0.5, yPkg.pitch});
    const double yPEt = yL - peGap, yPEb = yN + peGap;

    // ---- nets ----
    // 1 L_IN, 2 N_IN, 3..(stage nets), L_OUT, N_OUT, PE — numbered in order.
    std::vector<std::string> netNames{""};    // net 0 is the unnamed net
    auto add_net = [&](const std::string& n) {
        netNames.push_back(n);
        return (int)netNames.size() - 1;
    };
    const int nLIn = add_net("L_IN"), nNIn = add_net("N_IN");
    int nLMid = 0, nNMid = 0;
    if (d.stages == 2) { nLMid = add_net("L_M1"); nNMid = add_net("N_M1"); }
    const int nLOut = add_net("L_OUT"), nNOut = add_net("N_OUT");
    const int nPE = add_net("PE");

    std::vector<detail::Part> parts;
    std::vector<detail::Seg> segs;

    // A RAIL is a straight run that collects T-joints and is emitted SPLIT
    // at every joint — the KiCad idiom (the router always breaks tracks at
    // junctions), and the form every connectivity model, DRC and Faraday's
    // dangling-stub rule reason about soundly. The first emitted board used
    // single long segments with stubs T-ing into their interiors; Faraday
    // reviewed its own toolchain's output and flagged every one of them.
    struct Rail {
        double from = 0;
        int net = 0;
        std::vector<double> joints;
    };
    auto close_rail = [&](Rail& r, double to, double y, double w) {
        std::vector<double> xs{r.from};
        for (double j : r.joints)
            if (j > r.from + 1e-9 && j < to - 1e-9) xs.push_back(j);
        xs.push_back(to);
        std::sort(xs.begin(), xs.end());
        for (size_t i = 0; i + 1 < xs.size(); ++i)
            if (xs[i + 1] - xs[i] > 1e-9)
                segs.push_back({xs[i], y, xs[i + 1], y, w, r.net});
        r.joints.clear();
    };

    // vertical stub from a rail to a pad (same net; zero-length skipped).
    // WIDTH MATTERS: only the rails carry the rated current, so a shunt
    // part's leg is stubbed at its pad width — a rail-width stub to a small
    // Y-cap pad narrowed the line-to-PE gap to pitch minus trace width
    // (3.8 mm on the first emitted board; the test's independent re-parse
    // caught it). Series (choke) stubs keep the full rail width.
    auto stub = [&](double x, double yFrom, double yTo, int net, double w) {
        if (std::abs(yFrom - yTo) < 1e-9) return;
        segs.push_back({x, yFrom, x, yTo, w, net});
    };

    double x = p.edgeMarginmm;                // running x cursor
    const double xPeLink = x;                 // PE ring closes here, left of
    x += p.partGapmm + lug.bodyW;             // everything carrying L/N

    // ---- input lugs ----
    auto add_lug = [&](const std::string& ref, double lx, double ly, int net) {
        parts.push_back({ref, "M3 lug", lx, ly, lug.bodyW, lug.bodyH,
                         {{lx, ly, lug.drill, lug.pad, net, "1"}}});
    };
    const double xIn = x;
    add_lug("J1", xIn, yL, nLIn);
    add_lug("J2", xIn, yN, nNIn);
    add_lug("J3", xIn, yPEb, nPE);
    x += lug.bodyW / 2.0 + p.partGapmm;

    std::vector<StubRun> stubRuns;
    double firstChokeInX = 0, lastChokeOutX = 0;
    Rail railL{xIn, nLIn, {}}, railN{xIn, nNIn, {}};
    Rail peTop{xPeLink, nPE, {}}, peBot{xPeLink, nPE, {}};
    peBot.joints.push_back(xIn);              // J3 sits on the bottom spine

    // 2-pad part standing vertically between two rails, pads stubbed and the
    // joint registered on each rail it touches
    auto add_vertical = [&](const std::string& ref, const std::string& value,
                            const Package& pkg, int netTop, int netBot,
                            double yTopRail, double yBotRail, Rail* railTop,
                            Rail* railBot) {
        const double cx = x + pkg.bodyW / 2.0;
        const double yTop = (yTopRail + yBotRail) / 2.0 - pkg.pitch / 2.0;
        const double yBot = (yTopRail + yBotRail) / 2.0 + pkg.pitch / 2.0;
        parts.push_back({ref, value, cx, (yTop + yBot) / 2.0, pkg.bodyW,
                         std::max(pkg.bodyH, pkg.pitch + pkg.pad),
                         {{cx, yTop, pkg.drill, pkg.pad, netTop, "1"},
                          {cx, yBot, pkg.drill, pkg.pad, netBot, "2"}}});
        const double sw = std::clamp(pkg.pad, 1.0, std::max(tw, 1.0));
        stub(cx, yTopRail, yTop, netTop, sw);
        stub(cx, yBotRail, yBot, netBot, sw);
        stubRuns.push_back({ref, std::abs(yTop - yTopRail) +
                                     std::abs(yBot - yBotRail), sw});
        if (railTop) railTop->joints.push_back(cx);
        if (railBot) railBot->joints.push_back(cx);
        x = cx + pkg.bodyW / 2.0 + p.partGapmm;
        return cx;
    };

    // ---- per stage: X (+ discharge R on stage 1) then the choke ----
    // Rails are SERIES paths: each stage's rail run ends at the choke input
    // pins and resumes, on the next net, at its output pins.
    int chokeIdx = 0;
    for (int s = 0; s < d.stages; ++s) {
        const int inL = s == 0 ? nLIn : nLMid;
        const int inN = s == 0 ? nNIn : nNMid;
        const int outL = (d.stages == 2 && s == 0) ? nLMid : nLOut;
        const int outN = (d.stages == 2 && s == 0) ? nNMid : nNOut;

        if (s == 0) {
            add_vertical("R1", "discharge", rPkg, inL, inN, yL, yN, &railL,
                         &railN);
        }
        add_vertical("CX" + std::to_string(s + 1), "X2", xPkg, inL, inN, yL,
                     yN, &railL, &railN);

        // choke: pins 1/2 on the L row, 3/4 on the N row
        ++chokeIdx;
        const std::string ref = "CMC" + std::to_string(chokeIdx);
        const double ccx = x + cPkg.bodyW / 2.0;
        const double xin = ccx - cPkg.pitch / 2.0, xout = ccx + cPkg.pitch / 2.0;
        const double yTopPin = yc - cPkg.pitchY / 2.0;
        const double yBotPin = yc + cPkg.pitchY / 2.0;
        parts.push_back({ref, "CM choke", ccx, yc, cPkg.bodyW, cPkg.bodyH,
                         {{xin, yTopPin, cPkg.drill, cPkg.pad, inL, "1"},
                          {xout, yTopPin, cPkg.drill, cPkg.pad, outL, "2"},
                          {xin, yBotPin, cPkg.drill, cPkg.pad, inN, "3"},
                          {xout, yBotPin, cPkg.drill, cPkg.pad, outN, "4"}}});
        // close the input rails INTO the choke...
        close_rail(railL, xin, yL, tw);
        close_rail(railN, xin, yN, tw);
        stub(xin, yL, yTopPin, inL, tw);    // series path: full rail width
        stub(xin, yN, yBotPin, inN, tw);
        // ...and reopen them from its output pins
        stub(xout, yL, yTopPin, outL, tw);
        stub(xout, yN, yBotPin, outN, tw);
        if (s == 0) firstChokeInX = xin;
        lastChokeOutX = xout;
        railL = {xout, outL, {}};
        railN = {xout, outN, {}};
        x = ccx + cPkg.bodyW / 2.0 + p.partGapmm;
    }

    // ---- Y pair on the load side: L_OUT to the TOP spine, N_OUT to the
    // BOTTOM spine (that is why the earth ring exists — no crossings) ----
    const double cxCy1 =
        add_vertical("CY1", "Y2", yPkg, nPE, nLOut, yPEt, yL, &peTop, &railL);
    x -= p.partGapmm;  // CY1/CY2 share a column footprint-width apart
    add_vertical("CY2", "Y2", yPkg, nNOut, nPE, yN, yPEb, &railN, &peBot);

    // ---- output lugs ----
    const double xOut = x + lug.bodyW / 2.0;
    add_lug("J4", xOut, yL, nLOut);
    add_lug("J5", xOut, yN, nNOut);
    add_lug("J6", xOut, yPEb, nPE);
    peBot.joints.push_back(xOut);
    // close the output rails
    close_rail(railL, xOut, yL, tw);
    close_rail(railN, xOut, yN, tw);

    // ---- the earth ring: bottom spine, left-edge link, and a top spine
    // that ENDS at the Y capacitor it exists to serve — running it to the
    // board edge left 118 mm of open PE stub, a quarter-wave resonator by
    // Faraday's own review of the first emitted board ----
    const double xRight = xOut + lug.bodyW / 2.0 + p.edgeMarginmm;
    close_rail(peTop, cxCy1, yPEt, tw);
    close_rail(peBot, xOut, yPEb, tw);
    stub(xPeLink, yPEt, yPEb, nPE, tw);       // the left-edge link

    // ---- board outline ----
    // the margin is measured from the COPPER, not from run centerlines: the
    // PE lugs sit ON the bottom spine and their pad radius ate the whole
    // edge margin on the first emitted board (KiCad DRC: 0.0 mm edge
    // clearance on J3/J6)
    const double bx1 = 0.0;
    const double by1 = yPEt - std::max(p.edgeMarginmm, tw / 2.0 + 1.0);
    const double bx2 = xRight;
    const double by2 = yPEb + lug.pad / 2.0 + p.edgeMarginmm;

    // ---- self-check: MEASURE the emitted copper against the floors -------
    // Rails vs each other and vs the spines are parallel runs; parts are
    // sequenced with partGap. A generated board must prove its spacing, not
    // assume it.
    const double lnGap = detail::edge_gap(2.0 * railHalf, widest, widest);
    const double peGapMeasured = detail::edge_gap(peGap, widest, widest);
    if (lnGap < p.clearLNmm) {
        throw std::logic_error("generated L-N copper gap " + detail::fmt(lnGap) +
                               " mm violates the floor " + detail::fmt(p.clearLNmm) +
                               " mm — template bug, refusing to emit");
    }
    if (peGapMeasured < p.clearPEmm) {
        throw std::logic_error("generated line-PE copper gap " +
                               detail::fmt(peGapMeasured) + " mm violates the floor " +
                               detail::fmt(p.clearPEmm) + " mm — template bug");
    }
    // every across-rails part: its own pad-to-pad gap is a different-net gap
    auto pitch_check = [&](const char* what, double pitch, double pad,
                           double floor) {
        const double g = pitch - pad;
        if (g < floor) {
            throw std::logic_error(std::string("generated ") + what +
                                   " pad gap " + detail::fmt(g) +
                                   " mm violates the floor " +
                                   detail::fmt(floor) + " mm — template bug");
        }
    };
    // the narrowing copper at a shunt part is the WIDER of pad and stub
    auto eff = [&](const Package& pkg) {
        return std::max(pkg.pad, std::clamp(pkg.pad, 1.0, std::max(tw, 1.0)));
    };
    pitch_check("X-cap", xPkg.pitch, eff(xPkg), p.clearLNmm);
    pitch_check("discharge-R", rPkg.pitch, eff(rPkg), p.clearLNmm);
    // choke stubs carry the rated current: rail width against the floor
    pitch_check("choke L-N", cPkg.pitchY, std::max(cPkg.pad, tw), p.clearLNmm);
    pitch_check("Y-cap line-PE", yPkg.pitch, eff(yPkg), p.clearPEmm);

    // ---- emission (KiCad v6 s-expression; opened by KiCad 6..9) ----------
    std::string o;
    o.reserve(16384);
    o += "(kicad_pcb (version 20211014) (generator hertz_layoutgen)\n";
    o += "  (general (thickness 1.6))\n  (paper \"A4\")\n";
    o += "  (layers (0 \"F.Cu\" signal) (31 \"B.Cu\" signal)\n";
    o += "          (36 \"B.SilkS\" user) (37 \"F.SilkS\" user)\n";
    o += "          (44 \"Edge.Cuts\" user))\n";
    for (size_t i = 0; i < netNames.size(); ++i)
        o += "  (net " + std::to_string(i) + " \"" + netNames[i] + "\")\n";
    for (const auto& pt : parts) {
        o += "  (footprint \"hertz:" + pt.ref + "\" (layer \"F.Cu\") (at " +
             detail::fmt(pt.cx) + " " + detail::fmt(pt.cy) + ")\n";
        o += "    (fp_text reference \"" + pt.ref + "\" (at 0 " +
             detail::fmt(-pt.bodyH / 2.0 - 1.0) + ") (layer \"F.SilkS\"))\n";
        o += "    (fp_text value \"" + pt.value + "\" (at 0 " +
             detail::fmt(pt.bodyH / 2.0 + 1.0) + ") (layer \"F.Fab\"))\n";
        for (const auto& pd : pt.pads) {
            o += "    (pad \"" + pd.pin + "\" thru_hole circle (at " +
                 detail::fmt(pd.x - pt.cx) + " " + detail::fmt(pd.y - pt.cy) +
                 ") (size " + detail::fmt(pd.pad) + " " + detail::fmt(pd.pad) +
                 ") (drill " + detail::fmt(pd.drill) + ") (layers \"*.Cu\" \"*.Mask\")";
            if (pd.net > 0)
                o += " (net " + std::to_string(pd.net) + " \"" +
                     netNames[pd.net] + "\")";
            o += ")\n";
        }
        o += "  )\n";
    }
    for (const auto& s : segs) {
        o += "  (segment (start " + detail::fmt(s.x1) + " " + detail::fmt(s.y1) +
             ") (end " + detail::fmt(s.x2) + " " + detail::fmt(s.y2) +
             ") (width " + detail::fmt(s.w) + ") (layer \"F.Cu\") (net " +
             std::to_string(s.net) + "))\n";
    }
    auto edge = [&](double ax, double ay, double bx, double by) {
        o += "  (gr_line (start " + detail::fmt(ax) + " " + detail::fmt(ay) +
             ") (end " + detail::fmt(bx) + " " + detail::fmt(by) +
             ") (layer \"Edge.Cuts\") (width 0.1))\n";
    };
    edge(bx1, by1, bx2, by1);
    edge(bx2, by1, bx2, by2);
    edge(bx2, by2, bx1, by2);
    edge(bx1, by2, bx1, by1);
    o += "  (gr_text \"HERTZ generated filter — REVIEW BEFORE FABRICATION: "
         "verify packages, creepage and safety approvals\" (at " +
         detail::fmt((bx1 + bx2) / 2.0) + " " + detail::fmt(by2 - 1.2) +
         ") (layer \"F.SilkS\") (effects (font (size 1 1) (thickness 0.15))))\n";
    o += ")\n";

    GeneratedBoard g;
    g.kicadPcb = std::move(o);
    g.boardWmm = bx2 - bx1;
    g.boardHmm = by2 - by1;
    g.minLNGapmm = lnGap;
    g.minPEGapmm = peGapMeasured;
    g.parts = (int)parts.size();
    g.railSepMm = 2.0 * railHalf;
    g.traceWMm = tw;
    g.inX1 = xIn;
    g.inX2 = firstChokeInX;
    g.outX1 = lastChokeOutX;
    g.outX2 = xOut;
    g.peSpineLenMm = xOut - xIn;
    g.stubs = std::move(stubRuns);
    return g;
}

}  // namespace Hertz::layout
