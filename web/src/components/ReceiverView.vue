<script setup>
// CISPR 16-1-1 receiver emulation: time-domain samples in, peak / quasi-peak /
// average spectra out — the module no other free tool ships.
import { ref } from 'vue'
import LogChart from './LogChart.vue'
import { api } from '../engine.js'
import { demoWaveform } from '../demo.js'
import { fmtHz } from '../format.js'

const band = ref('B')
const reading = ref(null)
const sourceName = ref('')
const busy = ref(false)
const error = ref('')
const dragOver = ref(false)
const fileInput = ref(null)

async function run(samples, fsHz, name) {
  busy.value = true
  error.value = ''
  reading.value = null
  sourceName.value = name
  await new Promise((resolve) => setTimeout(resolve, 30)) // let the busy state paint
  try {
    const engine = await api()
    reading.value = engine.measureWaveform(samples, fsHz, band.value)
  } catch (e) {
    error.value = e.message
  } finally {
    busy.value = false
  }
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
      const nums = line.split(/[,;\t]/).map(Number).filter((x) => isFinite(x))
      if (nums.length >= 2) rows.push(nums)
    }
    if (rows.length < 100) throw new Error('expected a time,voltage CSV with at least 100 rows')
    const dt = []
    for (let i = 1; i < Math.min(rows.length, 200); i += 1) dt.push(rows[i][0] - rows[i - 1][0])
    dt.sort((a, b) => a - b)
    const median = dt[Math.floor(dt.length / 2)]
    if (median <= 0 || dt[dt.length - 1] > median * 1.01 || dt[0] < median * 0.99) {
      throw new Error('time column is not uniformly sampled — export a fixed-rate capture')
    }
    const samples = Float64Array.from(rows, (r) => r[1])
    run(samples, 1 / median, file.name)
  } catch (e) {
    error.value = e.message
  }
}

const seriesFor = (r) => [
  { id: 'qp', label: 'quasi-peak', color: 'var(--s-1)',
    points: r.frequenciesHz.map((f, i) => ({ f, v: r.quasiPeakDbuv[i] })).filter((p) => p.f > 0 && isFinite(p.v)) },
  { id: 'avg', label: 'average', color: 'var(--s-2)',
    points: r.frequenciesHz.map((f, i) => ({ f, v: r.averageDbuv[i] })).filter((p) => p.f > 0 && isFinite(p.v)) },
  { id: 'pk', label: 'peak', color: 'var(--s-3)', dash: '2 3',
    points: r.frequenciesHz.map((f, i) => ({ f, v: r.peakDbuv[i] })).filter((p) => p.f > 0 && isFinite(p.v)) },
]
</script>

<template>
  <div class="grid2">
    <div>
      <div class="panel">
        <p class="section-label">Waveform input</p>
        <div class="drop" :class="{ over: dragOver }" role="button" tabindex="0"
             @click="fileInput.click()" @keydown.enter="fileInput.click()"
             @dragover.prevent="dragOver = true" @dragleave="dragOver = false"
             @drop.prevent="dragOver = false; ingest([...$event.dataTransfer.files])">
          Drop a time,voltage CSV (scope export, fixed sample rate).
        </div>
        <input ref="fileInput" type="file" accept=".csv,.txt" hidden
               @change="ingest([...$event.target.files]); $event.target.value = ''" />
        <label class="field" style="margin-top: 0.7rem"><span>CISPR band (RBW + detector time constants)</span>
          <select v-model="band">
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
      <LogChart v-else-if="reading" :series="seriesFor(reading)" y-label="dBµV" data-test="receiver-chart" />
      <div v-else class="screen"><p class="note" style="padding: 2rem 1rem">Feed a waveform or run the demo — the three detector traces land here.</p></div>
      <p v-if="reading" class="note" style="margin-top: 0.5rem">{{ sourceName }} — band {{ band }}</p>
    </div>
  </div>
</template>
