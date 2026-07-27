/**
 * WASM capability probe — an MCP App view.
 *
 * Decides an architectural question rather than showing a result: can a widget
 * instantiate WebAssembly inside the host's sandbox iframe?
 *
 * WebAssembly.instantiate() requires 'wasm-unsafe-eval' (or 'unsafe-eval') in
 * the frame's script-src. The MCP Apps spec's recommended CSP grants neither:
 *
 *   specification/draft/apps.mdx:288 →  script-src 'self' 'unsafe-inline';
 *
 * ...while the reference basic-host ships 'unsafe-eval', so it passes there and
 * proves nothing about production. Load this in the host you actually care
 * about — the answer decides whether Kirchhoff's engine can live in the widget
 * or has to move server-side.
 */
import { App } from "@modelcontextprotocol/ext-apps";

const app = new App({ name: "Hertz WASM probe", version: "0.1.0" });
const el = (id) => document.getElementById(id);

// The smallest valid module: the 8-byte header. CSP gates compilation itself,
// so contents are irrelevant — a module that does nothing is the cleanest test
// of "am I allowed to compile at all".
const EMPTY_WASM = new Uint8Array([0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00]);

async function probe() {
  const out = {
    wasm_object: typeof WebAssembly !== "undefined",
    compile_sync: null,
    instantiate_async: null,
    error: null,
  };

  if (!out.wasm_object) {
    out.error = "WebAssembly global is not defined in this frame.";
    return out;
  }
  try {
    new WebAssembly.Module(EMPTY_WASM);
    out.compile_sync = true;
  } catch (e) {
    out.compile_sync = false;
    out.error = `${e.name}: ${e.message}`;
  }
  try {
    await WebAssembly.instantiate(EMPTY_WASM, {});
    out.instantiate_async = true;
  } catch (e) {
    out.instantiate_async = false;
    out.error = out.error || `${e.name}: ${e.message}`;
  }
  return out;
}

function render(r) {
  const ok = r.compile_sync && r.instantiate_async;
  el("verdict").textContent = ok ? "WASM ALLOWED" : "WASM BLOCKED";
  el("verdict").className = `verdict ${ok ? "ok" : "bad"}`;
  el("rows").innerHTML = [
    ["WebAssembly global", r.wasm_object],
    ["new WebAssembly.Module()", r.compile_sync],
    ["WebAssembly.instantiate()", r.instantiate_async],
  ].map(([k, v]) =>
    `<tr><td>${k}</td><td class="${v ? "y" : "n"}">${v ? "yes" : "no"}</td></tr>`
  ).join("");
  el("err").style.display = r.error ? "block" : "none";
  if (r.error) el("err").textContent = r.error;
  el("meaning").textContent = ok
    ? "This host grants wasm-unsafe-eval (or unsafe-eval). An engine can run inside the widget — no server round trip."
    : "This host follows the spec CSP. The engine cannot run in the widget; it must run server-side with the widget as a thin renderer.";
}

app.ontoolresult = async () => {
  const r = await probe();
  render(r);
  // Report the verdict to the model — this is the point of the tool.
  await app.updateModelContext({
    content: [{ type: "text", text:
      `[wasm probe] ${r.compile_sync && r.instantiate_async ? "ALLOWED" : "BLOCKED"} — ` +
      `compile=${r.compile_sync}, instantiate=${r.instantiate_async}` +
      (r.error ? `, error: ${r.error}` : "") }],
    structuredContent: JSON.parse(JSON.stringify(r)),
  });
};

await app.connect();

// Also probe immediately, so the panel is populated even if the host delivers
// no tool result (some hosts render the resource before the call resolves).
render(await probe());
