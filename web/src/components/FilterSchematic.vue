<script setup>
// The filter schematic, drawn from the same component/net truth the CIAS brick
// exports (ciasFilter.js). Geometry is local per stage count — CIAS carries
// none — mirroring the Kirchhoff pattern. Components are clickable: selecting
// one drives the parts-recommendation pane.
//
// Symbol conventions (round-16 user feedback): the CM choke is ONE compact
// coupled component — both windings jogged toward a shared two-bar core,
// polarity dots on the same (left) end; capacitors sit centered in their span;
// every electrical junction carries a dot; the L-side Y capacitor HOPS the
// neutral rail (no-connection arc) instead of ambiguously crossing it.
import { computed } from 'vue'
import { filterComponents, filterNets } from '../ciasFilter.js'

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
const yWindL = 110      // L winding, jogged down toward the core
const yWindN = 150      // N winding, jogged up toward the core
const yCore1 = 126
const yCore2 = 134
const yCapMid = (yLine + yNeutral) / 2   // X-cap plates centered in the span
const yCyPlates = 214                    // both Y caps side by side between N and PE

const svgWidth = (stages) => 150 + stages * STAGE_W + 90
const width = computed(() => svgWidth(props.stages))
const components = computed(() => filterComponents(props.stages))

// ── KH-style render verification ─────────────────────────────────────────────
// Values and components come from the CIAS truth (ciasFilter.js); only the
// GEOMETRY is local. To make drift impossible instead of merely unlikely, the
// drawn geometry's connectivity is EXTRACTED (pins + ports unioned along the
// rail segments actually drawn) and compared against filterNets() on every
// render — a mismatch shows an error instead of a lying schematic.
const railSegments = (stages) => {
  const segs = [
    [[30, yLine], [xCmc(1), yLine]],
    [[30, yNeutral], [xCmc(1), yNeutral]],
    [[30, yPe], [svgWidth(stages) - 40, yPe]],
  ]
  for (let s = 1; s <= stages; s += 1) {
    const endX = s === stages ? svgWidth(stages) - 40 : xCmc(s + 1)
    segs.push([[xCmc(s) + 60, yLine], [endX, yLine]])
    segs.push([[xCmc(s) + 60, yNeutral], [endX, yNeutral]])
  }
  return segs
}
const drawnEndpoints = (stages) => {
  const pts = [
    { key: 'port:line_in', x: 30, y: yLine },
    { key: 'port:neutral_in', x: 30, y: yNeutral },
    { key: 'port:pe', x: 30, y: yPe },
    { key: 'port:line_out', x: svgWidth(stages) - 40, y: yLine },
    { key: 'port:neutral_out', x: svgWidth(stages) - 40, y: yNeutral },
  ]
  for (let s = 1; s <= stages; s += 1) {
    pts.push(
      { key: `CMC${s}|P1`, x: xCmc(s), y: yLine }, { key: `CMC${s}|P2`, x: xCmc(s) + 60, y: yLine },
      { key: `CMC${s}|S1`, x: xCmc(s), y: yNeutral }, { key: `CMC${s}|S2`, x: xCmc(s) + 60, y: yNeutral },
      { key: `C_X${s}|1`, x: xCx(s), y: yLine }, { key: `C_X${s}|2`, x: xCx(s), y: yNeutral },
      { key: `C_YL${s}|1`, x: xCyL(s), y: yLine }, { key: `C_YL${s}|2`, x: xCyL(s), y: yPe },
      { key: `C_YN${s}|1`, x: xCyN(s), y: yNeutral }, { key: `C_YN${s}|2`, x: xCyN(s), y: yPe },
    )
  }
  return pts
}
const schError = computed(() => {
  const stages = props.stages
  const pts = drawnEndpoints(stages)
  const segs = railSegments(stages)
  // union endpoints that lie on the same drawn rail segment
  const parent = pts.map((_, i) => i)
  const find = (i) => (parent[i] === i ? i : (parent[i] = find(parent[i])))
  const onSeg = (p, [[x1, y1], [x2, y2]]) => p.y === y1 && p.y === y2 &&
    p.x >= Math.min(x1, x2) && p.x <= Math.max(x1, x2)
  for (const seg of segs) {
    let first = -1
    pts.forEach((p, i) => {
      if (!onSeg(p, seg)) return
      if (first === -1) first = i
      else parent[find(i)] = find(first)
    })
  }
  const groups = new Map()
  pts.forEach((p, i) => {
    const root = find(i)
    if (!groups.has(root)) groups.set(root, [])
    groups.get(root).push(p.key)
  })
  const canonical = (sets) => sets.map((set) => [...set].sort().join(',')).sort().join(';')
  const drawn = canonical([...groups.values()])
  const truth = canonical(filterNets(stages).map((net) => net.endpoints.map(
    (e) => e.port ? `port:${e.port}` : `${e.component}|${e.pin}`)))
  return drawn === truth ? null
    : `schematic geometry ≠ CIAS netlist for ${stages} stage(s) — drawing refused`
})

