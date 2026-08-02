#pragma once
// Layout parasitics of a generated filter board (filter-block plan S2,
// ABT #443) — the L and C the LAYOUT adds to the BOM, computed analytically
// from the same geometry the copper was emitted with.
//
// Three mechanisms decide how far a real filter falls short of its
// datasheet insertion loss:
//   1. BYPASS: the dirty-side rail pair couples magnetically to the
//      clean-side pair PAST the filter. That mutual M sets a hard IL
//      floor ~ -20·log10(omega·M / (Zs+Zl)) no component change can beat.
//   2. CONNECTION ESL: every millimetre of stub between a shunt capacitor
//      and the rails is series inductance the capacitor's own ESL does not
//      include — it shifts the branch's self-resonance down.
//   3. SHARED EARTH: the Y currents return through the same PE spine the
//      measurement references; its partial inductance sits in the CM shunt
//      branch.
//
// Formulas: Grover's flat-strip partial self-inductance and the exact
// finite-filament mutual (Neumann solution). GMD 0.2235·(w+t) stands in for
// the filament distance when runs are collinear. STATED BAND: partial-
// inductance closed forms on real copper are good to roughly +/-30 %; the
// FastHenry spot-check for this template is recorded in ABT #443. Rankings
// and floors are reliable; treat absolute dB near the floor as +/-3 dB.

#include <cmath>
#include <stdexcept>

#include "hertz/LayoutGen.hpp"
#include "hertz/Network.hpp"

