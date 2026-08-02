<script setup>
import { onMounted, ref } from 'vue'
import SpectrumView from './components/SpectrumView.vue'
import FilterView from './components/FilterView.vue'
import ReceiverView from './components/ReceiverView.vue'
import RadiatedView from './components/RadiatedView.vue'
import LisnView from './components/LisnView.vue'
import { api, loadEngine } from './engine.js'
import { store } from './store.js'

const engineState = ref('loading')
const handoffError = ref('')

// #handoff=<base64 JSON>: a PREDICTED per-mode spectrum arriving from
// Faraday's conducted estimate (faraday.openconverters.com). The data rides
// in the URL fragment, which never reaches any server. It is judged against
// the limit exactly like a measured scan and seeds the filter designer as
// binding sets — with its stated estimate bands carried into the note.
async function readHandoffFragment() {
  const m = location.hash.match(/^#handoff=(.+)$/)
  if (!m) return
  try {
    const p = JSON.parse(decodeURIComponent(escape(atob(m[1]))))
    if (p.v !== 1 || !p.spectra?.dm || !p.spectra?.cm) {
      throw new Error('unrecognized handoff payload')
    }
    const engine = await api()
    const sets = { cm: [], dm: [] }
    const traces = []
    for (const mode of ['cm', 'dm']) {
      const pts = p.spectra[mode]
      const freqs = pts.map((q) => q[0])
      const levels = pts.map((q) => q[1])
      const analysis = engine.limitAnalysis('cispr32_class_b', 'quasi_peak', freqs, levels, 10)
      analysis.marginsDb.forEach((margin, k) => {
        if (margin !== null && margin < 10) sets[mode].push([freqs[k], 10 - margin])
      })
      traces.push({ name: `Faraday prediction (${mode.toUpperCase()})`, mode,
                    frequenciesHz: freqs, levelsDbuv: levels })
    }
    if (sets.cm.length === 0 && sets.dm.length === 0) {
      // the PREDICTION already clears the limit everywhere: no filter to
      // design — say so instead of opening an unseeded designer
      handoffError.value = 'the Faraday prediction already meets CISPR 32 B ' +
        'quasi-peak across the band (within its stated estimate bands) — no ' +
        'line filter is required. Verify with a LISN before shipping.'
      history.replaceState(null, '', location.pathname)
      return
    }
    store.handoff = {
      binding: sets,
      fSwHz: p.fSwHz,
      scan: { standardId: 'cispr32_class_b', detector: 'quasi_peak', traces,
              predicted: true, note: p.note },
    }
    store.mode = 'filter'
    history.replaceState(null, '', location.pathname)   // consume the fragment
  } catch (e) {
    handoffError.value = 'handoff from Faraday could not be read: ' +
      String(e.message || e)
  }
}

onMounted(async () => {
  try {
    await loadEngine()
    engineState.value = 'ready'
  } catch (e) {
    engineState.value = 'fault'
    console.error('Hertz engine failed to load:', e)
  }
  await readHandoffFragment()
})

// The nav IS the workflow: MEASURE (analyzer scan, scope capture, or CM current
// probe) → DESIGN (filter). The MEASURE tools are first-class destinations, not
// options buried in the designer's pane dropdown — each produces a requirement
// that hands off to the filter bench. LISN is the test-setup reference, in MEASURE.
const measureModes = [
  { id: 'spectrum', label: 'SPECTRUM' },
  { id: 'scope', label: 'SCOPE CAPTURE' },
  { id: 'probe', label: 'CM PROBE' },
  { id: 'lisn', label: 'TEST SETUP (LISN)' },
]
</script>

<template>
  <div class="wrap" :class="{ fullbench: store.mode === 'filter' }">
    <header class="bar">
      <span class="brand">HERTZ <small>EMI RECEIVER</small></span>
      <span class="tagline">conducted emissions · limit verdicts · line-filter design — entirely in your browser</span>
      <span class="spacer"></span>
      <span class="engine-led" :class="engineState" data-test="engine-led">
        <i></i>{{ engineState === 'ready' ? 'engine ready' : engineState === 'fault' ? 'engine failed to load' : 'loading engine…' }}
      </span>
    </header>

    <nav class="modes" aria-label="Instrument mode">
      <span class="mode-group">MEASURE</span>
      <button v-for="m in measureModes" :key="m.id" class="mode-key" :class="{ active: store.mode === m.id }"
              :data-test="'mode-' + m.id" @click="store.mode = m.id">{{ m.label }}</button>
      <span class="mode-group">▸ DESIGN</span>
      <button class="mode-key" :class="{ active: store.mode === 'filter' }"
              data-test="mode-filter" @click="store.mode = 'filter'">FILTER</button>
    </nav>

    <p v-if="handoffError" class="footnote" style="color: var(--fault, #ff8a8a)"
       data-test="handoff-error">{{ handoffError }}</p>
    <SpectrumView v-if="store.mode === 'spectrum'" />
    <ReceiverView v-else-if="store.mode === 'scope'" />
    <RadiatedView v-else-if="store.mode === 'probe'" />
    <LisnView v-else-if="store.mode === 'lisn'" />
    <FilterView v-else />

    <p class="footnote">
      Hertz is an open-source pre-compliance instrument — estimates with engineering margins, not a
      certification. Accredited chamber testing remains mandatory. Part of the
      <a href="https://github.com/OpenConverters" rel="noopener">OpenConverters</a> bench alongside
      Kirchhoff (SPICE) and Kelvin (parts). MIT licensed.
    </p>
  </div>
</template>
