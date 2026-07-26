<script setup>
// Radiated pre-scan SCREENING estimator: a measured common-mode current
// spectrum (current probe / absorbing clamp, dBµA) in, an E-field estimate
// against the CISPR 32 radiated limits out. The model is the classic
// electrically-short CM radiator — a triage tool, never a measurement.
import { ref } from 'vue'
import LogChart from './LogChart.vue'
import { api } from '../engine.js'
import { fmtHz, fmtDb } from '../format.js'

const cableM = ref(1)
const distance = ref('3')     // measurement distance the limit is scaled to
const cls = ref('b')
const trace = ref(null)       // {name, frequenciesHz, dbua}
const result = ref(null)      // {efield, uncertainty, analysis, limitRuns}
const error = ref('')
const dragOver = ref(false)
const fileInput = ref(null)

const standardId = () => `cispr32_rad_${cls.value}_${distance.value}m`

async function ingest(files) {
  const file = files[0]
  if (!file) return
  error.value = ''
  try {
    const engine = await api()
    const text = typeof file === 'string' ? file : await file.text()
    const name = typeof file === 'string' ? 'example CM current comb' : file.name
    const parsed = engine.parseSpectrumCsv(text, '', '', 0)
    if (parsed.levelUnit !== 'dbua') {
      throw new Error('this screen needs a COMMON-MODE CURRENT spectrum — the file header must ' +
        'state dBµA (current-probe / absorbing-clamp export). Voltage scans belong on Spectrum.')
    }
    trace.value = { name, frequenciesHz: parsed.frequenciesHz, dbua: parsed.levelsDbuv }
    await estimate()
  } catch (e) {
    error.value = e.message
    trace.value = null
    result.value = null
  }
}

async function estimate() {
  if (!trace.value) return
  error.value = ''
  result.value = null
  try {
    const engine = await api()
    const est = engine.radiatedEstimate(trace.value.frequenciesHz, trace.value.dbua,
                                        Number(cableM.value), Number(distance.value))
    const analysis = engine.limitAnalysis(standardId(), 'quasi_peak',
                                          trace.value.frequenciesHz, est.efieldDbuvm)
    const fLo = Math.max(30e6, trace.value.frequenciesHz[0])
    const fHi = Math.min(1000e6, trace.value.frequenciesHz[trace.value.frequenciesHz.length - 1])
    const limitRuns = fLo < fHi ? engine.limitPolyline(standardId(), 'quasi_peak', fLo, fHi) : null
    result.value = { efield: est.efieldDbuvm, uncertainty: est.modelUncertaintyDb, analysis, limitRuns }
  } catch (e) {
    error.value = e.message
  }
}

// clearly-synthetic example: a 12 µA switching comb decaying over 30–300 MHz
function exampleCsv() {
  const lines = ['Frequency (MHz),CM current (dBuA)']
  for (let f = 30e6; f <= 300e6; f *= 1.02) {
    const comb = Math.abs(Math.sin(f / 7e6)) > 0.93 ? 6 : 0
    lines.push((f / 1e6).toFixed(3) + ',' + (21.6 - 8 * Math.log10(f / 30e6) + comb).toFixed(2))
  }
  return lines.join('\n')
}

const series = () => [{
  id: 'e', label: 'estimated E-field (screening)', color: 'var(--s-1)',
  points: trace.value.frequenciesHz.map((f, i) => ({ f, v: result.value.efield[i] })),
}]
const refRuns = () => (result.value.limitRuns ? [{
  label: 'QP limit', color: 'var(--s-limit)', dash: '7 5', runs: result.value.limitRuns.runs,
}] : [])
const violations = () => {
  const points = []
  result.value.analysis.marginsDb.forEach((m, i) => {
    if (m !== null && m < 0) points.push({ f: trace.value.frequenciesHz[i], v: result.value.efield[i] })
  })
  return points
}
</script>