// per-stage x anchors
const xCmc = (s) => 120 + (s - 1) * STAGE_W
const xCx = (s) => xCmc(s) + 130
const xCyL = (s) => xCmc(s) + 195
const xCyN = (s) => xCyL(s) + 26

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
  <div class="screen sch-fill">
    <div v-if="schError" class="err" data-test="sch-error" style="align-self: center; margin: auto">{{ schError }}</div>
    <svg v-else :viewBox="`0 0 ${width} 300`" style="width: 100%; height: 100%; display: block"
         preserveAspectRatio="xMidYMid meet" role="img" aria-label="Line filter schematic">
      <!-- rails -->
      <g stroke="var(--ink-dim)" stroke-width="1.6" fill="none">
        <line :x1="30" :y1="yLine" :x2="xCmc(1)" :y2="yLine" />
        <line :x1="30" :y1="yNeutral" :x2="xCmc(1)" :y2="yNeutral" />
        <template v-for="s in stages" :key="'rail' + s">
          <line :x1="xCmc(s) + 60" :y1="yLine" :x2="s === stages ? width - 40 : xCmc(s + 1)" :y2="yLine" />
          <line :x1="xCmc(s) + 60" :y1="yNeutral" :x2="s === stages ? width - 40 : xCmc(s + 1)" :y2="yNeutral" />
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
        <!-- CMC: one compact coupled choke — windings jogged toward the shared
             two-bar core, humps facing the core, dots on the same (left) end -->
        <g :class="cls(`CMC${s}`)" :data-test="`sch-CMC${s}`" @click="hotspot(`CMC${s}`)"
           @keydown.enter="hotspot(`CMC${s}`)" tabindex="0" role="button" :aria-label="`Select CMC${s}`">
          <rect :x="xCmc(s) - 8" :y="yLine - 16" width="76" :height="yNeutral - yLine + 32" fill="transparent" />
          <g stroke="currentColor" stroke-width="2" fill="none">
            <!-- jogs from the rails into the symbol -->
            <line :x1="xCmc(s)" :y1="yLine" :x2="xCmc(s)" :y2="yWindL" />
            <line :x1="xCmc(s) + 60" :y1="yWindL" :x2="xCmc(s) + 60" :y2="yLine" />
            <line :x1="xCmc(s)" :y1="yNeutral" :x2="xCmc(s)" :y2="yWindN" />
            <line :x1="xCmc(s) + 60" :y1="yWindN" :x2="xCmc(s) + 60" :y2="yNeutral" />
            <!-- windings: humps face the core -->
            <path :d="`M ${xCmc(s)} ${yWindL} ` + [0, 1, 2, 3].map(() => `a 7.5 9 0 0 0 15 0`).join(' ')" />
            <path :d="`M ${xCmc(s)} ${yWindN} ` + [0, 1, 2, 3].map(() => `a 7.5 9 0 0 1 15 0`).join(' ')" />
            <!-- shared core -->
            <line :x1="xCmc(s) + 4" :y1="yCore1" :x2="xCmc(s) + 56" :y2="yCore1" />
            <line :x1="xCmc(s) + 4" :y1="yCore2" :x2="xCmc(s) + 56" :y2="yCore2" />
          </g>
          <!-- same-side polarity dots -->
          <circle :cx="xCmc(s) + 5" :cy="yWindL - 7" r="2.6" fill="currentColor" />
          <circle :cx="xCmc(s) + 5" :cy="yWindN + 7" r="2.6" fill="currentColor" />
          <text :x="xCmc(s) + 30" :y="yLine - 4" text-anchor="middle" font-family="var(--mono)" font-size="12"
                fill="currentColor">CMC{{ s }}</text>
          <text :x="xCmc(s) + 66" :y="yCore1 + 6" font-family="var(--mono)"
                font-size="11" fill="var(--ink-dim)">{{ labels[`CMC${s}`] }}</text>
          <text v-if="bindings[`CMC${s}`]" :x="xCmc(s) + 30" :y="yNeutral + 16" text-anchor="middle"
                font-family="var(--mono)" font-size="10" fill="var(--amber, #ffb347)">{{ bindings[`CMC${s}`].mpn }}</text>
        </g>

        <!-- X capacitor between the lines, plates centered in the span -->
        <g :class="cls(`C_X${s}`)" :data-test="`sch-CX${s}`" @click="hotspot(`C_X${s}`)"
           @keydown.enter="hotspot(`C_X${s}`)" tabindex="0" role="button" :aria-label="`Select C_X${s}`">
          <rect :x="xCx(s) - 20" :y="yLine + 4" width="40" :height="yNeutral - yLine - 8" fill="transparent" />
          <g stroke="currentColor" stroke-width="1.6">
            <line :x1="xCx(s)" :y1="yLine" :x2="xCx(s)" :y2="yCapMid - 4" />
            <line :x1="xCx(s)" :y1="yCapMid + 4" :x2="xCx(s)" :y2="yNeutral" />
          </g>
          <g stroke="currentColor" stroke-width="2.4">
            <line :x1="xCx(s) - 12" :y1="yCapMid - 4" :x2="xCx(s) + 12" :y2="yCapMid - 4" />
            <line :x1="xCx(s) - 12" :y1="yCapMid + 4" :x2="xCx(s) + 12" :y2="yCapMid + 4" />
          </g>
          <!-- junction dots on both rails -->
          <circle :cx="xCx(s)" :cy="yLine" r="2.6" fill="currentColor" />
          <circle :cx="xCx(s)" :cy="yNeutral" r="2.6" fill="currentColor" />
          <text :x="xCx(s) + 17" :y="yCapMid - 6" font-family="var(--mono)" font-size="12" fill="currentColor">C_X{{ s }}</text>
          <text :x="xCx(s) + 17" :y="yCapMid + 9" font-family="var(--mono)" font-size="11" fill="var(--ink-dim)">{{ labels[`C_X${s}`] }}</text>
          <text v-if="bindings[`C_X${s}`]" :x="xCx(s) + 17" :y="yCapMid + 23" font-family="var(--mono)" font-size="10"
                fill="var(--amber, #ffb347)">{{ bindings[`C_X${s}`].mpn }}</text>
        </g>

        <!-- Y capacitors: both between N and PE, side by side. The L-side one
             HOPS the neutral rail (no-connection arc). Bound as a pair. -->
        <g :class="cls(`C_YL${s}`)" :data-test="`sch-CY${s}`" @click="hotspot(`C_YL${s}`)"
           @keydown.enter="hotspot(`C_YL${s}`)" tabindex="0" role="button" :aria-label="`Select Y capacitors, stage ${s}`">
          <rect :x="xCyL(s) - 16" :y="yLine + 4" width="58" :height="yPe - yLine - 8" fill="transparent" />
          <g stroke="currentColor" stroke-width="1.6" fill="none">
            <!-- L-side: down from L, hop over N, into the cap, on to PE -->
            <line :x1="xCyL(s)" :y1="yLine" :x2="xCyL(s)" :y2="yNeutral - 7" />
            <path :d="`M ${xCyL(s)} ${yNeutral - 7} a 7 7 0 0 1 0 14`" />
            <line :x1="xCyL(s)" :y1="yNeutral + 7" :x2="xCyL(s)" :y2="yCyPlates - 4" />
            <line :x1="xCyL(s)" :y1="yCyPlates + 4" :x2="xCyL(s)" :y2="yPe" />
            <!-- N-side: straight down from N -->
            <line :x1="xCyN(s)" :y1="yNeutral" :x2="xCyN(s)" :y2="yCyPlates - 4" />
            <line :x1="xCyN(s)" :y1="yCyPlates + 4" :x2="xCyN(s)" :y2="yPe" />
          </g>
          <g stroke="currentColor" stroke-width="2.4">
            <line :x1="xCyL(s) - 10" :y1="yCyPlates - 4" :x2="xCyL(s) + 10" :y2="yCyPlates - 4" />
            <line :x1="xCyL(s) - 10" :y1="yCyPlates + 4" :x2="xCyL(s) + 10" :y2="yCyPlates + 4" />
            <line :x1="xCyN(s) - 10" :y1="yCyPlates - 4" :x2="xCyN(s) + 10" :y2="yCyPlates - 4" />
            <line :x1="xCyN(s) - 10" :y1="yCyPlates + 4" :x2="xCyN(s) + 10" :y2="yCyPlates + 4" />
          </g>
          <!-- junction dots at every real connection -->
          <circle :cx="xCyL(s)" :cy="yLine" r="2.6" fill="currentColor" />
          <circle :cx="xCyN(s)" :cy="yNeutral" r="2.6" fill="currentColor" />
          <circle :cx="xCyL(s)" :cy="yPe" r="2.6" fill="currentColor" />
          <circle :cx="xCyN(s)" :cy="yPe" r="2.6" fill="currentColor" />
          <text :x="xCyN(s) + 15" :y="yCyPlates - 6" font-family="var(--mono)" font-size="12" fill="currentColor">C_Y{{ s }}</text>
          <text :x="xCyN(s) + 15" :y="yCyPlates + 9" font-family="var(--mono)" font-size="11" fill="var(--ink-dim)">2×{{ labels[`C_YL${s}`] }}</text>
          <text v-if="bindings[`C_YL${s}`]" :x="xCyN(s) + 15" :y="yCyPlates + 23" font-family="var(--mono)" font-size="10"
                fill="var(--amber, #ffb347)">{{ bindings[`C_YL${s}`].mpn }}</text>
        </g>
      </template>
    </svg>
  </div>
</template>

<style scoped>
.sch-fill { height: 100%; padding: 0.4rem; display: flex; }
.comp { color: var(--s-1); cursor: pointer; outline: none; }
.comp:hover { color: var(--phos); }
.comp:focus-visible { color: var(--phos); }
.comp-selected { color: var(--phos-hi); }
.comp-selected rect[fill="transparent"] { fill: rgba(63, 226, 127, 0.07); stroke: var(--phos-deep); }
.comp-bound { }
</style>
