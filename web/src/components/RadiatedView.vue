<script setup>
// Radiated pre-scan SCREENING estimator: a measured common-mode current
// spectrum (current probe / absorbing clamp, dBµA) in, an E-field estimate
// against the CISPR 32 radiated limits out. The model is the classic
// electrically-short CM radiator — a triage tool, never a measurement.
import { ref, onMounted } from 'vue'
import LogChart from './LogChart.vue'
import { api } from '../engine.js'
import { fmtHz, fmtDb } from '../format.js'

const cableM = ref(1)
const distance = ref('3')     // measurement distance the limit is scaled to
const cls = ref('b')
const margin = ref(6)         // buffer on the CM-attenuation target (dB)
const cmRef = ref(150)        // CM loop impedance (Ω) — genuinely uncertain (it IS the ±20 dB)
const maxTurns = ref(3)       // most cable passes the picker may use on one core
const cableOd = ref(null)     // OPTIONAL cable outer Ø (mm) — a recommended core must fit it
const trace = ref(null)       // {name, frequenciesHz, dbua}
const result = ref(null)      // {efield, uncertainty, analysis, limitRuns, mitigation}
const error = ref('')
const dragOver = ref(false)
const fileInput = ref(null)

// Real ferrite candidates: the TAS catalog slice (subtype chipBead) exported by
// scripts/export_ferrites.py — a main slice + a curves side file (|Z| magnitude
// with Bode-reconstructed phase), same as the CMC catalog the filter designer
// uses. Fetched once on mount; the picker consumes the curve-carrying parts.
const ferriteCatalog = ref(null)   // {version, count, parts:[{mpn, zAt100MHzOhm, ...}]}
const ferriteCurves = ref(null)    // {version, curves:{mpn:{f, re, im, rec}}}
const ferriteState = ref('loading') // 'loading' | 'ready' | 'unavailable'
let ferriteFetch = null
onMounted(() => {
  ferriteFetch = (async () => {
    try {
      const [main, curves] = await Promise.all([
        fetch('/kelvin/hertz-ferrites.v1.json'),
        fetch('/kelvin/hertz-ferrites-curves.v1.json'),
      ])
      if (!main.ok || !curves.ok) throw new Error(`${main.status}/${curves.status}`)
      ferriteCatalog.value = await main.json()
      ferriteCurves.value = await curves.json()
      ferriteState.value = 'ready'
    } catch {
      ferriteState.value = 'unavailable'
    }
  })()
})

const standardId = () => `cispr32_rad_${cls.value}_${distance.value}m`

// Curve-carrying catalog parts whose |Z|(f) spans [fLo, fHi], as FerritePart
// candidates (magnitude + reconstructed phase), sliced to the band (plus one
// bracketing point each side, for interpolation) and ordered SMALL-FIRST so the
// picker returns the least adequate part. Parts that do not cover the whole
// flagged band are dropped and COUNTED — never extrapolated, never hidden.
//
// This is a CABLE-mitigation picker, so real clamp-on / cable ferrite cores
// (kind 'cableCore') are the physically-correct candidates — you thread the
// cable through them. SMD suppression beads (kind 'bead') are used only if no
// cable cores are catalogued at all, and the UI says so.
function ferriteCandidates(fLo, fHi) {
  const curves = ferriteCurves.value?.curves || {}
  const all = ferriteCatalog.value?.parts || []
  const haveCableCores = all.some((p) => p.kind === 'cableCore')
  let pool = haveCableCores ? all.filter((p) => p.kind === 'cableCore') : all
  // Cable-fit: if a cable outer Ø is given, a core can only be recommended if the
  // cable physically threads through its hole. A core whose max cable Ø is
  // known-smaller — or has no published hole size — cannot be guaranteed to fit,
  // so it is excluded (a recommendation must be physically installable).
  let fitDropped = 0
  const odM = Number(cableOd.value) > 0 ? Number(cableOd.value) / 1000 : null
  if (odM != null && haveCableCores) {
    const before = pool.length
    pool = pool.filter((p) => typeof p.cableMaxM === 'number' && p.cableMaxM >= odM)
    fitDropped = before - pool.length
  }
  const parts = []
  let coverageDropped = 0
  let noCurveDropped = 0
  for (const p of pool) {
    const c = curves[p.mpn]
    // No curve in the side file — count it. The export already excludes parts
    // that carry no resolvable curve, so this is normally 0; leaving it a silent
    // `continue` meant a main/curves file that drifted out of step would shrink
    // the pool invisibly. Nothing here is allowed to vanish uncounted.
    if (!c || !c.f?.length) { noCurveDropped += 1; continue }
    if (c.f[0] > fLo || c.f[c.f.length - 1] < fHi) { coverageDropped += 1; continue }
    // keep the in-band points plus one bracket below fLo and above fHi
    let lo = 0
    while (lo + 1 < c.f.length && c.f[lo + 1] <= fLo) lo += 1
    let hi = c.f.length - 1
    while (hi > 0 && c.f[hi - 1] >= fHi) hi -= 1
    const f = [], zOhm = [], phaseRad = []
    for (let i = lo; i <= hi; i += 1) {
      f.push(c.f[i]); zOhm.push(Math.hypot(c.re[i], c.im[i])); phaseRad.push(Math.atan2(c.im[i], c.re[i]))
    }
    parts.push({ name: p.mpn, frequenciesHz: f, zOhm, phaseRad, _z: p.zAt100MHzOhm ?? p.zPeakOhm ?? 0, rec: !!c.rec })
  }
  parts.sort((a, b) => a._z - b._z)
  // Catalog-level exclusions from the export, for the pool actually in use. These
  // parts never reach the browser at all, so without carrying the tally forward
  // the picker would look like it had searched the whole catalog.
  const kind = haveCableCores ? 'cableCore' : 'bead'
  const catalogExcluded = ferriteCatalog.value?.excluded?.[kind] ?? null
  return { parts, coverageDropped, noCurveDropped, fitDropped,
           catalogExcluded, usingBeads: !haveCableCores }
}

