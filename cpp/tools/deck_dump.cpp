// deck_dump — print a Hertz filter or reference SPICE deck to stdout, so the
// Python cross-check tests (ABT #299) feed the EXACT deck SpiceDeck.hpp emits
// into PyKirchhoff's in-process ngspice. One emitter, two consumers — the deck
// under test can never drift from the deck the app ships.
//
// usage: deck_dump {filter|reference} {cm|dm} <nLines> <stages> <lCmH> <cXF> <cYPerLineF> <lDmH> {cispr16|cispr25}
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "hertz/SpiceDeck.hpp"

int main(int argc, char** argv) {
    if (argc != 10) {
        std::cerr << "usage: deck_dump {filter|reference} {cm|dm} nLines stages lCmH cXF cYPerLineF lDmH {cispr16|cispr25}\n";
        return 2;
    }
    const std::string which = argv[1];
    const std::string mode = argv[2];
    const int nLines = std::atoi(argv[3]);
    const int stages = std::atoi(argv[4]);
    const double lCm = std::atof(argv[5]);
    const double cX = std::atof(argv[6]);
    const double cY = std::atof(argv[7]);
    const double lDm = std::atof(argv[8]);
    const Hertz::Lisn lisn = std::strcmp(argv[9], "cispr25") == 0 ? Hertz::cispr25_lisn()
                                                                  : Hertz::cispr16_lisn();
    try {
        if (which == "reference") {
            std::cout << Hertz::lisn_reference_deck(lisn, mode, nLines);
        } else {
            std::cout << Hertz::filter_spice_deck(stages, lCm, cX, cY, lDm, lisn, mode, nLines);
        }
    } catch (const std::exception& e) {
        std::cerr << "deck_dump: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
