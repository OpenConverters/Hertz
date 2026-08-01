// LayoutGen (filter-block plan S1, ABT #442). The pins here are the stage-1
// acceptance: structural integrity of the emitted board, MEASURED copper
// spacing against the declared floors (re-parsed from the text, never trusted
// from the generator's own bookkeeping), and byte determinism.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "hertz/FilterDesign.hpp"
#include "hertz/LayoutGen.hpp"

namespace {

// a 10 A, 150 kHz single-phase design that needs a realistic filter
Hertz::LineFilterDesign demo_design(int stages = 1) {
    return Hertz::design_line_filter(
        150e3, 40.0, 30.0, 2.2e-9, 10e-6, stages,
        {0.5e-3, 1e-3, 3.3e-3, 10e-3, 39e-3},          // CM choke candidates
        {100e-9, 470e-9, 1e-6, 2.2e-6, 4.7e-6});       // X candidates
}

// minimal re-parse of the emitted board: pads (absolute) and segments,
// with net ids — enough to MEASURE spacing between different nets
struct Copper {
    struct P { double x, y, dia; int net; };
    struct S { double x1, y1, x2, y2, w; int net; };
    std::vector<P> pads;
    std::vector<S> segs;
};

Copper parse_copper(const std::string& b) {
    Copper c;
    std::regex fpRe(R"re(\(footprint [^\n]*\(at ([-\d.]+) ([-\d.]+)\))re");
    std::regex padRe(
        R"re(\(pad "[^"]+" thru_hole circle \(at ([-\d.]+) ([-\d.]+)\) \(size ([\d.]+) [\d.]+\) \(drill [\d.]+\) \(layers[^)]*\)(?: \(net (\d+) "[^"]*"\))?\))re");
    std::regex segRe(
        R"re(\(segment \(start ([-\d.]+) ([-\d.]+)\) \(end ([-\d.]+) ([-\d.]+)\) \(width ([\d.]+)\) \(layer "F.Cu"\) \(net (\d+)\)\))re");
    // pads are relative to their footprint origin: walk footprints in order
    double ox = 0, oy = 0;
    std::istringstream is(b);
    std::string line;
    while (std::getline(is, line)) {
        std::smatch m;
        if (std::regex_search(line, m, fpRe)) {
            ox = std::stod(m[1]);
            oy = std::stod(m[2]);
        } else if (std::regex_search(line, m, padRe)) {
            c.pads.push_back({ox + std::stod(m[1]), oy + std::stod(m[2]),
                              std::stod(m[3]), m[4].matched ? std::stoi(m[4]) : 0});
        } else if (std::regex_search(line, m, segRe)) {
            c.segs.push_back({std::stod(m[1]), std::stod(m[2]), std::stod(m[3]),
                              std::stod(m[4]), std::stod(m[5]), std::stoi(m[6])});
        }
    }
    return c;
}

double seg_point_dist(const Copper::S& s, double px, double py) {
    const double dx = s.x2 - s.x1, dy = s.y2 - s.y1;
    const double L2 = dx * dx + dy * dy;
    double t = L2 > 0 ? ((px - s.x1) * dx + (py - s.y1) * dy) / L2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    return std::hypot(px - (s.x1 + t * dx), py - (s.y1 + t * dy));
}

// smallest copper-edge gap between two different net GROUPS (pads + segments)
double min_gap(const Copper& c, const std::set<int>& a, const std::set<int>& b) {
    double best = 1e30;
    auto upd = [&](double centerDist, double halfA, double halfB) {
        best = std::min(best, centerDist - halfA - halfB);
    };
    for (const auto& p : c.pads) {
        if (!a.count(p.net)) continue;
        for (const auto& q : c.pads)
            if (b.count(q.net))
                upd(std::hypot(p.x - q.x, p.y - q.y), p.dia / 2, q.dia / 2);
        for (const auto& s : c.segs)
            if (b.count(s.net)) upd(seg_point_dist(s, p.x, p.y), p.dia / 2, s.w / 2);
    }
    for (const auto& s : c.segs) {
        if (!a.count(s.net)) continue;
        for (const auto& q : c.pads)
            if (b.count(q.net)) upd(seg_point_dist(s, q.x, q.y), s.w / 2, q.dia / 2);
        for (const auto& t : c.segs)
            if (b.count(t.net)) {
                // endpoint-vs-segment in both directions bounds parallel runs
                upd(seg_point_dist(t, s.x1, s.y1), s.w / 2, t.w / 2);
                upd(seg_point_dist(t, s.x2, s.y2), s.w / 2, t.w / 2);
                upd(seg_point_dist(s, t.x1, t.y1), s.w / 2, t.w / 2);
                upd(seg_point_dist(s, t.x2, t.y2), s.w / 2, t.w / 2);
            }
    }
    return best;
}

std::set<int> nets_matching(const std::string& b, const std::regex& nameRe) {
    std::set<int> out;
    std::regex netRe(R"re(\(net (\d+) "([^"]*)"\))re");
    auto begin = std::sregex_iterator(b.begin(), b.end(), netRe);
    for (auto it = begin; it != std::sregex_iterator(); ++it)
        if (std::regex_search((*it)[2].str(), nameRe)) out.insert(std::stoi((*it)[1]));
    return out;
}

}  // namespace

TEST_CASE("layoutgen: a 1-stage board emits complete and balanced", "[layout]") {
    auto g = Hertz::layout::generate_filter_board(demo_design(1), 10.0);
    const std::string& b = g.kicadPcb;

    long depth = 0;
    for (char ch : b) {
        if (ch == '(') ++depth;
        if (ch == ')') --depth;
        REQUIRE(depth >= 0);
    }
    CHECK(depth == 0);

    for (const char* tok :
         {"kicad_pcb", "\"L_IN\"", "\"N_IN\"", "\"L_OUT\"", "\"N_OUT\"", "\"PE\"",
          "\"CMC1\"", "\"CX1\"", "\"CY1\"", "\"CY2\"", "\"R1\"", "\"J1\"", "\"J6\"",
          "Edge.Cuts", "REVIEW BEFORE FABRICATION"})
        CHECK_THAT(b, Catch::Matchers::ContainsSubstring(tok));
    CHECK(g.parts == 11);   // 6 lugs, R1, CX1, CMC1, CY1, CY2
    CHECK(g.boardWmm > 50.0);
    CHECK(g.boardHmm > 20.0);
}

TEST_CASE("layoutgen: 2-stage adds the second X and choke on the mid nets",
          "[layout]") {
    auto g = Hertz::layout::generate_filter_board(demo_design(2), 10.0);
    for (const char* tok : {"\"CMC2\"", "\"CX2\"", "\"L_M1\"", "\"N_M1\""})
        CHECK_THAT(g.kicadPcb, Catch::Matchers::ContainsSubstring(tok));
    CHECK(g.parts == 13);
}

TEST_CASE("layoutgen: copper spacing MEASURED off the emitted text meets the "
          "declared floors",
          "[layout]") {
    for (int stages : {1, 2}) {
        auto g = Hertz::layout::generate_filter_board(demo_design(stages), 10.0);
        Copper c = parse_copper(g.kicadPcb);
        REQUIRE(c.pads.size() >= 10);
        REQUIRE(c.segs.size() >= 8);

        const auto lNets = nets_matching(g.kicadPcb, std::regex("^L_"));
        const auto nNets = nets_matching(g.kicadPcb, std::regex("^N_"));
        const auto peNets = nets_matching(g.kicadPcb, std::regex("^PE$"));
        REQUIRE(!lNets.empty());
        REQUIRE(!nNets.empty());
        REQUIRE(!peNets.empty());

        Hertz::layout::LayoutParams p = Hertz::layout::layout_params_for(10.0);
        CHECK(min_gap(c, lNets, nNets) >= p.clearLNmm);
        CHECK(min_gap(c, lNets, peNets) >= p.clearPEmm);
        CHECK(min_gap(c, nNets, peNets) >= p.clearPEmm);

        // copper-to-board-edge clearance (KiCad's default constraint is
        // 0.5 mm; the first emitted board FAILED its DRC here — the PE lug
        // pads touched the bottom edge)
        std::regex edgeRe(
            R"re(\(gr_line \(start ([-\d.]+) ([-\d.]+)\) \(end ([-\d.]+) ([-\d.]+)\) \(layer "Edge.Cuts"\))re");
        double ex1 = 1e30, ey1 = 1e30, ex2 = -1e30, ey2 = -1e30;
        for (auto it = std::sregex_iterator(g.kicadPcb.begin(),
                                            g.kicadPcb.end(), edgeRe);
             it != std::sregex_iterator(); ++it) {
            for (int k : {1, 3}) {
                ex1 = std::min(ex1, std::stod((*it)[k]));
                ex2 = std::max(ex2, std::stod((*it)[k]));
            }
            for (int k : {2, 4}) {
                ey1 = std::min(ey1, std::stod((*it)[k]));
                ey2 = std::max(ey2, std::stod((*it)[k]));
            }
        }
        REQUIRE(ex2 > ex1);
        double edgeClear = 1e30;
        auto upd_edge = [&](double x, double y, double half) {
            for (double d : {x - ex1, ex2 - x, y - ey1, ey2 - y})
                edgeClear = std::min(edgeClear, d - half);
        };
        for (const auto& q : c.pads) upd_edge(q.x, q.y, q.dia / 2);
        for (const auto& sg : c.segs) {
            upd_edge(sg.x1, sg.y1, sg.w / 2);
            upd_edge(sg.x2, sg.y2, sg.w / 2);
        }
        CHECK(edgeClear >= 0.5);
    }
}

TEST_CASE("layoutgen: emission is byte-deterministic", "[layout]") {
    auto a = Hertz::layout::generate_filter_board(demo_design(2), 10.0);
    auto b = Hertz::layout::generate_filter_board(demo_design(2), 10.0);
    CHECK(a.kicadPcb == b.kicadPcb);
}

TEST_CASE("layoutgen: trace width follows IPC-2221 and never lies thin",
          "[layout]") {
    // known point: 10 A at 30 K rise, 35 um external is ~3.5-4 mm in every
    // published IPC-2221 chart
    const double w10 = Hertz::layout::trace_width_ipc2221_mm(10.0);
    CHECK(w10 > 3.0);
    CHECK(w10 < 4.5);
    // monotone in current, floored for signal-level currents
    CHECK(Hertz::layout::trace_width_ipc2221_mm(20.0) > w10);
    CHECK(Hertz::layout::trace_width_ipc2221_mm(0.01) == 0.5);
}

TEST_CASE("layoutgen: refuses what it cannot lay out yet — loudly", "[layout]") {
    auto d = demo_design(1);
    d.nLines = 3;
    CHECK_THROWS_AS(Hertz::layout::generate_filter_board(d, 10.0),
                    std::invalid_argument);
    CHECK_THROWS_AS(
        Hertz::layout::generate_filter_board(demo_design(1), -1.0),
        std::invalid_argument);
}

// ---------------------------------------------------------------------------
// S2: layout parasitics (ABT #443)
// ---------------------------------------------------------------------------
#include "hertz/LayoutParasitics.hpp"

TEST_CASE("parasitics: Grover strip and Neumann mutual reproduce hand values",
          "[layout][parasitics]") {
    // 10 mm x 1 mm strip: 0.2·10·(ln(20/1.035)+0.5+0.0231) = 6.97 nH
    CHECK(std::abs(Hertz::layout::strip_partial_nh(10.0, 1.0) - 6.97) < 0.05);
    // mutual is symmetric and decays with distance
    const double near = Hertz::layout::filament_mutual_nh(0, 40, 60, 100, 2.0);
    const double far = Hertz::layout::filament_mutual_nh(0, 40, 60, 100, 20.0);
    CHECK(near > far);
    CHECK(far > 0.0);
    CHECK(std::abs(near -
                   Hertz::layout::filament_mutual_nh(60, 100, 0, 40, 2.0)) < 1e-12);
    // numeric Neumann double integral as the independent check
    auto numeric = [](double a1, double a2, double b1, double b2, double d) {
        const int N = 2000;
        const double da = (a2 - a1) / N, db = (b2 - b1) / N;
        double sum = 0.0;
        for (int i = 0; i < N; ++i) {
            const double xa = a1 + (i + 0.5) * da;
            for (int j = 0; j < N; ++j) {
                const double xb = b1 + (j + 0.5) * db;
                sum += da * db / std::hypot(xa - xb, d);
            }
        }
        return 0.1 * sum;  // nH
    };
    const double closed = Hertz::layout::filament_mutual_nh(0, 40, 60, 100, 2.0);
    CHECK(std::abs(closed - numeric(0, 40, 60, 100, 2.0)) < 0.01 * closed + 1e-6);
}

TEST_CASE("parasitics: the generated board yields plausible, monotone values",
          "[layout][parasitics]") {
    auto g1 = Hertz::layout::generate_filter_board(demo_design(1), 10.0);
    auto lp1 = Hertz::layout::layout_parasitics(g1);
    // bypass mutual: sub-10 nH for a 120 mm board, strictly positive
    CHECK(lp1.mDmNh > 0.0);
    CHECK(lp1.mDmNh < 10.0);
    // connection ESLs: single-digit nH, PE spine tens of nH
    CHECK(lp1.xConnNh >= 0.0);
    CHECK(lp1.yConnNh > 0.5);
    CHECK(lp1.peSpineNh > 20.0);
    // a 2-stage board separates the pairs further -> SMALLER bypass mutual
    auto g2 = Hertz::layout::generate_filter_board(demo_design(2), 10.0);
    auto lp2 = Hertz::layout::layout_parasitics(g2);
    CHECK(lp2.mDmNh < lp1.mDmNh);
}

TEST_CASE("parasitics: the bypass floor caps the achievable IL and the "
          "combined curve respects both limits",
          "[layout][parasitics]") {
    using namespace Hertz;
    const double f = 30e6, m = 1.0;   // 1 nH at 30 MHz in a 100 ohm system
    // floor = -20log10(2*pi*30e6*1e-9/100) = 54.5 dB
    CHECK(std::abs(layout::bypass_floor_il_db(f, m) - 54.49) < 0.05);

    // a strong ideal filter is cut down to the floor; a weak one is not
    Abcd strong = lc_filter_abcd(f, 10e-3, 1e-6, 2);
    Abcd weak = lc_filter_abcd(f, 1e-6, 10e-9, 1);
    const Complex z50{50.0, 0.0};
    const double ilStrong = layout::layout_aware_il_db(f, strong, m, z50, z50);
    const double ilWeak = layout::layout_aware_il_db(f, weak, m, z50, z50);
    CHECK(ilStrong < layout::bypass_floor_il_db(f, m) + 0.1);
    CHECK(std::abs(ilWeak - insertion_loss_db(weak, z50, z50)) < 3.0);
    // no coupling -> exactly the network's own IL
    CHECK(std::abs(layout::layout_aware_il_db(f, strong, 0.0, z50, z50) -
                   insertion_loss_db(strong, z50, z50)) < 1e-9);
}

// ---------------------------------------------------------------------------
// S3/S4: the block optimizer and the exports (ABT #444/#445)
// ---------------------------------------------------------------------------
#include "hertz/FilterBlock.hpp"

TEST_CASE("block: the optimizer meets the target deterministically and "
          "escalation is real",
          "[layout][block]") {
    using namespace Hertz;
    auto run = [&](double aCm, double aDm) {
        return layout::optimize_filter_block(
            150e3, aCm, aDm, 2.2e-9, 10e-6, 10.0,
            {0.5e-3, 1e-3, 3.3e-3, 10e-3, 39e-3},
            {100e-9, 470e-9, 1e-6, 2.2e-6, 4.7e-6});
    };
    auto fb = run(40.0, 30.0);
    CHECK(fb.meets);
    CHECK(fb.layoutAttenCmDb >= 40.0);
    CHECK(fb.layoutAttenDmDb >= 30.0);
    CHECK(fb.board.parts >= 11);
    // deterministic: same spec, same board bytes
    auto fb2 = run(40.0, 30.0);
    CHECK(fb.board.kicadPcb == fb2.board.kicadPcb);
    CHECK(fb.iterations == fb2.iterations);
    // a much harder DM target forces the second stage
    auto hard = run(40.0, 50.0);
    CHECK(hard.escalatedStages);
    // a target beyond the CATALOG refuses loudly — no board was possible
    CHECK_THROWS_AS(run(40.0, 200.0), std::invalid_argument);
    // a target beyond the LAYOUT (design feasible, bypass floor is not)
    // comes back honest, never a silent success: huge leakage makes the DM
    // design easy, but 105 dB at 150 kHz sits above the bypass floor even at maximum spacing
    auto layoutLimited = Hertz::layout::optimize_filter_block(
        150e3, 40.0, 105.0, 2.2e-9, 1e-3, 10.0,
        {5e-3, 10e-3, 39e-3}, {100e-9, 470e-9, 1e-6});
    CHECK(!layoutLimited.meets);
    CHECK(layoutLimited.board.parts > 0);   // the closest board still ships
}

TEST_CASE("block: curves carry both limits and the exports are well-formed",
          "[layout][block]") {
    using namespace Hertz;
    auto fb = layout::optimize_filter_block(
        150e3, 40.0, 30.0, 2.2e-9, 10e-6, 10.0,
        {0.5e-3, 1e-3, 3.3e-3, 10e-3, 39e-3},
        {100e-9, 470e-9, 1e-6, 2.2e-6, 4.7e-6});
    auto c = layout::block_curves(fb.design, fb.par);
    REQUIRE(c.fHz.size() > 50);
    // physically-true invariants only. The bypass floor bounds the DM curve
    // EVERYWHERE (|T_total| >= |T_bypass|). "layout <= ideal" does NOT hold
    // pointwise — extra branch ESL shifts the SRF down and locally IMPROVES
    // shunting between the two resonances — so it is asserted only at the
    // top of the band, where inductive branches make more ESL strictly
    // worse.
    for (size_t i = 0; i < c.fHz.size(); ++i) {
        CHECK(c.dmDb[i] <= c.dmFloorDb[i] + 1e-6);
        CHECK(std::isfinite(c.dmDb[i]));
        CHECK(std::isfinite(c.cmDb[i]));
    }
    CHECK(c.dmDb.back() <= c.dmIdealDb.back() + 1e-6);
    CHECK(c.cmDb.back() <= c.cmIdealDb.back() + 1e-6);

    // s2p: header + one row per point, finite numbers
    const std::string s2p = layout::touchstone_s2p(fb.design, fb.par, "dm");
    CHECK_THAT(s2p, Catch::Matchers::ContainsSubstring("# HZ S RI R 50"));
    CHECK_THAT(s2p, Catch::Matchers::ContainsSubstring("bypass M"));
    CHECK_THROWS_AS(layout::touchstone_s2p(fb.design, fb.par, "xx"),
                    std::invalid_argument);

    // spice: coupled choke with a PHYSICAL k, the shared PE inductor, the
    // bypass pair
    const std::string sp = layout::block_spice_subckt(fb.design, fb.par);
    CHECK_THAT(sp, Catch::Matchers::ContainsSubstring(".subckt HERTZ_FILTER_BLOCK"));
    CHECK_THAT(sp, Catch::Matchers::ContainsSubstring("Kcm1 Lcm1a Lcm1b 0."));
    CHECK_THAT(sp, Catch::Matchers::ContainsSubstring("Lpe pe_i PE"));
    CHECK_THAT(sp, Catch::Matchers::ContainsSubstring("Kbyp Lbyp1 Lbyp2"));
    CHECK_THAT(sp, Catch::Matchers::ContainsSubstring(".ends"));

    // bom csv: one line per role
    const std::string bom = layout::block_bom_csv(fb.design);
    CHECK_THAT(bom, Catch::Matchers::ContainsSubstring("ref,qty,value"));
    CHECK_THAT(bom, Catch::Matchers::ContainsSubstring("CM choke"));
    CHECK_THAT(bom, Catch::Matchers::ContainsSubstring("Y2"));
}
