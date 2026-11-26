#!/usr/bin/env bash
# Fails unless a Linux build of Moonsprout has a real WebSocket backend for the online lobby, i.e. it
# calls libcurl's curl_ws_send, either imported from a shared libcurl or linked in from the vendored
# static one. v0.1.0-rc.3 shipped without it (borealis fell back to src/ws/no_backend.cpp) and could
# not reach the lobby at all.
#   tools/release/check-websocket.sh <moonsprout or mppm binary>
# Needs the symbol table, so run it on an unstripped binary or one stripped with --strip-debug only.
set -euo pipefail
bin=${1:?usage: check-websocket.sh <binary>}
[ -f "$bin" ] || { echo "::error::$bin does not exist"; exit 1; }

# Read the symbol tables whole: `nm | grep -q` would let nm die of SIGPIPE, which pipefail reports.
dynamic=$(nm -D --undefined-only "$bin" 2>/dev/null || true)
static=$(nm --defined-only "$bin" 2>/dev/null || true)

if grep -Eq '^ +U curl_ws_send(@|$)' <<<"$dynamic"; then
    echo "OK: $bin imports curl_ws_send from a shared libcurl"
    exit 0
fi
if grep -Eq ' T curl_ws_send$' <<<"$static"; then
    version=$(grep -aoE -m1 'libcurl/[0-9]+\.[0-9]+\.[0-9]+' "$bin" | head -1 || true)
    echo "OK: $bin links curl_ws_send from a static libcurl${version:+ ($version)}"
    exit 0
fi
if [ -z "$static" ]; then
    echo "::error::$bin has no symbol table, so its WebSocket backend cannot be checked"
    exit 1
fi
echo "::error::$bin has no curl_ws_send: borealis::ws was built without a WebSocket backend, so online play cannot reach the lobby"
exit 1
