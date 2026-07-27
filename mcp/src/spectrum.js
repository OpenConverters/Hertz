/**
 * Hertz spectrum widget — an MCP App view.
 *
 * Renders the measured scan against the limit line and, when the engineer
 * clicks a point, tells the model what they clicked. Two distinct channels,
 * used for two distinct purposes:
 *
 *   app.updateModelContext()  ambient selection state. Silent, no new turn,
 *                             and OVERWRITES the previous value — clicking
 *                             forty points costs one line of context, not
 *                             forty.
 *   app.sendMessage()         a deliberate act that deserves an answer. Starts
 *                             a turn.
 */
import { App } from "@modelcontextprotocol/ext-apps";

const app = new App({ name: "Hertz Spectrum", version: "0.1.0" });

const el = (id) => document.getElementById(id);
let data = null;
let selected = null;

// ---------------------------------------------------------------- geometry
const PAD = { l: 56, r: 14, t: 34, b: 40 };
const W = 760;
const H = 380;

function scales(d) {
  const fs = d.trace.f_hz;
  let lo = fs[0];
  let hi = fs[fs.length - 1];
  for (const s of d.limit_segments) {
    lo = Math.min(lo, s.f_start_hz);
    hi = Math.max(hi, s.f_stop_hz);
  }
  const levels = d.trace.level_dbuv;
  let yLo = Math.min(...levels);
  let yHi = Math.max(...levels);
  for (const s of d.limit_segments) {
    yLo = Math.min(yLo, s.level_start_dbuv, s.level_stop_dbuv);
    yHi = Math.max(yHi, s.level_start_dbuv, s.level_stop_dbuv);
  }
  const pad = Math.max(6, (yHi - yLo) * 0.1);
  yLo -= pad;
  yHi += pad;

  const lgLo = Math.log10(lo);
  const lgHi = Math.log10(hi);
  return {
    x: (f) => PAD.l + ((Math.log10(f) - lgLo) / (lgHi - lgLo)) * (W - PAD.l - PAD.r),
    y: (v) => PAD.t + (1 - (v - yLo) / (yHi - yLo)) * (H - PAD.t - PAD.b),
    invX: (px) => Math.pow(10, lgLo + ((px - PAD.l) / (W - PAD.l - PAD.r)) * (lgHi - lgLo)),
    lo, hi, yLo, yHi,
  };
}

function fmtHz(f) {
  if (f >= 1e6) return `${(f / 1e6).toPrecision(4).replace(/\.?0+$/, "")} MHz`;
  if (f >= 1e3) return `${(f / 1e3).toPrecision(4).replace(/\.?0+$/, "")} kHz`;
  return `${f.toFixed(0)} Hz`;
}

// ------------------------------------------------------------------ render
function render() {
  if (!data) return;
  const s = scales(data);
  const parts = [];

  // decade gridlines
  for (let d = Math.ceil(Math.log10(s.lo)); Math.pow(10, d) <= s.hi; d++) {
    const f = Math.pow(10, d);
    const px = s.x(f);
    parts.push(
      `<line class="grid" x1="${px}" y1="${PAD.t}" x2="${px}" y2="${H - PAD.b}"/>`,
      `<text class="tick" x="${px}" y="${H - PAD.b + 15}" text-anchor="middle">${fmtHz(f)}</text>`
    );
  }
  const yStep = (s.yHi - s.yLo) > 60 ? 20 : 10;
  for (let v = Math.ceil(s.yLo / yStep) * yStep; v <= s.yHi; v += yStep) {
    const py = s.y(v);
    parts.push(
      `<line class="grid" x1="${PAD.l}" y1="${py}" x2="${W - PAD.r}" y2="${py}"/>`,
      `<text class="tick" x="${PAD.l - 7}" y="${py + 4}" text-anchor="end">${v}</text>`
    );
  }

  // Limit: ONE polyline per segment. Joining them would draw a limit across
  // frequencies the standard leaves unregulated.
  for (const seg of data.limit_segments) {
    parts.push(
      `<line class="limit" x1="${s.x(seg.f_start_hz)}" y1="${s.y(seg.level_start_dbuv)}" ` +
      `x2="${s.x(seg.f_stop_hz)}" y2="${s.y(seg.level_stop_dbuv)}"/>`
    );
  }

  // unswept regions inside coverage
  for (const h of data.unswept || []) {
    const x0 = s.x(h.f_lo_hz);
    const x1 = s.x(h.f_hi_hz);
    parts.push(
      `<rect class="hole" x="${x0}" y="${PAD.t}" width="${Math.max(1, x1 - x0)}" height="${H - PAD.t - PAD.b}"/>`
    );
  }

  // measured trace
  const pts = data.trace.f_hz
    .map((f, i) => `${s.x(f).toFixed(1)},${s.y(data.trace.level_dbuv[i]).toFixed(1)}`)
    .join(" ");
  parts.push(`<polyline class="trace" points="${pts}"/>`);

  // offenders
  for (const o of data.offenders || []) {
    if (o.margin_db >= 0) continue;
    parts.push(
      `<circle class="offender" cx="${s.x(o.f_hz)}" cy="${s.y(o.level_dbuv)}" r="4.5"/>`
    );
  }

  if (selected) {
    parts.push(
      `<line class="cursor" x1="${s.x(selected.f_hz)}" y1="${PAD.t}" x2="${s.x(selected.f_hz)}" y2="${H - PAD.b}"/>`,
      `<circle class="sel" cx="${s.x(selected.f_hz)}" cy="${s.y(selected.level_dbuv)}" r="5.5"/>`
    );
  }

  parts.push(
    `<text class="axis" x="${PAD.l}" y="${PAD.t - 14}">dBµV</text>`
  );

  el("chart").innerHTML = parts.join("");
}

