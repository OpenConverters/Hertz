#pragma once
// The filter BLOCK (filter-block plan S3+S4, ABT #444/#445): the joint
// layout + BOM iteration, the layout-aware curves, and the one-block
// exports. The optimizer is DETERMINISTIC — a fixed spacing ladder, fixed
// escalation order (spacing first, then a second stage), no wall clock, no
// randomness — so the same spec always yields the same board.

#include <string>
#include <vector>

#include "hertz/FilterDesign.hpp"
#include "hertz/LayoutGen.hpp"
#include "hertz/LayoutParasitics.hpp"
#include "hertz/Network.hpp"

namespace Hertz::layout {

// ---------------------------------------------------------------------------
// Layout-aware IL curves (50/50 ohm; the bench's CISPR 17 worst-case panes
// stay available for the termination story)
// ---------------------------------------------------------------------------

struct BlockCurves {
    std::vector<double> fHz;
    std::vector<double> dmDb, dmIdealDb;   // with layout / components-only
    std::vector<double> cmDb, cmIdealDb;
    std::vector<double> dmFloorDb, cmFloorDb;   // the bypass floors (DM, CM)
};

inline BlockCurves block_curves(const LineFilterDesign& d,
                                const LayoutParasitics& lp,
                                double f1Hz = 150e3, double f2Hz = 30e6,
                                int pointsPerDecade = 30,
                                double capEslH = 5e-9,
                                double capEsrOhm = 0.02) {
    if (f1Hz <= 0.0 || f2Hz <= f1Hz || pointsPerDecade < 2) {
        throw std::invalid_argument("bad frequency grid");
    }
    BlockCurves c;
    const Complex z50{50.0, 0.0};
    const double step = std::pow(10.0, 1.0 / pointsPerDecade);
    for (double f = f1Hz; f <= f2Hz * (1.0 + 1e-9); f *= step) {
        c.fHz.push_back(f);
        const double cxEff = d.cXDmFactor * d.cXSelectedF;
        Abcd dmIdeal = lc_filter_abcd(f, d.lDmH, cxEff, d.stages, capEslH,
                                      capEsrOhm);
        Abcd dm = lc_filter_abcd(f, d.lDmH, cxEff, d.stages,
                                 capEslH + lp.xConnNh * 1e-9, capEsrOhm);
        Abcd cmIdeal = lc_filter_abcd(f, d.lCmSelectedH, d.cYgF, d.stages,
                                      capEslH, capEsrOhm);
        // the Y branch carries its stubs AND the shared PE spine
        Abcd cm = lc_filter_abcd(
            f, d.lCmSelectedH, d.cYgF, d.stages,
            capEslH + (lp.yConnNh + lp.peSpineNh) * 1e-9, capEsrOhm);
        c.dmIdealDb.push_back(insertion_loss_db(dmIdeal, z50, z50));
        c.dmDb.push_back(layout_aware_il_db(f, dm, lp.mDmNh, z50, z50));
        c.cmIdealDb.push_back(insertion_loss_db(cmIdeal, z50, z50));
        // CM gets the layout bypass floor too (was plain insertion_loss_db, which
        // read an unphysical ~140 dB at the LC notch); the rail-run mutual bounds it.
        c.cmDb.push_back(layout_aware_il_db(f, cm, lp.mDmNh, z50, z50));
        c.dmFloorDb.push_back(bypass_floor_il_db(f, lp.mDmNh));
        c.cmFloorDb.push_back(bypass_floor_il_db(f, lp.mDmNh));
    }
    return c;
}

// ---------------------------------------------------------------------------
// S3: the joint iteration — spacing ladder first, then a second stage
// ---------------------------------------------------------------------------

struct FilterBlock {
    LineFilterDesign design;
    LayoutParams params;
    GeneratedBoard board;
    LayoutParasitics par;
    double layoutAttenCmDb = 0, layoutAttenDmDb = 0;  // at f_design, 50/50
    double idealAttenCmDb = 0, idealAttenDmDb = 0;    // components only
    bool meets = false;
    bool escalatedStages = false;
    int iterations = 0;
    // when the target is NOT met, whose fault is it? "catalog" = even the
    // components alone (no layout) miss it — bind larger parts; "layout" =
    // the parts would make it but the board's parasitics eat the rest —
    // spacing/shielding, not shopping. The live demo chain surfaced the
    // difference: a 0.1 dB miss on the manual value ladder was labeled
    // LAYOUT-LIMITED, which pointed the user at the wrong fix.
    std::string limitedBy;    // "" when meets
};

inline FilterBlock optimize_filter_block(
    double fSwHz, double aReqCmDb, double aReqDmDb, double cYPerLineF,
    double lDmH, double ratedCurrentA,
    const std::vector<double>& lCmCandidates,
    const std::vector<double>& cXCandidates, int maxStages = 2,
    double capEslH = 5e-9, double capEsrOhm = 0.02) {
    if (maxStages < 1 || maxStages > 2) {
        throw std::invalid_argument("maxStages must be 1 or 2 (ANP015)");
    }
    // spacing ladder: monotone, bounded — more gap lowers the bypass mutual
    // and grows the board; the smallest PASSING board wins
    const std::vector<double> gaps{3.2, 4.0, 5.0, 6.5, 8.0, 10.0, 13.0,
                                   16.0, 20.0};
    const Complex z50{50.0, 0.0};
    FilterBlock best;
    double bestShortfall = 1e30;
    std::string lastDesignRefusal;
    for (int stages = 1; stages <= maxStages; ++stages) {
        LineFilterDesign d;
        try {
            d = design_line_filter(fSwHz, aReqCmDb, aReqDmDb, cYPerLineF,
                                   lDmH, stages, lCmCandidates, cXCandidates);
        } catch (const std::invalid_argument& e) {
            // this stage count cannot even be DESIGNED from the catalog —
            // remember why and try the next escalation
            lastDesignRefusal = e.what();
            continue;
        }
        bool catalogLimitedThisStage = false;
        // The ideal (parasitic-free) attenuation is gap-INDEPENDENT. If the
        // CATALOG itself cannot meet the target, a wider board does not help — so
        // evaluate only the COMPACT board and let a higher stage count try, instead
        // of returning a needlessly huge 20 mm fail-artifact board. Only sweep the
        // spacing ladder when widening can actually cross the target (layout-limited).
        {
            const double cxE = d.cXDmFactor * d.cXSelectedF;
            Abcd cmI = lc_filter_abcd(d.fDesignCmHz, d.lCmSelectedH, d.cYgF, stages, capEslH, capEsrOhm);
            Abcd dmI = lc_filter_abcd(d.fDesignDmHz, d.lDmH, cxE, stages, capEslH, capEsrOhm);
            catalogLimitedThisStage = insertion_loss_db(cmI, z50, z50) < aReqCmDb ||
                                      insertion_loss_db(dmI, z50, z50) < aReqDmDb;
        }
        const std::vector<double> tryGaps =
            catalogLimitedThisStage ? std::vector<double>{gaps.front()} : gaps;
        for (double gap : tryGaps) {
            LayoutParams p = layout_params_for(ratedCurrentA);
            p.partGapmm = std::max(p.partGapmm, gap);
            GeneratedBoard g = generate_filter_board(d, ratedCurrentA, p);
            LayoutParasitics lp = layout_parasitics(g);
            const double cxEff = d.cXDmFactor * d.cXSelectedF;
            Abcd dm = lc_filter_abcd(d.fDesignDmHz, d.lDmH, cxEff, stages,
                                     capEslH + lp.xConnNh * 1e-9, capEsrOhm);
            Abcd cm = lc_filter_abcd(
                d.fDesignCmHz, d.lCmSelectedH, d.cYgF, stages,
                capEslH + (lp.yConnNh + lp.peSpineNh) * 1e-9, capEsrOhm);
            const double attDm = layout_aware_il_db(d.fDesignDmHz, dm,
                                                    lp.mDmNh, z50, z50);
            // CM gets a layout bypass floor too — without one the ideal LC notch
            // reads an unphysical ~140 dB. The same rail-run mutual couples input to
            // output around the choke for CM (the traces are the same copper); it
            // caps the achievable CM IL, so the verdict is bounded like DM. (The
            // choke's own inter-winding EPC is a further, separate bound.)
            const double attCm = layout_aware_il_db(d.fDesignCmHz, cm,
                                                    lp.mDmNh, z50, z50);
            Abcd dmIdeal = lc_filter_abcd(d.fDesignDmHz, d.lDmH, cxEff,
                                          stages, capEslH, capEsrOhm);
            Abcd cmIdeal = lc_filter_abcd(d.fDesignCmHz, d.lCmSelectedH,
                                          d.cYgF, stages, capEslH, capEsrOhm);
            FilterBlock fb;
            fb.design = d;
            fb.params = p;
            fb.board = std::move(g);
            fb.par = lp;
            fb.layoutAttenCmDb = attCm;
            fb.layoutAttenDmDb = attDm;
            fb.idealAttenCmDb = insertion_loss_db(cmIdeal, z50, z50);
            fb.idealAttenDmDb = insertion_loss_db(dmIdeal, z50, z50);
            fb.escalatedStages = stages > 1;
            fb.iterations = best.iterations + 1;
            const double shortfall = std::max(
                {aReqCmDb - attCm, aReqDmDb - attDm, 0.0});
            fb.meets = shortfall <= 0.0;
            ++best.iterations;
            if (fb.meets) {
                fb.iterations = best.iterations;
                return fb;   // smallest passing board of the fewest stages
            }
            if (shortfall < bestShortfall) {
                bestShortfall = shortfall;
                const int it = best.iterations;
                best = std::move(fb);
                best.iterations = it;
            }
        }
    }
    // no stage count could even be designed: refuse with the catalog's own
    // words — a board was never possible, which is different from a board
    // that exists but misses its target
    if (best.board.kicadPcb.empty()) {
        throw std::invalid_argument(
            "no realizable design at any stage count: " + lastDesignRefusal);
    }
    // designs existed but none met the target THROUGH its layout: return
    // the closest board, loudly not-meeting — and DIAGNOSE the limiter so
    // the caller points at the right fix
    best.limitedBy = (best.idealAttenCmDb < aReqCmDb ||
                      best.idealAttenDmDb < aReqDmDb)
                         ? "catalog"
                         : "layout";
    return best;
}

// ---------------------------------------------------------------------------
// S4: one block, several formats
// ---------------------------------------------------------------------------

// Touchstone .s2p (50 ohm, RI). |S21| carries the bypass path (worst-case
// magnitude sum, filter-path phase) so the exported file matches the curve
// the pane shows.
inline std::string touchstone_s2p(const LineFilterDesign& d,
                                  const LayoutParasitics& lp,
                                  const std::string& mode,
                                  double f1Hz = 150e3, double f2Hz = 30e6,
                                  int pointsPerDecade = 20,
                                  double capEslH = 5e-9,
                                  double capEsrOhm = 0.02) {
    const bool dmMode = mode == "dm";
    if (!dmMode && mode != "cm") {
        throw std::invalid_argument("mode must be \"dm\" or \"cm\"");
    }
    const double z0 = 50.0;
    std::string o = "! Hertz filter block, " + mode +
                    " mode, layout-aware (bypass M " +
                    std::to_string(lp.mDmNh) + " nH)\n# HZ S RI R 50\n";
    const double step = std::pow(10.0, 1.0 / pointsPerDecade);
    char line[220];
    for (double f = f1Hz; f <= f2Hz * (1.0 + 1e-9); f *= step) {
        Abcd n = dmMode
                     ? lc_filter_abcd(f, d.lDmH, d.cXDmFactor * d.cXSelectedF,
                                      d.stages, capEslH + lp.xConnNh * 1e-9,
                                      capEsrOhm)
                     : lc_filter_abcd(f, d.lCmSelectedH, d.cYgF, d.stages,
                                      capEslH +
                                          (lp.yConnNh + lp.peSpineNh) * 1e-9,
                                      capEsrOhm);
        const Complex den = n.a + n.b / z0 + n.c * z0 + n.d;
        Complex s21 = 2.0 / den;
        Complex s11 = (n.a + n.b / z0 - n.c * z0 - n.d) / den;
        if (dmMode && lp.mDmNh > 0.0) {
            const double bypass =
                2.0 * std::numbers::pi * f * lp.mDmNh * 1e-9 / (2.0 * z0);
            const double mag = std::abs(s21) + bypass;
            s21 *= mag / std::abs(s21);
        }
        std::snprintf(line, sizeof line,
                      "%.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g\n", f,
                      s11.real(), s11.imag(), s21.real(), s21.imag(),
                      s21.real(), s21.imag(), s11.real(), s11.imag());
        o += line;
    }
    return o;
}

// SPICE subcircuit of the WHOLE block: components AND layout. The bypass
// mutual is two 1:1-coupled series inductors of value M (adds M in series
// per side — nanohenries against the filter's millihenries, stated here
// rather than hidden). K for the CM choke from the leakage:
// L_dm = 2·L(1-k)  ->  k = 1 - L_dm/(2·L_cm).
inline std::string block_spice_subckt(const LineFilterDesign& d,
                                      const LayoutParasitics& lp,
                                      double capEslH = 5e-9,
                                      double capEsrOhm = 0.02) {
    const double k = 1.0 - d.lDmH / (2.0 * d.lCmSelectedH);
    if (!(k > 0.0 && k < 1.0)) {
        throw std::invalid_argument(
            "CM choke cannot carry the assumed leakage (k out of (0,1)) — "
            "the design should have refused this earlier");
    }
    char b[512];
    std::string o =
        "* Hertz filter block — components AND layout parasitics\n"
        "* bands: connection/spine partials ~+/-30%, bypass M ~+/-50%\n"
        ".subckt HERTZ_FILTER_BLOCK L_IN N_IN L_OUT N_OUT PE\n";
    std::snprintf(b, sizeof b, "Lbyp1 L_IN l1 %.4gn\nLbyp2 l_pre L_OUT %.4gn\n"
                  "Kbyp Lbyp1 Lbyp2 0.9999\n",
                  std::max(lp.mDmNh, 1e-3), std::max(lp.mDmNh, 1e-3));
    o += b;
    // in -> CMC -> [X + Y] PER STAGE — the exact topology of the schematic, the
    // CIAS brick, the board and the N-section CM/DM verdict, so all five artifacts
    // are one filter (X on the choke output, one Y pair per stage into the shared
    // PE node — not the old X-on-input / single-Y-pair deck).
    std::string inL = "l1", inN = "N_IN";
    for (int s = 1; s <= d.stages; ++s) {
        const std::string suf = std::to_string(s);
        const bool last = s == d.stages;
        const std::string outL = last ? "l_pre" : "l" + suf + "b";
        const std::string outN = last ? "N_OUT" : "n" + suf + "b";
        // the choke: coupled pair, input -> output
        std::snprintf(b, sizeof b,
                      "Lcm%sa %s %s %.6gm\nLcm%sb %s %s %.6gm\n"
                      "Kcm%s Lcm%sa Lcm%sb %.6f\n",
                      suf.c_str(), inL.c_str(), outL.c_str(),
                      d.lCmSelectedH * 1e3, suf.c_str(), inN.c_str(),
                      outN.c_str(), d.lCmSelectedH * 1e3, suf.c_str(),
                      suf.c_str(), suf.c_str(), k);
        o += b;
        // X cap on the choke OUTPUT (ESR + full series inductance: own ESL + stubs)
        std::snprintf(b, sizeof b,
                      "Cx%s %s x%sa %.6gu\nLx%s x%sa x%sb %.4gn\n"
                      "Rx%s x%sb %s %.4g\n",
                      suf.c_str(), outL.c_str(), suf.c_str(),
                      d.cXSelectedF * 1e6, suf.c_str(), suf.c_str(),
                      suf.c_str(), capEslH * 1e9 + lp.xConnNh, suf.c_str(),
                      suf.c_str(), outN.c_str(), capEsrOhm);
        o += b;
        // Y pair on the choke OUTPUT, both into the shared PE node (one pair/stage)
        std::snprintf(b, sizeof b,
                      "Cy%sL %s y%sLa %.6gn\nLy%sL y%sLa pe_i %.4gn\n"
                      "Cy%sN %s y%sNa %.6gn\nLy%sN y%sNa pe_i %.4gn\n",
                      suf.c_str(), outL.c_str(), suf.c_str(),
                      d.cYPerLineF * 1e9, suf.c_str(), suf.c_str(),
                      capEslH * 1e9 + lp.yConnNh,
                      suf.c_str(), outN.c_str(), suf.c_str(),
                      d.cYPerLineF * 1e9, suf.c_str(), suf.c_str(),
                      capEslH * 1e9 + lp.yConnNh);
        o += b;
        inL = outL;
        inN = outN;
    }
    // the shared PE return (the earth plane): ONE inductor every Y branch
    // traverses — the common-impedance point
    std::snprintf(b, sizeof b, "Lpe pe_i PE %.4gn\n", lp.peSpineNh);
    o += b;
    o += ".ends HERTZ_FILTER_BLOCK\n";
    return o;
}

// BOM CSV — values from the design, packages from the same defaults the
// board was drawn with (overridden packages should be passed through the
// same way the board generation was called)
inline std::string block_bom_csv(const LineFilterDesign& d) {
    char b[200];
    std::string o = "ref,qty,value,package,role\n";
    const Package xp = default_x2_package(d.cXSelectedF);
    const Package yp = default_y2_package(d.cYPerLineF);
    const Package cp = default_choke_package(d.lCmSelectedH);
    std::snprintf(b, sizeof b, "CX1..CX%d,%d,%.3g uF X2,pitch %.1f mm,DM shunt\n",
                  d.stages, d.stages, d.cXSelectedF * 1e6, xp.pitch);
    o += b;
    std::snprintf(b, sizeof b,
                  "CMC1..CMC%d,%d,%.3g mH CM choke,%.0fx%.0f mm,CM series\n",
                  d.stages, d.stages, d.lCmSelectedH * 1e3, cp.bodyW, cp.bodyH);
    o += b;
    // one Y PAIR per stage (matches the board and the N-section CM verdict)
    std::snprintf(b, sizeof b, "CY1..CY%d,%d,%.3g nF Y2,pitch %.1f mm,CM shunt\n",
                  2 * d.stages, 2 * d.stages, d.cYPerLineF * 1e9, yp.pitch);
    o += b;
    o += "R1,1,X-cap discharge (size per max_discharge_resistance; a series PAIR "
         "is preferred for open-fault safety),axial 10mm,safety\n"
         "J1..J6,6,M3 ring lug,8x8 mm,terminals\n";
    return o;
}

}  // namespace Hertz::layout
