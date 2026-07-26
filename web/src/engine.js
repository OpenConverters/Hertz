// Loads the Hertz WASM engine (public/hertz.js, built by scripts/build_wasm.sh)
// and wraps its JSON-string API. Engine errors arrive as {"error": msg} — they
// are rethrown here so views handle one failure path.
let modulePromise = null

export function loadEngine() {
  if (!modulePromise) {
    modulePromise = import(/* @vite-ignore */ new URL('hertz.js', window.location.origin + '/').href)
      .then((m) => m.default())
  }
  return modulePromise
}

function unwrap(jsonText) {
  const value = JSON.parse(jsonText)
  if (value && value.error) throw new Error(value.error)
  return value
}

export async function api() {
  const mod = await loadEngine()
  return {
    version: () => mod.version(),
    parseSpectrumCsv: (text, freqUnit = '', levelUnit = '', levelColumn = 0) =>
      unwrap(mod.parseSpectrumCsv(text, freqUnit, levelUnit, levelColumn)),
    spectrumCsvColumns: (text) => unwrap(mod.spectrumCsvColumns(text)),
    limitAnalysis: (standardId, detector, freqs, levels, marginBufferDb = 10) =>
      unwrap(mod.limitAnalysis(standardId, detector, JSON.stringify(freqs), JSON.stringify(levels), marginBufferDb)),
    limitPolyline: (standardId, detector, fMin, fMax, pointsPerDecade = 40) =>
      unwrap(mod.limitPolyline(standardId, detector, fMin, fMax, pointsPerDecade)),
    designFilter: (params) => unwrap(mod.designFilter(JSON.stringify(params))),
    filterSpiceNetlist: (design, lisnKind, mode = 'dm') => {
      const text = mod.filterSpiceNetlist(JSON.stringify(design), lisnKind, mode)
      if (text.startsWith('{')) unwrap(text)
      return text
    },
    lisnData: (kind, fMin, fMax, pointsPerDecade = 60) =>
      unwrap(mod.lisnData(kind, fMin, fMax, pointsPerDecade)),
    separateTraces: (line, neutral) =>
      unwrap(mod.separateTraces(Float64Array.from(line), Float64Array.from(neutral))),
    measureWaveform: (samples, fsHz, band = 'B', overlap = 0.9) =>
      unwrap(mod.measureWaveform(samples instanceof Float64Array ? samples : Float64Array.from(samples), fsHz, band, overlap)),
    detectComb: (freqs, levels) =>
      unwrap(mod.detectComb(JSON.stringify(freqs), JSON.stringify(levels))),
    insertionLossCurves: (params) => unwrap(mod.insertionLossCurves(JSON.stringify(params))),
    inputFilterInteraction: (lH, cF, vInMin, pIn) =>
      unwrap(mod.inputFilterInteraction(lH, cF, vInMin, pIn)),
    radiatedEstimate: (freqs, dbua, cableM, distM) =>
      unwrap(mod.radiatedEstimate(JSON.stringify(freqs), JSON.stringify(dbua), cableM, distM)),
    measuredIlCurves: (freqs, zRe, zIm, cShuntF, stages, refZ) =>
      unwrap(mod.measuredIlCurves(JSON.stringify(freqs), JSON.stringify(zRe), JSON.stringify(zIm), cShuntF, stages, refZ)),
  }
}

export const STANDARDS = [
  { id: 'cispr32_class_b', name: 'CISPR 32 / EN 55032 Class B (mains)', detectors: ['quasi_peak', 'average'] },
  { id: 'cispr32_class_a', name: 'CISPR 32 / EN 55032 Class A (mains)', detectors: ['quasi_peak', 'average'] },
  { id: 'cispr25_class_5', name: 'CISPR 25 Class 5 (automotive)', detectors: ['quasi_peak', 'peak', 'average'] },
  { id: 'cispr25_class_4', name: 'CISPR 25 Class 4 (automotive)', detectors: ['quasi_peak', 'peak', 'average'] },
  { id: 'cispr25_class_3', name: 'CISPR 25 Class 3 (automotive)', detectors: ['quasi_peak', 'peak', 'average'] },
]