async function ingest(files) {
  const file = files[0]
  if (!file) return
  error.value = ''
  try {
    const engine = await api()
    const text = typeof file === 'string' ? file : await file.text()
    const name = typeof file === 'string' ? 'example CM current comb' : file.name
    const parsed = engine.parseSpectrumCsv(text, '', '', 0)
    if (parsed.levelUnit !== 'dbua') {
      throw new Error('this screen needs a COMMON-MODE CURRENT spectrum — the file header must ' +
        'state dBµA (current-probe / absorbing-clamp export). Voltage scans belong on Spectrum.')
    }
    trace.value = { name, frequenciesHz: parsed.frequenciesHz, dbua: parsed.levelsDbuv }
    await estimate()
  } catch (e) {
    error.value = e.message
    trace.value = null
    result.value = null
  }
}

async function estimate() {
  if (!trace.value) return
  error.value = ''
  result.value = null
  try {
    const engine = await api()
    const est = engine.radiatedEstimate(trace.value.frequenciesHz, trace.value.dbua,
                                        Number(cableM.value), Number(distance.value))
    const analysis = engine.limitAnalysis(standardId(), 'quasi_peak',
                                          trace.value.frequenciesHz, est.efieldDbuvm)
    const fLo = Math.max(30e6, trace.value.frequenciesHz[0])
    const fHi = Math.min(1000e6, trace.value.frequenciesHz[trace.value.frequenciesHz.length - 1])
    const limitRuns = fLo < fHi ? engine.limitPolyline(standardId(), 'quasi_peak', fLo, fHi) : null
    const mitigation = await cableMitigation(engine, analysis)
    result.value = { efield: est.efieldDbuvm, uncertainty: est.modelUncertaintyDb, analysis, limitRuns, mitigation }
  } catch (e) {
    error.value = e.message
  }
}

