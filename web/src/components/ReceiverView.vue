<script setup>
// CISPR 16-1-1 receiver emulation: time-domain samples in, peak / quasi-peak /
// average spectra out — the module no other free tool ships.
import { ref } from 'vue'
import LogChart from './LogChart.vue'
import { api, STANDARDS } from '../engine.js'
import { demoWaveform } from '../demo.js'
import { store } from '../store.js'
import { fmtHz, fmtDb } from '../format.js'

const BAND_RANGE = { A: [9e3, 150e3], B: [150e3, 30e6], C: [30e6, 300e6] }

const band = ref('B')
const lastInput = ref(null)     // retained so a band change re-MEASURES, never re-labels
const reading = ref(null)
const modal = ref(null)         // dual-channel result: {cm: reading, dm: reading}
const targetStandard = ref('cispr32_class_b')
const targets = ref(null)       // {aReqCm, aReqDm, lineWorst} — judged on the LINE voltages
const channels = ref(null)      // retained raw channels for line-spectrum judgement
const sourceName = ref('')
const busy = ref(false)
const error = ref('')
const dragOver = ref(false)
const fileInput = ref(null)

async function run(samples, fsHz, name, secondChannel = null) {
  lastInput.value = { samples, fsHz, name, secondChannel }
  busy.value = true
  error.value = ''
  reading.value = null
  modal.value = null
  targets.value = null
  sourceName.value = name
  await new Promise((resolve) => setTimeout(resolve, 30)) // let the busy state paint
  try {
    const engine = await api()
    if (secondChannel) {
      // two synchronized line channels: exact CM/DM separation, then a
      // receiver run per mode — the only honest way to split the modes
      const separated = engine.separateTraces(samples, secondChannel)
      channels.value = { v1: samples, v2: secondChannel, fsHz }
      modal.value = {
        cm: engine.measureWaveform(separated.commonMode, fsHz, band.value),
        dm: engine.measureWaveform(separated.differentialMode, fsHz, band.value),
      }
    } else {
      reading.value = engine.measureWaveform(samples, fsHz, band.value)
    }
  } catch (e) {
    error.value = e.message
  } finally {
    busy.value = false
  }
}

async function computeTargets() {
  error.value = ''
  try {
    const engine = await api()
    const inBand = (r) => {
      const freqs = [], levels = []
      for (let i = 0; i < r.frequenciesHz.length; i += 1) {
        const f = r.frequenciesHz[i]
        if (f >= BAND_RANGE[band.value][0] && f <= BAND_RANGE[band.value][1] && Number.isFinite(r.quasiPeakDbuv[i])) {
          freqs.push(f); levels.push(r.quasiPeakDbuv[i])
        }
      }
      return { freqs, levels }
    }
    // What CISPR limits is each LINE voltage V_L, V_N — judge those first.
    const lineL = engine.measureWaveform(channels.value.v1, channels.value.fsHz, band.value)
    const lineN = engine.measureWaveform(channels.value.v2, channels.value.fsHz, band.value)
    const judge = (r, marginBufferDb) => {
      const { freqs, levels } = inBand(r)
      if (!freqs.length) return { required: 0, worst: null, lowestBinding: null }
      const analysis = engine.limitAnalysis(targetStandard.value, 'quasi_peak', freqs, levels, marginBufferDb)
      // lowest frequency where the requirement binds: sizing a low-pass there
      // with the MAX requirement guarantees every higher frequency too
      // every binding point (margin < buffer) with ITS OWN local requirement —
      // the designer reduces the whole set with the min-f_co rule; pairing one
      // frequency with a requirement measured elsewhere over- or under-designs
      const binding = []
      for (let i = 0; i < freqs.length; i += 1) {
        const margin = analysis.marginsDb[i]
        if (margin !== null && margin < marginBufferDb) {
          binding.push([freqs[i], marginBufferDb - margin])
        }
      }
      return { required: Math.max(0, Math.ceil(analysis.requiredAttenuationDb)),
               worst: analysis.worst, binding }
    }
    const jL = judge(lineL, 10), jN = judge(lineN, 10)
    // Per-mode targets carry a 6 dB in-phase summation allowance on top of the
    // 10 dB margin: at a frequency where CM and DM are comparable, each mode
    // must sit ~6 dB below the limit for their SUM on one line to meet it.
    const jCm = judge(modal.value.cm, 16), jDm = judge(modal.value.dm, 16)
    targets.value = {
      aReqCm: jCm.required,
      aReqDm: jDm.required,
      lineL: jL.worst, lineN: jN.worst,
      lineRequired: Math.max(jL.required, jN.required),
      binding: { cm: jCm.binding, dm: jDm.binding },
    }
    // never hand over per-mode targets that jointly under-shoot the line requirement
    if (targets.value.aReqCm + targets.value.aReqDm < targets.value.lineRequired) {
      targets.value.aReqCm = Math.max(targets.value.aReqCm, targets.value.lineRequired)
      targets.value.aReqDm = Math.max(targets.value.aReqDm, targets.value.lineRequired)
    }
  } catch (e) {
    error.value = 'targets: ' + e.message
  }
}

