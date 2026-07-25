<script setup>
// ANP015 line-filter designer: attenuation target in, component values out,
// with the safety checks (Y-cap leakage, X-cap discharge) and SPICE export.
import { onMounted, ref } from 'vue'
import LogChart from './LogChart.vue'
import FilterSchematic from './FilterSchematic.vue'
import { api } from '../engine.js'
import { buildFilterCias, filterComponents } from '../ciasFilter.js'
import { store } from '../store.js'
import { fmtHz, fmtDb, fmtSi } from '../format.js'

const fSwKhz = ref(300)
const aReqCm = ref(40)
const aReqDm = ref(40)
const cYnF = ref(4.7)
const stages = ref(1)
const dmMode = ref('impedance')     // 'impedance' | 'inductance'
const dmImpedanceOhm = ref(92)
const dmImpedanceMhz = ref(1)
const lDmUh = ref(14.6)
const lCandidatesMh = ref('0.47, 0.68, 1, 1.5, 2.2, 3.3, 4.7, 6.8, 10')
const cxCandidatesUf = ref('0.1, 0.15, 0.22, 0.33, 0.47, 0.68, 1, 1.5, 2.2, 3.3')
const gridVrms = ref(230)
const gridHz = ref(50)
const design = ref(null)
const netlist = ref('')
const error = ref('')
const ilCm = ref(null)
const ilDm = ref(null)
const vInMin = ref(207)
const pIn = ref(25)
const interaction = ref(null)
const lCmSource = ref('manual')      // 'manual' | 'catalog'
const catalog = ref(null)            // {count, parts:[{mpn,manufacturer,family,inductanceH,ratedCurrentA,dcrOhm}]}
const catalogState = ref('loading')  // 'loading' | 'ready' | 'unavailable'
const mfrFilter = ref('')
const minRatedA = ref(1)
const capsCatalog = ref(null)
const selectedRef = ref('')
const bindings = ref({})

onMounted(async () => {
  try {
    const response = await fetch('/kelvin/hertz-cmc.v1.json')
    if (!response.ok) throw new Error(String(response.status))
    catalog.value = await response.json()
    catalogState.value = 'ready'
  } catch {
    catalogState.value = 'unavailable'
  }
  try {
    const response = await fetch('/kelvin/hertz-safety-caps.v1.json')
    if (response.ok) capsCatalog.value = await response.json()
  } catch { /* caps panel shows its own unavailable state */ }
  if (store.handoff) {
    aReqCm.value = store.handoff.aReqDb
    aReqDm.value = store.handoff.aReqDb
    if (store.handoff.fSwHz) fSwKhz.value = Math.round(store.handoff.fSwHz / 1e3)
    store.handoff = null
    compute()
  }
})

const manufacturers = () =>
  catalog.value ? [...new Set(catalog.value.parts.map((p) => p.manufacturer))].sort() : []

function catalogParts() {
  if (!catalog.value) return []
  return catalog.value.parts.filter((p) =>
    (!mfrFilter.value || p.manufacturer === mfrFilter.value) &&
    (p.ratedCurrentA === null || p.ratedCurrentA >= Number(minRatedA.value)) &&
    p.inductanceH > 0)
}

function catalogCandidates() {
  const parts = catalogParts()
  if (!parts.length) throw new Error('no catalog parts match the manufacturer/current filter')
  return [...new Set(parts.map((p) => p.inductanceH))].sort((a, b) => a - b)
}

const matchedParts = () => (!design.value || !catalog.value) ? [] : catalogParts()
  .filter((p) => Math.abs(p.inductanceH - design.value.lCmSelectedH) < 1e-3 * design.value.lCmSelectedH)
  .sort((a, b) => (b.ratedCurrentA ?? 0) - (a.ratedCurrentA ?? 0))
  .slice(0, 6)

function parseList(text, scale) {
  const values = text.split(/[,;\s]+/).filter(Boolean).map(Number)
  if (!values.length || values.some((x) => !isFinite(x) || x <= 0)) {
    throw new Error('candidate list must be positive numbers')
  }
  return values.map((x) => x * scale)
}