// Turn the screen into a fix: the CM-CURRENT attenuation target (dB vs freq) over
// the covered radiated band, then the least catalogued ferrite that meets it.
// Returns null off-band, {needed:false} when the screen already passes, else
// {pick,...}. A pick needs the catalog — if it is unreachable, that is surfaced
// (the target is still shown), never papered over with invented parts.
async function cableMitigation(engine, analysis) {
  const freqs = trace.value.frequenciesHz
  const idx = analysis.limitsDbuv.map((l, i) => (l != null ? i : -1)).filter((i) => i >= 0)
  if (!idx.length) return null
  const fc = idx.map((i) => freqs[i])
  const target = engine.radiatedCmTarget(fc, idx.map((i) => trace.value.dbua[i]),
    Number(cableM.value), Number(distance.value), idx.map((i) => analysis.limitsDbuv[i]), Number(margin.value))
  if (!target.needed) return { needed: false, target }
  await ferriteFetch
  if (ferriteState.value !== 'ready') {
    return { needed: true, target, pick: { error: 'ferrite catalog unreachable from this deployment — the target above is the attenuation to add; select a cable ferrite / clip-on CM core whose |Z| curve meets it' } }
  }
  const { parts, coverageDropped, noCurveDropped, fitDropped, catalogExcluded, usingBeads } =
    ferriteCandidates(fc[0], fc[fc.length - 1])
  const bookkeeping = { coverageDropped, noCurveDropped, fitDropped, catalogExcluded, usingBeads }
  if (!parts.length) {
    const fitNote = fitDropped ? `${fitDropped} excluded for not fitting a ${cableOd.value} mm cable; ` : ''
    return { needed: true, target, ...bookkeeping,
             pick: { error: `no catalogued ${usingBeads ? 'ferrite bead' : 'cable ferrite core'} covers the whole flagged band (${fmtHz(fc[0])}–${fmtHz(fc[fc.length - 1])}) — ${fitNote}${coverageDropped} excluded for insufficient |Z|-curve coverage` } }
  }
  let pick = null
  try { pick = engine.cableMitigation(fc, target.target, parts, Number(cmRef.value), Number(maxTurns.value)) }
  catch (e) { pick = { error: e.message } }
  if (pick && pick.partName) pick.rec = parts.find((p) => p.name === pick.partName)?.rec === true
  return { needed: true, target, pick, candidateCount: parts.length, ...bookkeeping }
}

// The parts the picker never saw: excluded at EXPORT for carrying too little
// measured impedance to place against a band. A single measured |Z| point is a
// real part we cannot position (one point fixes no slope, so any curve through
// it is invented); no point at all is missing source data. Surfaced so the pick
// reads as "best of a stated pool", not "best in the world".
function catalogExclusionNote(mit) {
  const ex = mit.catalogExcluded
  if (!ex) return ''
  const noun = mit.usingBeads ? 'beads' : 'cable cores'
  const bits = []
  if (ex.singlePointOnly) bits.push(`${ex.singlePointOnly} publish a single |Z| point only`)
  if (ex.noImpedanceData) bits.push(`${ex.noImpedanceData} publish no impedance data`)
  if (!bits.length) return ''
  return ` A further ${bits.join(' and ')} — those ${noun} carry too little measured `
       + 'impedance to place against a band, and are not candidates here.'
}

// catalog row behind the pick, for the maker / form / fit / headline-|Z| readout
function pickPart() {
  const name = result.value?.mitigation?.pick?.partName
  if (!name) return null
  return (ferriteCatalog.value?.parts || []).find((p) => p.mpn === name) || null
}
const FORM_LABEL = { solidRing: 'solid ring', snapOn: 'snap-on', split: 'split', screwable: 'screwable' }
function pickMeta() {
  const p = pickPart()
  if (!p) return ''
  const bits = [p.manufacturer]
  if (FORM_LABEL[p.mountingForm]) bits.push(FORM_LABEL[p.mountingForm])
  if (p.cableMaxM) bits.push(`≤${Math.round(p.cableMaxM * 1000)} mm cable`)
  if (p.zAt100MHzOhm) bits.push(`${Math.round(p.zAt100MHzOhm)} Ω @100 MHz`)
  return bits.join(' · ')
}

// clearly-synthetic example: a 12 µA switching comb decaying over 30–300 MHz
function exampleCsv() {
  const lines = ['Frequency (MHz),CM current (dBuA)']
  for (let f = 30e6; f <= 300e6; f *= 1.02) {
    const comb = Math.abs(Math.sin(f / 7e6)) > 0.93 ? 6 : 0
    lines.push((f / 1e6).toFixed(3) + ',' + (21.6 - 8 * Math.log10(f / 30e6) + comb).toFixed(2))
  }
  return lines.join('\n')
}

const series = () => [{
  id: 'e', label: 'estimated E-field (screening)', color: 'var(--s-1)',
  points: trace.value.frequenciesHz.map((f, i) => ({ f, v: result.value.efield[i] })),
}]
const refRuns = () => (result.value.limitRuns ? [{
  label: 'QP limit', color: 'var(--s-limit)', dash: '7 5', runs: result.value.limitRuns.runs,
}] : [])
const violations = () => {
  const points = []
  result.value.analysis.marginsDb.forEach((m, i) => {
    if (m !== null && m < 0) points.push({ f: trace.value.frequenciesHz[i], v: result.value.efield[i] })
  })
  return points
}
</script>