namespace Hertz::layout {

inline constexpr double COPPER_T_MM = 0.035;  // 1 oz

// Grover flat strip, nH (l, w in mm): L = 0.2·l·[ln(2l/(w+t)) + 0.5 +
// 0.2235(w+t)/l]
inline double strip_partial_nh(double lMm, double wMm) {
    if (lMm <= 0.0) return 0.0;
    if (wMm <= 0.0) throw std::invalid_argument("strip width must be positive");
    const double wt = wMm + COPPER_T_MM;
    return 0.2 * lMm *
           (std::log(2.0 * lMm / wt) + 0.5 + 0.2235 * wt / lMm);
}

// Exact mutual of two parallel filaments (nH). A spans [a1,a2], B spans
// [b1,b2] along the shared axis, separated by d perpendicular. Neumann:
// M = 0.1·[F(a2-b1) + F(a1-b2) - F(a2-b2) - F(a1-b1)],
// F(x) = x·asinh(x/d) - sqrt(x²+d²)   (F is even; 0.1 nH/mm = mu0/4pi)
inline double filament_mutual_nh(double a1, double a2, double b1, double b2,
                                 double dMm) {
    if (dMm <= 0.0) throw std::invalid_argument("filament distance must be positive");
    auto F = [&](double x) {
        return x * std::asinh(x / dMm) - std::sqrt(x * x + dMm * dMm);
    };
    return 0.1 * (F(a2 - b1) + F(a1 - b2) - F(a2 - b2) - F(a1 - b1));
}

// DM bypass mutual of two rail PAIRS (nH). Input pair spans [i1,i2], output
// pair [o1,o2], rails separated by railSep; both loops lie in the plane, so
// the pair-to-pair mutual is the four-filament combination
//   M = M(L,L') + M(N,N') - M(L,N') - M(N,L').
// Collinear runs (L with L') use the GMD of the two strips as d.
inline double dm_bypass_mutual_nh(double i1, double i2, double o1, double o2,
                                  double railSepMm, double traceWMm) {
    if (railSepMm <= 0.0 || traceWMm <= 0.0) {
        throw std::invalid_argument("rail separation and trace width must be positive");
    }
    const double gmd = 0.2235 * (2.0 * traceWMm + 2.0 * COPPER_T_MM);
    const double same = filament_mutual_nh(i1, i2, o1, o2, gmd);
    const double cross = filament_mutual_nh(i1, i2, o1, o2, railSepMm);
    // horizontal rails: L-L' and N-N' equal by symmetry, as are the crosses
    const double horiz = 2.0 * (same - cross);
    // ...and the loops' CLOSING VERTICALS: the input pair closes on-board at
    // the first series element (i2), the output pair at the output lugs
    // (o2). Ignoring them measured 2.2x LOW against FastHenry on the
    // template (1.26 nH horizontal-only vs 2.74 nH solved; the vertical
    // term is 1.46 nH). With it the closed form lands within 1 % of the
    // solver for this arrangement — but the split is CONFIGURATION-
    // dependent, so the stated absolute band stays +/-50 % (floors +/-4 dB);
    // rankings across layouts of one template family are far tighter.
    const double vert =
        filament_mutual_nh(0.0, railSepMm, 0.0, railSepMm,
                           std::max(std::abs(o2 - i2), gmd));
    return horiz + vert;
}

struct LayoutParasitics {
    double mDmNh = 0;        // dirty<->clean pair bypass mutual
    double xConnNh = 0;      // X-cap connection ESL (stubs), per capacitor
    double yConnNh = 0;      // Y-cap connection ESL, per capacitor
    double peSpineNh = 0;    // shared PE run, series in the CM shunt branch
};

// From the generator's geometry summary — the SAME numbers the copper was
// emitted with, so template and model can never drift apart. (Faraday's
// independent import of the emitted text is the cross-check that the copper
// really says what the summary claims.)
inline LayoutParasitics layout_parasitics(const GeneratedBoard& g) {
    if (g.railSepMm <= 0.0 || g.traceWMm <= 0.0) {
        throw std::invalid_argument(
            "board carries no geometry summary — generate it with this "
            "library version, not from a bare file");
    }
    LayoutParasitics lp;
    lp.mDmNh = dm_bypass_mutual_nh(g.inX1, g.inX2, g.outX1, g.outX2,
                                   g.railSepMm, g.traceWMm);
    int nx = 0, ny = 0;
    for (const auto& st : g.stubs) {
        const double nh = strip_partial_nh(st.lenMm, st.wMm);
        if (st.ref.rfind("CX", 0) == 0) { lp.xConnNh += nh; ++nx; }
        if (st.ref.rfind("CY", 0) == 0) { lp.yConnNh += nh; ++ny; }
    }
    if (nx) lp.xConnNh /= nx;   // per capacitor (stages use identical parts)
    if (ny) lp.yConnNh /= ny;
    // CM return inductance: with the solid B.Cu earth pour, both Y-caps drop
    // straight into the plane, so the shared return is the SHORT local drop into a
    // WIDE plane (earthReturnLenMm at ~4x trace width) — symmetric, low-inductance —
    // not the long asymmetric top-spine run the three-sided trace ring imposed.
    lp.peSpineNh = g.hasEarthPlane
                       ? strip_partial_nh(g.earthReturnLenMm, std::max(g.traceWMm * 4.0, 4.0))
                       : strip_partial_nh(g.peSpineLenMm, g.traceWMm);
    return lp;
}

// The IL floor the bypass mutual imposes in a Zs/Zl system:
// -20·log10(omega·M/(Zs+Zl)). No component change moves it — only distance,
// orientation or a shield between the rail pairs.
inline double bypass_floor_il_db(double fHz, double mNh, double zSysOhm = 100.0) {
    if (fHz <= 0.0 || zSysOhm <= 0.0) {
        throw std::invalid_argument("frequency and system impedance must be positive");
    }
    if (mNh <= 0.0) return 1e9;   // no coupling, no floor
    const double t = 2.0 * std::numbers::pi * fHz * mNh * 1e-9 / zSysOhm;
    return -20.0 * std::log10(t);
}

// Insertion loss of the filter WITH its layout: the filtered path and the
// bypass path add as voltages at the load; the worst case (in-phase) is the
// honest bound, so |T| = |T_filter| + |T_bypass|.
inline double layout_aware_il_db(double fHz, const Abcd& network, double mNh,
                                 Complex zSource, Complex zLoad) {
    const double ilDb = insertion_loss_db(network, zSource, zLoad);
    const double tFilter = std::pow(10.0, -ilDb / 20.0);
    const double zSys = std::abs(zSource + zLoad);
    const double tBypass =
        mNh > 0.0 ? 2.0 * std::numbers::pi * fHz * mNh * 1e-9 / zSys : 0.0;
    return -20.0 * std::log10(tFilter + tBypass);
}

// Nominal CM-choke inter-winding (parasitic) capacitance. A real common-mode
// choke's windings couple through a few pF, and THAT — not the board layout — is
// what caps CM attenuation: above the choke's self-resonance the winding EPC
// shunts the CM inductance, so CM IL follows a DECLINING ceiling instead of the
// ideal notch (with 3 pF, ~71 dB at 150 kHz falling to ~25 dB at 30 MHz — a real
// 1-2 stage CM path tops out in this band, not the hundreds of dB the ideal LC
// shows). 3 pF is a typical single-section value; it is an ASSUMPTION, not a
// per-part number, and is stated as such in the UI.
inline constexpr double CM_CHOKE_EPC_F = 3e-12;

// CM insertion loss floored by BOTH the rail-run layout mutual (mNh, inductive,
// rises with f) AND the choke winding EPC (capacitive, its bypass also rises with
// f) — so the ideal LC notch can no longer read an unphysical ~140-240 dB; it
// plateaus where a real CM choke does.
inline double cm_layout_il_db(double fHz, const Abcd& network, double mNh,
                              double cwF, Complex zSource, Complex zLoad) {
    const double ilDb = insertion_loss_db(network, zSource, zLoad);
    const double tFilter = std::pow(10.0, -ilDb / 20.0);
    const double zSys = std::abs(zSource + zLoad);
    const double w = 2.0 * std::numbers::pi * fHz;
    const double tBypassL = mNh > 0.0 ? w * mNh * 1e-9 / zSys : 0.0;  // layout
    const double tBypassC = cwF > 0.0 ? w * cwF * zSys : 0.0;         // choke EPC
    return -20.0 * std::log10(tFilter + tBypassL + tBypassC);
}

// The pure CM bypass CEILING (perfect filter, tFilter -> 0): the IL the CM path
// cannot exceed given the layout mutual + choke EPC. This is the plateau the CM
// curve approaches.
inline double cm_bypass_floor_il_db(double fHz, double mNh, double cwF, double zSysOhm) {
    const double w = 2.0 * std::numbers::pi * fHz;
    const double tBypassL = mNh > 0.0 ? w * mNh * 1e-9 / zSysOhm : 0.0;
    const double tBypassC = cwF > 0.0 ? w * cwF * zSysOhm : 0.0;
    const double t = tBypassL + tBypassC;
    return t > 0.0 ? -20.0 * std::log10(t) : 300.0;
}

}  // namespace Hertz::layout
