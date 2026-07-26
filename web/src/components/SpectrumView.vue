<script setup>
// Spectrum Doctor: analyzer scan in, limit verdict + required attenuation out.
import { computed, ref } from 'vue'
import LogChart from './LogChart.vue'
import { api, STANDARDS } from '../engine.js'
import { store } from '../store.js'
import { demoScanCsv, realScanCsv, realCmDmScans, mdpiQrfCsv,
         REAL_SCAN_NAME, REAL_CM_NAME, REAL_DM_NAME, MDPI_SCAN_NAME } from '../demo.js'
import { fmtHz, fmtDb } from '../format.js'

const traces = ref([])          // {name, frequenciesHz, levelsDbuv, analysis, uncovered}
const rawFiles = ref([])        // {name, text} — kept so unit changes re-read files
const parseProblems = ref([])   // per-file failures — never erased by a later analyze()
const columnPickers = ref([])   // multi-trace exports awaiting a level-column choice
const columnChoice = ref({})    // file name -> 1-based numeric column to judge
const comb = ref(null)
const U_CISPR_DB = 3.6          // CISPR 16-4-2 measurement uncertainty, conducted mains
const standardId = ref('cispr32_class_b')
const detector = ref('quasi_peak')
const freqUnit = ref('')        // '' = read from header
const levelUnit = ref('')
const limitRuns = ref(null)
const error = ref('')
const busy = ref(false)
const dragOver = ref(false)
const fileInput = ref(null)

const detectors = computed(() =>
  STANDARDS.find((s) => s.id === standardId.value)?.detectors ?? ['quasi_peak'])

// The judged column's header often NAMES its detector ("Average (dBuV)");
// judging it against another detector's limit reads 6-20 dB wrong. The tool
// never guesses, but it must not stay silent when both strings are in hand.
function detectorOfLabel(label) {
  const lowered = (label || '').toLowerCase()
  if (/quasi|(^|[^a-z])qp/.test(lowered)) return 'quasi_peak'
  if (/average|(^|[^a-z])avg/.test(lowered)) return 'average'
  if (/peak|(^|[^a-z])pk/.test(lowered)) return 'peak'
  return null
}
const detectorMismatches = computed(() => traces.value
  .filter((t) => {
    const named = detectorOfLabel(t.columnLabel)
    return named && named !== detector.value
  })
  .map((t) => `${t.name} → ${t.columnLabel}`))

const seriesColors = ['var(--s-1)', 'var(--s-2)', 'var(--s-3)']

async function ingest(files) {
  for (const file of files) {
    const text = typeof file === 'string' ? file : await file.text()
    const name = typeof file === 'string' ? 'demo scan' : file.name
    // same name replaces (re-drop after a unit fix must not duplicate)
    rawFiles.value = [...rawFiles.value.filter((f) => f.name !== name), { name, text }].slice(-2)
  }
  await parseAll()
}

