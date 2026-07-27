<script setup>
// The filter schematic, drawn from the same component/net truth the CIAS brick
// exports (ciasFilter.js). Geometry is local per stage count — CIAS carries
// none — mirroring the Kirchhoff pattern. Components are clickable: selecting
// one drives the parts-recommendation pane.
//
// Symbol conventions (round-16 user feedback): the CM choke is ONE compact
// coupled component; capacitors sit centered in their span; every electrical
// junction carries a dot; a wire CROSSING a rail it does not join HOPS it
// (no-connection arc).
//
// Topologies: 'mains'/'dc' draw the historical two-rail layout (L/N or +/RTN);
// '3ph' draws three phase rails with delta X capacitors (the wrap-around
// L3-L1 capacitor hops L2); '3phn' draws four rails with star X capacitors
// phase-to-neutral. Y capacitors: one per rail to PE, hopping lower rails.
import { computed } from 'vue'
import { filterComponents, filterNets, topologyLines } from '../ciasFilter.js'

const props = defineProps({
  stages: { type: Number, required: true },
  labels: { type: Object, required: true },    // ref -> value label ("3.3 mH")
  bindings: { type: Object, required: true },  // ref -> {mpn} | undefined
  selected: { type: String, default: '' },
  topology: { type: String, default: 'mains' },   // mains | dc | 3ph | 3phn
  interactive: { type: Boolean, default: true },  // false = static illustration (report sheet)
})
const emit = defineEmits(['select'])

const N = computed(() => topologyLines(props.topology))

// ── shared geometry ─────────────────────────────────────────────────────────
const GEOM = {
  2: { rails: [70, 190], yPe: 268, viewH: 300, stageW: 300 },
  3: { rails: [56, 126, 196], yPe: 272, viewH: 300, stageW: 340 },
  4: { rails: [46, 106, 166, 226], yPe: 292, viewH: 320, stageW: 360 },
}
const geom = computed(() => GEOM[N.value])
const rails = computed(() => geom.value.rails)
const yPe = computed(() => geom.value.yPe)

// two-rail constants (legacy layout, kept exactly)
const yLine = 70
const yNeutral = 190
const yWindL = 110      // L winding, jogged down toward the core
const yWindN = 150      // N winding, jogged up toward the core
const yCore1 = 126
const yCore2 = 134
const yCapMid = (yLine + yNeutral) / 2   // X-cap plates centered in the span
const yCyPlates = 214                    // both Y caps side by side between N and PE

const svgWidth = (stages, n) => 150 + stages * GEOM[n].stageW + 90
const width = computed(() => svgWidth(props.stages, N.value))
const components = computed(() => filterComponents(props.stages, N.value))

// per-stage x anchors
const xCmc = (s) => 120 + (s - 1) * geom.value.stageW
const xCx = (s) => xCmc(s) + (N.value === 2 ? 130 : 105)
const xCyL = (s) => xCmc(s) + 195
const xCyN = (s) => xCyL(s) + 26
const xXk = (s, k) => xCx(s) + k * 34            // n>2: X-capacitor cluster
const xYk = (s, i) => xCmc(s) + 205 + i * 26     // n>2: Y-capacitor cluster
// n-rail CMC core: one bar pair BETWEEN each adjacent winding pair (the
// multi-winding transformer convention). Bars run parallel to the windings and
// never cross a conductor — vertical bars through the coils read as junctions.
const coreY = (i) => (rails.value[i] + rails.value[i + 1]) / 2

