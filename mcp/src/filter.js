/**
 * Hertz filter widget — the web app's own FilterSchematic + LogChart.
 *
 * Mirrors the web workbench: one view dropdown over several result panes, and
 * clicking a schematic component surfaces its catalog so a real part can be
 * bound and the design re-run as-built.
 *
 * Three distinct channels to the model, used deliberately:
 *   updateModelContext()  ambient selection — silent, overwriting
 *   callServerTool()      the widget acts on its own (list parts, re-design);
 *                         no LLM turn, but the result's `content` still lands
 *                         in model context so Claude can follow along
 *   sendMessage()         a deliberate question — starts a turn
 */
import { createApp, h, reactive } from "vue";
import { App } from "@modelcontextprotocol/ext-apps";
// The web app's own design tokens — both components bind them directly.
import "../../web/src/style.css";
import FilterSchematic from "../../web/src/components/FilterSchematic.vue";
import LogChart from "../../web/src/components/LogChart.vue";

const app = new App({ name: "Hertz Filter", version: "0.1.0" });

const VIEWS = [
  ["schematic", "Schematic"],
  ["il", "Insertion loss"],
  ["parts", "Catalog parts"],
  ["bom", "BOM"],
  ["values", "Sizing values"],
  ["lisn", "LISN impedance"],
];

const state = reactive({
  title: "", subtitle: "", note: "",
  view: "schematic",
  stages: 1, topology: "mains", labels: {}, bindings: {}, bom: [], targets: {},
  values: [], lisn: null,
  selected: "",
  series: [], refRuns: [], yLabel: "",
  candidates: null, candidateNote: "", busy: false, error: "",
  lastArgs: null,
});

function toRuns(points) {
  const runs = [];
  let run = [];
  for (const [f, v] of points) {
    if (!(f > 0) || !Number.isFinite(v)) {
      if (run.length > 1) runs.push(run);
      run = [];
      continue;
    }
    run.push({ f, v });
  }
  if (run.length > 1) runs.push(run);
  return runs;
}

const kindOf = (ref) =>
  ref.startsWith("CMC") ? "cmc" : ref.startsWith("C_X") ? "cx" : "cy";

/**
 * Strip Vue reactivity before anything crosses postMessage.
 *
 * `reactive()` hands back Proxy objects, and the structured-clone algorithm
 * refuses to clone a Proxy — the bridge fails with "[object Object] could not
 * be cloned", which reads like a protocol error but is purely local. Anything
 * derived from `state` must be plain first.
 */
const plain = (v) => JSON.parse(JSON.stringify(v));

/** Click a component: report the selection, then fetch its catalog. */
async function selectComponent(ref) {
  state.selected = ref;
  state.error = "";
  const label = state.labels[ref] || "(unlabelled)";
  const mpn = state.bindings[ref]?.mpn;
  await app.updateModelContext({
    content: [{ type: "text",
      text: `[filter selection] ${ref} = ${label}${mpn ? ` (${mpn})` : ""}` }],
    structuredContent: plain({ selected_ref: ref, value: label, mpn: mpn || null }),
  });
  await loadCandidates(ref);
}

/** Widget-initiated tool call — no LLM turn. */
async function loadCandidates(ref) {
  const kind = kindOf(ref);
  const target = state.targets[kind];
  if (!target) {
    state.error = `No design target for ${ref}; re-run design_filter.`;
    return;
  }
  state.busy = true;
  state.candidates = null;
  state.view = "parts";
  try {
    const res = await app.callServerTool({
      name: "list_candidates",
      arguments: { kind, target_value: target, limit: 12 },
    });
    const sc = res.structuredContent || {};
    state.candidates = sc.parts || [];
    state.candidateNote = sc.note || "";
  } catch (e) {
    state.error = `Catalog lookup failed: ${e?.message || e}`;
  } finally {
    state.busy = false;
  }
}

/** Bind a part and re-run the design as-built. */
async function bindPart(part) {
  if (!state.selected) return;
  state.bindings[state.selected] = {
    mpn: part.mpn, manufacturer: part.manufacturer,
  };
  state.busy = true;
  state.error = "";
  try {
    await app.updateModelContext({
      content: [{ type: "text",
        text: `[filter binding] ${state.selected} := ${part.mpn} ` +
              `(${part.manufacturer}, ${part.value}, ${part.deviation_pct >= 0 ? "+" : ""}` +
              `${part.deviation_pct.toFixed(1)}% from target)` }],
      structuredContent: plain({ ref: state.selected, ...part }),
    });
    if (state.lastArgs) {
      const res = await app.callServerTool({
        name: "design_filter",
        arguments: plain({ ...state.lastArgs, bindings: state.bindings }),
      });
      if (res.structuredContent) ingest(res.structuredContent, state.lastArgs);
      state.view = "bom";
    }
  } catch (e) {
    state.error = `Re-design failed: ${e?.message || e}`;
  } finally {
    state.busy = false;
  }
}

function ingest(sc, args) {
  state.title = sc.title || "Line filter";
  state.subtitle = sc.subtitle || "";
  state.note = sc.note || "";
  state.yLabel = sc.y_label || "Insertion loss (dB)";
  state.stages = sc.stages || 1;
  state.topology = sc.topology || "mains";
  state.labels = sc.labels || {};
  state.bindings = sc.bindings || {};
  state.bom = sc.bom || [];
  state.values = sc.values || [];
  state.lisn = sc.lisn || null;
  state.targets = sc.targets || {};
  if (args) state.lastArgs = args;
  state.series = [];
  state.refRuns = [];
  (sc.series || []).forEach((s, i) => {
    const runs = toRuns(s.points || []);
    if (!runs.length) return;
    const dash = s.style === "dashed" ? "6 4" : undefined;
    if (runs.length > 1) {
      state.refRuns.push({ label: s.name, color: s.color, dash, runs });
    } else {
      state.series.push({ id: `s${i}`, label: s.name, color: s.color, dash, points: runs[0] });
    }
  });
}