async function parseAll() {
  error.value = ''
  busy.value = true
  try {
    const engine = await api()
    const parsed = []
    const problems = []
    // every numeric column except the (header-identified) frequency axis is a
    // candidate level trace
    const pickerFor = (name, info) => ({
      name,
      chosen: columnChoice.value[name] ?? '',
      options: info.names
        .map((label, i) => ({ index: i + 1, label: label || `column ${i + 1}` }))
        .filter((option) => option.index !== info.frequencyColumn),
    })
    const pickers = rawFiles.value
      .filter(({ name }) => columnChoice.value[name])
      .map(({ name, text }) => pickerFor(name, engine.spectrumCsvColumns(text)))
    for (const { name, text } of rawFiles.value) {
      // MINIMAL override: each stated header unit always wins; an override may
      // only fill an axis the header left silent. Trying (none), (freq only),
      // (level only), (both) in order guarantees a stated dBm header can never
      // be clobbered into dBuV by the level dropdown.
      const attempts = []
      const seen = new Set()
      for (const pair of [['', ''], [freqUnit.value, ''], ['', levelUnit.value],
                          [freqUnit.value, levelUnit.value]]) {
        const key = pair.join('|')
        if (!seen.has(key)) { seen.add(key); attempts.push(pair) }
      }
      const levelColumn = columnChoice.value[name] ?? 0
      let trace = null, lastError = null, usedFu = '', usedLu = ''
      for (const [fu, lu] of attempts) {
        try {
          trace = engine.parseSpectrumCsv(text, fu, lu, levelColumn)
          usedFu = fu
          usedLu = lu
          break
        } catch (attemptError) {
          lastError = attemptError
        }
      }
      if (trace) {
        // per axis: a set selector that the successful parse did not need
        const overrideIgnored = Boolean((freqUnit.value && !usedFu) || (levelUnit.value && !usedLu))
        const picker = pickers.find((p) => p.name === name)
        const columnLabel = picker?.options.find((o) => o.index === levelColumn)?.label ?? ''
        parsed.push({ name, ...trace, analysis: null, uncovered: false, overrideIgnored, columnLabel })
      } else if (lastError.message.startsWith('multiple level columns')) {
        // a multi-trace export: never guess which trace to judge — ask
        try {
          pickers.push(pickerFor(name, engine.spectrumCsvColumns(text)))
        } catch (columnsError) {
          problems.push(`${name}: ${columnsError.message}`)
        }
      } else {
        problems.push(`${name}: ${lastError.message}`)
      }
    }
    traces.value = parsed
    parseProblems.value = problems
    columnPickers.value = pickers
    if (parsed.length) await analyze()
    else if (problems.length) error.value = problems.join(' · ')
  } catch (e) {
    error.value = e.message
  } finally {
    busy.value = false
  }
}

async function analyze() {
  if (!traces.value.length) return
  error.value = ''
  try {
    const engine = await api()
    let fMin = Infinity, fMax = 0
    for (const t of traces.value) {
      try {
        t.analysis = engine.limitAnalysis(standardId.value, detector.value, t.frequenciesHz, t.levelsDbuv)
        t.uncovered = false
      } catch (traceError) {
        // one out-of-band trace must not poison the others
        t.analysis = null
        t.uncovered = true
      }
      fMin = Math.min(fMin, t.frequenciesHz[0])
      fMax = Math.max(fMax, t.frequenciesHz[t.frequenciesHz.length - 1])
    }
    if (traces.value.length && traces.value.every((t) => t.uncovered)) {
      throw new Error('no trace point falls inside the selected limit — check the standard and the frequency units')
    }
    limitRuns.value = engine.limitPolyline(standardId.value, detector.value, fMin, fMax)
    try {
      const first = traces.value.find((t) => t.analysis) ?? traces.value[0]
      comb.value = engine.detectComb(first.frequenciesHz, first.levelsDbuv)
    } catch {
      comb.value = null  // ancillary: must never poison the limit verdict
    }
    traces.value = [...traces.value]
  } catch (e) {
    error.value = e.message
    limitRuns.value = null
    traces.value.forEach((t) => { t.analysis = null })
  }
}

const worst = computed(() => {
  const all = traces.value.filter((t) => t.analysis).map((t) => t.analysis.worst)
  if (!all.length) return null
  return all.reduce((a, b) => (a.marginDb <= b.marginDb ? a : b))
})
// three-state verdict: FAIL / MARGINAL (inside CISPR 16-4-2 uncertainty) / PASS
const verdictState = computed(() => {
  if (!worst.value) return null
  if (worst.value.marginDb < 0) return 'fail'
  if (worst.value.marginDb < U_CISPR_DB) return 'marginal'
  return 'pass'
})
const uncoveredNames = computed(() => traces.value.filter((t) => t.uncovered).map((t) => t.name))
const requiredAttenuation = computed(() => {
  const all = traces.value.filter((t) => t.analysis).map((t) => t.analysis.requiredAttenuationDb)
  return all.length ? Math.max(...all) : null
})

