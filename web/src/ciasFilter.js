// The single-phase line filter as a CIAS brick (Circuit Agnostic Structure).
// One wiring function is the single source of connectivity truth: the schematic
// view-model and the exported brick both derive from it, so the drawing can
// never drift from the netlist. The exported brick references every component
// as a TAS part URI — export therefore REQUIRES all parts bound: a CIAS with an
// invented placeholder part would be schema-shaped but semantically false, and
// no invalid-or-fake CIAS object is ever produced (throws instead).

// Component kinds: 'cmc' (pins P1,P2 line winding / S1,S2 neutral winding),
// 'cx' and 'cy' (pins 1,2). References are brick-local: CMC1, C_X1, C_YL1, ...

export function filterComponents(stages) {
  const components = []
  for (let stage = 1; stage <= stages; stage += 1) {
    components.push(
      { ref: `CMC${stage}`, kind: 'cmc', stage },
      { ref: `C_X${stage}`, kind: 'cx', stage },
      { ref: `C_YL${stage}`, kind: 'cy', stage },
      { ref: `C_YN${stage}`, kind: 'cy', stage },
    )
  }
  return components
}

export function filterNets(stages) {
  const nets = []
  const pe = { name: 'pe', endpoints: [{ port: 'pe' }] }
  for (let stage = 1; stage <= stages; stage += 1) {
    const lineIn = stage === 1 ? { port: 'line_in' } : null
    const neutralIn = stage === 1 ? { port: 'neutral_in' } : null
    nets.push({
      name: `line_${stage - 1}`,
      endpoints: [
        ...(lineIn ? [lineIn] : []),
        { component: `CMC${stage}`, pin: 'P1' },
      ],
    })
    nets.push({
      name: `neutral_${stage - 1}`,
      endpoints: [
        ...(neutralIn ? [neutralIn] : []),
        { component: `CMC${stage}`, pin: 'S1' },
      ],
    })
    nets.push({
      name: `line_${stage}`,
      endpoints: [
        { component: `CMC${stage}`, pin: 'P2' },
        { component: `C_X${stage}`, pin: '1' },
        { component: `C_YL${stage}`, pin: '1' },
        ...(stage === stages ? [{ port: 'line_out' }] : []),
      ],
    })
    nets.push({
      name: `neutral_${stage}`,
      endpoints: [
        { component: `CMC${stage}`, pin: 'S2' },
        { component: `C_X${stage}`, pin: '2' },
        { component: `C_YN${stage}`, pin: '1' },
        ...(stage === stages ? [{ port: 'neutral_out' }] : []),
      ],
    })
    pe.endpoints.push({ component: `C_YL${stage}`, pin: '2' },
                      { component: `C_YN${stage}`, pin: '2' })
  }
  // Stage s+1's input net carries the same name as stage s's output net
  // (line_{s} / neutral_{s}); welding same-name nets joins the stages.
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

// bindings: { ref: { mpn, manufacturer, ... } } — every component must be bound.
export function buildFilterCias(stages, bindings) {
  const components = filterComponents(stages)
  const unbound = components.filter((c) => !bindings[c.ref]?.mpn)
  if (unbound.length) {
    throw new Error(`cannot export CIAS: unbound components ${unbound.map((c) => c.ref).join(', ')}`)
  }
  return {
    name: `single-phase-line-filter-${stages}stage`,
    ports: [
      { name: 'line_in', description: 'mains line, LISN side' },
      { name: 'neutral_in', description: 'mains neutral, LISN side' },
      { name: 'line_out', description: 'line, equipment side' },
      { name: 'neutral_out', description: 'neutral, equipment side' },
      { name: 'pe', description: 'protective earth (Y-capacitor return)' },
    ],
    components: components.map((c) => ({
      name: c.ref,
      data: TAS_URI[c.kind](bindings[c.ref].mpn),
    })),
    connections: filterNets(stages),
  }
}