function designFromModes() {
  // Hand over the full binding sets: the designer reduces them with
  // f_co = min_i(f_i / 10^(A_i/(40 n))) for ITS stage count — the only
  // reduction that guarantees every binding frequency without pairing one
  // frequency with a requirement measured at another.
  const inBand = (r) => {
    const freqs = [], levels = []
    for (let i = 0; i < r.frequenciesHz.length; i += 1) {
      const f = r.frequenciesHz[i]
      if (f >= BAND_RANGE[band.value][0] && f <= BAND_RANGE[band.value][1] && Number.isFinite(r.quasiPeakDbuv[i])) {
        freqs.push(f); levels.push(r.quasiPeakDbuv[i])
      }
    }
    return { freqs, levels }
  }
  const cm = inBand(modal.value.cm)
  const dm = inBand(modal.value.dm)
  store.handoff = { binding: targets.value.binding,
                    aReqCmDb: targets.value.aReqCm, aReqDmDb: targets.value.aReqDm,
                    scan: { standardId: targetStandard.value, detector: 'quasi_peak', traces: [
                      { name: 'CM (receiver QP)', mode: 'cm', frequenciesHz: cm.freqs, levelsDbuv: cm.levels },
                      { name: 'DM (receiver QP)', mode: 'dm', frequenciesHz: dm.freqs, levelsDbuv: dm.levels },
                    ] } }
  store.mode = 'filter'
}

// The band sets the RBW and the detector time constants — switching it must
// re-run the whole receiver chain on the retained samples. Re-clipping the old
// result to the new axis would display 200 Hz-RBW numbers under a 9 kHz label
// (29 dB apart on pulsed noise), and a record too short for the new band must
// fail loudly instead of keeping the stale curves.
function remeasure() {
  if (!lastInput.value) return
  const { samples, fsHz, name, secondChannel } = lastInput.value
  run(samples, fsHz, name, secondChannel)
}

function runDemo() {
  const { samples, fsHz } = demoWaveform()
  run(samples, fsHz, 'demo: 300 kHz tone, 100 Hz PRF, 10 % duty')
}

