#!/usr/bin/env bash
set -euo pipefail

BIN_FILE="${1:-./vishwakarma_cz_engine}"
INPUT_FILE="${2:-}"
WORK_DIR="${3:-./cz_test_out}"

if [[ ! -x "$BIN_FILE" ]]; then
  echo "Error: executable '$BIN_FILE' not found. Run ./build.sh first."
  exit 1
fi

mkdir -p "$WORK_DIR"
CLEANUP_INPUT=0

if [[ -z "$INPUT_FILE" ]]; then
  INPUT_FILE="$WORK_DIR/sample_test.bin"
  python3 - "$INPUT_FILE" <<'PY'
import sys
path = sys.argv[1]
with open(path, "wb") as f:
    f.write(b"\x00" * (4 * 1024 * 1024))
    f.write(b"CZ_TEST_BLOCK_" * ((6 * 1024 * 1024) // 14))
    f.write(bytes(range(256)) * ((6 * 1024 * 1024) // 256))
PY
  CLEANUP_INPUT=1
fi

BASE_NAME="$(basename "$INPUT_FILE")"
CZ_FILE="$WORK_DIR/${BASE_NAME}.cz"
RESTORED_FILE="$WORK_DIR/${BASE_NAME}.restored"
TELEMETRY_LOG="$WORK_DIR/runtime_load.log"

measure_stage() {
  local label="$1"
  shift
  local t_file
  t_file="$(mktemp)"

  local start_ns end_ns
  start_ns=$(date +%s%N)

  if command -v /usr/bin/time >/dev/null 2>&1; then
    /usr/bin/time -f "user=%U sys=%S max_rss_kb=%M" -o "$t_file" "$@"
  else
    { time "$@" ; } 2>"$t_file"
  fi

  end_ns=$(date +%s%N)
  local elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))

  echo "[$label] Wall Time: ${elapsed_ms} ms"
  sed 's/^/  /' "$t_file"
  rm -f "$t_file"
}

{
  echo "=== CZ Runtime Load & Data Read/Write Report ==="
  echo "Input File : $INPUT_FILE"
  echo "Input Bytes: $(stat -c%s "$INPUT_FILE")"
  sha256sum "$INPUT_FILE"
  echo ""

  echo "=== 1. Pack / Send to .cz ==="
  measure_stage "PACK/SEND" "$BIN_FILE" --pack "$INPUT_FILE" "$CZ_FILE"
  ls -lh "$INPUT_FILE" "$CZ_FILE"
  echo ""

  echo "=== 2. Unpack / Restore ==="
  measure_stage "UNPACK/RECV" "$BIN_FILE" --unpack "$CZ_FILE" "$RESTORED_FILE"
  ls -lh "$CZ_FILE" "$RESTORED_FILE"
  echo ""

  echo "=== 3. SHA-256 Verification ==="
  ORIG_HASH="$(sha256sum "$INPUT_FILE" | awk '{print $1}')"
  REST_HASH="$(sha256sum "$RESTORED_FILE" | awk '{print $1}')"
  echo "Original SHA-256 : $ORIG_HASH"
  echo "Restored SHA-256 : $REST_HASH"

  if [[ "$ORIG_HASH" == "$REST_HASH" ]]; then
    echo "RESULT: PASS (Bit-for-bit restoration verified)"
  else
    echo "RESULT: FAIL (Hash mismatch)"
    exit 1
  fi

  if [[ "$CLEANUP_INPUT" -eq 1 ]]; then
    echo ""
    echo "Test artifacts saved in '$WORK_DIR'."
  fi
} | tee "$TELEMETRY_LOG"
