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
  "tests/fixtures/valid_relative_time_with_marker.bin"
  "tests/fixtures/valid_zero_op_skip.bin"
  "tests/fixtures/valid_pre_marker_skip.bin"
)

SKIP_CASES=(
  "tests/fixtures/invalid_op.bin"
  "tests/fixtures/invalid_rw_reserved.bin"
  "tests/fixtures/invalid_trim_total_ranges.bin"
  "tests/fixtures/invalid_alignment.bin"
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
if ! ./post_action_file_tester \
  --read-count \
  --w2fr \
  --life-cycle \
  "tests/fixtures/valid_mixed.bin" >/tmp/post_action_lba_stats.log 2>&1; then
  echo "unexpected failure for lba stats output case" >&2
  exit 1
fi
if ! rg -i "Read Count distribution|Write-to-First-Read Latency\\(real ms\\) distribution|Life Cycle\\(real ms\\) distribution" "/tmp/post_action_lba_stats.log" >/dev/null; then
  echo "missing segmented lba-stats output sections" >&2
  exit 1
fi
if ! rg -i "Read Count.*bytes=|Life Cycle.*bytes=" "/tmp/post_action_lba_stats.log" >/dev/null; then
  echo "missing data-volume bytes column for read-count/life-cycle output" >&2
  exit 1
fi

echo "  LBA-STATS selective output expected"
if ! ./post_action_file_tester --read-count "tests/fixtures/valid_mixed.bin" >/tmp/post_action_lba_read_only.log 2>&1; then
  echo "unexpected failure for lba read-count output case" >&2
  exit 1
fi
if ! rg -i "Read Count distribution" "/tmp/post_action_lba_read_only.log" >/dev/null; then
  echo "missing Read Count distribution section" >&2
  exit 1
fi
if rg -i "Write-to-First-Read Latency\\(real ms\\) distribution|Life Cycle\\(real ms\\) distribution" "/tmp/post_action_lba_read_only.log" >/dev/null; then
  echo "unexpected non-read-count sections in read-count-only output" >&2
  exit 1
fi

echo "  LBA-STATS request-count expected: one 32K read should count as one request"
if ! ./post_action_file_tester --read-count "tests/fixtures/valid_request_count_32k.bin" >/tmp/post_action_lba_req_count.log 2>&1; then
  echo "unexpected failure for request-count fixture case" >&2
  exit 1
fi
if ! rg -i "Read Count.*1-25.*count=1" "/tmp/post_action_lba_req_count.log" >/dev/null; then
  echo "read-count distribution does not reflect request-based counting" >&2
  exit 1
fi

echo "  WORKLOAD-STATS expected: qd/wa/read-write-trim size distribution output"
if ! ./post_action_file_tester \
  --qd-dist \
  --wa-dist \
  --read-size-dist \
  --write-size-dist \
  --trim-size-dist \
  "tests/fixtures/valid_mixed.bin" >/tmp/post_action_workload_stats.log 2>&1; then
  echo "unexpected failure for workload stats output case" >&2
  exit 1
fi
if ! rg -i "QD distribution:|WA distribution:|Read Size distribution \\(4K blocks\\):|Write Size distribution \\(4K blocks\\):|Trim Size distribution \\(4K blocks\\):" "/tmp/post_action_workload_stats.log" >/dev/null; then
  echo "missing workload distribution output sections" >&2
  exit 1
fi
if ! rg "\\[1\\.0,1\\.4\\)|\\[4\\.6,5\\.0\\]" "/tmp/post_action_workload_stats.log" >/dev/null; then
  echo "missing compressed WA bucket labels [1.0,1.4) or [4.6,5.0]" >&2
  exit 1
fi
if ! rg "Read Size\\s+4K|Write Size\\s+8K|Trim Size\\s+16K" "/tmp/post_action_workload_stats.log" >/dev/null; then
  echo "missing explicit IO-size labels (4K/8K/16K...) in workload output" >&2
  exit 1
fi

echo "  LATENCY expected: -l enables latency summary output"
if ! ./post_action_file_tester --latency "tests/fixtures/valid_mixed.bin" >/tmp/post_action_latency.log 2>&1; then
  echo "unexpected failure for latency output case" >&2
  exit 1
fi
if ! rg -i "latency summary" "/tmp/post_action_latency.log" >/dev/null; then
  echo "missing latency summary output when --latency enabled" >&2
  exit 1
fi
if ! rg -i "Qos p10=.*p20=.*p30=.*p40=.*p50=.*p60=.*p70=.*p80=.*p90=.*p99=.*p99\\.9=.*p99\\.99=" "/tmp/post_action_latency.log" >/dev/null; then
  echo "missing expected percentile coverage (p10~p90 step10, p99, p99.9, p99.99)" >&2
  exit 1
fi

echo "  CROSS-CHUNK marker continuity expected"
if ! ./post_action_file_tester --split-bytes 8 "tests/fixtures/valid_relative_time_with_marker.bin" >/tmp/post_action_chunk_continuity.log 2>&1; then
  echo "unexpected failure for cross-chunk marker continuity case" >&2
  exit 1
fi
if rg -i "missing marker" "/tmp/post_action_chunk_continuity.log" >/dev/null; then
  echo "unexpected missing marker when parsing split chunks" >&2
  exit 1
fi

for f in "${SOFT_STOP_CASES[@]}"; do
  echo "  SOFT-STOP expected (overwrite should stop parse/read but not fail): ${f}"
  if ! ./post_action_file_tester --debug "${f}" >/tmp/post_action_overwrite.log 2>&1; then
    echo "unexpected failure for ${f}" >&2
    exit 1
  fi
  if ! rg -i "overwrite.*lba|lba.*overwrite" "/tmp/post_action_overwrite.log" >/dev/null; then
    echo "missing overwrite+lba warning for ${f}" >&2
    exit 1
  fi
done

echo "  SOFT-STOP expected (all-zero record indicates invalid tail): tests/fixtures/valid_termination.bin"
if ! ./post_action_file_tester --debug "tests/fixtures/valid_termination.bin" >/tmp/post_action_all_zero.log 2>&1; then
  echo "unexpected failure for valid_termination.bin" >&2
  exit 1
fi
if ! rg -i "all-zero record detected|soft-stop" "/tmp/post_action_all_zero.log" >/dev/null; then
  echo "missing all-zero soft-stop hint in output" >&2
  exit 1
fi

echo "all post-action tests passed"
