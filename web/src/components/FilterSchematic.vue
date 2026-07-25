<script setup>
// The filter schematic, drawn from the same component/net truth the CIAS brick
// exports (ciasFilter.js). Geometry is local per stage count — CIAS carries
// none — mirroring the Kirchhoff pattern. Components are clickable: selecting
// one drives the parts-recommendation panel.
import { computed } from 'vue'
import { filterComponents } from '../ciasFilter.js'

const props = defineProps({
  stages: { type: Number, required: true },
  labels: { type: Object, required: true },    // ref -> value label ("3.3 mH")
  bindings: { type: Object, required: true },  // ref -> {mpn} | undefined
  selected: { type: String, default: '' },
})
const emit = defineEmits(['select'])

const STAGE_W = 300
const yLine = 70
const yNeutral = 190
const yPe = 268

const width = computed(() => 150 + props.stages * STAGE_W + 90)
const components = computed(() => filterComponents(props.stages))

// per-stage x anchors
const xCmc = (s) => 120 + (s - 1) * STAGE_W
const xCx = (s) => xCmc(s) + 130
const xCy = (s) => xCmc(s) + 205

function hotspot(ref) {
  emit('select', ref)
}
const cls = (ref) => ({
  comp: true,
  'comp-selected': props.selected === ref,
  'comp-bound': !!props.bindings[ref]?.mpn,
})
</script>