// ------------------------------------------------------------------ rendering
const partsPane = () => {
  if (state.busy && !state.candidates) return h("div", { class: "pane-msg" }, "Loading catalog…");
  if (!state.selected) return h("div", { class: "pane-msg" }, "Click a component in the schematic to see its catalog.");
  if (!state.candidates) return h("div", { class: "pane-msg" }, "No catalog loaded.");
  if (!state.candidates.length)
    return h("div", { class: "pane-msg" }, state.candidateNote || "No matching parts.");
  const bound = state.bindings[state.selected]?.mpn;
  return h("div", {}, [
    h("div", { class: "pane-msg" }, `${state.selected} — ${state.candidateNote}`),
    h("table", { class: "tbl" }, [
      h("thead", {}, h("tr", {}, ["MPN", "Manufacturer", "Value", "Δ", ""].map((x) => h("th", {}, x)))),
      h("tbody", {}, state.candidates.map((p) =>
        h("tr", { class: p.mpn === bound ? "bound" : "" }, [
          h("td", {}, p.mpn),
          h("td", {}, p.manufacturer || "—"),
          h("td", {}, p.value),
          h("td", {}, `${p.deviation_pct >= 0 ? "+" : ""}${p.deviation_pct.toFixed(1)}%`),
          h("td", {}, p.mpn === bound
            ? h("span", { class: "tag" }, "bound")
            : h("button", { onClick: () => bindPart(p), disabled: state.busy }, "Bind")),
        ]))),
    ]),
  ]);
};

const valuesPane = () => {
  if (!state.values.length) return h("div", { class: "pane-msg" }, "No sizing values.");
  return h("table", { class: "tbl" }, [
    h("thead", {}, h("tr", {}, ["Quantity", "Value"].map((x) => h("th", {}, x)))),
    h("tbody", {}, state.values.map((r) =>
      h("tr", {}, [h("td", {}, r.k), h("td", {}, r.v)]))),
  ]);
};

const lisnPane = () => {
  if (!state.lisn) return h("div", { class: "pane-msg" }, "No LISN data.");
  const runs = (state.lisn.series || []).map((s, i) => ({
    id: `l${i}`, label: s.name, color: s.color,
    points: (s.points || []).map(([f, v]) => ({ f, v })),
  }));
  return h("div", {}, [
    h("div", { class: "note" }, state.lisn.note || ""),
    h(LogChart, { series: runs, yLabel: state.lisn.y_label || "ohm", height: 320 }),
  ]);
};

const bomPane = () => {
  if (!state.bom.length) return h("div", { class: "pane-msg" }, "No BOM yet.");
  return h("table", { class: "tbl" }, [
    h("thead", {}, h("tr", {}, ["Ref", "Value", "MPN", "Status"].map((x) => h("th", {}, x)))),
    h("tbody", {}, state.bom.map((r) =>
      h("tr", { class: r.status === "bound" ? "bound" : "" }, [
        h("td", {}, r.ref),
        h("td", {}, r.value),
        h("td", {}, r.mpn || "—"),
        h("td", {}, r.status === "bound"
          ? h("span", { class: "tag" }, "bound")
          : h("span", { class: "muted" }, "design value")),
      ]))),
  ]);
};

const Root = {
  setup() {
    return () =>
      h("div", { class: "wrap" }, [
        h("div", { class: "head" }, [
          h("h1", state.title),
          h("select", {
            value: state.view,
            onChange: (e) => {
              state.view = e.target.value;
              if (state.view === "parts" && state.selected && !state.candidates) {
                loadCandidates(state.selected);
              }
            },
          }, VIEWS.map(([v, label]) => h("option", { value: v }, label))),
        ]),
        state.subtitle ? h("div", { class: "sub" }, state.subtitle) : null,
        state.error ? h("div", { class: "err" }, state.error) : null,
        state.note && state.view === "il" ? h("div", { class: "note" }, state.note) : null,

        state.view === "schematic"
          ? h(FilterSchematic, {
              stages: state.stages, labels: state.labels, bindings: state.bindings,
              selected: state.selected, topology: state.topology, interactive: true,
              onSelect: selectComponent,
            })
          : state.view === "il"
          ? h(LogChart, {
              series: state.series, refRuns: state.refRuns,
              yLabel: state.yLabel, height: 340,
            })
          : state.view === "parts" ? partsPane()
          : state.view === "values" ? valuesPane()
          : state.view === "lisn" ? lisnPane() : bomPane(),

        state.view === "schematic"
          ? h("div", { class: "readout" }, state.selected
              ? `${state.selected} = ${state.labels[state.selected] || "—"}` +
                (state.bindings[state.selected]?.mpn
                  ? ` → ${state.bindings[state.selected].mpn}` : "")
              : "Click a component to see catalog parts for it.")
          : null,
        h("p", { class: "disclaimer" },
          "Pre-compliance estimate only — not a compliance statement."),
      ]);
  },
};

createApp(Root).mount("#app");

app.ontoolresult = (result) => {
  const sc = result.structuredContent;
  if (!sc) {
    state.title = "No filter data in tool result.";
    return;
  }
  ingest(sc, null);
  state.selected = "";
  state.candidates = null;
};

// The host delivers the originating tool's arguments separately from its
// result; keep them so a binding can re-run the SAME design rather than
// guessing the inputs.
app.ontoolinput = (params) => {
  state.lastArgs = params?.arguments ? plain(params.arguments) : (params ? plain(params) : null);
};

await app.connect();
