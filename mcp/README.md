# Hertz as an MCP App

Exposes the Hertz conducted-EMI engine as [MCP](https://modelcontextprotocol.io)
tools, plus an interactive spectrum-vs-limit widget as an
[MCP Apps](https://modelcontextprotocol.io/extensions/apps/build) (SEP-1865) UI
resource.

The point: an engineer asks a chat assistant "does this scan pass CISPR 32
Class B?", gets a **chart they can click**, and clicking tells the model what
they clicked — so the follow-up question needs no re-explaining.

Hosts that understand MCP Apps (Claude web/Desktop, ChatGPT, VS Code Copilot)
render the widget. Hosts that speak only plain MCP (Langdock, Cursor, …) still
get the tools and the text verdict — the `_meta` they don't understand is
ignored. One server, graceful degradation.

## Build and run

```bash
cd mcp
npm install && npm run build      # bundles the widget → dist/spectrum.html
python server.py                  # streamable HTTP on 127.0.0.1:8400/mcp
```

Python needs `mcp`, `numpy`, `uvicorn`, `starlette`. The widget bundle must
exist before the server can serve the UI resource — it raises rather than
serving a blank page.

## Use it from Claude (web or Desktop)

Claude reaches your server over the public internet, so a localhost port needs
a tunnel. Custom connectors require a paid plan (Pro, Max, or Team).

1. **Start the tunnel first** — you need its hostname before starting the
   server:

   ```bash
   npx cloudflared tunnel --url http://localhost:8400
   ```

   Copy the generated `https://<random>.trycloudflare.com` URL.

2. **Start the server with that hostname allowed:**

   ```bash
   HERTZ_PUBLIC_HOST=<random>.trycloudflare.com python server.py
   ```

   Hostname only — no `https://`, no `/mcp`. For throwaway tunnels whose name
   changes every run, `HERTZ_ALLOW_ANY_HOST=1` skips the check entirely (fine
   for a laptop, not for anything public).

   > **Why:** the MCP SDK enables DNS-rebinding protection by default and
   > rejects unrecognised `Host` headers with a bare `421 Invalid Host header`.
   > Behind a tunnel the Host is the *public* name, so every request 421s.
   > Claude then can't speak MCP, falls back to probing for OAuth, and reports
   > **"Couldn't register with <name>'s sign-in service"** — an authentication
   > error for what is actually a Host-header rejection. If you see that
   > message, `curl -i https://<tunnel>/mcp` before touching anything OAuth.

3. **Add the connector.** In Claude: profile → **Settings** → **Connectors** →
   **Add custom connector**. Paste the tunnel URL **with the `/mcp` path**:
   `https://<random>.trycloudflare.com/mcp` — the bare hostname 404s, which
   produces the same misleading OAuth error.

3. **Ask.** Start a chat, attach a spectrum-analyzer CSV, and say something
   like:

   > Does this conducted scan pass CISPR 32 Class B? It's a peak sweep.

   Claude calls `check_spectrum`, the chart renders inline, and clicking a
   point pushes that selection into its context for the next turn.

Good first questions once the chart is up: *"what's the worst offender?"*,
*"how much attenuation do I need at 500 kHz?"*, *"is that a harmonic comb?"*

## Testing without Claude

The reference host from the `ext-apps` repo renders MCP Apps locally:

```bash
git clone --depth 1 https://github.com/modelcontextprotocol/ext-apps.git
cd ext-apps && bun install
cd examples/basic-host
SERVERS='["http://127.0.0.1:8400/mcp"]' npm start   # → http://localhost:8080
```

It shows the widget alongside raw panels for **Model Context** and **Messages**,
which is the only convenient way to see what your widget is actually telling
the model.

## Tools

17 tools over two widgets. Everything `hertz.*` exposes is reachable.

**Measurement → verdict**

| Tool | Widget | |
|---|---|---|
| `inspect_spectrum_csv` | — | columns in an export; call first on multi-trace files |
| `check_spectrum` | spectrum | scan vs limit: verdict, offenders, unswept regions |
| `detect_switching_frequency` | curves | harmonic comb → f_sw, matched + residual peaks |
| `measure_receiver` | curves | CISPR 16-1-1 peak/QP/average on a sampled waveform |
| `separate_cm_dm` | curves | exact CM/DM split (requires phase) |

**Limits**

| Tool | Widget | |
|---|---|---|
| `list_standards` | — | which standards/classes/detectors are accepted |
| `get_limit_line` | curves | the limit itself — mains, CISPR 25, or radiated |
| `estimate_radiated` | curves | CM current → E-field vs CISPR 32 radiated (±20 dB) |

**Filter design (ANP015)**

| Tool | Widget | |
|---|---|---|
| `filter_requirements` | — | measured + limit → A_req, design frequency, cutoff |
| `design_filter` | curves | full CM+DM sizing, 1φ/DC/3φ/3φ+N, with rounding |
| `filter_insertion_loss` | curves | IL of an explicit LC, with ESL/ESR + CISPR 17 worst case |
| `check_filter_safety` | — | Y-cap earth leakage, X-cap discharge resistor |
| `check_input_filter_stability` | — | Middlebrook check + damping branch |

**Network / helpers**

| Tool | Widget | |
|---|---|---|
| `lisn_impedance` | curves | EUT-side impedance — the real source, not 50 Ω |
| `lisn_spice_model` | — | LISN as a SPICE subcircuit |
| `convert_level` | — | dBµV ↔ dBm ↔ Vrms |
| `round_to_series` | — | round a computed value onto an E-series part |

### Widgets

`spectrum.html` is purpose-built for scan-vs-limit (offender dots, unswept
shading). `curves.html` is generic — any set of named series against log
frequency — and is shared by the other seven. Adding a tool usually means
emitting series into `_curves_result()`, not writing a widget.

## Design notes

**Two channels, and they must stay separate.** `content` carries a ~600-char
digest for the model; `structuredContent` carries the trace for the widget.
Returning a plain `dict` from a FastMCP tool serialises the *whole payload*
into `content` — 28 KB of array into the context window on every call. The tool
therefore returns a `CallToolResult` explicitly, which FastMCP passes through
verbatim.

**Decimation is peak-preserving.** A chart has ~1000 useful pixels; scans have
tens of thousands of points. Each output point is the *loudest actual sample*
in its log-frequency bin, so a clicked point is a real measurement and the
offender never averages away.

**Selection uses `updateModelContext`, not `sendMessage`.** Model context
overwrites, so clicking forty points costs one line of context rather than
forty, and starts no turn. `sendMessage` is reserved for the explicit "ask
about this point" button — a deliberate act that deserves an answer.

**Limits are drawn per segment.** CISPR 25 defines limits only inside protected
bands; joining segments would draw a limit across frequencies that are
genuinely unregulated.

**Peak scans are screened, not judged.** `peak >= quasi_peak >= average` for any
signal, so a peak trace *under* a QP limit definitively passes while one *over*
it is `INCONCLUSIVE` until re-measured. Pass `trace_detector="peak"` to get that
distinction instead of a misleading `FAIL`.

**CORS is required.** Browser-resident hosts fetch `/mcp` from page JavaScript,
and the streamable transport must read the `Mcp-Session-Id` response header —
which cross-origin JS cannot do unless it is explicitly exposed. `allow_origins`
is `*` here; tighten it for anything public.

## Status

Spike. Uses the Python reference engine (`src/hertz/`), not the C++/WASM core —
an MCP server is a server, so the WASM path's advantage (zero server compute)
does not apply here, and the Python reference is cross-validated against the
same golden vectors.

Not yet done: filter design and LISN widgets, file upload rather than CSV-as-
argument, auth.
