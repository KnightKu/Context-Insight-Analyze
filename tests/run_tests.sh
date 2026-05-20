#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

echo "[insight-api] build testers"
make insight_api_tester insight_api_json_tester >/dev/null

echo "[insight-api] run insight_api_tester"
./insight_api_tester

echo "[insight-api] run insight_api_json_tester"
./insight_api_json_tester

echo "insight api tests passed"