<template>
  <div class="grid2">
    <div>
      <div class="panel">
        <p class="section-label">Common-mode current input</p>
        <p class="note" style="margin-bottom: 0.6rem">Why this screen: radiated emissions above
          30 MHz are usually driven by common-mode current on the cables. Measure I<sub>CM</sub>
          with a current probe (dBµA) and this estimates the E-field an OATS/SAC would see —
          the other half of the EMC report, as a screening number.</p>
        <div class="drop" :class="{ over: dragOver }" role="button" tabindex="0"
             @click="fileInput.click()" @keydown.enter="fileInput.click()"
             @dragover.prevent="dragOver = true" @dragleave="dragOver = false"
             @drop.prevent="dragOver = false; ingest([...$event.dataTransfer.files])">
          Drop a CM-current CSV here (header must state dBµA).
        </div>
        <input ref="fileInput" type="file" accept=".csv,.txt" hidden
               @change="ingest([...$event.target.files]); $event.target.value = ''" />
        <div class="row" style="margin-top: 0.7rem">
          <button class="ghost" data-test="radiated-example" @click="ingest([exampleCsv()])">Load synthetic example</button>
        </div>
        <div class="row" style="margin-top: 0.7rem">
          <label class="field"><span>Cable / harness length (m)</span>
            <input v-model.number="cableM" type="number" min="0.1" step="0.1" data-test="cable-length" @change="estimate" /></label>
          <label class="field"><span>Limit distance</span>
            <select v-model="distance" data-test="radiated-distance" @change="estimate">
              <option value="3">3 m</option><option value="10">10 m</option>
            </select></label>
        </div>
        <label class="field"><span>CISPR 32 class</span>
          <select v-model="cls" data-test="radiated-class" @change="estimate">
            <option value="b">Class B (residential)</option>
            <option value="a">Class A (industrial)</option>
          </select></label>
        <p class="note">Model: |E| = 1.257·10⁻⁶ · f · L<sub>eff</sub> · I<sub>CM</sub> / d, the
          electrically-short CM radiator (Ott/Paul), L<sub>eff</sub> capped at λ/4 above cable
          resonance — the model behind “≈5 µA on 1 m fails Class B at 3 m”.</p>
      </div>
      <div v-if="error" class="err" data-test="error">{{ error }}</div>
    </div>

    <div>
      <div v-if="result" class="panel panel-hi" style="margin-bottom: 1rem">
        <div class="readout">
          <div class="cell"><b>Screening verdict</b>
            <span class="chip" :class="result.analysis.pass ? 'pass' : 'fail'" data-test="radiated-verdict">
              {{ result.analysis.pass ? 'PASS (screen)' : 'FAIL (screen)' }}</span></div>
          <div class="cell"><b>Worst margin</b>
            <span :style="{ color: result.analysis.worst.marginDb < 0 ? 'var(--fault)' : 'var(--ok)' }">
              {{ fmtDb(result.analysis.worst.marginDb) }}</span><span class="unit">dB</span></div>
          <div class="cell"><b>at</b><span>{{ fmtHz(result.analysis.worst.frequencyHz) }}</span></div>
          <div class="cell"><b>E / limit</b>
            <span>{{ fmtDb(result.analysis.worst.levelDbuv) }} / {{ fmtDb(result.analysis.worst.limitDbuv) }}</span>
            <span class="unit">dBµV/m</span></div>
        </div>
        <p class="note" data-test="radiated-uncertainty" style="color: var(--amber)">
          SCREENING ESTIMATE, ±{{ result.uncertainty }} dB typical: real cables resonate and couple
          far off this worst-case short-radiator model. A pass with more than
          {{ result.uncertainty }} dB in hand is meaningful; anything closer — and every fail —
          belongs on an open-area test site or in a chamber. This is triage, not a measurement.</p>
      </div>
      <LogChart v-if="result" :series="series()" :ref-runs="refRuns()" :violations="violations()"
                violation-label="over limit" y-label="dBµV/m" data-test="radiated-chart" />
      <div v-else class="screen"><p class="note" style="padding: 2rem 1rem">Feed a CM-current
        spectrum (or the synthetic example) — the estimated E-field lands here against the
        CISPR 32 radiated limit.</p></div>
    </div>
  </div>
</template>
