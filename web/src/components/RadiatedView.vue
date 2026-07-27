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
const margin = ref(6)         // buffer on the CM-attenuation target (dB)
const cmRef = ref(150)        // CM loop impedance (Ω) — genuinely uncertain (it IS the ±20 dB)
const maxTurns = ref(3)       // most cable passes the picker may use on one core
const trace = ref(null)       // {name, frequenciesHz, dbua}
const result = ref(null)      // {efield, uncertainty, analysis, limitRuns, mitigation}
const error = ref('')
const dragOver = ref(false)
const fileInput = ref(null)

const standardId = () => `cispr32_rad_${cls.value}_${distance.value}m`

// Illustrative cable-ferrite / clip-on CM-choke |Z| curves (magnitude, Ω) — a
// datasheet stand-in, ordered small-first so the picker returns the least part.
// Swap for a real vendor catalog. Each spans the radiated band (30 MHz–1 GHz).
const EXAMPLE_FERRITES = [
  { name: 'clip-on S', frequenciesHz: [10e6, 30e6, 100e6, 300e6, 1e9], zOhm: [30, 90, 180, 220, 200] },
  { name: 'clip-on M', frequenciesHz: [10e6, 30e6, 100e6, 300e6, 1e9], zOhm: [60, 170, 330, 400, 360] },
  { name: 'clip-on L', frequenciesHz: [10e6, 30e6, 100e6, 300e6, 1e9], zOhm: [120, 300, 560, 650, 600] },
]

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
    const mitigation = cableMitigation(engine, analysis)
    result.value = { efield: est.efieldDbuvm, uncertainty: est.modelUncertaintyDb, analysis, limitRuns, mitigation }
  } catch (e) {
    error.value = e.message
  }
}

// Turn the screen into a fix: the CM-CURRENT attenuation target (dB vs freq) over
// the covered radiated band, then the least cable ferrite that meets it. Returns
// null off-band, {needed:false} when the screen already passes, else {pick,...}.
function cableMitigation(engine, analysis) {
  const freqs = trace.value.frequenciesHz
  const idx = analysis.limitsDbuv.map((l, i) => (l != null ? i : -1)).filter((i) => i >= 0)
  if (!idx.length) return null
  const fc = idx.map((i) => freqs[i])
  const target = engine.radiatedCmTarget(fc, idx.map((i) => trace.value.dbua[i]),
    Number(cableM.value), Number(distance.value), idx.map((i) => analysis.limitsDbuv[i]), Number(margin.value))
  if (!target.needed) return { needed: false, target }
  let pick = null
  try { pick = engine.cableMitigation(fc, target.target, EXAMPLE_FERRITES, Number(cmRef.value), Number(maxTurns.value)) }
  catch (e) { pick = { error: e.message } }
  return { needed: true, target, pick }
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
        <div class="row" style="margin-top: 0.7rem">
          <label class="field"><span>CM loop impedance (Ω)</span>
            <input v-model.number="cmRef" type="number" min="1" step="1" data-test="cm-ref" @change="estimate" /></label>
          <label class="field"><span>Max cable turns</span>
            <input v-model.number="maxTurns" type="number" min="1" max="10" step="1" data-test="max-turns" @change="estimate" /></label>
        </div>
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
      <div v-if="result && result.mitigation" class="panel" style="margin-bottom: 1rem" data-test="mitigation">
        <p class="section-label">Cable common-mode mitigation</p>
        <template v-if="result.mitigation.needed">
          <p class="note">Because |E| ∝ I<sub>CM</sub>, the field is over the limit by exactly the
            common-mode <b>current</b> attenuation you must add — worst case
            <b>{{ fmtDb(result.mitigation.target.governingDb) }} dB at {{ fmtHz(result.mitigation.target.governingHz) }}</b>.
            Up here a mains filter's choke has self-resonated, so the fix is a cable ferrite / clip-on CM choke.</p>
          <div v-if="result.mitigation.pick.error" class="err">{{ result.mitigation.pick.error }}</div>
          <div v-else class="readout">
            <div class="cell"><b>Suggested part</b><span data-test="mitigation-part">{{ result.mitigation.pick.partName }}</span></div>
            <div class="cell"><b>Cable turns</b><span>{{ result.mitigation.pick.turns }}</span></div>
            <div class="cell"><b>Meets target</b>
              <span class="chip" :class="result.mitigation.pick.meetsTarget ? 'pass' : 'fail'" data-test="mitigation-meets">
                {{ result.mitigation.pick.meetsTarget ? 'YES' : 'NO — best effort' }}</span></div>
            <div class="cell"><b>Worst margin</b>
              <span :style="{ color: result.mitigation.pick.worstMarginDb < 0 ? 'var(--fault)' : 'var(--ok)' }">
                {{ fmtDb(result.mitigation.pick.worstMarginDb) }}</span><span class="unit">dB</span></div>
          </div>
          <p class="note">Candidate list is an illustrative stand-in; the CM loop impedance ({{ cmRef }} Ω)
            is genuinely uncertain — it IS the ±{{ result.uncertainty }} dB — so treat the pick as a
            starting part, not a spec.</p>
        </template>
        <p v-else class="note" data-test="mitigation-none">Screen passes — no cable common-mode mitigation indicated.</p>
      </div>
      <LogChart v-if="result" :series="series()" :ref-runs="refRuns()" :violations="violations()"
                violation-label="over limit" y-label="dBµV/m" data-test="radiated-chart" />
      <div v-else class="screen"><p class="note" style="padding: 2rem 1rem">Feed a CM-current
        spectrum (or the synthetic example) — the estimated E-field lands here against the
        CISPR 32 radiated limit.</p></div>
    </div>
  </div>
</template>
