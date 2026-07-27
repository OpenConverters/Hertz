/**
 * Generic log-frequency curve viewer — an MCP App view.
 *
 * One widget for every "quantity vs frequency" result Hertz produces: LISN
 * impedance, filter insertion loss, receiver detector levels, radiated E-field
 * estimates, bare limit lines. The server decides the series; this only draws
 * and reports what was clicked.
 */
import { App } from "@modelcontextprotocol/ext-apps";

const app = new App({ name: "Hertz Curves", version: "0.1.0" });

const el = (id) => document.getElementById(id);
let data = null;
let selected = null;

const PAD = { l: 62, r: 14, t: 34, b: 40 };
const W = 760;
const H = 380;
const COLORS = ["var(--c1)", "var(--c2)", "var(--c3)", "var(--c4)", "var(--c5)"];

function bounds(d) {
  let xLo = Infinity, xHi = -Infinity, yLo = Infinity, yHi = -Infinity;
  for (const s of d.series) {
    for (const [x, y] of s.points) {
      if (!(x > 0) || !isFinite(y)) continue;
      if (x < xLo) xLo = x;
      if (x > xHi) xHi = x;
      if (y < yLo) yLo = y;
      if (y > yHi) yHi = y;
    }
  }
  const pad = Math.max(3, (yHi - yLo) * 0.1);
  return { xLo, xHi, yLo: yLo - pad, yHi: yHi + pad };
}

function scales(d) {
  const b = bounds(d);
  const lgLo = Math.log10(b.xLo);
  const lgHi = Math.log10(b.xHi);
  const span = lgHi - lgLo || 1;
  return {
    ...b,
    x: (v) => PAD.l + ((Math.log10(v) - lgLo) / span) * (W - PAD.l - PAD.r),
    y: (v) => PAD.t + (1 - (v - b.yLo) / (b.yHi - b.yLo || 1)) * (H - PAD.t - PAD.b),
    invX: (px) => Math.pow(10, lgLo + ((px - PAD.l) / (W - PAD.l - PAD.r)) * span),
  };
}

function fmtHz(f) {
  if (f >= 1e9) return `${trim(f / 1e9)} GHz`;
  if (f >= 1e6) return `${trim(f / 1e6)} MHz`;
  if (f >= 1e3) return `${trim(f / 1e3)} kHz`;
  return `${trim(f)} Hz`;
}
const trim = (v) => String(Number(v.toPrecision(4)));

function render() {
  if (!data) return;
  const s = scales(data);
  const p = [];

  for (let d = Math.ceil(Math.log10(s.xLo)); Math.pow(10, d) <= s.xHi; d++) {
    const f = Math.pow(10, d);
    const px = s.x(f);
    p.push(
      `<line class="grid" x1="${px}" y1="${PAD.t}" x2="${px}" y2="${H - PAD.b}"/>`,
      `<text class="tick" x="${px}" y="${H - PAD.b + 15}" text-anchor="middle">${fmtHz(f)}</text>`
    );
  }
  const range = s.yHi - s.yLo;
  const step = range > 200 ? 50 : range > 100 ? 20 : range > 40 ? 10 : range > 12 ? 5 : 1;
  for (let v = Math.ceil(s.yLo / step) * step; v <= s.yHi; v += step) {
    const py = s.y(v);
    p.push(
      `<line class="grid" x1="${PAD.l}" y1="${py}" x2="${W - PAD.r}" y2="${py}"/>`,
      `<text class="tick" x="${PAD.l - 7}" y="${py + 4}" text-anchor="end">${trim(v)}</text>`
    );
  }

  data.series.forEach((ser, i) => {
    const color = ser.color || COLORS[i % COLORS.length];
    const dash = ser.style === "dashed" ? ' stroke-dasharray="6 4"' : "";
    // Break the polyline on non-finite gaps: a limit that does not apply in a
    // band must not be drawn through it.
    let run = [];
    const flush = () => {
      if (run.length > 1) {
        p.push(`<polyline class="ser" points="${run.join(" ")}" stroke="${color}"${dash}/>`);
      }
      run = [];
    };
    for (const [x, y] of ser.points) {
      if (!(x > 0) || !isFinite(y)) { flush(); continue; }
      run.push(`${s.x(x).toFixed(1)},${s.y(y).toFixed(1)}`);
    }
    flush();
  });

  for (const m of data.markers || []) {
    p.push(`<circle class="mark" cx="${s.x(m.x)}" cy="${s.y(m.y)}" r="4.5"/>`);
  }

  if (selected) {
    p.push(
      `<line class="cursor" x1="${s.x(selected.x)}" y1="${PAD.t}" x2="${s.x(selected.x)}" y2="${H - PAD.b}"/>`,
      `<circle class="sel" cx="${s.x(selected.x)}" cy="${s.y(selected.y)}" r="5.5"/>`
    );
  }

  p.push(`<text class="axis" x="${PAD.l}" y="${PAD.t - 14}">${data.y_label || ""}</text>`);
  el("chart").innerHTML = p.join("");

  el("legend").innerHTML = data.series
    .map((ser, i) => {
      const color = ser.color || COLORS[i % COLORS.length];
      return `<span class="key"><i style="background:${color}"></i>${ser.name}</span>`;
    })
    .join("");
}

function nearest(px) {
  const s = scales(data);
  const target = Math.log10(s.invX(px));
  let best = null;
  let bestD = Infinity;
  for (const ser of data.series) {
    for (const [x, y] of ser.points) {
      if (!(x > 0) || !isFinite(y)) continue;
      const d = Math.abs(Math.log10(x) - target);
      if (d < bestD) { bestD = d; best = { x, y, series: ser.name }; }
    }
  }
  return best;
}

async function select(pt) {
  selected = pt;
  render();
  const unit = data.y_unit || "";
  const text = `${fmtHz(pt.x)} — ${pt.series}: ${trim(pt.y)} ${unit}`.trim();
  el("readout").textContent = text;
  await app.updateModelContext({
    content: [{ type: "text", text: `[${data.title || "curve"} selection] ${text}` }],
    structuredContent: { selected_x: pt.x, selected_y: pt.y, series: pt.series, unit },
  });
}

app.ontoolresult = (result) => {
  const sc = result.structuredContent;
  if (!sc || !Array.isArray(sc.series) || !sc.series.length) {
    el("readout").textContent = "No curve data in tool result.";
    return;
  }
  data = sc;
  selected = null;
  el("title").textContent = sc.title || "Result";
  el("subtitle").textContent = sc.subtitle || "";
  el("note").style.display = sc.note ? "block" : "none";
  if (sc.note) el("note").textContent = sc.note;
  render();
};

el("chart").addEventListener("click", (ev) => {
  if (!data) return;
  const r = el("chart").getBoundingClientRect();
  select(nearest(((ev.clientX - r.left) / r.width) * W));
});

el("fullscreen").addEventListener("click", () => {
  app.requestDisplayMode({ mode: "fullscreen" }).catch(() => {});
});

await app.connect();
