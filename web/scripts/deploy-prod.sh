#!/usr/bin/env bash
# Deploy the Hertz site to hertz.openconverters.com (Scaleway box 51.15.253.66,
# same host as kelvin/kirchhoff). Static SPA + WASM engine → /opt/hertz/dist.
# Idempotent — safe to re-run for every release.
#
#   web/scripts/deploy-prod.sh                    # rebuild WASM + build + rsync + verify
#   SKIP_WASM_BUILD=1 web/scripts/deploy-prod.sh  # skip the emscripten rebuild (engine already current)
#
# The /kelvin catalog data (served off /cache) and the nginx vhost are managed
# separately; this ships the SPA + the hertz.js/hertz.wasm engine only.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"     # web/
HOST=root@51.15.253.66
SSH="ssh -i $HOME/.ssh/om_scaleway -o StrictHostKeyChecking=no"
DOCROOT=/opt/hertz/dist

# 0. Keep the WASM engine CURRENT. `npm run build` (vite) only COPIES
#    web/public/hertz.js into dist — it does NOT rebuild it — so a stale engine
#    ships silently. Rebuild it (scripts/build_wasm.sh) unless told it's fresh.
if [[ -z "${SKIP_WASM_BUILD:-}" ]]; then
  echo "Rebuilding the WASM engine (npm run wasm)…"
  ( cd "$HERE" && npm run wasm )
fi

# 1. Clean-HEAD build (a build bundles the working tree — refuse uncommitted work).
if [[ -n "$(git -C "$HERE" status --porcelain -- . 2>/dev/null)" ]]; then
  echo "REFUSING to deploy: web/ has uncommitted changes (a build bundles the working tree)." >&2
  exit 1
fi
( cd "$HERE" && npm run build )

# 2. SPA + engine rsync (the /kelvin data path is served off /cache, never the docroot).
$SSH "$HOST" "mkdir -p $DOCROOT"
rsync -az --delete --exclude 'kelvin/' -e "$SSH" "$HERE/dist/" "$HOST:$DOCROOT/"

# 3. gzip sidecars for the text assets (gzip_static on — a stale .gz silently
#    serves old bytes to browsers). The .wasm stays raw (already compact binary).
$SSH "$HOST" "cd $DOCROOT && find . -type f \( -name '*.js' -o -name '*.css' -o -name '*.html' -o -name '*.svg' \) -not -name '*.gz' -exec gzip -kf9 {} \;"

# 4. Byte-verify the live artifacts against this clean-HEAD build. Text assets are
#    checked over BOTH the plain and gzip paths (gzip_static serves <file>.gz to
#    every real browser); the raw .wasm is checked plain.
BASE=https://hertz.openconverters.com
if curl -sfI "$BASE/" >/dev/null 2>&1; then
  for f in hertz.js index.html $(cd "$HERE/dist" && ls assets/*.js assets/*.css); do
    local_=$(sha256sum "$HERE/dist/$f" | cut -d' ' -f1)
    plain=$(curl -sf "$BASE/$f" | sha256sum | cut -d' ' -f1)
    [[ "$plain" == "$local_" ]] || { echo "BYTE MISMATCH on $f (live $plain vs built $local_)" >&2; exit 1; }
    gz=$(curl -sf --compressed "$BASE/$f" | sha256sum | cut -d' ' -f1)
    [[ "$gz" == "$local_" ]] || { echo "STALE GZIP SIDECAR on $f — browsers get $gz, built $local_" >&2; exit 1; }
    echo "verified $f (plain + gzip)"
  done
  wlive=$(curl -sf "$BASE/hertz.wasm" | sha256sum | cut -d' ' -f1)
  wlocal=$(sha256sum "$HERE/dist/hertz.wasm" | cut -d' ' -f1)
  [[ "$wlive" == "$wlocal" ]] || { echo "BYTE MISMATCH on hertz.wasm (live $wlive vs built $wlocal)" >&2; exit 1; }
  echo "verified hertz.wasm"
  echo "deploy verified."
else
  echo "https not answering — check the vhost." >&2
fi