async function compute() {
  error.value = ''
  design.value = null
  netlist.value = ''
  try {
    const engine = await api()
    const params = {
      fSwHz: fSwKhz.value * 1e3,
      aReqCmDb: Number(aReqCm.value),
      aReqDmDb: Number(aReqDm.value),
      cYPerLineF: cYnF.value * 1e-9,
      stages: Number(stages.value),
      lCmCandidatesH: lCmSource.value === 'catalog' && catalogState.value === 'ready'
        ? catalogCandidates() : parseList(lCandidatesMh.value, 1e-3),
      cXCandidatesF: parseList(cxCandidatesUf.value, 1e-6),
      grid: { vRms: Number(gridVrms.value), fHz: Number(gridHz.value), vSafe: 60, tDischargeS: 1 },
    }
    if (dmMode.value === 'impedance') {
      params.dmImpedanceOhm = Number(dmImpedanceOhm.value)
      params.dmImpedanceFrequencyHz = dmImpedanceMhz.value * 1e6
    } else {
      params.lDmH = lDmUh.value * 1e-6
    }
    bindings.value = {}
    selectedRef.value = ''
    design.value = engine.designFilter(params)
    netlist.value = engine.filterSpiceNetlist(design.value, 'cispr16')
    const d = design.value
    const span = { fMinHz: 150e3, fMaxHz: 30e6, pointsPerDecade: 30 }
    ilCm.value = engine.insertionLossCurves({ inductanceH: d.lCmSelectedH, capacitanceF: d.cYgF,
      stages: d.stages, referenceImpedanceOhm: 25, ...span })
    ilDm.value = engine.insertionLossCurves({ inductanceH: d.lDmH, capacitanceF: d.cXSelectedF,
      stages: d.stages, referenceImpedanceOhm: 100, ...span })
    interaction.value = engine.inputFilterInteraction(d.lDmH, d.cXSelectedF,
      Number(vInMin.value), Number(pIn.value))
  } catch (e) {
    error.value = e.message
  }
}

const ilSeries = () => [
  { id: 'cm', label: 'CM in-circuit (25 Ω)', color: 'var(--s-1)',
    points: ilCm.value.frequenciesHz.map((f, i) => ({ f, v: ilCm.value.standardDb[i] })) },
  { id: 'cmw', label: 'CM worst case (CISPR 17)', color: 'var(--s-1)', dash: '3 4',
    points: ilCm.value.frequenciesHz.map((f, i) => ({ f, v: ilCm.value.worstCaseDb[i] })) },
  { id: 'dm', label: 'DM in-circuit (100 Ω)', color: 'var(--s-2)',
    points: ilDm.value.frequenciesHz.map((f, i) => ({ f, v: ilDm.value.standardDb[i] })) },
  { id: 'dmw', label: 'DM worst case (CISPR 17)', color: 'var(--s-2)', dash: '3 4',
    points: ilDm.value.frequenciesHz.map((f, i) => ({ f, v: ilDm.value.worstCaseDb[i] })) },
]
const requirementMarkers = () => [
  { f: design.value.fDesignHz, v: Number(aReqCm.value) },
]

const schematicLabels = () => {
  const labels = {}
  for (let s = 1; s <= design.value.stages; s += 1) {
    labels[`CMC${s}`] = fmtSi(design.value.lCmSelectedH, 'H')
    labels[`C_X${s}`] = fmtSi(design.value.cXSelectedF, 'F')
    labels[`C_YL${s}`] = fmtSi(design.value.cYPerLineF, 'F')
    labels[`C_YN${s}`] = fmtSi(design.value.cYPerLineF, 'F')
  }
  return labels
}

const kindOf = (ref) => ref.startsWith('CMC') ? 'cmc' : ref.startsWith('C_X') ? 'cx' : 'cy'
const targetValueOf = (kind) => kind === 'cmc' ? design.value.lCmSelectedH
  : kind === 'cx' ? design.value.cXSelectedF : design.value.cYPerLineF