<template>
  <div class="grid2">
    <div>
      <div class="panel">
        <p class="section-label">Common-mode current input</p>
        <p class="note" style="margin-bottom: 0.6rem">Why this screen: radiated emissions above
          30 MHz are usually driven by common-mode current on the cables. Measure I<sub>CM</sub>
          with a current probe (dBµA) and this estimates the E-field an OATS/SAC would see —
          the other half of the EMC report, as a screening number.</p>
        <div class="drop" :class="{ over: dragOver }" role="button" tabindex="0"
             @click="fileInput.click()" @keydown.enter="fileInput.click()"
             @dragover.prevent="dragOver = true" @dragleave="dragOver = false"
             @drop.prevent="dragOver = false; ingest([...$event.dataTransfer.files])">
          Drop a CM-current CSV here (header must state dBµA).
        </div>
        <input ref="fileInput" type="file" accept=".csv,.txt" hidden
               @change="ingest([...$event.target.files]); $event.target.value = ''" />
        <div class="row" style="margin-top: 0.7rem">
          <button class="ghost" data-test="radiated-example" @click="ingest([exampleCsv()])">Load synthetic example</button>
        </div>
        <div class="row" style="margin-top: 0.7rem">
          <label class="field"><span>Cable / harness length (m)</span>
            <input v-model.number="cableM" type="number" min="0.1" step="0.1" data-test="cable-length" @change="estimate" /></label>
          <label class="field"><span>Limit distance</span>
            <select v-model="distance" data-test="radiated-distance" @change="estimate">
              <option value="3">3 m</option><option value="10">10 m</option>
            </select></label>
        </div>
        <label class="field"><span>CISPR 32 class</span>
          <select v-model="cls" data-test="radiated-class" @change="estimate">
            <option value="b">Class B (residential)</option>
            <option value="a">Class A (industrial)</option>
          </select></label>
        <div class="row" style="margin-top: 0.7rem">
          <label class="field"><span>CM loop impedance (Ω)</span>
            <input v-model.number="cmRef" type="number" min="1" step="1" data-test="cm-ref" @change="estimate" /></label>
          <label class="field"><span>Max cable turns</span>
            <input v-model.number="maxTurns" type="number" min="1" max="10" step="1" data-test="max-turns" @change="estimate" /></label>
          <label class="field"><span>Cable Ø (mm, optional)</span>
            <input v-model.number="cableOd" type="number" min="0" step="0.5" placeholder="any" data-test="cable-od" @change="estimate" /></label>
        </div>
        <p class="note">Model: |E| = 1.257·10⁻⁶ · f · L<sub>eff</sub> · I<sub>CM</sub> / d, the
          electrically-short CM radiator (Ott/Paul), L<sub>eff</sub> capped at λ/4 above cable
          resonance — the model behind “≈5 µA on 1 m fails Class B at 3 m”.</p>
      </div>
      <div v-if="error" class="err" data-test="error">{{ error }}</div>
    </div>

    <div>
      <div v-if="result" class="panel panel-hi" style="margin-bottom: 1rem">
        <div class="readout">
          <div class="cell"><b>Screening verdict</b>
            <span class="chip" :class="result.analysis.pass ? 'pass' : 'fail'" data-test="radiated-verdict">
              {{ result.analysis.pass ? 'PASS (screen)' : 'FAIL (screen)' }}</span></div>
          <div class="cell"><b>Worst margin</b>
            <span :style="{ color: result.analysis.worst.marginDb < 0 ? 'var(--fault)' : 'var(--ok)' }">
              {{ fmtDb(result.analysis.worst.marginDb) }}</span><span class="unit">dB</span></div>
          <div class="cell"><b>at</b><span>{{ fmtHz(result.analysis.worst.frequencyHz) }}</span></div>
          <div class="cell"><b>E / limit</b>
            <span>{{ fmtDb(result.analysis.worst.levelDbuv) }} / {{ fmtDb(result.analysis.worst.limitDbuv) }}</span>
            <span class="unit">dBµV/m</span></div>
        </div>
        <p class="note" data-test="radiated-uncertainty" style="color: var(--amber)">
          SCREENING ESTIMATE, ±{{ result.uncertainty }} dB typical: real cables resonate and couple
          far off this worst-case short-radiator model. A pass with more than
          {{ result.uncertainty }} dB in hand is meaningful; anything closer — and every fail —
          belongs on an open-area test site or in a chamber. This is triage, not a measurement.</p>
      </div>
      <div v-if="result && result.mitigation" class="panel" style="margin-bottom: 1rem" data-test="mitigation">
        <p class="section-label">Cable common-mode mitigation</p>
        <template v-if="result.mitigation.needed">
          <p class="note">Because |E| ∝ I<sub>CM</sub>, the field is over the limit by exactly the
            common-mode <b>current</b> attenuation you must add — worst case
            <b>{{ fmtDb(result.mitigation.target.governingDb) }} dB at {{ fmtHz(result.mitigation.target.governingHz) }}</b>.
            Up here a mains filter's choke has self-resonated, so the fix is a cable ferrite / clip-on CM choke.</p>
          <div v-if="result.mitigation.pick.error" class="err">{{ result.mitigation.pick.error }}</div>
          <div v-else class="readout">
            <div class="cell"><b>Suggested part</b>
              <span data-test="mitigation-part">{{ result.mitigation.pick.partName
                }}<span v-if="result.mitigation.pick.rec" title="phase reconstructed from |Z| (Bode gain–phase)"> ~</span></span>
              <span v-if="pickPart()" class="unit" data-test="mitigation-meta">{{ pickMeta() }}</span></div>
            <div class="cell"><b>Cable turns</b><span>{{ result.mitigation.pick.turns }}</span></div>
            <div class="cell"><b>Meets target</b>
              <span class="chip" :class="result.mitigation.pick.meetsTarget ? 'pass' : 'fail'" data-test="mitigation-meets">
                {{ result.mitigation.pick.meetsTarget ? 'YES' : 'NO — best effort' }}</span></div>
            <div class="cell"><b>Worst margin</b>
              <span :style="{ color: result.mitigation.pick.worstMarginDb < 0 ? 'var(--fault)' : 'var(--ok)' }">
                {{ fmtDb(result.mitigation.pick.worstMarginDb) }}</span><span class="unit">dB</span></div>
          </div>
          <p v-if="!result.mitigation.usingBeads" class="note">Picked from <b>{{ result.mitigation.candidateCount }}</b>
            real clamp-on / cable ferrite cores (9 makers) whose measured |Z| curve covers the band (MAS catalog,
            <code>cableCore</code> — 1-turn |Z|; phase reconstructed from |Z| where only magnitude is published, marked <b>~</b>).
            Thread the cable through the core; more turns raise |Z| by ≈turns². The CM loop impedance
            ({{ cmRef }} Ω) is genuinely uncertain — it IS the ±{{ result.uncertainty }} dB — so treat the pick
            as a starting part, not a spec.<span v-if="result.mitigation.coverageDropped">
            {{ result.mitigation.coverageDropped }} cores were excluded for not covering the whole band.</span><span
            v-if="result.mitigation.fitDropped"> {{ result.mitigation.fitDropped }} excluded for not fitting a
            {{ cableOd }} mm cable.</span><span v-if="result.mitigation.noCurveDropped">
            {{ result.mitigation.noCurveDropped }} excluded for carrying no |Z| curve in this deployment.</span><span
            data-test="catalog-excluded">{{ catalogExclusionNote(result.mitigation) }}</span></p>
          <p v-else class="note">Picked from <b>{{ result.mitigation.candidateCount }}</b> catalogued ferrite beads
            whose measured |Z| curve covers the band (<code>chipBead</code> — SMD suppression beads; phase
            reconstructed from |Z|, marked <b>~</b>). No cable cores are catalogued in this deployment, so these
            single-pass beads stand in — same series-impedance model, but not clamp-on parts. The CM loop
            impedance ({{ cmRef }} Ω) is genuinely uncertain — it IS the ±{{ result.uncertainty }} dB — so
            treat the pick as a starting part, not a spec.<span v-if="result.mitigation.coverageDropped">
            {{ result.mitigation.coverageDropped }} parts were excluded for not covering the whole band.</span><span
            v-if="result.mitigation.noCurveDropped"> {{ result.mitigation.noCurveDropped }} excluded for
            carrying no |Z| curve in this deployment.</span><span
            data-test="catalog-excluded">{{ catalogExclusionNote(result.mitigation) }}</span></p>
        </template>
        <p v-else class="note" data-test="mitigation-none">Screen passes — no cable common-mode mitigation indicated.</p>
      </div>
      <LogChart v-if="result" :series="series()" :ref-runs="refRuns()" :violations="violations()"
                violation-label="over limit" y-label="dBµV/m" data-test="radiated-chart" />
      <div v-else class="screen"><p class="note" style="padding: 2rem 1rem">Feed a CM-current
        spectrum (or the synthetic example) — the estimated E-field lands here against the
        CISPR 32 radiated limit.</p></div>
    </div>
  </div>
</template>