// ── KH-style render verification ─────────────────────────────────────────────
// Values and components come from the CIAS truth (ciasFilter.js); only the
// GEOMETRY is local. To make drift impossible instead of merely unlikely, the
// drawn geometry's connectivity is EXTRACTED (pins + ports unioned along the
// rail segments actually drawn) and compared against filterNets() on every
// render — a mismatch shows an error instead of a lying schematic.
const railSegments = (stages, n) => {
  const g = GEOM[n]
  const segs = []
  for (let i = 0; i < n; i += 1) {
    segs.push([[30, g.rails[i]], [120, g.rails[i]]])
  }
  segs.push([[30, g.yPe], [svgWidth(stages, n) - 40, g.yPe]])
  for (let s = 1; s <= stages; s += 1) {
    const x0 = 120 + (s - 1) * g.stageW
    const endX = s === stages ? svgWidth(stages, n) - 40 : x0 + g.stageW
    for (let i = 0; i < n; i += 1) {
      segs.push([[x0 + 60, g.rails[i]], [endX, g.rails[i]]])
    }
  }
  return segs
}
const drawnEndpoints = (stages, n) => {
  const g = GEOM[n]
  const lineNames = n === 2 ? ['line', 'neutral'] : n === 3 ? ['l1', 'l2', 'l3'] : ['l1', 'l2', 'l3', 'n']
  const rightX = svgWidth(stages, n) - 40
  const pts = [{ key: 'port:pe', x: 30, y: g.yPe }]
  for (let i = 0; i < n; i += 1) {
    pts.push({ key: `port:${lineNames[i]}_in`, x: 30, y: g.rails[i] })
    pts.push({ key: `port:${lineNames[i]}_out`, x: rightX, y: g.rails[i] })
  }
  for (let s = 1; s <= stages; s += 1) {
    const x0 = 120 + (s - 1) * g.stageW
    if (n === 2) {
      pts.push(
        { key: `CMC${s}|P1`, x: x0, y: yLine }, { key: `CMC${s}|P2`, x: x0 + 60, y: yLine },
        { key: `CMC${s}|S1`, x: x0, y: yNeutral }, { key: `CMC${s}|S2`, x: x0 + 60, y: yNeutral },
        { key: `C_X${s}|1`, x: x0 + 130, y: yLine }, { key: `C_X${s}|2`, x: x0 + 130, y: yNeutral },
        { key: `C_YL${s}|1`, x: x0 + 195, y: yLine }, { key: `C_YL${s}|2`, x: x0 + 195, y: g.yPe },
        { key: `C_YN${s}|1`, x: x0 + 221, y: yNeutral }, { key: `C_YN${s}|2`, x: x0 + 221, y: g.yPe },
      )
      continue
    }
    for (let i = 0; i < n; i += 1) {
      pts.push({ key: `CMC${s}|W${i + 1}A`, x: x0, y: g.rails[i] })
      pts.push({ key: `CMC${s}|W${i + 1}B`, x: x0 + 60, y: g.rails[i] })
      pts.push({ key: `C_Y${i + 1}_${s}|1`, x: x0 + 205 + i * 26, y: g.rails[i] })
      pts.push({ key: `C_Y${i + 1}_${s}|2`, x: x0 + 205 + i * 26, y: g.yPe })
    }
    if (n === 3) {
      const pairs = [['12', 0, 1], ['23', 1, 2], ['31', 2, 0]]
      pairs.forEach(([suffix, a, b], k) => {
        pts.push({ key: `C_X${s}_${suffix}|1`, x: x0 + 105 + k * 34, y: g.rails[a] })
        pts.push({ key: `C_X${s}_${suffix}|2`, x: x0 + 105 + k * 34, y: g.rails[b] })
      })
    } else {
      for (let i = 0; i < 3; i += 1) {
        pts.push({ key: `C_X${s}_${i + 1}n|1`, x: x0 + 105 + i * 34, y: g.rails[i] })
        pts.push({ key: `C_X${s}_${i + 1}n|2`, x: x0 + 105 + i * 34, y: g.rails[3] })
      }
    }
  }
  return pts
}
const schError = computed(() => {
  const stages = props.stages
  const pts = drawnEndpoints(stages, N.value)
  const segs = railSegments(stages, N.value)
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
  const truth = canonical(filterNets(stages, N.value).map((net) => net.endpoints.map(
    (e) => e.port ? `port:${e.port}` : `${e.component}|${e.pin}`)))
  return drawn === truth ? null
    : `schematic geometry ≠ CIAS netlist for ${stages} stage(s) — drawing refused`
})

