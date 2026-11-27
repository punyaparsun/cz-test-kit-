#!/usr/bin/env bash
set -euo pipefail

SRC_FILE="${1:-vishwakarma_cz_file_engine.cpp}"
BIN_FILE="${2:-vishwakarma_cz_engine}"
BUILD_LOG="${3:-build_runtime.log}"

START_NS=$(date +%s%N)
CPU_START=$(awk '/^cpu /{print $2+$3+$4+$5+$6+$7+$8, $5}' /proc/stat)

g++ -std=c++17 -O3 -Wall -Wextra "$SRC_FILE" -o "$BIN_FILE"

END_NS=$(date +%s%N)
CPU_END=$(awk '/^cpu /{print $2+$3+$4+$5+$6+$7+$8, $5}' /proc/stat)

ELAPSED_MS=$(( (END_NS - START_NS) / 1000000 ))
T1=$(echo "$CPU_START" | awk '{print $1}')
I1=$(echo "$CPU_START" | awk '{print $2}')
T2=$(echo "$CPU_END" | awk '{print $1}')
I2=$(echo "$CPU_END" | awk '{print $2}')

DT=$((T2 - T1))
DI=$((I2 - I1))
CPU_UTIL=$(awk -v dt="$DT" -v di="$DI" 'BEGIN {
  if (dt <= 0) print "0.00%";
  else printf "%.2f%%", ((dt - di) / dt) * 100;
}')

{
  echo "=== Build Runtime & System Load ==="
  echo "Source File  : $SRC_FILE"
  echo "Binary File  : $BIN_FILE"
  echo "Build Time   : ${ELAPSED_MS} ms"
  echo "System CPU % : $CPU_UTIL"
} | tee "$BUILD_LOG"
