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

## baltic-lab-cm.csv / baltic-lab-dm.csv
Common-mode and differential-mode separated conducted-emissions traces
(peak + average each, 150 kHz – 107 MHz) of the same DUT, measured through a
TekBox TBLM2 LISN Mate — digitized from Figure 18 of the same paper
(DOI 10.5281/zenodo.18202069, CC-BY-4.0). The y-axis was calibrated from the
10 dB gridline raster (12.3 px/dB, frame top = 40 dBµV); the x-axis uses the
same 150 kHz – 108 MHz log span as Fig. 15, cross-checked by the DUT's comb
fundamental landing at ~401 kHz in both figures. Label boxes in the figure
occlude short spans of the DM curves; occluded columns are omitted, never
interpolated. Engine-verified: CM average fails CISPR 25 Class 5 by 9.3 dB
near 76 MHz; DM meets every raw limit (worst +5.7 dB) but not the 10+6 dB
engineering buffer.
