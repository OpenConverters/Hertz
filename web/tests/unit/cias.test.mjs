// The exported brick must validate against the REAL CIAS schema (with its PEAS
// refs) — the house rule is that no schema-invalid CIAS object is ever
// produced, so this test is the gate. Schemas come from a sibling checkout
// (HERTZ_SCHEMA_ROOT, default ../../Kirchhoff/deps); a missing schema tree is
// a hard FAIL, never a skip.
import { test } from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync, readdirSync, existsSync } from 'node:fs'
import { join, resolve } from 'node:path'
import Ajv2020 from 'ajv/dist/2020.js'
import { buildFilterCias, filterComponents, filterNets } from '../../src/ciasFilter.js'

// Colon-separated roots; each root's immediate children are schema packages
// with a schemas/ dir. Defaults cover the sibling checkouts (Kirchhoff deps +
// Heaviside, which carries TDAS/CTAS).
const roots = (process.env.HERTZ_SCHEMA_ROOT ??
  [resolve(import.meta.dirname, '../../../../Kirchhoff/deps'),
   resolve(import.meta.dirname, '../../../../Heaviside')].join(':')).split(':')
for (const root of roots) {
  assert.ok(existsSync(root), `schema root not found: ${root} — set HERTZ_SCHEMA_ROOT`)
}

// addSchema only — ajv.getSchema during loading would force compilation
// before all $ref targets are registered (TDAS sorts after CIAS/PEAS).
const ajv = new Ajv2020({ strict: false, allErrors: true, logger: false })
const seenIds = new Set()
function loadDir(dir) {
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const path = join(dir, entry.name)
    if (entry.isDirectory()) {
      loadDir(path)
    } else if (entry.name.endsWith('.json')) {
      const schema = JSON.parse(readFileSync(path, 'utf8'))
      if (schema.$id && !seenIds.has(schema.$id)) {
        seenIds.add(schema.$id)
        ajv.addSchema(schema)
      }
    }
  }
}
for (const root of roots) {
  for (const pkg of readdirSync(root)) {
    const dir = join(root, pkg, 'schemas')
    if (existsSync(dir)) loadDir(dir)
  }
}
const validate = ajv.getSchema('https://psma.com/cias/CIAS.json')
assert.ok(validate, 'CIAS schema not found in schema root')

const BINDINGS = {
  CMC1: { mpn: 'ACMS-1065-332-T', manufacturer: 'Abracon' },
  C_X1: { mpn: 'EFX2S30M225C102LH', manufacturer: 'Eaton' },
  C_YL1: { mpn: 'MKY22W14703F00JB00', manufacturer: 'WIMA' },
  C_YN1: { mpn: 'MKY22W14703F00JB00', manufacturer: 'WIMA' },
  CMC2: { mpn: 'ACMS-1065-332-T', manufacturer: 'Abracon' },
  C_X2: { mpn: 'EFX2S30M225C102LH', manufacturer: 'Eaton' },
  C_YL2: { mpn: 'MKY22W14703F00JB00', manufacturer: 'WIMA' },
  C_YN2: { mpn: 'MKY22W14703F00JB00', manufacturer: 'WIMA' },
}

test('1-stage brick validates against the CIAS schema', () => {
  const brick = buildFilterCias(1, BINDINGS)
  const valid = validate(brick)
  assert.ok(valid, JSON.stringify(validate.errors, null, 1))
})

test('2-stage brick validates and welds interstage nets', () => {
  const brick = buildFilterCias(2, BINDINGS)
  assert.ok(validate(brick), JSON.stringify(validate.errors, null, 1))
  const line1 = brick.connections.find((n) => n.name === 'line_1')
  const pins = line1.endpoints.filter((e) => e.component).map((e) => `${e.component}|${e.pin}`)
  assert.ok(pins.includes('CMC1|P2') && pins.includes('CMC2|P1'), 'stages must share line_1')
  const pe = brick.connections.find((n) => n.name === 'pe')
  assert.equal(pe.endpoints.length, 1 + 2 * 2)  // pe port + two Y caps per stage
})

test('every net endpoint references a declared component or port', () => {
  const brick = buildFilterCias(2, BINDINGS)
  const componentNames = new Set(brick.components.map((c) => c.name))
  const portNames = new Set(brick.ports.map((p) => p.name))
  for (const net of brick.connections) {
    for (const ep of net.endpoints) {
      if (ep.component) assert.ok(componentNames.has(ep.component), `${net.name}: ${ep.component}`)
      else assert.ok(portNames.has(ep.port), `${net.name}: ${ep.port}`)
    }
  }
})

