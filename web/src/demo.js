// Demo data: a failing 300 kHz flyback scan (harmonic comb + broadband floor),
// emitted as CSV so the demo exercises the real ingestion path.
export function demoScanCsv() {
  const lines = ['Frequency [MHz];Level [dBµV]']
  const fSw = 300e3
  for (let f = 150e3; f <= 30e6; f *= 1.02) {
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