async function ingest(files) {
  const file = files[0]
  if (!file) return
  error.value = ''
  try {
    const text = await file.text()
    const rows = []
    for (const line of text.split(/\r?\n/)) {
      const nums = line.split(/[,;\t]/).map(Number).filter((x) => Number.isFinite(x))
      if (nums.length >= 2) rows.push(nums)
    }
    if (rows.length < 100) throw new Error('expected a time,voltage CSV (or time,v_line,v_neutral for CM/DM) with at least 100 rows')
    const dt = []
    for (let i = 1; i < Math.min(rows.length, 200); i += 1) dt.push(rows[i][0] - rows[i - 1][0])
    dt.sort((a, b) => a - b)
    const median = dt[Math.floor(dt.length / 2)]
    if (median <= 0 || dt[dt.length - 1] > median * 1.01 || dt[0] < median * 0.99) {
      throw new Error('time column is not uniformly sampled — export a fixed-rate capture')
    }
    const samples = Float64Array.from(rows, (r) => r[1])
    const second = rows[0].length >= 3 ? Float64Array.from(rows, (r) => r[2]) : null
    run(samples, 1 / median, file.name + (second ? ' (2-channel CM/DM)' : ''), second)
  } catch (e) {
    error.value = e.message
  }
}

const clampToBand = (p) => p.f >= BAND_RANGE[band.value][0] && p.f <= BAND_RANGE[band.value][1] && Number.isFinite(p.v)
const seriesFor = (r) => [
  { id: 'qp', label: 'quasi-peak', color: 'var(--s-1)',
    points: r.frequenciesHz.map((f, i) => ({ f, v: r.quasiPeakDbuv[i] })).filter(clampToBand) },
  { id: 'avg', label: 'average', color: 'var(--s-2)',
    points: r.frequenciesHz.map((f, i) => ({ f, v: r.averageDbuv[i] })).filter(clampToBand) },
  { id: 'pk', label: 'peak', color: 'var(--s-3)', dash: '2 3',
    points: r.frequenciesHz.map((f, i) => ({ f, v: r.peakDbuv[i] })).filter(clampToBand) },
]
const modalSeries = () => [
  { id: 'cmqp', label: 'CM quasi-peak', color: 'var(--s-1)',
    points: modal.value.cm.frequenciesHz.map((f, i) => ({ f, v: modal.value.cm.quasiPeakDbuv[i] })).filter(clampToBand) },
  { id: 'dmqp', label: 'DM quasi-peak', color: 'var(--s-2)',
    points: modal.value.dm.frequenciesHz.map((f, i) => ({ f, v: modal.value.dm.quasiPeakDbuv[i] })).filter(clampToBand) },
  { id: 'cmavg', label: 'CM average', color: 'var(--s-1)', dash: '2 3',
    points: modal.value.cm.frequenciesHz.map((f, i) => ({ f, v: modal.value.cm.averageDbuv[i] })).filter(clampToBand) },
  { id: 'dmavg', label: 'DM average', color: 'var(--s-2)', dash: '2 3',
    points: modal.value.dm.frequenciesHz.map((f, i) => ({ f, v: modal.value.dm.averageDbuv[i] })).filter(clampToBand) },
]
</script>

