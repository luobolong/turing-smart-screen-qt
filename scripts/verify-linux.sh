#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
binary="${1:-build/linux/turing-screen-qt}"
name="${2:-native}"
mode="${3:---smoke-test}"
seconds="${4:-60}"
output="artifacts/linux-$name"
mkdir -p "$output/data"
printf '%s\n' '{"language":"chinese","interval_ms":1000,"minimize_to_tray":true}' > "$output/data/config.json"
rm -f "$output/smoke-ok.txt"
timeout "$((seconds + 30))s" "$binary" "$mode" "$output" --test-seconds "$seconds" --data-dir "$output/data" 2>"$output/stderr.txt"
test -f "$output/smoke-ok.txt"
python3 - "$output/resources.json" <<'PY'
import json,sys
r=json.load(open(sys.argv[1]));print({k:v for k,v in r.items() if k!='samples'})
if not r['passed']:raise SystemExit('100 MB target exceeded')
PY
"$binary" --render-theme assets/official/config/微星.data --output "$output/msi.png" --snapshot tests/fixtures/official-editor.json --editor-preview
