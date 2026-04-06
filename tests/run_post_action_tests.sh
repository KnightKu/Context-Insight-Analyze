#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "${REPO_ROOT}"

echo "[1/4] build test binary"
make post_action_file_tester >/dev/null

echo "[2/4] generate fixtures"
python3 tests/gen_post_action_fixtures.py

PASS_CASES=(
  "tests/fixtures/valid_mixed.bin"
  "tests/fixtures/valid_termination.bin"
  "tests/fixtures/valid_relative_time_with_marker.bin"
  "tests/fixtures/valid_zero_op_skip.bin"
)

SKIP_CASES=(
  "tests/fixtures/invalid_op.bin"
  "tests/fixtures/invalid_rw_reserved.bin"
  "tests/fixtures/invalid_trim_total_ranges.bin"
  "tests/fixtures/invalid_alignment.bin"
  "tests/fixtures/invalid_missing_marker.bin"
)

SOFT_STOP_CASES=(
  "tests/fixtures/invalid_marker_overwrite.bin"
  "tests/fixtures/invalid_marker_nonincreasing.bin"
)

echo "[3/4] run pass cases"
for f in "${PASS_CASES[@]}"; do
  echo "  PASS expected: ${f}"
  ./post_action_file_tester "${f}" 0 >/dev/null 2>&1
done

echo "[4/4] run invalid-data skip cases"
for f in "${SKIP_CASES[@]}"; do
  echo "  SKIP expected (success with invalid-count in debug): ${f}"
  ./post_action_file_tester "${f}" >/dev/null 2>&1
done

echo "  LBA-STATS expected: segmented distribution output"
if ! ./post_action_file_tester --lba-stats "tests/fixtures/valid_mixed.bin" >/tmp/post_action_lba_stats.log 2>&1; then
  echo "unexpected failure for --lba-stats output case" >&2
  exit 1
fi
if ! rg -i "Read Count distribution|Write-to-First-Read Latency\\(real ms\\) distribution|LBA Life Cycle\\(real ms\\) distribution" "/tmp/post_action_lba_stats.log" >/dev/null; then
  echo "missing segmented lba-stats output sections" >&2
  exit 1
fi

for f in "${SOFT_STOP_CASES[@]}"; do
  echo "  SOFT-STOP expected (overwrite should stop parse/read but not fail): ${f}"
  if ! ./post_action_file_tester "${f}" >/tmp/post_action_overwrite.log 2>&1; then
    echo "unexpected failure for ${f}" >&2
    exit 1
  fi
  if ! rg -i "overwrite.*lba|lba.*overwrite" "/tmp/post_action_overwrite.log" >/dev/null; then
    echo "missing overwrite+lba warning for ${f}" >&2
    exit 1
  fi
done

echo "all post-action tests passed"