<template>
  <div class="grid2">
    <div>
      <div class="panel">
        <p class="section-label">Waveform input</p>
        <p class="note" style="margin-bottom: 0.6rem">Why this screen: it turns a plain oscilloscope
          capture into what a CISPR 16-1-1 EMI receiver would display (peak / quasi-peak / average)
          — pre-compliance without owning a receiver. A two-channel capture additionally separates
          common and differential mode and hands per-mode targets straight to the filter designer.</p>
        <div class="drop" :class="{ over: dragOver }" role="button" tabindex="0"
             @click="fileInput.click()" @keydown.enter="fileInput.click()"
             @dragover.prevent="dragOver = true" @dragleave="dragOver = false"
             @drop.prevent="dragOver = false; ingest([...$event.dataTransfer.files])">
          Drop a time,voltage CSV (scope export, fixed sample rate) — or
          time,v<sub>line</sub>,v<sub>neutral</sub> from two synchronized channels for exact CM/DM separation.
        </div>
        <input ref="fileInput" type="file" accept=".csv,.txt" hidden
               @change="ingest([...$event.target.files]); $event.target.value = ''" />
        <label class="field" style="margin-top: 0.7rem"><span>CISPR band (RBW + detector time constants)</span>
          <select v-model="band" data-test="band" @change="remeasure">
            <option value="A">A — 9–150 kHz (200 Hz RBW)</option>
            <option value="B">B — 150 kHz–30 MHz (9 kHz RBW)</option>
            <option value="C">C — 30–300 MHz (120 kHz RBW)</option>
          </select>
        </label>
        <button class="ghost" data-test="run-demo" :disabled="busy" @click="runDemo">Run demo signal</button>
        <p class="note" style="margin-top: 0.7rem">
          The emulation follows CISPR 16-1-1: Gaussian-RBW envelope, quasi-peak charge/discharge,
          critically damped meter. A CW tone reads the same on all three detectors; pulsed noise
          separates them — that separation is what the quasi-peak detector exists to score.
        </p>
      </div>
      <div v-if="error" class="err" data-test="error">{{ error }}</div>
    </div>

    <div>
      <div v-if="busy" class="panel"><p class="note">Measuring… the receiver chains run over every envelope sample.</p></div>
      <template v-else-if="modal">
        <LogChart :series="modalSeries()" y-label="dBµV" :clamp-range-db="110" data-test="receiver-chart" />
        <div class="panel" style="margin-top: 1rem">
          <p class="section-label">Design targets from the separated modes</p>
          <div class="row">
            <label class="field"><span>Limit</span>
              <select v-model="targetStandard">
                <option v-for="s in STANDARDS" :key="s.id" :value="s.id">{{ s.name }}</option>
              </select></label>
            <button class="ghost" data-test="compute-targets" @click="computeTargets" style="align-self: end">Compute CM/DM targets</button>
          </div>
          <div v-if="targets" class="readout">
            <div class="cell"><b>Line L worst (the limited quantity)</b>
              <span>{{ targets.lineL ? fmtDb(targets.lineL.levelDbuv) : '—' }}</span>
              <span class="unit">{{ targets.lineL ? 'dBµV vs ' + fmtDb(targets.lineL.limitDbuv) + ' @ ' + fmtHz(targets.lineL.frequencyHz) : 'silent' }}</span></div>
            <div class="cell"><b>Line N worst</b>
              <span>{{ targets.lineN ? fmtDb(targets.lineN.levelDbuv) : '—' }}</span>
              <span class="unit">{{ targets.lineN ? 'dBµV vs ' + fmtDb(targets.lineN.limitDbuv) : 'silent' }}</span></div>
            <div class="cell"><b>Required CM attenuation</b><span data-test="target-cm">{{ fmtDb(targets.aReqCm, 0) }}</span><span class="unit">dB</span></div>
            <div class="cell"><b>Required DM attenuation</b><span>{{ fmtDb(targets.aReqDm, 0) }}</span><span class="unit">dB</span></div>
          </div>
          <p v-if="targets" class="note">Targets are judged on the reconstructed LINE voltages
            (what the standard limits), with a 6 dB in-phase mode-summation allowance folded into
            each per-mode requirement, plus the 10 dB engineering margin.</p>
          <button v-if="targets && (targets.binding.cm.length || targets.binding.dm.length)"
                  class="act" data-test="design-from-modes" style="margin-top: 0.6rem" @click="designFromModes">Design filter for these modes →</button>
          <p v-else-if="targets" class="note" data-test="no-binding-note">
            Nothing binds: every point in both modes clears the limit with the full buffer in
            hand — this measurement needs no filter, so there is nothing to hand to the designer.</p>
        </div>
      </template>
      <LogChart v-else-if="reading" :series="seriesFor(reading)" y-label="dBµV" :clamp-range-db="110" data-test="receiver-chart" />
      <div v-else class="screen"><p class="note" style="padding: 2rem 1rem">Feed a waveform or run the demo — the three detector traces land here.</p></div>
      <p v-if="reading" class="note" style="margin-top: 0.5rem">{{ sourceName }} — band {{ band }}</p>
    </div>
  </div>
</template>
