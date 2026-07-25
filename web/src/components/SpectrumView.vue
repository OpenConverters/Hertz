<script setup>
// Spectrum Doctor: analyzer scan in, limit verdict + required attenuation out.
import { computed, ref } from 'vue'
import LogChart from './LogChart.vue'
import { api, STANDARDS } from '../engine.js'
import { store } from '../store.js'
import { demoScanCsv } from '../demo.js'
import { fmtHz, fmtDb } from '../format.js'

const traces = ref([])          // {name, frequenciesHz, levelsDbuv, analysis}
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
  error.value = ''
  busy.value = true
  try {
    const engine = await api()
    for (const file of files) {
      const text = typeof file === 'string' ? file : await file.text()
      const name = typeof file === 'string' ? 'demo scan' : file.name
      const trace = engine.parseSpectrumCsv(text, freqUnit.value, levelUnit.value)
      traces.value = [...traces.value.slice(-1), { name, ...trace, analysis: null }].slice(0, 2)
    }
    await analyze()
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
      t.analysis = engine.limitAnalysis(standardId.value, detector.value, t.frequenciesHz, t.levelsDbuv)
      fMin = Math.min(fMin, t.frequenciesHz[0])
      fMax = Math.max(fMax, t.frequenciesHz[t.frequenciesHz.length - 1])
    }
    limitRuns.value = engine.limitPolyline(standardId.value, detector.value, fMin, fMax)
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
const pass = computed(() => traces.value.length > 0 &&
  traces.value.every((t) => t.analysis && t.analysis.pass))
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

const chartSeries = computed(() => traces.value.map((t, i) => ({
  id: t.name + i, label: t.name, color: seriesColors[i],
  points: t.frequenciesHz.map((f, k) => ({ f, v: t.levelsDbuv[k] })),
})))
const chartRefRuns = computed(() => (limitRuns.value ? [{
  label: detector.value === 'average' ? 'AVG limit' : detector.value === 'peak' ? 'PK limit' : 'QP limit',
  color: 'var(--s-limit)', dash: '7 5', runs: limitRuns.value.runs,
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
  store.handoff = { aReqDb: Math.ceil((requiredAttenuation.value ?? 40) / 5) * 5 }
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
          <button v-if="traces.length" class="ghost" @click="traces = []; limitRuns = null">Clear</button>
        </div>
        <div class="row" style="margin-top: 0.7rem">
          <label class="field"><span>Frequency unit</span>
            <select v-model="freqUnit"><option value="">from header</option><option>Hz</option><option>kHz</option><option>MHz</option></select>
          </label>
          <label class="field"><span>Level unit</span>
            <select v-model="levelUnit"><option value="">from header</option><option>dBuV</option><option>dBm</option></select>
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
        <p class="note">CISPR 25 defines limits only inside protected broadcast bands — points between bands are reported as not covered, never as passing.</p>
      </div>

      <div v-if="error" class="err" data-test="error">{{ error }}</div>
    </div>

    <div>
      <LogChart :series="chartSeries" :ref-runs="chartRefRuns" :violations="chartViolations" y-label="dBµV" />

      <div v-if="worst" class="panel panel-hi" style="margin-top: 1rem">
        <div class="readout">
          <div class="cell"><b>Verdict</b>
            <span class="chip" :class="pass ? 'pass' : 'fail'" data-test="verdict">{{ pass ? 'PASS' : 'FAIL' }}</span>
          </div>
          <div class="cell"><b>Worst margin</b>
            <span :style="{ color: worst.marginDb < 0 ? 'var(--fault)' : 'var(--ok)' }">{{ fmtDb(worst.marginDb) }}</span><span class="unit">dB</span>
          </div>
          <div class="cell"><b>at</b><span>{{ fmtHz(worst.frequencyHz) }}</span></div>
          <div class="cell"><b>Level / limit</b>
            <span>{{ fmtDb(worst.levelDbuv) }} / {{ fmtDb(worst.limitDbuv) }}</span><span class="unit">dBµV</span>
          </div>
          <div class="cell"><b>Required attenuation (+10 dB buffer)</b>
            <span data-test="areq">{{ fmtDb(requiredAttenuation) }}</span><span class="unit">dB</span>
          </div>
        </div>
        <div style="margin-top: 0.8rem">
          <button v-if="!pass" class="act" data-test="design-fix" @click="designTheFix">Design the fix →</button>
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
