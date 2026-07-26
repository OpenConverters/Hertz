<script setup>
// Spectrum Doctor: analyzer scan in, limit verdict + required attenuation out.
import { computed, ref } from 'vue'
import LogChart from './LogChart.vue'
import { api, STANDARDS } from '../engine.js'
import { store } from '../store.js'
import { demoScanCsv } from '../demo.js'
import { fmtHz, fmtDb } from '../format.js'

const traces = ref([])          // {name, frequenciesHz, levelsDbuv, analysis, uncovered}
const rawFiles = ref([])        // {name, text} — kept so unit changes re-read files
const parseProblems = ref([])   // per-file failures — never erased by a later analyze()
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
      let trace = null, lastError = null, usedFu = '', usedLu = ''
      for (const [fu, lu] of attempts) {
        try {
          trace = engine.parseSpectrumCsv(text, fu, lu)
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
        parsed.push({ name, ...trace, analysis: null, uncovered: false, overrideIgnored })
      } else {
        problems.push(`${name}: ${lastError.message}`)
      }
    }
    traces.value = parsed
    parseProblems.value = problems
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
      const covered = [], uncovered = []
      t.analysis.covered.forEach((isCovered, k) => {
        (isCovered ? covered : uncovered).push({ f: t.frequenciesHz[k], v: t.levelsDbuv[k] })
      })
      series.push({ id: t.name + i, label: t.name, color: seriesColors[i], points: covered })
      if (uncovered.length) {
        series.push({ id: t.name + i + 'u', label: `${t.name} — outside limit coverage (not judged)`,
                      color: 'var(--ink-dim)', dash: '2 3', points: uncovered })
      }
    } else {
      series.push({ id: t.name + i, label: t.name + (t.uncovered ? ' (not covered)' : ''),
                    color: t.uncovered ? 'var(--ink-dim)' : seriesColors[i],
                    points: t.frequenciesHz.map((f, k) => ({ f, v: t.levelsDbuv[k] })) })
    }
  })
  return series
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

function designTheFix() {
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
          <button class="ghost" data-test="load-demo" @click="ingest([demoScanCsv()])">Load demo scan</button>
          <button v-if="traces.length" class="ghost" @click="traces = []; rawFiles = []; limitRuns = null; comb = null">Clear</button>
        </div>
        <div class="row" style="margin-top: 0.7rem">
          <label class="field"><span>Frequency unit</span>
            <select v-model="freqUnit" @change="parseAll"><option value="">from header</option><option>Hz</option><option>kHz</option><option>MHz</option></select>
          </label>
          <label class="field"><span>Level unit</span>
            <select v-model="levelUnit" @change="parseAll"><option value="">from header</option><option>dBuV</option><option>dBm</option></select>
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
          <select v-model="detector" @change="analyze">
            <option v-for="d in detectors" :key="d" :value="d">{{ d.replace('_', '-') }}</option>
          </select>
        </label>
        <p v-if="standardId.startsWith('cispr25')" class="note">CISPR 25 defines limits only inside
          protected broadcast bands — points between the bands are shown grey and never judged.</p>
      </div>

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