<template>
  <div class="screen" style="padding: 0.6rem">
    <svg :viewBox="`0 0 ${width} 300`" style="width: 100%; height: auto; display: block"
         role="img" aria-label="Line filter schematic">
      <!-- rails -->
      <g stroke="var(--ink-dim)" stroke-width="1.6" fill="none">
        <line :x1="30" :y1="yLine" :x2="xCmc(1)" :y2="yLine" />
        <line :x1="30" :y1="yNeutral" :x2="xCmc(1)" :y2="yNeutral" />
        <template v-for="s in stages" :key="'rail' + s">
          <line :x1="xCmc(s) + 60" :y1="yLine" :x2="s === stages ? width - 40 : xCmc(s + 1)" :y2="yLine" />
          <line :x1="xCmc(s) + 60" :y1="yNeutral" :x2="s === stages ? width - 40 : xCmc(s + 1)" :y2="yNeutral" />
          <line :x1="xCy(s)" :y1="yLine" :x2="xCy(s)" :y2="yLine + 18" />
          <line :x1="xCy(s)" :y1="yNeutral" :x2="xCy(s)" :y2="yNeutral + 18" />
          <line :x1="xCy(s)" :y1="yLine + 46" :x2="xCy(s)" :y2="yPe" />
          <line :x1="xCy(s)" :y1="yNeutral + 46" :x2="xCy(s)" :y2="yPe" />
          <line :x1="xCx(s)" :y1="yLine" :x2="xCx(s)" :y2="yLine + 30" />
          <line :x1="xCx(s)" :y1="yNeutral" :x2="xCx(s)" :y2="yNeutral - 30" />
        </template>
        <line :x1="30" :y1="yPe" :x2="width - 40" :y2="yPe" />
      </g>

      <!-- ports -->
      <g font-family="var(--mono)" font-size="12" fill="var(--ink-dim)">
        <circle v-for="p in [[30, yLine], [30, yNeutral], [30, yPe], [width - 40, yLine], [width - 40, yNeutral]]"
                :key="p.join()" :cx="p[0]" :cy="p[1]" r="3.5" fill="var(--bg-deep)" stroke="var(--ink-dim)" stroke-width="1.6" />
        <text :x="14" :y="yLine + 4">L</text>
        <text :x="14" :y="yNeutral + 4">N</text>
        <text :x="10" :y="yPe + 4">PE</text>
        <text :x="width - 30" :y="yLine + 4">L'</text>
        <text :x="width - 30" :y="yNeutral + 4">N'</text>
      </g>

      <template v-for="s in stages" :key="'stage' + s">
        <!-- CMC: two coupled windings with core bars and polarity dots -->
        <g :class="cls(`CMC${s}`)" :data-test="`sch-CMC${s}`" @click="hotspot(`CMC${s}`)"
           @keydown.enter="hotspot(`CMC${s}`)" tabindex="0" role="button" :aria-label="`Select CMC${s}`">
          <rect :x="xCmc(s) - 8" :y="yLine - 22" width="76" height="240" fill="transparent" />
          <g stroke="currentColor" stroke-width="2" fill="none">
            <path :d="`M ${xCmc(s)} ${yLine} ` + [0, 1, 2, 3].map(i => `a 7.5 9 0 0 1 15 0`).join(' ')" />
            <path :d="`M ${xCmc(s)} ${yNeutral} ` + [0, 1, 2, 3].map(i => `a 7.5 9 0 0 0 15 0`).join(' ')" />
            <line :x1="xCmc(s) + 6" :y1="yLine + 22" :x2="xCmc(s) + 54" :y2="yLine + 22" />
            <line :x1="xCmc(s) + 6" :y1="yNeutral - 22" :x2="xCmc(s) + 54" :y2="yNeutral - 22" />
          </g>
          <circle :cx="xCmc(s) + 4" :cy="yLine - 14" r="2.6" fill="currentColor" />
          <circle :cx="xCmc(s) + 4" :cy="yNeutral - 14" r="2.6" fill="currentColor" />
          <text :x="xCmc(s) + 30" :y="yLine - 28" text-anchor="middle" font-family="var(--mono)" font-size="12"
                fill="currentColor">CMC{{ s }}</text>
          <text :x="xCmc(s) + 30" :y="(yLine + yNeutral) / 2 + 4" text-anchor="middle" font-family="var(--mono)"
                font-size="11" fill="var(--ink-dim)">{{ labels[`CMC${s}`] }}</text>
          <text v-if="bindings[`CMC${s}`]" :x="xCmc(s) + 30" :y="(yLine + yNeutral) / 2 + 20" text-anchor="middle"
                font-family="var(--mono)" font-size="10" fill="var(--amber, #ffb347)">{{ bindings[`CMC${s}`].mpn }}</text>
        </g>

        <!-- X capacitor between the lines -->
        <g :class="cls(`C_X${s}`)" :data-test="`sch-CX${s}`" @click="hotspot(`C_X${s}`)"
           @keydown.enter="hotspot(`C_X${s}`)" tabindex="0" role="button" :aria-label="`Select C_X${s}`">
          <rect :x="xCx(s) - 24" :y="yLine + 26" width="48" height="108" fill="transparent" />
          <g stroke="currentColor" stroke-width="2.4">
            <line :x1="xCx(s) - 12" :y1="yLine + 30" :x2="xCx(s) + 12" :y2="yLine + 30" />
            <line :x1="xCx(s) - 12" :y1="yLine + 38" :x2="xCx(s) + 12" :y2="yLine + 38" />
          </g>
          <line :x1="xCx(s)" :y1="yLine + 38" :x2="xCx(s)" :y2="yNeutral - 30" stroke="currentColor" stroke-width="1.6" />
          <text :x="xCx(s) + 18" :y="yLine + 40" font-family="var(--mono)" font-size="12" fill="currentColor">C_X{{ s }}</text>
          <text :x="xCx(s) + 18" :y="yLine + 55" font-family="var(--mono)" font-size="11" fill="var(--ink-dim)">{{ labels[`C_X${s}`] }}</text>
          <text v-if="bindings[`C_X${s}`]" :x="xCx(s) + 18" :y="yLine + 70" font-family="var(--mono)" font-size="10"
                fill="var(--amber, #ffb347)">{{ bindings[`C_X${s}`].mpn }}</text>
        </g>

        <!-- Y capacitors to PE (drawn once per line; bound as a pair) -->
        <g :class="cls(`C_YL${s}`)" :data-test="`sch-CY${s}`" @click="hotspot(`C_YL${s}`)"
           @keydown.enter="hotspot(`C_YL${s}`)" tabindex="0" role="button" :aria-label="`Select Y capacitors, stage ${s}`">
          <rect :x="xCy(s) - 20" :y="yLine + 12" width="40" height="44" fill="transparent" />
          <g stroke="currentColor" stroke-width="2.4">
            <line :x1="xCy(s) - 11" :y1="yLine + 18" :x2="xCy(s) + 11" :y2="yLine + 18" />
            <line :x1="xCy(s) - 11" :y1="yLine + 26" :x2="xCy(s) + 11" :y2="yLine + 26" />
          </g>
          <text :x="xCy(s) + 15" :y="yLine + 26" font-family="var(--mono)" font-size="12" fill="currentColor">C_Y{{ s }}</text>
          <text :x="xCy(s) + 15" :y="yLine + 41" font-family="var(--mono)" font-size="11" fill="var(--ink-dim)">2×{{ labels[`C_YL${s}`] }}</text>
          <text v-if="bindings[`C_YL${s}`]" :x="xCy(s) + 15" :y="yLine + 56" font-family="var(--mono)" font-size="10"
                fill="var(--amber, #ffb347)">{{ bindings[`C_YL${s}`].mpn }}</text>
        </g>
        <g :class="cls(`C_YN${s}`)" @click="hotspot(`C_YL${s}`)">
          <rect :x="xCy(s) - 20" :y="yNeutral + 12" width="40" height="44" fill="transparent" />
          <g stroke="currentColor" stroke-width="2.4">
            <line :x1="xCy(s) - 11" :y1="yNeutral + 18" :x2="xCy(s) + 11" :y2="yNeutral + 18" />
            <line :x1="xCy(s) - 11" :y1="yNeutral + 26" :x2="xCy(s) + 11" :y2="yNeutral + 26" />
          </g>
        </g>
      </template>
    </svg>
    <p class="note" style="padding: 0 0.5rem 0.3rem">Click a component to see catalog parts for it. Amber part numbers are bound; identical stages share bindings.</p>
  </div>
</template>

<style scoped>
.comp { color: var(--s-1); cursor: pointer; outline: none; }
.comp:hover { color: var(--phos); }
.comp:focus-visible { color: var(--phos); }
.comp-selected { color: var(--phos-hi); }
.comp-selected rect[fill="transparent"] { fill: rgba(63, 226, 127, 0.07); stroke: var(--phos-deep); }
.comp-bound { }
</style>
