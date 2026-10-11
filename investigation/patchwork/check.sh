#!/bin/bash
export PATH=/opt/devkitpro/devkitARM/bin:$PATH
cd /work/investigation/patchwork
for o in orig patched; do
  A=$(nm $o.o | awk '/cold_reset/{print $1}')
  NEXT=$(nm $o.o | sort | awk -v a=$A '$1 ~ /^[0-9a-f]+$/ && $1>a {print $1; exit}')
  echo "=== $o (cold_reset 0x$A -> 0x$NEXT) primeiros 12 instr ==="
  objdump -d -marm --start-address=0x$A --stop-address=0x$NEXT $o.o 2>/dev/null | sed -n '7,18p'
  echo
done