// wire from (x, yFrom) down to (x, yTo) hopping every rail strictly between
const hopPath = (x, yFrom, yTo, hops) => {
  let d = `M ${x} ${yFrom}`
  let y = yFrom
  for (const hy of hops) {
    d += ` L ${x} ${hy - 7} a 7 7 0 0 1 0 14`
    y = hy + 7
  }
  d += ` L ${x} ${yTo}`
  return d
}
// rails strictly between two rail indices (for hop arcs)
const railsBetween = (a, b) => {
  const lo = Math.min(a, b), hi = Math.max(a, b)
  return rails.value.filter((_, i) => i > lo && i < hi)
}
// Y-capacitor wire: hops every rail below its own
const railsBelow = (i) => rails.value.filter((_, j) => j > i)

// delta X plate positions: adjacent pairs center in their span; the L3-L1
// wrap (which hops L2) puts its plates in the lower half so they never sit
// on the hopped rail
const DELTA_PAIRS = [[0, 1], [1, 2], [2, 0]]
const deltaPlateY = (k) => k === 0 ? (rails.value[0] + rails.value[1]) / 2
  : k === 1 ? (rails.value[1] + rails.value[2]) / 2
  : (rails.value[1] + rails.value[2]) / 2 + 18

const portLabels = computed(() => {
  if (props.topology === 'dc') return { in: ['+', 'RTN'], out: ["+'", "RTN'"], pe: 'CH' }
  if (N.value === 3) return { in: ['L1', 'L2', 'L3'], out: ["L1'", "L2'", "L3'"], pe: 'PE' }
  if (N.value === 4) return { in: ['L1', 'L2', 'L3', 'N'], out: ["L1'", "L2'", "L3'", "N'"], pe: 'PE' }
  return { in: ['L', 'N'], out: ["L'", "N'"], pe: 'PE' }
})

