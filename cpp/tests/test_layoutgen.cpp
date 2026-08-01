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
