#!/usr/bin/env bash
set -euo pipefail

SRC_FILE="${1:-vishwakarma_cz_file_engine.cpp}"
BIN_FILE="${2:-vishwakarma_cz_engine}"

g++ -std=c++17 -O3 -Wall -Wextra "$SRC_FILE" -o "$BIN_FILE"
echo "Built '$BIN_FILE' from '$SRC_FILE' successfully."