// selection refs for the clustered n>2 symbols (all X / all Y share one part)
const firstXRef = (s) => N.value === 3 ? `C_X${s}_12` : N.value === 4 ? `C_X${s}_1n` : `C_X${s}`
const firstYRef = (s) => N.value === 2 ? `C_YL${s}` : `C_Y1_${s}`

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
    <svg v-else :viewBox="`0 0 ${width} ${geom.viewH}`" style="width: 100%; height: 100%; display: block"
         preserveAspectRatio="xMidYMid meet" role="img" aria-label="Line filter schematic">
      <!-- rails -->
      <g stroke="var(--ink-dim)" stroke-width="1.6" fill="none">
        <template v-for="(ry, i) in rails" :key="'railin' + i">
          <line :x1="30" :y1="ry" :x2="xCmc(1)" :y2="ry" />
        </template>
        <template v-for="s in stages" :key="'rail' + s">
          <line v-for="(ry, i) in rails" :key="'railseg' + s + '-' + i"
                :x1="xCmc(s) + 60" :y1="ry" :x2="s === stages ? width - 40 : xCmc(s + 1)" :y2="ry" />
        </template>
        <line :x1="30" :y1="yPe" :x2="width - 40" :y2="yPe" />
      </g>

      <!-- ports -->
      <g font-family="var(--mono)" font-size="12" fill="var(--ink-dim)">
        <circle v-for="(ry, i) in rails" :key="'pin' + i" :cx="30" :cy="ry" r="3.5"
                fill="var(--bg-deep)" stroke="var(--ink-dim)" stroke-width="1.6" />
        <circle v-for="(ry, i) in rails" :key="'pout' + i" :cx="width - 40" :cy="ry" r="3.5"
                fill="var(--bg-deep)" stroke="var(--ink-dim)" stroke-width="1.6" />
        <circle :cx="30" :cy="yPe" r="3.5" fill="var(--bg-deep)" stroke="var(--ink-dim)" stroke-width="1.6" />
        <text v-for="(ry, i) in rails" :key="'plin' + i" :x="portLabels.in[i].length > 2 ? 2 : 8" :y="ry + 4">{{ portLabels.in[i] }}</text>
        <text v-for="(ry, i) in rails" :key="'plout' + i" :x="width - 34" :y="ry + 4">{{ portLabels.out[i] }}</text>
        <text :x="portLabels.pe.length > 2 ? 4 : 8" :y="yPe + 4">{{ portLabels.pe }}</text>
      </g>

      <!-- ══ two-rail layout (mains / dc) ══ -->
      <template v-if="N === 2">
      <template v-for="s in stages" :key="'stage' + s">
        <!-- CMC: one compact coupled choke — windings jogged toward the shared
             two-bar core, humps facing the core, dots on the same (left) end -->
        <g :class="cls(`CMC${s}`)" :data-test="interactive ? `sch-CMC${s}` : null" @click="hotspot(`CMC${s}`)"
           @keydown.enter="hotspot(`CMC${s}`)" :tabindex="interactive ? 0 : null"
           :role="interactive ? 'button' : null" :aria-label="`Select CMC${s}`">
          <rect :x="xCmc(s) - 8" :y="yLine - 16" width="76" :height="yNeutral - yLine + 32" fill="transparent" />
          <g stroke="currentColor" stroke-width="2" fill="none">
            <line :x1="xCmc(s)" :y1="yLine" :x2="xCmc(s)" :y2="yWindL" />
            <line :x1="xCmc(s) + 60" :y1="yWindL" :x2="xCmc(s) + 60" :y2="yLine" />
            <line :x1="xCmc(s)" :y1="yNeutral" :x2="xCmc(s)" :y2="yWindN" />
            <line :x1="xCmc(s) + 60" :y1="yWindN" :x2="xCmc(s) + 60" :y2="yNeutral" />
            <path :d="`M ${xCmc(s)} ${yWindL} ` + [0, 1, 2, 3].map(() => `a 7.5 9 0 0 0 15 0`).join(' ')" />
            <path :d="`M ${xCmc(s)} ${yWindN} ` + [0, 1, 2, 3].map(() => `a 7.5 9 0 0 1 15 0`).join(' ')" />
            <line :x1="xCmc(s) + 4" :y1="yCore1" :x2="xCmc(s) + 56" :y2="yCore1" />
            <line :x1="xCmc(s) + 4" :y1="yCore2" :x2="xCmc(s) + 56" :y2="yCore2" />
          </g>
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
        <g :class="cls(`C_X${s}`)" :data-test="interactive ? `sch-CX${s}` : null" @click="hotspot(`C_X${s}`)"
           @keydown.enter="hotspot(`C_X${s}`)" :tabindex="interactive ? 0 : null"
           :role="interactive ? 'button' : null" :aria-label="`Select C_X${s}`">
          <rect :x="xCx(s) - 20" :y="yLine + 4" width="40" :height="yNeutral - yLine - 8" fill="transparent" />
          <g stroke="currentColor" stroke-width="1.6">
            <line :x1="xCx(s)" :y1="yLine" :x2="xCx(s)" :y2="yCapMid - 4" />
            <line :x1="xCx(s)" :y1="yCapMid + 4" :x2="xCx(s)" :y2="yNeutral" />
          </g>
          <g stroke="currentColor" stroke-width="2.4">
            <line :x1="xCx(s) - 12" :y1="yCapMid - 4" :x2="xCx(s) + 12" :y2="yCapMid - 4" />
            <line :x1="xCx(s) - 12" :y1="yCapMid + 4" :x2="xCx(s) + 12" :y2="yCapMid + 4" />
          </g>
          <circle :cx="xCx(s)" :cy="yLine" r="2.6" fill="currentColor" />
          <circle :cx="xCx(s)" :cy="yNeutral" r="2.6" fill="currentColor" />
          <text :x="xCx(s) + 17" :y="yCapMid - 6" font-family="var(--mono)" font-size="12" fill="currentColor">C_X{{ s }}</text>
          <text :x="xCx(s) + 17" :y="yCapMid + 9" font-family="var(--mono)" font-size="11" fill="var(--ink-dim)">{{ labels[`C_X${s}`] }}</text>
          <text v-if="bindings[`C_X${s}`]" :x="xCx(s) + 17" :y="yCapMid + 23" font-family="var(--mono)" font-size="10"
                fill="var(--amber, #ffb347)">{{ bindings[`C_X${s}`].mpn }}</text>
        </g>

        <!-- Y capacitors: both between N and PE, side by side. The L-side one
             HOPS the neutral rail (no-connection arc). Bound as a pair. -->
        <g :class="cls(`C_YL${s}`)" :data-test="interactive ? `sch-CY${s}` : null" @click="hotspot(`C_YL${s}`)"
           @keydown.enter="hotspot(`C_YL${s}`)" :tabindex="interactive ? 0 : null"
           :role="interactive ? 'button' : null" :aria-label="`Select Y capacitors, stage ${s}`">
          <rect :x="xCyL(s) - 16" :y="yLine + 4" width="58" :height="yPe - yLine - 8" fill="transparent" />
          <g stroke="currentColor" stroke-width="1.6" fill="none">
            <line :x1="xCyL(s)" :y1="yLine" :x2="xCyL(s)" :y2="yNeutral - 7" />
            <path :d="`M ${xCyL(s)} ${yNeutral - 7} a 7 7 0 0 1 0 14`" />
            <line :x1="xCyL(s)" :y1="yNeutral + 7" :x2="xCyL(s)" :y2="yCyPlates - 4" />
            <line :x1="xCyL(s)" :y1="yCyPlates + 4" :x2="xCyL(s)" :y2="yPe" />
            <line :x1="xCyN(s)" :y1="yNeutral" :x2="xCyN(s)" :y2="yCyPlates - 4" />
            <line :x1="xCyN(s)" :y1="yCyPlates + 4" :x2="xCyN(s)" :y2="yPe" />
          </g>
          <g stroke="currentColor" stroke-width="2.4">
            <line :x1="xCyL(s) - 10" :y1="yCyPlates - 4" :x2="xCyL(s) + 10" :y2="yCyPlates - 4" />
            <line :x1="xCyL(s) - 10" :y1="yCyPlates + 4" :x2="xCyL(s) + 10" :y2="yCyPlates + 4" />
            <line :x1="xCyN(s) - 10" :y1="yCyPlates - 4" :x2="xCyN(s) + 10" :y2="yCyPlates - 4" />
            <line :x1="xCyN(s) - 10" :y1="yCyPlates + 4" :x2="xCyN(s) + 10" :y2="yCyPlates + 4" />
          </g>
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
      </template>

      <!-- ══ n-rail layout (3ph / 3phn) ══ -->
      <template v-else>
      <template v-for="s in stages" :key="'nstage' + s">
        <!-- CMC: one winding per rail, dashed shared core crossing all rails -->
        <g :class="cls(`CMC${s}`)" :data-test="interactive ? `sch-CMC${s}` : null" @click="hotspot(`CMC${s}`)"
           @keydown.enter="hotspot(`CMC${s}`)" :tabindex="interactive ? 0 : null"
           :role="interactive ? 'button' : null" :aria-label="`Select CMC${s}`">
          <rect :x="xCmc(s) - 8" :y="rails[0] - 22" width="76" :height="rails[N - 1] - rails[0] + 34" fill="transparent" />
          <g stroke="currentColor" stroke-width="2" fill="none">
            <!-- windings: humps face the core bar beside them (top winding down,
                 every other up) — the same convention as the 2-rail symbol -->
            <path v-for="(ry, i) in rails" :key="'w' + i"
                  :d="`M ${xCmc(s)} ${ry} ` + [0, 1, 2, 3].map(() => `a 7.5 9 0 0 ${i === 0 ? 0 : 1} 15 0`).join(' ')" />
            <!-- shared core: a bar pair between each adjacent winding pair -->
            <template v-for="(_, i) in rails.slice(0, N - 1)" :key="'core' + i">
              <line :x1="xCmc(s) + 4" :y1="coreY(i) - 4" :x2="xCmc(s) + 56" :y2="coreY(i) - 4" />
              <line :x1="xCmc(s) + 4" :y1="coreY(i) + 4" :x2="xCmc(s) + 56" :y2="coreY(i) + 4" />
            </template>
          </g>
          <!-- polarity dots: same (left) end on every winding, clear of the humps -->
          <circle v-for="(ry, i) in rails" :key="'d' + i" :cx="xCmc(s) + 5" :cy="ry + (i === 0 ? -7 : 7)"
                  r="2.6" fill="currentColor" />
          <text :x="xCmc(s) + 30" :y="rails[0] - 18" text-anchor="middle" font-family="var(--mono)" font-size="12"
                fill="currentColor">CMC{{ s }}</text>
          <text :x="xCmc(s) + 64" :y="rails[0] - 6" font-family="var(--mono)"
                font-size="11" fill="var(--ink-dim)">{{ labels[`CMC${s}`] }}</text>
          <text v-if="bindings[`CMC${s}`]" :x="xCmc(s) + 30" :y="rails[N - 1] + 18" text-anchor="middle"
                font-family="var(--mono)" font-size="10" fill="var(--amber, #ffb347)">{{ bindings[`CMC${s}`].mpn }}</text>
        </g>

        <!-- X capacitors: delta (3ph, the L3-L1 wrap hops L2) or star (3phn) -->
        <g :class="cls(firstXRef(s))" :data-test="interactive ? `sch-CX${s}` : null" @click="hotspot(firstXRef(s))"
           @keydown.enter="hotspot(firstXRef(s))" :tabindex="interactive ? 0 : null"
           :role="interactive ? 'button' : null" :aria-label="`Select X capacitors, stage ${s}`">
          <rect :x="xCx(s) - 16" :y="rails[0] + 4" :width="100" :height="rails[N - 1] - rails[0] - 8" fill="transparent" />
          <template v-if="N === 3">
            <template v-for="([a, b], k) in DELTA_PAIRS" :key="'xd' + k">
              <g stroke="currentColor" stroke-width="1.6" fill="none">
                <path :d="hopPath(xXk(s, k), rails[Math.min(a, b)], deltaPlateY(k) - 4,
                                  k === 2 ? [rails[1]] : [])" />
                <line :x1="xXk(s, k)" :y1="deltaPlateY(k) + 4"
                      :x2="xXk(s, k)" :y2="rails[Math.max(a, b)]" />
              </g>
              <g stroke="currentColor" stroke-width="2.4">
                <line :x1="xXk(s, k) - 10" :y1="deltaPlateY(k) - 4" :x2="xXk(s, k) + 10" :y2="deltaPlateY(k) - 4" />
                <line :x1="xXk(s, k) - 10" :y1="deltaPlateY(k) + 4" :x2="xXk(s, k) + 10" :y2="deltaPlateY(k) + 4" />
              </g>
              <circle :cx="xXk(s, k)" :cy="rails[a]" r="2.6" fill="currentColor" />
              <circle :cx="xXk(s, k)" :cy="rails[b]" r="2.6" fill="currentColor" />
            </template>
          </template>
          <template v-else>
            <template v-for="i in 3" :key="'xs' + i">
              <g stroke="currentColor" stroke-width="1.6" fill="none">
                <path :d="hopPath(xXk(s, i - 1), rails[i - 1], rails[3] - 26, railsBetween(i - 1, 3))" />
                <line :x1="xXk(s, i - 1)" :y1="rails[3] - 18" :x2="xXk(s, i - 1)" :y2="rails[3]" />
              </g>
              <g stroke="currentColor" stroke-width="2.4">
                <line :x1="xXk(s, i - 1) - 10" :y1="rails[3] - 26" :x2="xXk(s, i - 1) + 10" :y2="rails[3] - 26" />
                <line :x1="xXk(s, i - 1) - 10" :y1="rails[3] - 18" :x2="xXk(s, i - 1) + 10" :y2="rails[3] - 18" />
              </g>
              <circle :cx="xXk(s, i - 1)" :cy="rails[i - 1]" r="2.6" fill="currentColor" />
              <circle :cx="xXk(s, i - 1)" :cy="rails[3]" r="2.6" fill="currentColor" />
            </template>
          </template>
          <text :x="xCx(s) + 6" :y="yPe - 34" font-family="var(--mono)" font-size="12"
                fill="currentColor">C_X{{ s }} {{ N === 3 ? 'Δ' : 'Y' }}</text>
          <text :x="xCx(s) + 6" :y="yPe - 20" font-family="var(--mono)" font-size="11"
                fill="var(--ink-dim)">3×{{ labels[firstXRef(s)] }}</text>
          <text v-if="bindings[firstXRef(s)]" :x="xCx(s) + 6" :y="yPe - 6" font-family="var(--mono)" font-size="10"
                fill="var(--amber, #ffb347)">{{ bindings[firstXRef(s)].mpn }}</text>
        </g>

        <!-- Y capacitors: one per rail to PE, hopping lower rails -->
        <g :class="cls(firstYRef(s))" :data-test="interactive ? `sch-CY${s}` : null" @click="hotspot(firstYRef(s))"
           @keydown.enter="hotspot(firstYRef(s))" :tabindex="interactive ? 0 : null"
           :role="interactive ? 'button' : null" :aria-label="`Select Y capacitors, stage ${s}`">
          <rect :x="xYk(s, 0) - 16" :y="rails[0] + 4" :width="N * 26 + 30" :height="yPe - rails[0] - 8" fill="transparent" />
          <template v-for="i in N" :key="'y' + i">
            <g stroke="currentColor" stroke-width="1.6" fill="none">
              <path :d="hopPath(xYk(s, i - 1), rails[i - 1], yPe - 28, railsBelow(i - 1))" />
              <line :x1="xYk(s, i - 1)" :y1="yPe - 20" :x2="xYk(s, i - 1)" :y2="yPe" />
            </g>
            <g stroke="currentColor" stroke-width="2.4">
              <line :x1="xYk(s, i - 1) - 10" :y1="yPe - 28" :x2="xYk(s, i - 1) + 10" :y2="yPe - 28" />
              <line :x1="xYk(s, i - 1) - 10" :y1="yPe - 20" :x2="xYk(s, i - 1) + 10" :y2="yPe - 20" />
            </g>
            <circle :cx="xYk(s, i - 1)" :cy="rails[i - 1]" r="2.6" fill="currentColor" />
            <circle :cx="xYk(s, i - 1)" :cy="yPe" r="2.6" fill="currentColor" />
          </template>
          <text :x="xYk(s, N - 1) + 16" :y="yPe - 34" font-family="var(--mono)" font-size="12"
                fill="currentColor">C_Y{{ s }}</text>
          <text :x="xYk(s, N - 1) + 16" :y="yPe - 20" font-family="var(--mono)" font-size="11"
                fill="var(--ink-dim)">{{ N }}×{{ labels[firstYRef(s)] }}</text>
          <text v-if="bindings[firstYRef(s)]" :x="xYk(s, N - 1) + 16" :y="yPe - 6" font-family="var(--mono)" font-size="10"
                fill="var(--amber, #ffb347)">{{ bindings[firstYRef(s)].mpn }}</text>
        </g>
      </template>
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
