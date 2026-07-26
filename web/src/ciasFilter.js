// The line filter as a CIAS brick (Circuit Agnostic Structure).
// One wiring function is the single source of connectivity truth: the schematic
// view-model and the exported brick both derive from it, so the drawing can
// never drift from the netlist. The exported brick references every component
// as a TAS part URI — export therefore REQUIRES all parts bound: a CIAS with an
// invented placeholder part would be schema-shaped but semantically false, and
// no invalid-or-fake CIAS object is ever produced (throws instead).
//
// nLines: 2 = single-phase L/N (or DC +/RTN), 3 = 3-phase 3-wire with delta X
// capacitors, 4 = 3-phase + neutral with star (phase-to-neutral) X capacitors.
// The 2-line naming (line/neutral, CMC pins P/S, C_YL/C_YN) is the historical
// shape and is kept EXACTLY; n-line variants use l1..l3/n rails, CMC pins
// W{i}A/W{i}B, and per-line Y refs.

// Component kinds: 'cmc', 'cx', 'cy' (pins 1,2 for capacitors).

const LINE_NAMES = { 2: ['line', 'neutral'], 3: ['l1', 'l2', 'l3'], 4: ['l1', 'l2', 'l3', 'n'] }

export function topologyLines(topology) {
  return topology === '3ph' ? 3 : topology === '3phn' ? 4 : 2
}

function xRefs(stage, nLines) {
  if (nLines === 2) return [{ ref: `C_X${stage}`, from: 0, to: 1 }]
  if (nLines === 3) {
    // delta: one X capacitor across every line pair
    return [{ ref: `C_X${stage}_12`, from: 0, to: 1 },
            { ref: `C_X${stage}_23`, from: 1, to: 2 },
            { ref: `C_X${stage}_31`, from: 2, to: 0 }]
  }
  // star: one X capacitor from every phase to the neutral rail
  return [{ ref: `C_X${stage}_1n`, from: 0, to: 3 },
          { ref: `C_X${stage}_2n`, from: 1, to: 3 },
          { ref: `C_X${stage}_3n`, from: 2, to: 3 }]
}

function yRef(stage, lineIdx, nLines) {
  if (nLines === 2) return lineIdx === 0 ? `C_YL${stage}` : `C_YN${stage}`
  return `C_Y${lineIdx + 1}_${stage}`
}

function cmcPins(lineIdx, nLines) {
  if (nLines === 2) return lineIdx === 0 ? ['P1', 'P2'] : ['S1', 'S2']
  return [`W${lineIdx + 1}A`, `W${lineIdx + 1}B`]
}

export function filterComponents(stages, nLines = 2) {
  const components = []
  for (let stage = 1; stage <= stages; stage += 1) {
    components.push({ ref: `CMC${stage}`, kind: 'cmc', stage })
    components.push(...xRefs(stage, nLines).map((x) => ({ ref: x.ref, kind: 'cx', stage })))
    for (let i = 0; i < nLines; i += 1) {
      components.push({ ref: yRef(stage, i, nLines), kind: 'cy', stage })
    }
  }
  return components
}

export function filterNets(stages, nLines = 2) {
  const lines = LINE_NAMES[nLines]
  if (!lines) throw new Error(`unsupported line count ${nLines}`)
  const nets = []
  const pe = { name: 'pe', endpoints: [{ port: 'pe' }] }
  const inPort = (i) => (nLines === 2 ? `${lines[i]}_in` : `${lines[i]}_in`)
  const outPort = (i) => (nLines === 2 ? `${lines[i]}_out` : `${lines[i]}_out`)
  for (let stage = 1; stage <= stages; stage += 1) {
    for (let i = 0; i < nLines; i += 1) {
      const [pinA, pinB] = cmcPins(i, nLines)
      nets.push({
        name: `${lines[i]}_${stage - 1}`,
        endpoints: [
          ...(stage === 1 ? [{ port: inPort(i) }] : []),
          { component: `CMC${stage}`, pin: pinA },
        ],
      })
      nets.push({
        name: `${lines[i]}_${stage}`,
        endpoints: [
          { component: `CMC${stage}`, pin: pinB },
          ...xRefs(stage, nLines).flatMap((x) => [
            ...(x.from === i ? [{ component: x.ref, pin: '1' }] : []),
            ...(x.to === i ? [{ component: x.ref, pin: '2' }] : []),
          ]),
          { component: yRef(stage, i, nLines), pin: '1' },
          ...(stage === stages ? [{ port: outPort(i) }] : []),
        ],
      })
      pe.endpoints.push({ component: yRef(stage, i, nLines), pin: '2' })
    }
  }
  // Stage s+1's input net carries the same name as stage s's output net;
  // welding same-name nets joins the stages.
  const merged = []
  for (const net of nets) {
    const twin = merged.find((m) => m.name === net.name)
    if (twin) {
      twin.endpoints.push(...net.endpoints)
    } else {
      merged.push(net)
    }
  }
  merged.push(pe)
  return merged
}