// ------------------------------------------------------------------ picking
function nearest(px) {
  const s = scales(data);
  const fTarget = s.invX(px);
  const fs = data.trace.f_hz;
  let best = 0;
  let bestD = Infinity;
  for (let i = 0; i < fs.length; i++) {
    const d = Math.abs(Math.log10(fs[i]) - Math.log10(fTarget));
    if (d < bestD) { bestD = d; best = i; }
  }
  return { f_hz: fs[best], level_dbuv: data.trace.level_dbuv[best] };
}

function limitAt(f) {
  for (const seg of data.limit_segments) {
    if (f >= seg.f_start_hz && f <= seg.f_stop_hz) {
      const frac =
        (Math.log10(f) - Math.log10(seg.f_start_hz)) /
        (Math.log10(seg.f_stop_hz) - Math.log10(seg.f_start_hz));
      return seg.level_start_dbuv + frac * (seg.level_stop_dbuv - seg.level_start_dbuv);
    }
  }
  return null; // genuinely unregulated here — not a missing value
}

function describe(p) {
  const lim = limitAt(p.f_hz);
  const harmonic =
    data.f_sw_hz ? ` ~harmonic ${Math.round(p.f_hz / data.f_sw_hz)} of ${fmtHz(data.f_sw_hz)}` : "";
  if (lim === null) {
    return `${fmtHz(p.f_hz)}: ${p.level_dbuv.toFixed(1)} dBµV — outside ${data.limit_name} coverage (unregulated here)${harmonic}`;
  }
  const margin = lim - p.level_dbuv;
  return (
    `${fmtHz(p.f_hz)}: ${p.level_dbuv.toFixed(1)} dBµV vs ${lim.toFixed(1)} limit — ` +
    `${margin >= 0 ? "passing" : "EXCEEDS"} by ${Math.abs(margin).toFixed(1)} dB${harmonic}`
  );
}

async function select(p) {
  selected = p;
  render();
  const text = describe(p);
  el("readout").textContent = text;
  el("ask").disabled = false;

  // Ambient: the model sees the current selection on its next turn. Overwrite
  // semantics mean this stays one line no matter how much the user clicks.
  await app.updateModelContext({
    content: [{ type: "text", text: `[spectrum selection] ${text}` }],
    structuredContent: {
      selected_f_hz: p.f_hz,
      selected_level_dbuv: p.level_dbuv,
      limit_dbuv: limitAt(p.f_hz),
      limit_name: data.limit_name,
    },
  });
}

// -------------------------------------------------------------------- setup
// Handlers MUST be registered before connect() — the host may deliver the
// tool result during the ui/initialize handshake, and a listener added after
// that point misses it.
app.ontoolresult = (result) => {
  const sc = result.structuredContent;
  if (!sc || !sc.trace) {
    el("readout").textContent = "No spectrum in tool result.";
    return;
  }
  data = sc;
  selected = null;
  el("verdict").textContent = sc.verdict;
  el("verdict").className = `verdict ${sc.verdict.toLowerCase()}`;
  el("subtitle").textContent =
    `${sc.limit_name} · worst ${sc.worst.margin_db >= 0 ? "+" : ""}` +
    `${sc.worst.margin_db.toFixed(1)} dB at ${fmtHz(sc.worst.f_hz)}` +
    (sc.f_sw_hz ? ` · f_sw ≈ ${fmtHz(sc.f_sw_hz)}` : "") +
    (sc.trace.decimated_from > sc.trace.f_hz.length
      ? ` · ${sc.trace.decimated_from} pts → ${sc.trace.f_hz.length} shown (peak-preserving)`
      : "");
  el("holes").style.display = (sc.unswept || []).length ? "block" : "none";
  if ((sc.unswept || []).length) {
    el("holes").textContent =
      `⚠ ${sc.unswept.length} unswept region(s) inside the regulated range — ` +
      `this scan does not measure the whole band.`;
  }
  render();
};

el("chart").addEventListener("click", (ev) => {
  if (!data) return;
  const r = el("chart").getBoundingClientRect();
  const px = ((ev.clientX - r.left) / r.width) * W;
  select(nearest(px));
});

el("ask").addEventListener("click", async () => {
  if (!selected) return;
  // Deliberate act → a real turn.
  await app.sendMessage({
    role: "user",
    content: [{
      type: "text",
      text: `Explain the emission at ${fmtHz(selected.f_hz)} (${describe(selected)}) and how to attenuate it.`,
    }],
  });
});

el("fullscreen").addEventListener("click", () => {
  app.requestDisplayMode({ mode: "fullscreen" }).catch(() => {});
});

await app.connect();