test('unbound components refuse to export', () => {
  assert.throws(() => buildFilterCias(1, { CMC1: { mpn: 'X' } }), /unbound components/)
})

test('view-model and brick share one truth', () => {
  assert.equal(filterComponents(2).length, 8)
  const nets = filterNets(2)
  assert.equal(new Set(nets.map((n) => n.name)).size, nets.length)
})

test('a quantity-2 X-cap bank expands into parallel siblings and validates (#294)', () => {
  const bindings = { ...BINDINGS, C_X1: { ...BINDINGS.C_X1, quantity: 2 } }
  const brick = buildFilterCias(1, bindings)
  assert.ok(validate(brick), JSON.stringify(validate.errors, null, 1))
  const names = brick.components.map((c) => c.name)
  assert.ok(names.includes('C_X1.1') && names.includes('C_X1.2') && !names.includes('C_X1'))
  // both siblings sit on BOTH X-cap nets (true parallel), and every endpoint
  // still references a declared component
  for (const netName of ['line_1', 'neutral_1']) {
    const pins = brick.connections.find((n) => n.name === netName)
      .endpoints.filter((e) => e.component).map((e) => e.component)
    assert.ok(pins.includes('C_X1.1') && pins.includes('C_X1.2'), netName)
  }
  const componentNames = new Set(names)
  for (const net of brick.connections) {
    for (const ep of net.endpoints) {
      if (ep.component) assert.ok(componentNames.has(ep.component), `${net.name}: ${ep.component}`)
    }
  }
})

test('the DC-supply topology brick validates with chassis-role ports (#292)', () => {
  const brick = buildFilterCias(1, BINDINGS, 'dc')
  assert.ok(validate(brick), JSON.stringify(validate.errors, null, 1))
  assert.equal(brick.name, 'dc-supply-filter-1stage')
  assert.match(brick.ports.find((p) => p.name === 'pe').description, /chassis/)
})

// #292 3-phase: bindings keyed by the n-line refs
const bindAll = (topology) => {
  const nLines = topology === '3ph' ? 3 : 4
  const b = {}
  for (const c of filterComponents(1, nLines)) {
    b[c.ref] = c.kind === 'cmc' ? { mpn: '744837010290', manufacturer: 'Würth Elektronik' }
      : c.kind === 'cx' ? { mpn: 'EFX2S30M225C102LH', manufacturer: 'Eaton' }
      : { mpn: 'MKY22W14703F00JB00', manufacturer: 'WIMA' }
  }
  return b
}

test('the 3-phase delta brick validates: 3 X caps across pairs, 3 Y caps, W-pins (#292)', () => {
  const brick = buildFilterCias(1, bindAll('3ph'), '3ph')
  assert.ok(validate(brick), JSON.stringify(validate.errors, null, 1))
  assert.equal(brick.name, 'three-phase-line-filter-1stage')
  assert.equal(brick.ports.length, 7)   // 3 in + 3 out + pe
  assert.equal(brick.components.filter((c) => c.name.startsWith('C_X')).length, 3)
  assert.equal(brick.components.filter((c) => c.name.startsWith('C_Y')).length, 3)
  // the wrap capacitor closes the delta: l3 net carries C_X1_31 pin 1
  const l3 = brick.connections.find((n) => n.name === 'l3_1')
  assert.ok(l3.endpoints.some((e) => e.component === 'C_X1_31' && e.pin === '1'))
  const l1 = brick.connections.find((n) => n.name === 'l1_1')
  assert.ok(l1.endpoints.some((e) => e.component === 'C_X1_31' && e.pin === '2'))
  assert.ok(l1.endpoints.some((e) => e.component === 'CMC1' && e.pin === 'W1B'))
})

test('the 3-phase + neutral brick validates: star X to the neutral rail, 4 Y caps (#292)', () => {
  const brick = buildFilterCias(1, bindAll('3phn'), '3phn')
  assert.ok(validate(brick), JSON.stringify(validate.errors, null, 1))
  assert.equal(brick.name, 'three-phase-neutral-line-filter-1stage')
  assert.equal(brick.ports.length, 9)   // 4 in + 4 out + pe
  assert.equal(brick.components.filter((c) => c.name.startsWith('C_Y')).length, 4)
  const neutral = brick.connections.find((n) => n.name === 'n_1')
  assert.equal(neutral.endpoints.filter((e) => e.component?.startsWith('C_X')).length, 3)
  // no line-to-line X capacitors in the star variant
  assert.ok(!brick.components.some((c) => c.name === 'C_X1_12'))
})
