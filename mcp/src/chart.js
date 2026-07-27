/**
 * Hertz chart widget — the web app's own LogChart, driven by an MCP tool result.
 *
 * This imports `web/src/components/LogChart.vue` directly rather than
 * reimplementing it, so an engineer looking at a chart in Claude is looking at
 * exactly the chart they'd see on hertz.openconverters.com. One definition,
 * two surfaces.
 */
import { createApp, h, reactive } from "vue";
import { App } from "@modelcontextprotocol/ext-apps";
// The web app's own design tokens (--s-1, --s-limit, --fault, --mono, ...).
// LogChart binds them directly, so without this its strokes resolve to nothing.
import "../../web/src/style.css";
import LogChart from "../../web/src/components/LogChart.vue";

const app = new App({ name: "Hertz Chart", version: "0.1.0" });
const state = reactive({
  title: "", subtitle: "", note: "",
  series: [], refRuns: [], violations: [], yLabel: "", unit: "",
});

/**
 * Split a server series on non-finite values.
 *
 * The server marks "no limit applies here" with NaN so a limit is never drawn
 * across an unregulated band. LogChart expresses the same idea as `refRuns`
 * (a labelled set of separate runs), so broken series go there and continuous
 * ones stay in `series`.
 */
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

function ingest(sc) {
  state.title = sc.title || "Result";
  state.subtitle = sc.subtitle || "";
  state.note = sc.note || "";
  state.yLabel = sc.y_label || "";
  state.unit = sc.y_unit || "";
  state.series = [];
  state.refRuns = [];
  state.violations = (sc.markers || []).map((m) => ({ f: m.x, v: m.y }));

  (sc.series || []).forEach((s, i) => {
    const runs = toRuns(s.points || []);
    if (!runs.length) return;
    const color = s.color || undefined;
    const dash = s.style === "dashed" ? "6 4" : undefined;
    if (runs.length > 1) {
      state.refRuns.push({ label: s.name, color, dash, runs });
    } else {
      state.series.push({
        id: `s${i}`, label: s.name, color, dash, points: runs[0],
      });
    }
  });
}

const Root = {
  setup() {
    return () =>
      h("div", { class: "wrap" }, [
        h("h1", state.title),
        state.subtitle ? h("div", { class: "sub" }, state.subtitle) : null,
        state.note ? h("div", { class: "note" }, state.note) : null,
        h(LogChart, {
          series: state.series,
          refRuns: state.refRuns,
          violations: state.violations,
          yLabel: state.yLabel,
          height: 380,
        }),
        h("p", { class: "disclaimer" },
          "Pre-compliance estimate only — not a compliance statement."),
      ]);
  },
};

createApp(Root).mount("#app");

// Handlers before connect(): the host may push the tool result during the
// ui/initialize handshake, and a late listener misses it.
app.ontoolresult = (result) => {
  const sc = result.structuredContent;
  if (!sc || !Array.isArray(sc.series)) {
    state.title = "No curve data in tool result.";
    return;
  }
  ingest(sc);
};

await app.connect();
