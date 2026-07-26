<script setup>
import { onMounted, ref } from 'vue'
import SpectrumView from './components/SpectrumView.vue'
import FilterView from './components/FilterView.vue'
import ReceiverView from './components/ReceiverView.vue'
import LisnView from './components/LisnView.vue'
import { loadEngine } from './engine.js'
import { store } from './store.js'

const engineState = ref('loading')

onMounted(async () => {
  try {
    await loadEngine()
    engineState.value = 'ready'
  } catch (e) {
    engineState.value = 'fault'
    console.error('Hertz engine failed to load:', e)
  }
})

const modes = [
  { id: 'spectrum', label: 'SPECTRUM' },
  { id: 'filter', label: 'FILTER' },
  { id: 'receiver', label: 'RECEIVER' },
  { id: 'lisn', label: 'LISN' },
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
      <button v-for="m in modes" :key="m.id" class="mode-key" :class="{ active: store.mode === m.id }"
              :data-test="'mode-' + m.id" @click="store.mode = m.id">{{ m.label }}</button>
    </nav>

    <SpectrumView v-if="store.mode === 'spectrum'" />
    <FilterView v-else-if="store.mode === 'filter'" />
    <ReceiverView v-else-if="store.mode === 'receiver'" />
    <LisnView v-else />

    <p class="footnote">
      Hertz is an open-source pre-compliance instrument — estimates with engineering margins, not a
      certification. Accredited chamber testing remains mandatory. Part of the
      <a href="https://github.com/OpenConverters" rel="noopener">OpenConverters</a> bench alongside
      Kirchhoff (SPICE) and Kelvin (parts). MIT licensed.
    </p>
  </div>
</template>
