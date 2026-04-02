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
)

SKIP_CASES=(
  "tests/fixtures/invalid_op.bin"
  "tests/fixtures/invalid_rw_reserved.bin"
  "tests/fixtures/invalid_trim_total_ranges.bin"
  "tests/fixtures/invalid_alignment.bin"
  "tests/fixtures/invalid_missing_marker.bin"
)

FAIL_CASES=(
  "tests/fixtures/invalid_marker_overwrite.bin"
)

echo "[3/4] run pass cases"
for f in "${PASS_CASES[@]}"; do
  echo "  PASS expected: ${f}"
  csv_out="tests/fixtures/$(basename "${f}").csv"
  qd_csv_out="tests/fixtures/$(basename "${f}").stat_qd.csv"
  wa_csv_out="tests/fixtures/$(basename "${f}").stat_wa.csv"
  ./post_action_file_tester \
    --export-bucket-csv "${csv_out}" 0 64 \
    --export-stat-qd-csv "${qd_csv_out}" \
    --export-stat-wa-csv "${wa_csv_out}" \
    "${f}" 0 >/dev/null 2>&1
  test -s "${csv_out}"
  test -s "${qd_csv_out}"
  test -s "${wa_csv_out}"
done

echo "[4/4] run invalid-data skip cases"
for f in "${SKIP_CASES[@]}"; do
  echo "  SKIP expected (success with invalid-count in debug): ${f}"
  ./post_action_file_tester "${f}" >/dev/null 2>&1
done

for f in "${FAIL_CASES[@]}"; do
  echo "  FAIL expected (overwrite should stop parsing): ${f}"
  if ./post_action_file_tester "${f}" >/tmp/post_action_overwrite.log 2>&1; then
    echo "unexpected success for ${f}" >&2
    exit 1
  fi
  if ! rg -i "overwrite.*lba|lba.*overwrite" "/tmp/post_action_overwrite.log" >/dev/null; then
    echo "missing overwrite+lba warning for ${f}" >&2
    exit 1
  fi
done

echo "all post-action tests passed"
