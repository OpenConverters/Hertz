# Bundled measurement data

## baltic-lab-cispr25-bench.csv
Real benchtop conducted-emissions measurement (peak + average detectors,
150 kHz – 107 MHz, 1385 points), digitized from Figure 15 of:

S. Westerhold, "A Benchtop Approach to Conducted Emissions Testing According
to CISPR 25 Using the Voltage Method", Baltic Lab, ISSN 2751-8140,
9 January 2026. DOI: 10.5281/zenodo.18202069 — License: CC-BY-4.0.

Setup: Rigol RSA5065N (EMI option) + TekBox TBL0510-1 5 µH LISNs on a bonded
reference ground plane, TekBox EMCview, CISPR 16-1-1 bandwidths/detectors.
Digitization: pixel-envelope extraction of the peak (magenta) and average
(green) traces; the frequency axis was calibrated and verified against the
CISPR 25 Class 3 limit staircase drawn in the same figure (all band edges
land within 0.5 %). Digitization adds up to ~0.5 dB amplitude quantization.
