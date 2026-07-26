<script setup>
// Log-frequency chart on the receiver screen: measured series as thin phosphor
// traces, limit lines as dashed amber reference runs with direct labels,
// violations as status-red markers, hover crosshair with a per-series tooltip.
import { computed, ref } from 'vue'
import { fmtHz, fmtDb } from '../format.js'

const props = defineProps({
  series: { type: Array, default: () => [] },      // {id,label,color,points:[{f,v}],dash?}
  refRuns: { type: Array, default: () => [] },     // {label,color,dash,runs:[[{f,v}]]}
  violations: { type: Array, default: () => [] },  // {f,v}
  yLabel: { type: String, required: true },
  height: { type: Number, default: 380 },
  violationLabel: { type: String, default: 'over limit' },
  clampRangeDb: { type: Number, default: 0 },  // 0 = off; else vMin >= vMax - clamp
})

const width = 920
const margins = { left: 58, right: 86, top: 16, bottom: 40 }
const hover = ref(null)

const domain = computed(() => {
  let fMin = Infinity, fMax = -Infinity, vMin = Infinity, vMax = -Infinity
  const scanPoint = (p) => {
    fMin = Math.min(fMin, p.f); fMax = Math.max(fMax, p.f)
    vMin = Math.min(vMin, p.v); vMax = Math.max(vMax, p.v)
  }
  props.series.forEach((s) => s.points.forEach(scanPoint))
  props.refRuns.forEach((r) => r.runs.forEach((run) => run.forEach(scanPoint)))
  // a log axis cannot draw f <= 0, and a non-finite domain would loop the
  // tick generator forever (-Infinity + 1 === -Infinity) — refuse to render
  if (!Number.isFinite(fMin) || fMin <= 0 || !Number.isFinite(vMin) || !Number.isFinite(vMax)) return null
  if (props.clampRangeDb > 0) vMin = Math.max(vMin, vMax - props.clampRangeDb)
  const pad = Math.max(4, (vMax - vMin) * 0.08)
  return { fMin, fMax, vMin: Math.floor((vMin - pad) / 10) * 10, vMax: Math.ceil((vMax + pad) / 10) * 10 }
})

const x = (f) => margins.left + ((Math.log10(f) - Math.log10(domain.value.fMin)) /
  (Math.log10(domain.value.fMax) - Math.log10(domain.value.fMin))) * (width - margins.left - margins.right)
const y = (v) => margins.top + (1 - (v - domain.value.vMin) / (domain.value.vMax - domain.value.vMin)) *
  (props.height - margins.top - margins.bottom)

const xTicks = computed(() => {
  if (!domain.value) return []
  const ticks = []
  const eMin = Math.floor(Math.log10(domain.value.fMin))
  const eMax = Math.ceil(Math.log10(domain.value.fMax))
  for (let e = eMin; e <= eMax; e += 1) {
    for (const m of [1, 3]) {
      const f = m * 10 ** e
      if (f >= domain.value.fMin * 0.999 && f <= domain.value.fMax * 1.001) ticks.push(f)
    }
  }
  return ticks
})
const yTicks = computed(() => {
  if (!domain.value) return []
  const span = domain.value.vMax - domain.value.vMin
  const step = span > 80 ? 20 : 10
  const ticks = []
  for (let v = domain.value.vMin; v <= domain.value.vMax + 0.001; v += step) ticks.push(v)
  return ticks
})

const path = (points) => {
  // a single-point run has no line geometry — draw a short tick so an
  // isolated sample stays visible instead of silently vanishing
  if (points.length === 1) {
    const px = x(points[0].f), py = y(points[0].v)
    return 'M' + (px - 3).toFixed(1) + ' ' + py.toFixed(1) + ' L' + (px + 3).toFixed(1) + ' ' + py.toFixed(1)
  }
  return points.map((p, i) => (i === 0 ? 'M' : 'L') + x(p.f).toFixed(1) + ' ' + y(p.v).toFixed(1)).join(' ')
}

function onMove(event) {
  if (!domain.value || props.series.length === 0) return
  const rect = event.currentTarget.getBoundingClientRect()
  const px = ((event.clientX - rect.left) / rect.width) * width
  if (px < margins.left || px > width - margins.right) { hover.value = null; return }
  const logF = Math.log10(domain.value.fMin) + ((px - margins.left) / (width - margins.left - margins.right)) *
    (Math.log10(domain.value.fMax) - Math.log10(domain.value.fMin))
  const f = 10 ** logF
  const rows = []
  for (const s of props.series) {
    let best = null
    for (const p of s.points) {
      if (best === null || Math.abs(Math.log10(p.f) - logF) < Math.abs(Math.log10(best.f) - logF)) best = p
    }
    if (best) rows.push({ label: s.label, color: s.color, f: best.f, v: best.v })
  }
  for (const r of props.refRuns) {
    for (const run of r.runs) {
      if (f >= run[0].f && f <= run[run.length - 1].f) {
        let best = run[0]
        for (const p of run) if (Math.abs(Math.log10(p.f) - logF) < Math.abs(Math.log10(best.f) - logF)) best = p
        rows.push({ label: r.label, color: r.color, f: best.f, v: best.v })
        break
      }
    }
  }
  hover.value = { f, xPx: x(rows[0] ? rows[0].f : f), rows }
}

const legend = computed(() => [
  // unlabeled entries are continuation runs of an already-listed series
  ...props.series.filter((s) => s.label).map((s) => ({ label: s.label, color: s.color, dash: s.dash || null })),
  ...props.refRuns.map((r) => ({ label: r.label, color: r.color, dash: r.dash || '6 4' })),
  ...(props.violations.length ? [{ label: props.violationLabel, color: 'var(--fault)', marker: true }] : []),
])
</script>

