#!/usr/bin/env bash
#
# scripts/lint.sh
#
# Run the same lint checks that GitHub Actions runs, locally. Requires Node
# 18+ (for npx). The first run downloads JSHint and Prettier into the
# repo's local npm cache; subsequent runs reuse the cache.
#
# Usage:
#   scripts/lint.sh             # lint everything
#   scripts/lint.sh --js-only   # just the embedded JS
#
# GPL-3.0-or-later

set -euo pipefail
cd "$(dirname "$0")/.."

JS_ONLY=0
FIX=0
for arg in "$@"; do
  case "$arg" in
    --js-only) JS_ONLY=1 ;;
    --fix)     FIX=1 ;;
    -h|--help)
      sed -n '/^# Usage:/,/^# GPL/p' "$0"
      exit 0
      ;;
    *) echo "unknown flag: $arg" >&2; exit 1 ;;
  esac
done

if ! command -v npx >/dev/null 2>&1; then
  echo "error: npx not found. Install Node.js 18 or newer."
  exit 1
fi

# Stash for the extracted JS so we don't litter the repo
TMP_JS="$(mktemp -t pinscope-XXXXXX.js)"
trap "rm -f '$TMP_JS'" EXIT

echo "[lint] extracting embedded JS from pinscope.html"
# Find line numbers of the first <script> and last </script> tags. Using
# the LAST closing tag protects against inline strings that contain a
# literal </script> substring (e.g. the plugin iframe srcdoc).
START_LINE=$(grep -n '<script>' pinscope.html | head -1 | cut -d: -f1)
END_LINE=$(grep -n '</script>' pinscope.html | tail -1 | cut -d: -f1)
if [[ -z "$START_LINE" || -z "$END_LINE" ]]; then
  echo "[lint] could not locate <script>...</script> in pinscope.html"
  exit 1
fi
sed -n "$((START_LINE + 1)),$((END_LINE - 1))p" pinscope.html > "$TMP_JS"
JS_LINES=$(wc -l < "$TMP_JS")
echo "[lint] $JS_LINES lines extracted (from line $START_LINE to $END_LINE)"

echo "[lint] running JSHint"
npx --yes jshint@2.13.6 --config .jshintrc "$TMP_JS"
echo "[lint] JSHint: OK"

if [[ $JS_ONLY -eq 1 ]]; then
  exit 0
fi

echo "[lint] running Prettier (text files only; pinscope.html is ignored)"
if [[ $FIX -eq 1 ]]; then
  npx --yes prettier@3.3.3 --write 'README.md' '.jshintrc' '.prettierrc' 'CONTRIBUTING.md'
  echo "[lint] Prettier: rewrote text files in place"
else
  npx --yes prettier@3.3.3 --check 'README.md' '.jshintrc' '.prettierrc' 'CONTRIBUTING.md' || {
    echo "[lint] Prettier found formatting issues. Run 'scripts/lint.sh --fix' to auto-fix."
    exit 1
  }
  echo "[lint] Prettier: OK"
fi

echo "[lint] checking for em dashes in tracked sources"
DASH=$(printf '\u2014')
if grep -l "$DASH" README.md CONTRIBUTING.md pinscope.html pinscope*.ino pinscope_mqtt_bridge.py 2>/dev/null; then
  echo "[lint] em dashes detected; replace with commas, periods, or parentheses."
  exit 1
fi
echo "[lint] em-dash check: OK"

echo
echo "[lint] all checks passed."
