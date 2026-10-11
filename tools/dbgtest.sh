#!/usr/bin/env bash
# 3dsdbgkit smoke test: ping the extension protocol, then read a pixel dump.
HOST="${1:-10.0.0.115}"
PORT="${2:-5002}"
echo "=== porta $PORT ==="
timeout 5 bash -c "echo >/dev/tcp/$HOST/$PORT" 2>/dev/null && echo "porta aberta" || { echo "porta fechada (jogo nao esta a correr?)"; exit 1; }
echo "=== ping ==="
timeout 20 python3 tools/netdbg.py "$HOST" "$PORT" ping
