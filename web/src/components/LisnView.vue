<script setup>
// LISN explorer: what the EUT actually sees, plus the SPICE model to take away.
import { onMounted, ref } from 'vue'
import LogChart from './LogChart.vue'
import { api } from '../engine.js'

const kind = ref('cispr16')
const data = ref(null)
const error = ref('')

async function refresh() {
  error.value = ''
  try {
    const engine = await api()
    const fMax = kind.value === 'cispr25' ? 108e6 : 30e6
    data.value = engine.lisnData(kind.value, 150e3, fMax)
  } catch (e) {
    error.value = e.message
    data.value = null
  }
}
onMounted(refresh)

const series = () => [{
  id: 'z', label: '|Z| seen by the EUT', color: 'var(--s-1)',
  points: data.value.frequenciesHz.map((f, i) => ({ f, v: data.value.impedanceOhm[i] })),
}]
</script>

<template>
  <div class="grid2">
    <div>
      <div class="panel">
        <p class="section-label">Artificial network</p>
        <label class="field"><span>Network</span>
          <select v-model="kind" @change="refresh">
            <option value="cispr16">CISPR 16 — 50 µH / 50 Ω (mains: CISPR 32, CISPR 11)</option>
            <option value="cispr25">CISPR 25 — 5 µH / 50 Ω (automotive DC)</option>
          </select>
        </label>
        <p class="note">
          Simplified single-line model: main inductor in parallel with the 0.1 µF coupling branch
          into the 50 Ω receiver. Valid over the measurement bands (150 kHz up); the CISPR band A
          low-frequency branch is not modeled.
        </p>
      </div>
      <div v-if="data" class="panel">
        <p class="section-label">SPICE subcircuit</p>
        <pre class="code" data-test="subckt">{{ data.subckt }}</pre>
        <button class="ghost" @click="navigator.clipboard.writeText(data.subckt)">Copy</button>
      </div>
      <div v-if="error" class="err">{{ error }}</div>
    </div>
    <div>
      <LogChart v-if="data" :series="series()" y-label="Ω" :height="340" />
    </div>
  </div>
</template>