const TAS_URI = {
  cmc: (mpn) => `TAS/data/magnetics.ndjson?partNumber=${mpn}`,
  cx: (mpn) => `TAS/data/capacitors.ndjson?partNumber=${mpn}`,
  cy: (mpn) => `TAS/data/capacitors.ndjson?partNumber=${mpn}`,
}

function brickPorts(topology, nLines) {
  const dc = topology === 'dc'
  if (nLines === 2) {
    return [
      { name: 'line_in', description: dc ? 'positive supply, LISN side' : 'mains line, LISN side' },
      { name: 'neutral_in', description: dc ? 'supply return, LISN side' : 'mains neutral, LISN side' },
      { name: 'line_out', description: dc ? 'positive supply, equipment side' : 'line, equipment side' },
      { name: 'neutral_out', description: dc ? 'supply return, equipment side' : 'neutral, equipment side' },
      { name: 'pe', description: dc ? 'chassis (Y-capacitor return)' : 'protective earth (Y-capacitor return)' },
    ]
  }
  const lines = LINE_NAMES[nLines]
  const role = (i) => (i === 3 ? 'neutral' : `phase ${i + 1}`)
  return [
    ...lines.map((l, i) => ({ name: `${l}_in`, description: `${role(i)}, LISN side` })),
    ...lines.map((l, i) => ({ name: `${l}_out`, description: `${role(i)}, equipment side` })),
    { name: 'pe', description: 'protective earth (Y-capacitor return)' },
  ]
}

// bindings: { ref: { mpn, manufacturer, quantity?, ... } } — every component
// must be bound. A binding with quantity n > 1 is a PARALLEL BANK of n
// identical parts (capacitors: n x C to hit the required capacitance and
// split the ripple current); the brick expands it into n sibling components
// `${ref}.k` sharing the same nets, so the export is buildable as drawn.
// The single-instance netlist (filterNets) stays the drawing/verification
// truth — banks are an EXPANSION of it, never a different topology.
export function buildFilterCias(stages, bindings, topology = 'mains') {
  const nLines = topologyLines(topology)
  const components = filterComponents(stages, nLines)
  const unbound = components.filter((c) => !bindings[c.ref]?.mpn)
  if (unbound.length) {
    throw new Error(`cannot export CIAS: unbound components ${unbound.map((c) => c.ref).join(', ')}`)
  }
  const qtyOf = (ref) => Math.max(1, Math.round(bindings[ref].quantity ?? 1))
  const expandedNames = (ref) => qtyOf(ref) === 1 ? [ref]
    : Array.from({ length: qtyOf(ref) }, (_, k) => `${ref}.${k + 1}`)
  const name = topology === 'dc' ? `dc-supply-filter-${stages}stage`
    : topology === '3ph' ? `three-phase-line-filter-${stages}stage`
    : topology === '3phn' ? `three-phase-neutral-line-filter-${stages}stage`
    : `single-phase-line-filter-${stages}stage`
  return {
    name,
    ports: brickPorts(topology, nLines),
    components: components.flatMap((c) => expandedNames(c.ref).map((cname) => ({
      name: cname,
      data: TAS_URI[c.kind](bindings[c.ref].mpn),
    }))),
    connections: filterNets(stages, nLines).map((net) => ({
      ...net,
      endpoints: net.endpoints.flatMap((ep) => ep.component
        ? expandedNames(ep.component).map((cname) => ({ component: cname, pin: ep.pin }))
        : [ep]),
    })),
  }
}
