#!/usr/bin/env bash
# Deploy and exercise the headless Azahar build on the Steam Deck.
#
# Usage: deck-headless-test.sh <binary> [max_frames] [rom]
#
# The guest's SNES ROM lives in the Deck's Azahar SDMC dir and is never copied
# off the machine (T031). The headless binary uses the default data dir
# ($XDG_DATA_HOME/azahar-emu), which is where that SDMC dir lives.
set -u

BIN="${1:?usage: deck-headless-test.sh <binary> [max_frames] [rom]}"
MAX_FRAMES="${2:-6000}"
ROM="${3:-/home/deck/simcity-test/simcity_t038_audio_fix.3dsx}"

DECK=deck@steamdeck
SDMC=/home/deck/.local/share/azahar-emu/sdmc
TRACE="$SDMC/simcity-trace.log"

echo "== deploying $BIN =="
scp -q "$BIN" "$DECK:/home/deck/simcity-test/azahar-headless" || exit 1
ssh -o BatchMode=yes "$DECK" chmod +x /home/deck/simcity-test/azahar-headless || exit 1

echo "== running =="
ssh -o BatchMode=yes "$DECK" bash -s -- "$MAX_FRAMES" "$ROM" <<'SCRIPT'
set -u
MAX_FRAMES="$1"
ROM="$2"
SDMC=/home/deck/.local/share/azahar-emu/sdmc
TRACE="$SDMC/simcity-trace.log"

pkill -f azahar-headless 2>/dev/null
sleep 1

rm -f "$TRACE"
# Keep the previous log for comparison.
[ -f "$TRACE" ] && cp "$TRACE" /tmp/trace.prev

cd /home/deck/simcity-test
timeout 300 ./azahar-headless --max-frames "$MAX_FRAMES" "$ROM" \
  >/home/deck/simcity-test/headless_run.log 2>&1
RC=$?

echo "== exit code: $RC =="
echo "== stdout =="
tail -20 /home/deck/simcity-test/headless_run.log
echo "== trace log (tail 60) =="
tail -60 "$TRACE" 2>/dev/null || echo "NO TRACE LOG"
echo "== trace log line count =="
wc -l "$TRACE" 2>/dev/null || echo 0
echo "== [T5] pump lines =="
grep -c "\[T5\] pump" "$TRACE" 2>/dev/null || echo 0
grep "\[T5\] pump" "$TRACE" 2>/dev/null | head -10
echo "== port 5002 =="
(timeout 2 bash -c "echo >/dev/tcp/127.0.0.1/5002" 2>/dev/null && echo OPEN || echo closed)
SCRIPT
