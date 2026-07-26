import balticLabCsv from './assets/baltic-lab-cispr25-bench.csv?raw'
import balticLabCmCsv from './assets/baltic-lab-cm.csv?raw'
import balticLabDmCsv from './assets/baltic-lab-dm.csv?raw'

// A REAL benchtop conducted-emissions measurement: S. Westerhold (Baltic Lab),
// "A Benchtop Approach to Conducted Emissions Testing According to CISPR 25
// Using the Voltage Method", Jan 2026, DOI 10.5281/zenodo.18202069, CC-BY-4.0.
// Peak + average traces digitized from Fig. 15 (1385 points, 150 kHz-107 MHz);
// the x-calibration was verified against the CISPR 25 limit staircase drawn in
// the figure (band edges land within 0.5 %).
export const REAL_SCAN_NAME = 'baltic-lab-cispr25-bench.csv'
export function realScanCsv() {
  return balticLabCsv
}

// The same paper's Fig. 18: the SAME DUT with its noise SEPARATED into common
// and differential mode by a TekBox TBLM2 LISN Mate (CM PK/AVG, DM PK/AVG).
// Digitized the same way (y from the gridline raster, x verified via the
// comb fundamental landing on the DUT's ~404 kHz switching frequency).
// Reality check worth teaching: CM average FAILS class 5 by 9.3 dB at
// ~76 MHz; DM meets the raw limit (+5.7 dB) but not the 10+6 dB engineering
// buffer — real SMPS noise above 30 MHz is usually CM-dominated, and the
// per-mode designer sizes DM lightly at its own frequency instead of from
// the CM offence.
export const REAL_CM_NAME = 'baltic-lab-cm-bench.csv'
export const REAL_DM_NAME = 'baltic-lab-dm-bench.csv'
export function realCmDmScans() {
  return { cm: balticLabCmCsv, dm: balticLabDmCsv }
}

// Demo data: a failing 300 kHz flyback scan (harmonic comb + broadband floor),
// emitted as CSV so the demo exercises the real ingestion path.
export function demoScanCsv() {
  const lines = ['Frequency [MHz];Level [dBµV]']
  const fSw = 300e3
  // the multiplicative loop must END exactly at 30 MHz or the demo trips its
  // own band-coverage note with a 350 kHz tail shortfall
  const points = []
  for (let f = 150e3; f < 30e6; f *= 1.02) points.push(f)
  points.push(30e6)
  for (const f of points) {
    let level = 28 - 6 * Math.log10(f / 150e3)
    for (let n = 1; n <= 60; n += 1) {
      const fh = n * fSw
      const sigma = fh * 0.012
      const peak = 84 - 22 * Math.log10(n)
      level = Math.max(level, peak - 0.5 * ((f - fh) / sigma) ** 2)
    }
    lines.push((f / 1e6).toFixed(5) + ';' + level.toFixed(2))
  }
  return lines.join('\n')
}

// Demo waveform for the receiver view: a 300 kHz tone gated at 100 Hz / 10 %
// duty — the classic case where peak, quasi-peak and average disagree.
export function demoWaveform(fsHz = 1e6, durationS = 0.5) {
  const n = Math.round(fsHz * durationS)
  const samples = new Float64Array(n)
  for (let i = 0; i < n; i += 1) {
    const t = i / fsHz
    const gate = (t * 100) % 1 < 0.1 ? 1 : 0
    samples[i] = 1e-3 * gate * Math.sin(2 * Math.PI * 300e3 * t)
  }
  return { samples, fsHz }
}