const offenders = computed(() => {
  const rows = []
  for (const t of traces.value) {
    if (!t.analysis) continue
    t.analysis.marginsDb.forEach((margin, i) => {
      if (margin !== null && margin < 0) {
        rows.push({ trace: t.name, f: t.frequenciesHz[i], level: t.levelsDbuv[i],
                    limit: t.analysis.limitsDbuv[i], margin })
      }
    })
  }
  return rows.sort((a, b) => a.margin - b.margin).slice(0, 8)
})

const chartSeries = computed(() => {
  const series = []
  traces.value.forEach((t, i) => {
    if (t.analysis) {
      // runs of CONSECUTIVE same-coverage points: a polyline may only connect
      // neighboring samples — a chord across the other set's span (or across a
      // sampling HOLE the sweep jumped over) draws data where nothing was
      // measured. 0.35 decades matches the engine's unswept-hole criterion.
      const runs = []
      t.analysis.covered.forEach((isCovered, k) => {
        const point = { f: t.frequenciesHz[k], v: t.levelsDbuv[k] }
        const last = runs[runs.length - 1]
        const prevF = last?.points[last.points.length - 1]?.f
        if (!last || last.covered !== isCovered ||
            (prevF > 0 && Math.log10(point.f / prevF) > 0.35)) {
          runs.push({ covered: isCovered, points: [] })
        }
        runs[runs.length - 1].points.push(point)
      })
      let labeled = { covered: false, uncovered: false }
      runs.forEach((run, r) => {
        if (run.covered) {
          series.push({ id: `${t.name}${i}c${r}`, label: labeled.covered ? '' : t.name,
                        color: seriesColors[i], points: run.points })
          labeled.covered = true
        } else {
          series.push({ id: `${t.name}${i}u${r}`,
                        label: labeled.uncovered ? '' : `${t.name} — outside limit coverage (not judged)`,
                        color: 'var(--ink-dim)', dash: '2 3', points: run.points })
          labeled.uncovered = true
        }
      })
    } else {
      series.push({ id: t.name + i, label: t.name + (t.uncovered ? ' (not covered)' : ''),
                    color: t.uncovered ? 'var(--ink-dim)' : seriesColors[i],
                    points: t.frequenciesHz.map((f, k) => ({ f, v: t.levelsDbuv[k] })) })
    }
  })
  return series
})
// R10-1: a verdict naming a standard implies the standard's WHOLE range —
// bands the scan never reached must be named next to the verdict, or a
// 150 kHz-30 MHz sweep "passes" CISPR 25 with the FM band unmeasured.
const unsweptSummary = computed(() => {
  const regions = traces.value.flatMap((t) => t.analysis?.unsweptHz ?? [])
    .sort((a, b) => a[0] - b[0])
  const merged = []
  for (const [f0, f1] of regions) {
    if (merged.length && f0 <= merged[merged.length - 1][1] * 1.000000001) {
      merged[merged.length - 1][1] = Math.max(merged[merged.length - 1][1], f1)
    } else {
      merged.push([f0, f1])
    }
  }
  // full precision, never the display formatter's 3 significant figures: a
  // NOT SWEPT endpoint printed as "16.9 MHz" would understate a 16.95 MHz
  // gap edge by 50 kHz
  const preciseHz = (f) => {
    const strip = (s) => s.replace(/\.?0+$/, '')
    return f >= 1e6 ? `${strip((f / 1e6).toFixed(2))} MHz` : `${strip((f / 1e3).toFixed(1))} kHz`
  }
  return merged.map(([f0, f1]) => `${preciseHz(f0)}–${preciseHz(f1)}`)
})
const uncoveredPointSummary = computed(() => {
  const rows = []
  for (const t of traces.value) {
    if (!t.analysis) continue
    const n = t.analysis.covered.filter((c) => !c).length
    if (n > 0) rows.push(`${t.name}: ${n} of ${t.analysis.covered.length} points`)
  }
  return rows
})
const chartRefRuns = computed(() => (limitRuns.value ? [{
  label: detector.value === 'average' ? 'AVG limit' : detector.value === 'peak' ? 'PK limit' : 'QP limit',
  color: 'var(--s-limit)', dash: '7 5', runs: limitRuns.value.runs,
}, {
  label: 'limit − U(CISPR)',
  color: 'var(--s-limit)', dash: '2 6',
  runs: limitRuns.value.runs.map((run) => run.map((p) => ({ f: p.f, v: p.v - U_CISPR_DB }))),
}] : []))
const chartViolations = computed(() => {
  const points = []
  for (const t of traces.value) {
    if (!t.analysis) continue
    t.analysis.marginsDb.forEach((margin, i) => {
      if (margin !== null && margin < 0) points.push({ f: t.frequenciesHz[i], v: t.levelsDbuv[i] })
    })
  }
  return points
})

