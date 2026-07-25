<script setup>
// ANP015 line-filter designer: attenuation target in, component values out,
// with the safety checks (Y-cap leakage, X-cap discharge) and SPICE export.
import { onMounted, ref } from 'vue'
import { api } from '../engine.js'
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

onMounted(() => {
  if (store.handoff) {
    aReqCm.value = store.handoff.aReqDb
    aReqDm.value = store.handoff.aReqDb
    store.handoff = null
    compute()
  }
})

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
      lCmCandidatesH: parseList(lCandidatesMh.value, 1e-3),
      cXCandidatesF: parseList(cxCandidatesUf.value, 1e-6),
      grid: { vRms: Number(gridVrms.value), fHz: Number(gridHz.value), vSafe: 60, tDischargeS: 1 },
    }
    if (dmMode.value === 'impedance') {
      params.dmImpedanceOhm = Number(dmImpedanceOhm.value)
      params.dmImpedanceFrequencyHz = dmImpedanceMhz.value * 1e6
    } else {
      params.lDmH = lDmUh.value * 1e-6
    }
    design.value = engine.designFilter(params)
    netlist.value = engine.filterSpiceNetlist(design.value, 'cispr16')
  } catch (e) {
    error.value = e.message
  }
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
        <label class="field"><span>CM choke candidates (mH) — your catalog, any manufacturer</span>
          <input v-model="lCandidatesMh" type="text" /></label>
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