// Catalog recommendations for the selected component: closest values first,
// exact matches naturally leading. Identical stages share bindings, so binding
// one CMC binds them all (and Y caps bind as the full set).
const recommendations = () => {
  if (!selectedRef.value || !design.value) return null
  const kind = kindOf(selectedRef.value)
  const target = targetValueOf(kind)
  let pool
  if (kind === 'cmc') {
    pool = (catalog.value?.parts ?? []).map((p) => ({ ...p, valueF: p.inductanceH }))
  } else {
    const cls = kind === 'cx' ? 'X2' : 'Y2'
    pool = (capsCatalog.value?.parts ?? [])
      .filter((p) => p.safetyClass === cls)
      .map((p) => ({ ...p, valueF: p.capacitanceF }))
  }
  if (!pool.length) return { kind, target, parts: [], unavailable: true }
  const parts = pool
    .map((p) => ({ ...p, deviation: Math.abs(p.valueF - target) / target }))
    .sort((a, b) => a.deviation - b.deviation || (b.ratedCurrentA ?? b.ratedVoltageV ?? 0) - (a.ratedCurrentA ?? a.ratedVoltageV ?? 0))
    .slice(0, 8)
  return { kind, target, parts, unavailable: false }
}

function bindPart(part) {
  const kind = kindOf(selectedRef.value)
  for (const c of filterComponents(design.value.stages)) {
    if (c.kind === kind) bindings.value[c.ref] = { mpn: part.mpn, manufacturer: part.manufacturer }
  }
  bindings.value = { ...bindings.value }
}

const bomRows = () => filterComponents(design.value.stages).map((c) => ({
  ref: c.ref,
  value: schematicLabels()[c.ref],
  binding: bindings.value[c.ref] ?? null,
}))
const allBound = () => filterComponents(design.value.stages).every((c) => bindings.value[c.ref]?.mpn)

function downloadCias() {
  const brick = buildFilterCias(design.value.stages, bindings.value)
  const blob = new Blob([JSON.stringify(brick, null, 2)], { type: 'application/json' })
  const a = document.createElement('a')
  a.href = URL.createObjectURL(blob)
  a.download = brick.name + '.cias.json'
  a.click()
  URL.revokeObjectURL(a.href)
}

function downloadNetlist() {
  const blob = new Blob([netlist.value], { type: 'text/plain' })
  const a = document.createElement('a')
  a.href = URL.createObjectURL(blob)
  a.download = 'hertz_line_filter.cir'
  a.click()
  URL.revokeObjectURL(a.href)
}
</script>