<template>
  <div class="screen">
    <svg v-if="domain" :viewBox="`0 0 ${width} ${height}`" role="img" :aria-label="yLabel + ' vs frequency'"
         style="width: 100%; height: auto; display: block" @mousemove="onMove" @mouseleave="hover = null">
      <defs>
        <clipPath id="plotarea">
          <rect :x="margins.left" :y="margins.top" :width="width - margins.left - margins.right"
                :height="height - margins.top - margins.bottom" />
        </clipPath>
        <filter id="phosphor" x="-20%" y="-20%" width="140%" height="140%">
          <feGaussianBlur stdDeviation="2.2" result="blur" />
          <feMerge><feMergeNode in="blur" /><feMergeNode in="SourceGraphic" /></feMerge>
        </filter>
      </defs>

      <!-- graticule -->
      <g>
        <line v-for="f in xTicks" :key="'gx' + f" :x1="x(f)" :x2="x(f)" :y1="margins.top" :y2="height - margins.bottom"
              stroke="var(--grat-strong)" stroke-width="1" />
        <line v-for="v in yTicks" :key="'gy' + v" :x1="margins.left" :x2="width - margins.right" :y1="y(v)" :y2="y(v)"
              stroke="var(--grat-strong)" stroke-width="1" />
      </g>

      <!-- axes labels -->
      <g font-family="var(--mono)" font-size="11" fill="var(--ink-dim)">
        <text v-for="f in xTicks" :key="'tx' + f" :x="x(f)" :y="height - margins.bottom + 16" text-anchor="middle">{{ fmtHz(f) }}</text>
        <text v-for="v in yTicks" :key="'ty' + v" :x="margins.left - 8" :y="y(v) + 4" text-anchor="end">{{ v }}</text>
        <text :x="margins.left" :y="margins.top - 4" fill="var(--ink-dim)">{{ yLabel }}</text>
      </g>

      <!-- limit reference runs (dashed, direct-labeled at the right end) -->
      <g v-for="(r, ri) in refRuns" :key="'ref' + ri">
        <path v-for="(run, i) in r.runs" :key="i" :d="path(run)" fill="none" :stroke="r.color"
              stroke-width="2" :stroke-dasharray="r.dash || '6 4'" />
        <text v-if="r.runs.length" :x="x(r.runs[r.runs.length - 1][r.runs[r.runs.length - 1].length - 1].f) + 6"
              :y="y(r.runs[r.runs.length - 1][r.runs[r.runs.length - 1].length - 1].v) + 4"
              font-family="var(--mono)" font-size="11" :fill="r.color">{{ r.label }}</text>
      </g>

      <!-- measured traces: thin phosphor with a soft glow, clipped to the plot -->
      <g clip-path="url(#plotarea)">
        <path v-for="s in series" :key="s.id" :d="path(s.points)" fill="none" :stroke="s.color"
              stroke-width="2" :stroke-dasharray="s.dash || 'none'" filter="url(#phosphor)" />
      </g>

      <!-- violations: status red, ringed for overlap; clipped with the series -->
      <g clip-path="url(#plotarea)">
        <circle v-for="(p, i) in violations" :key="'v' + i" :cx="x(p.f)" :cy="y(p.v)" r="3.2"
                :fill="p.color || 'var(--fault)'" stroke="var(--bg-deep)" stroke-width="1.5" />
      </g>

      <!-- hover crosshair + tooltip -->
      <g v-if="hover">
        <line :x1="hover.xPx" :x2="hover.xPx" :y1="margins.top" :y2="height - margins.bottom"
              stroke="var(--ink-dim)" stroke-width="1" stroke-dasharray="2 3" />
        <g :transform="`translate(${Math.min(hover.xPx + 10, width - 240)}, ${margins.top + 8})`">
          <rect width="230" :height="16 + hover.rows.length * 16" rx="4" fill="var(--panel-hi)"
                stroke="var(--line)" opacity="0.96" />
          <text x="8" y="13" font-family="var(--mono)" font-size="11" fill="var(--ink-dim)">{{ fmtHz(hover.f) }}</text>
          <g v-for="(row, i) in hover.rows" :key="i" :transform="`translate(8, ${28 + i * 16})`">
            <rect width="9" height="3" y="-4" :fill="row.color" />
            <text x="14" font-family="var(--mono)" font-size="11" fill="var(--ink)">{{ row.label }}: {{ fmtDb(row.v) }}</text>
          </g>
        </g>
      </g>
    </svg>
    <p v-else class="note" style="padding: 2rem 1rem">No data yet — the screen lights up when a trace arrives.</p>

    <div v-if="legend.length" style="display: flex; gap: 1.1rem; flex-wrap: wrap; padding: 0.45rem 0.6rem 0.5rem">
      <span v-for="(item, i) in legend" :key="i" style="display: inline-flex; align-items: center; gap: 0.4em; font-family: var(--mono); font-size: 0.75rem; color: var(--ink-dim)">
        <svg width="20" height="8"><circle v-if="item.marker" cx="10" cy="4" r="3.4" :fill="item.color" />
          <line v-else x1="1" y1="4" x2="19" y2="4" :stroke="item.color" stroke-width="2.5" :stroke-dasharray="item.dash || 'none'" /></svg>
        {{ item.label }}
      </span>
    </div>
  </div>
</template>