async function designTheFix() {
  const judged = traces.value.filter((t) => t.analysis)
  if (traceSemantics.value !== 'lines' && judged.length === 2) {
    // separated CM/DM pair: hand over PER-MODE binding sets, judged with the
    // 6 dB in-phase mode-summation allowance on top of the 10 dB margin —
    // exactly the Receiver's rule, so the designer sizes each mode at its own
    // critical point (a passing mode arrives empty, never over-designed)
    const engine = await api()
    const order = traceSemantics.value === 'cm-first' ? ['cm', 'dm'] : ['dm', 'cm']
    const sets = { cm: [], dm: [] }
    judged.forEach((t, i) => {
      const analysis = engine.limitAnalysis(standardId.value, detector.value,
                                            t.frequenciesHz, t.levelsDbuv, 16)
      analysis.marginsDb.forEach((margin, k) => {
        if (margin !== null && margin < 16) sets[order[i]].push([t.frequenciesHz[k], 16 - margin])
      })
    })
    store.handoff = { binding: sets }
    store.mode = 'filter'
    return
  }
  store.handoff = {
    aReqDb: Math.ceil((requiredAttenuation.value ?? 40) / 5) * 5,
    fSwHz: comb.value?.found ? comb.value.fSwHz : null,
  }
  store.mode = 'filter'
}

function onDrop(event) {
  dragOver.value = false
  ingest([...event.dataTransfer.files])
}

// The real measurement ships as a genuine multi-trace export (Frequency, Peak,
// Average) — the Average column is pre-chosen and judged against the class it
// actually fails (CISPR 25 Class 5 average; it PASSES the paper's Class 3
// target). Switch class/detector/column freely afterwards.
const REAL_NAMES = [REAL_SCAN_NAME, REAL_CM_NAME, REAL_DM_NAME]
const realScanLoaded = computed(() => traces.value.some((t) => REAL_NAMES.includes(t.name)) ||
  columnPickers.value.some((p) => REAL_NAMES.includes(p.name)))
// What two loaded traces ARE decides what "Design the fix" hands over: the
// two LISN lines share one requirement; a separated CM/DM pair hands over
// PER-MODE binding sets (the same contract as the Receiver's 2-channel path).
const traceSemantics = ref('lines')
async function loadRealScan() {
  standardId.value = 'cispr25_class_5'
  detector.value = 'average'
  columnChoice.value[REAL_SCAN_NAME] = 3
  const file = new File([realScanCsv()], REAL_SCAN_NAME, { type: 'text/csv' })
  await ingest([file])
}

async function loadMdpi() {
  standardId.value = 'cispr32_class_b'
  detector.value = 'average'
  await ingest([new File([mdpiQrfCsv()], MDPI_SCAN_NAME, { type: 'text/csv' })])
}

function loadExample(id) {
  if (id === 'demo') return ingest([demoScanCsv()])
  if (id === 'baltic') return loadRealScan()
  if (id === 'baltic-cmdm') return loadRealCmDm()
  if (id === 'mdpi') return loadMdpi()
}

async function loadRealCmDm() {
  standardId.value = 'cispr25_class_5'
  detector.value = 'average'
  columnChoice.value[REAL_CM_NAME] = 3
  columnChoice.value[REAL_DM_NAME] = 3
  traceSemantics.value = 'cm-first'
  const { cm, dm } = realCmDmScans()
  await ingest([new File([cm], REAL_CM_NAME, { type: 'text/csv' }),
                new File([dm], REAL_DM_NAME, { type: 'text/csv' })])
}
</script>