<template>
  <div class="grid2">
    <div>
      <div class="panel">
        <p class="section-label">Requirement</p>
        <div class="row">
          <label class="field"><span>Switching frequency (kHz)</span>
            <input v-model.number="fSwKhz" type="number" min="1" data-test="fsw" /></label>
          <label class="field"><span>Stages</span>
            <select v-model.number="stages"><option :value="1">1 (40 dB/dec)</option><option :value="2">2 (80 dB/dec)</option></select></label>
        </div>
        <div class="row">
          <label class="field"><span>Required CM attenuation (dB)</span>
            <input v-model.number="aReqCm" type="number" data-test="areq-cm" /></label>
          <label class="field"><span>Required DM attenuation (dB)</span>
            <input v-model.number="aReqDm" type="number" /></label>
        </div>
        <p class="note">Below 150 kHz the design moves to the first harmonic inside the measured band, as ANP015 prescribes.</p>
      </div>

      <div class="panel">
        <p class="section-label">Components</p>
        <label class="field"><span>Y capacitor per line (nF) — bounded by leakage current</span>
          <select v-model.number="cYnF"><option>1</option><option>2.2</option><option>3.3</option><option>4.7</option><option>10</option></select></label>
        <label class="field"><span>DM inductance source</span>
          <select v-model="dmMode">
            <option value="impedance">from choke DM impedance curve (Z at f)</option>
            <option value="inductance">leakage inductance directly</option>
          </select></label>
        <div v-if="dmMode === 'impedance'" class="row">
          <label class="field"><span>|Z| (Ω)</span><input v-model.number="dmImpedanceOhm" type="number" /></label>
          <label class="field"><span>at (MHz)</span><input v-model.number="dmImpedanceMhz" type="number" /></label>
        </div>
        <label v-else class="field"><span>Leakage inductance (µH)</span>
          <input v-model.number="lDmUh" type="number" /></label>
        <label class="field"><span>CM choke source</span>
          <select v-model="lCmSource" data-test="lcm-source">
            <option value="manual">manual value list</option>
            <option value="catalog" :disabled="catalogState !== 'ready'">
              parts catalog{{ catalogState === 'ready' ? ` (${catalog.count} chokes)` : catalogState === 'loading' ? ' (loading…)' : ' (unavailable here)' }}
            </option>
          </select></label>
        <label v-if="lCmSource === 'manual'" class="field"><span>CM choke candidates (mH) — any manufacturer</span>
          <input v-model="lCandidatesMh" type="text" /></label>
        <div v-else class="row">
          <label class="field"><span>Manufacturer</span>
            <select v-model="mfrFilter" data-test="mfr-filter"><option value="">all manufacturers</option>
              <option v-for="m in manufacturers()" :key="m" :value="m">{{ m }}</option></select></label>
          <label class="field"><span>Min. rated current (A)</span>
            <input v-model.number="minRatedA" type="number" min="0" /></label>
        </div>
        <label class="field"><span>X capacitor candidates (µF)</span>
          <input v-model="cxCandidatesUf" type="text" /></label>
        <div class="row">
          <label class="field"><span>Grid voltage (V RMS)</span><input v-model.number="gridVrms" type="number" /></label>
          <label class="field"><span>Grid frequency (Hz)</span><input v-model.number="gridHz" type="number" /></label>
        </div>
        <button class="act" data-test="compute" @click="compute">Design filter</button>
      </div>

      <div v-if="error" class="err" data-test="error">{{ error }}</div>
    </div>

    <div v-if="design">
      <div class="panel panel-hi">
        <div class="readout">
          <div class="cell"><b>Design frequency</b><span>{{ fmtHz(design.fDesignHz) }}</span></div>
          <div class="cell"><b>Target cutoff</b><span>{{ fmtHz(design.fCutoffTargetHz) }}</span></div>
          <div class="cell"><b>Stages</b><span>{{ design.stages }}</span></div>
        </div>
      </div>

      <FilterSchematic :stages="design.stages" :labels="schematicLabels()" :bindings="bindings"
                       :selected="selectedRef" @select="selectedRef = $event" />

      <div v-if="selectedRef && recommendations()" class="panel" data-test="part-panel">
        <p class="section-label">Catalog parts for {{ selectedRef.replace('C_YL', 'C_Y (pair) ').replace('C_YN', 'C_Y (pair) ') }}
          — target {{ fmtSi(recommendations().target, kindOf(selectedRef) === 'cmc' ? 'H' : 'F') }}</p>
        <p v-if="recommendations().unavailable" class="note">Parts catalog not reachable from this deployment — bind manually via the BOM later, or design on values only.</p>
        <table v-else class="data">
          <thead><tr><th>Part</th><th>Manufacturer</th><th>Value</th><th>{{ kindOf(selectedRef) === 'cmc' ? 'Rated A' : 'Class / V' }}</th><th></th></tr></thead>
          <tbody>
            <tr v-for="p in recommendations().parts" :key="p.mpn">
              <td><strong>{{ p.mpn }}</strong></td><td>{{ p.manufacturer }}</td>
              <td>{{ fmtSi(p.valueF, kindOf(selectedRef) === 'cmc' ? 'H' : 'F') }}
                <span v-if="p.deviation > 0.001" class="note">({{ (p.deviation * 100).toFixed(0) }}% off)</span></td>
              <td>{{ kindOf(selectedRef) === 'cmc'
                ? (p.ratedCurrentA !== null && p.ratedCurrentA !== undefined ? p.ratedCurrentA.toFixed(1) : '—')
                : p.safetyClass + (p.ratedVoltageV ? ' / ' + p.ratedVoltageV + ' V' : '') }}</td>
              <td><button class="ghost" data-test="bind-part" @click="bindPart(p)">Use</button></td>
            </tr>
          </tbody>
        </table>
      </div>

      <div class="panel">
        <p class="section-label">Bill of materials</p>
        <table class="data" data-test="bom">
          <thead><tr><th>Ref</th><th>Value</th><th>Bound part</th></tr></thead>
          <tbody>
            <tr v-for="row in bomRows()" :key="row.ref">
              <td>{{ row.ref }}</td><td>{{ row.value }}</td>
              <td v-if="row.binding"><strong>{{ row.binding.mpn }}</strong> <span class="note">{{ row.binding.manufacturer }}</span></td>
              <td v-else class="note">unbound — click the part in the schematic</td>
            </tr>
          </tbody>
        </table>
        <div class="row" style="margin-top: 0.6rem">
          <button class="ghost" data-test="download-cias" :disabled="!allBound()" @click="downloadCias()"
                  :title="allBound() ? 'Download the CIAS circuit brick' : 'Bind every component first — a CIAS with placeholder parts is never produced'">
            Download CIAS brick
          </button>
        </div>
      </div>

      <div class="panel">
        <p class="section-label">Common mode — choke against 2×C<sub>Y</sub></p>
        <table class="data">
          <tbody>
            <tr><td>L<sub>CM</sub> required</td><td>{{ fmtSi(design.lCmRequiredH, 'H') }}</td></tr>
            <tr><td>L<sub>CM</sub> selected</td><td data-test="lcm"><strong>{{ fmtSi(design.lCmSelectedH, 'H') }}</strong> (per stage)</td></tr>
            <tr><td>C<sub>Y</sub></td><td>2 × {{ fmtSi(design.cYPerLineF, 'F') }} per stage</td></tr>
            <tr><td>Achieved CM attenuation</td>
              <td :class="design.attenuationCmDb >= aReqCm ? 'pos' : 'neg'" data-test="il-cm">{{ fmtDb(design.attenuationCmDb) }} dB</td></tr>
          </tbody>
        </table>
      </div>

      <div class="panel">
        <p class="section-label">Differential mode — leakage against C<sub>X</sub></p>
        <table class="data">
          <tbody>
            <tr><td>L<sub>DM</sub> (leakage)</td><td>{{ fmtSi(design.lDmH, 'H') }}</td></tr>
            <tr><td>C<sub>X</sub> required</td><td>{{ fmtSi(design.cXRequiredF, 'F') }}</td></tr>
            <tr><td>C<sub>X</sub> selected</td><td><strong>{{ fmtSi(design.cXSelectedF, 'F') }}</strong> (per stage)</td></tr>
            <tr><td>Achieved DM attenuation</td>
              <td :class="design.attenuationDmDb >= aReqDm ? 'pos' : 'neg'">{{ fmtDb(design.attenuationDmDb) }} dB</td></tr>
          </tbody>
        </table>
      </div>

      <div class="panel" v-if="lCmSource === 'catalog' && matchedParts().length">
        <p class="section-label">Catalog parts at {{ fmtSi(design.lCmSelectedH, 'H') }}</p>
        <table class="data" data-test="catalog-parts">
          <thead><tr><th>Part</th><th>Manufacturer</th><th>Family</th><th>Rated A</th><th>DCR</th></tr></thead>
          <tbody>
            <tr v-for="p in matchedParts()" :key="p.mpn">
              <td><strong>{{ p.mpn }}</strong></td><td>{{ p.manufacturer }}</td><td>{{ p.family || '—' }}</td>
              <td>{{ p.ratedCurrentA !== null ? p.ratedCurrentA.toFixed(1) : '—' }}</td>
              <td>{{ p.dcrOhm !== null ? fmtSi(p.dcrOhm, 'Ω') : '—' }}</td>
            </tr>
          </tbody>
        </table>
      </div>

      <div class="panel" v-if="ilCm && ilDm">
        <p class="section-label">In-circuit insertion loss — solid: nominal terminations · dashed: CISPR 17 worst case (0.1 Ω/100 Ω) · dot: your requirement</p>
        <LogChart :series="ilSeries()" :violations="requirementMarkers()" y-label="dB" :height="300" data-test="il-chart" />
        <p class="note">If the dashed worst-case curve still clears your requirement at the design frequency, termination uncertainty cannot eat the margin.</p>
      </div>

      <div class="panel" v-if="interaction">
        <p class="section-label">Input-filter interaction (Middlebrook)</p>
        <div class="row">
          <label class="field"><span>Converter min. input voltage (V)</span>
            <input v-model.number="vInMin" type="number" @change="compute" /></label>
          <label class="field"><span>Input power (W)</span>
            <input v-model.number="pIn" type="number" @change="compute" /></label>
        </div>
        <table class="data">
          <tbody>
            <tr><td>Filter resonance</td><td>{{ fmtHz(interaction.resonanceHz) }}</td></tr>
            <tr><td>Filter peak output impedance R₀</td><td>{{ fmtSi(interaction.characteristicImpedanceOhm, 'Ω') }}</td></tr>
            <tr><td>Converter input impedance V²/P</td><td>{{ fmtSi(interaction.converterInputImpedanceOhm, 'Ω') }}</td></tr>
            <tr><td>Stability margin</td>
              <td :class="interaction.marginDb >= 12 ? 'pos' : interaction.marginDb >= 6 ? '' : 'neg'" data-test="middlebrook-margin">
                {{ fmtDb(interaction.marginDb) }} dB
                <span class="note">(≥ 12 dB comfortable, &lt; 6 dB add damping: R<sub>d</sub> = {{ fmtSi(interaction.dampingResistorOhm, 'Ω') }},
                C<sub>d</sub> = {{ fmtSi(interaction.dampingCapacitorMinF, 'F') }}–{{ fmtSi(interaction.dampingCapacitorMaxF, 'F') }})</span>
              </td></tr>
          </tbody>
        </table>
      </div>

      <div class="panel" v-if="design.leakageCurrentA !== undefined">
        <p class="section-label">Safety checks (worst case: V+10 %, C+20 %)</p>
        <table class="data">
          <tbody>
            <tr><td>PE leakage current</td>
              <td :class="design.leakageCurrentA < 3.5e-3 ? 'pos' : 'neg'">
                {{ fmtSi(design.leakageCurrentA, 'A') }} <span class="note">(3.5 mA typical limit)</span></td></tr>
            <tr><td>X discharge resistor</td>
              <td>≤ {{ fmtSi(design.dischargeResistorMaxOhm, 'Ω') }} → <strong>{{ fmtSi(design.dischargeResistorOhm, 'Ω') }}</strong>
                ({{ fmtSi(design.dischargeResistorPowerW, 'W') }} continuous)</td></tr>
          </tbody>
        </table>
      </div>

      <div class="panel">
        <p class="section-label">SPICE export — filter + CISPR 16 LISN, ready for Kirchhoff / ngspice / LTspice</p>
        <pre class="code" data-test="netlist">{{ netlist }}</pre>
        <div class="row" style="margin-top: 0.6rem">
          <button class="ghost" @click="downloadNetlist">Download .cir</button>
          <button class="ghost" @click="navigator.clipboard.writeText(netlist)">Copy</button>
        </div>
      </div>
    </div>
    <div v-else class="panel">
      <p class="note">Set the requirement (or bring one over from a failed scan on the Spectrum screen) and press <em>Design filter</em>. Candidate lists take any manufacturer's catalog values; paste your preferred vendor's series to design with real parts.</p>
    </div>
  </div>
</template>
