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
)

FAIL_CASES=(
  "tests/fixtures/invalid_op.bin"
  "tests/fixtures/invalid_rw_reserved.bin"
  "tests/fixtures/invalid_trim_total_ranges.bin"
  "tests/fixtures/invalid_alignment.bin"
)

echo "[3/4] run pass cases"
for f in "${PASS_CASES[@]}"; do
  echo "  PASS expected: ${f}"
  ./post_action_file_tester "${f}" >/dev/null 2>&1
done

echo "[4/4] run fail cases"
for f in "${FAIL_CASES[@]}"; do
  echo "  FAIL expected: ${f}"
  if ./post_action_file_tester "${f}" >/dev/null 2>&1; then
    echo "unexpected success for ${f}" >&2
    exit 1
  fi
done

echo "all post-action tests passed"