<template>
  <div class="grid2">
    <div>
      <div class="panel">
        <p class="section-label">Scan input</p>
        <div class="drop" :class="{ over: dragOver }" role="button" tabindex="0"
             @click="fileInput.click()" @keydown.enter="fileInput.click()"
             @dragover.prevent="dragOver = true" @dragleave="dragOver = false" @drop.prevent="onDrop">
          Drop an analyzer CSV export here (or click to browse).<br />
          <span class="note">Two files show both LISN lines side by side.</span>
        </div>
        <input ref="fileInput" type="file" accept=".csv,.txt" multiple hidden
               @change="ingest([...$event.target.files]); $event.target.value = ''" />
        <div class="row" style="margin-top: 0.7rem">
          <label class="field"><span>Example scans</span>
            <select data-test="scan-example" value=""
                    @change="loadExample($event.target.value); $event.target.value = ''">
              <option value="" disabled selected>load an example…</option>
              <option value="demo">synthetic — 300 kHz flyback (guaranteed comb)</option>
              <option value="baltic">REAL — CISPR 25 bench DUT (fails Class 5 up high)</option>
              <option value="baltic-cmdm">REAL — same DUT, CM/DM separated</option>
              <option value="mdpi">REAL — QR flyback pre-scan (fails low AND high)</option>
            </select></label>
          <button v-if="traces.length || columnPickers.length" class="ghost" style="align-self: end"
                  @click="traces = []; rawFiles = []; limitRuns = null; comb = null; columnPickers = []; columnChoice = {}; parseProblems = []; traceSemantics = 'lines'">Clear</button>
        </div>
        <label v-if="traces.length === 2" class="field" style="margin-top: 0.7rem"><span>What the two traces are</span>
          <select v-model="traceSemantics" data-test="trace-semantics">
            <option value="lines">the two LISN lines (L and N)</option>
            <option value="cm-first">CM &amp; DM separated — first loaded is CM</option>
            <option value="dm-first">CM &amp; DM separated — first loaded is DM</option>
          </select></label>
        <div class="row" style="margin-top: 0.7rem">
          <label class="field"><span>Frequency unit</span>
            <select v-model="freqUnit" data-test="freq-unit" @change="parseAll"><option value="">from header</option><option>Hz</option><option>kHz</option><option>MHz</option></select>
          </label>
          <label class="field"><span>Level unit</span>
            <select v-model="levelUnit" data-test="level-unit" @change="parseAll"><option value="">from header</option><option>dBuV</option><option>dBm</option></select>
          </label>
        </div>
      </div>

      <div class="panel">
        <p class="section-label">Limit</p>
        <label class="field"><span>Standard</span>
          <select v-model="standardId" data-test="standard" @change="analyze">
            <option v-for="s in STANDARDS" :key="s.id" :value="s.id">{{ s.name }}</option>
          </select>
        </label>
        <label class="field"><span>Detector the scan was taken with</span>
          <select v-model="detector" data-test="detector" @change="analyze">
            <option v-for="d in detectors" :key="d" :value="d">{{ d.replace('_', '-') }}</option>
          </select>
        </label>
        <p v-if="standardId.startsWith('cispr25')" class="note">CISPR 25 defines limits only inside
          protected broadcast bands — points between the bands are shown grey and never judged.</p>
        <p class="note">Coverage is checked against the selected limit: bands or spans the scan never
          reached are called out as NOT SWEPT. Gaps narrower than 10× the band's CISPR RBW
          (2 kHz below 150 kHz, 90 kHz to 30 MHz, 1.2 MHz above) are below the reporting
          threshold — silence means covered at that resolution, not bin-by-bin.</p>
      </div>

      <p v-if="traces.some((t) => t.name === MDPI_SCAN_NAME)" class="note" data-test="mdpi-attribution">
        Real pre-scan: M.-T. Kuo &amp; M.-C. Tsou, “Novel Frequency Swapping Technique for Conducted
        Electromagnetic Interference Suppression in Power Converter Applications”, Energies 10(1):24,
        2017 — <a href="https://doi.org/10.3390/en10010024" rel="noopener">DOI 10.3390/en10010024</a>,
        CC-BY-4.0. Peak-hold trace of a 24 W QR flyback, digitized from Fig. 20a (calibration verified
        against the figure's marker table, all five real values within ~1 dB). Judged the standard
        pre-scan way: peak against the AVERAGE limit — wherever peak clears it, average complies;
        here it fails around 150–510 kHz AND 5–7 MHz while QP passes everywhere.</p>
      <p v-if="realScanLoaded" class="note" data-test="real-scan-attribution">
        Real measurement: S. Westerhold, “A Benchtop Approach to Conducted Emissions Testing
        According to CISPR 25 Using the Voltage Method”, Baltic Lab, Jan 2026 —
        <a href="https://doi.org/10.5281/zenodo.18202069" rel="noopener">DOI 10.5281/zenodo.18202069</a>,
        CC-BY-4.0. Digitized traces: total scan from Fig. 15; CM/DM-separated pair (TekBox LISN
        Mate) from Fig. 18. The DUT passes the paper's Class 3 target and fails Class 5 average,
        CM-dominated: DM meets the raw limit but not the 10+6 dB engineering buffer — “Design the
        fix” therefore hands the designer a hard CM requirement and a light DM one, each at its
        own critical frequency.</p>
      <div v-for="p in columnPickers" :key="p.name" class="panel" data-test="column-picker">
        <p class="section-label">{{ p.name }} — multi-trace export</p>
        <label class="field"><span>The file carries several traces — which column is the one to judge?</span>
          <select :value="p.chosen" data-test="column-select"
                  @change="columnChoice[p.name] = Number($event.target.value); parseAll()">
            <option value="" disabled>choose the trace…</option>
            <option v-for="c in p.options" :key="c.index" :value="c.index">{{ c.label }}</option>
          </select></label>
        <p class="note">Detector column names come from the file header. Judging a QP limit against
          an Average trace (or vice versa) reads 6–20 dB wrong — that is why nothing is guessed.</p>
      </div>
      <p v-if="detectorMismatches.length" class="err" data-test="detector-mismatch">
        Detector mismatch: {{ detectorMismatches.join(' · ') }} names a detector, but the verdict
        uses the <strong>{{ detector.replace('_', '-') }}</strong> limit — switch the detector
        selector or the judged column. Limits differ per detector by 6–20 dB.</p>
      <p v-if="traces.some((t) => t.columnLabel)" class="note" data-test="column-note">
        Judged column: {{ traces.filter((t) => t.columnLabel).map((t) => `${t.name} → ${t.columnLabel}`).join(' · ') }}</p>
      <div v-if="parseProblems.length" class="err" data-test="file-problems">
        Files not read: {{ parseProblems.join(' · ') }}</div>
      <p v-if="traces.some((t) => t.overrideIgnored)" class="note" data-test="override-note">
        Header units win: the selectors only fill units a file's header omits —
        {{ traces.filter((t) => t.overrideIgnored).map((t) => t.name).join(', ') }} kept
        {{ traces.length > 1 ? 'their' : 'its' }} stated units.</p>
      <div v-if="error" class="err" data-test="error">{{ error }}</div>
    </div>

    <div>
      <LogChart :series="chartSeries" :ref-runs="chartRefRuns" :violations="chartViolations" y-label="dBµV" />

      <div v-if="worst" class="panel panel-hi" style="margin-top: 1rem">
        <div class="readout">
          <div class="cell"><b>Verdict</b>
            <span class="chip" :class="verdictState === 'pass' ? 'pass' : verdictState === 'marginal' ? 'warn' : 'fail'"
                  data-test="verdict">{{ verdictState === 'pass' ? 'PASS' : verdictState === 'marginal' ? 'MARGINAL' : 'FAIL' }}</span>
          </div>
          <div class="cell"><b>Worst margin</b>
            <span :style="{ color: worst.marginDb < 0 ? 'var(--fault)' : 'var(--ok)' }">{{ fmtDb(worst.marginDb) }}</span><span class="unit">dB</span>
          </div>
          <div class="cell"><b>at</b><span>{{ fmtHz(worst.frequencyHz) }}</span></div>
          <div class="cell"><b>Level / limit</b>
            <span>{{ fmtDb(worst.levelDbuv) }} / {{ fmtDb(worst.limitDbuv) }}</span><span class="unit">dBµV</span>
          </div>
          <div class="cell"><b>Attenuation for a 10 dB margin</b>
            <span data-test="areq">{{ requiredAttenuation !== null && requiredAttenuation <= 0 ? '0' : fmtDb(requiredAttenuation) }}</span>
            <span class="unit">{{ requiredAttenuation !== null && requiredAttenuation <= 0 ? '(≥10 dB in hand)' : 'dB' }}</span>
          </div>
          <div v-if="comb" class="cell"><b>Switching frequency</b>
            <span v-if="comb.found" data-test="fsw-detected">{{ fmtHz(comb.fSwHz) }}</span>
            <span v-else class="unit">no comb detected</span>
            <span v-if="comb.found" class="unit">{{ comb.harmonics.length }} harmonics</span>
          </div>
        </div>
        <p v-if="verdictState === 'fail' && worst.marginDb > -U_CISPR_DB" class="note" style="color: var(--amber)">
          FAIL by less than the ±{{ U_CISPR_DB }} dB measurement uncertainty — an accredited chamber
          might pass this article, but resolve it with margin, not hope.</p>
        <p v-if="verdictState === 'marginal'" class="note" style="color: var(--amber)">
          Worst margin is inside the ±{{ U_CISPR_DB }} dB CISPR 16-4-2 measurement uncertainty — an
          accredited chamber can read this either way. Treat as unresolved, not as passing.</p>
        <p v-if="unsweptSummary.length" class="note" data-test="unswept-note" style="color: var(--amber)">
          NOT SWEPT: {{ unsweptSummary.join(', ') }} — the selected limit regulates these spans but
          the scan never reached them. The verdict above covers only the measured spans; an
          accredited report would reject this shortfall.</p>
        <p v-if="uncoveredNames.length" class="note" data-test="uncovered-note">
          Not covered by this limit (ignored in the verdict): {{ uncoveredNames.join(', ') }}</p>
        <p v-if="uncoveredPointSummary.length" class="note" data-test="uncovered-points" style="color: var(--amber)">
          Outside the limit's bands and therefore NOT judged (grey dashes on the chart):
          {{ uncoveredPointSummary.join(' · ') }}{{ standardId.startsWith('cispr25')
            ? ' — CISPR 25 defines no limit between the protected broadcast bands, but other services may.'
            : ' — this limit does not extend to those frequencies.' }}</p>
        <div style="margin-top: 0.8rem">
          <button v-if="verdictState !== 'pass'" class="act" data-test="design-fix" @click="designTheFix">Design the fix →</button>
        </div>
      </div>

      <div v-if="offenders.length" class="panel" style="margin-top: 1rem">
        <p class="section-label">Worst offenders</p>
        <table class="data" data-test="offenders">
          <thead><tr><th>Trace</th><th>Frequency</th><th>Level dBµV</th><th>Limit dBµV</th><th>Margin dB</th></tr></thead>
          <tbody>
            <tr v-for="(row, i) in offenders" :key="i">
              <td>{{ row.trace }}</td><td>{{ fmtHz(row.f) }}</td><td>{{ fmtDb(row.level) }}</td>
              <td>{{ fmtDb(row.limit) }}</td><td class="neg">{{ fmtDb(row.margin) }}</td>
            </tr>
          </tbody>
        </table>
      </div>
    </div>
  </div>
</template>
