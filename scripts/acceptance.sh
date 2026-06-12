#!/bin/bash
# §11 acceptance checks (Linux x86_64 leg). Slow: starts the real organs and
# pays CPU-inference prices. Run with the dev env sourced, or after packaging
# with payloads embedded.
#
#   source scripts/dev-env.sh && bash scripts/acceptance.sh [BINARY]
#
# CPU-only note: extraction costs minutes/chunk at ~7 tok/s, so this script
# keeps corpora tiny (and uses content-duplicate files for the 1000-doc
# identity-check timing, which dedups to a single embedded document).
set -u
BIN="${1:-build-native/chimera}"
WORK="$(mktemp -d /tmp/chimera-accept-XXXX)"
trap 'rm -rf "$WORK"' EXIT
pass=0; fail=0
ok()   { echo "PASS $1"; pass=$((pass+1)); }
bad()  { echo "FAIL $1"; fail=$((fail+1)); }

echo "=== acceptance against $BIN ==="

# -- 2. file confirms APE; nothing dynamically linked (cosmo build only) -----
if [ -f build-cosmo/chimera ]; then
  file build-cosmo/chimera | grep -q "DOS/MBR" && ok "2a: APE magic" || bad "2a: APE magic"
  ldd build-cosmo/chimera 2>&1 | grep -qi "not a dynamic\|statically" && ok "2b: static" || bad "2b: static"
else
  echo "SKIP 2: no build-cosmo/chimera"
fi

# -- 3. mixed corpus ingest + fast idempotent re-run -------------------------
mkdir -p "$WORK/corpus/sub"
cat > "$WORK/corpus/notes.md" <<'EOF'
# Meeting notes

Maria Chen leads Project Phoenix, the billing system rewrite.
It launched in March 2024 after Acme Corp acquired WidgetCo.
EOF
printf 'def total(xs):\n    return sum(xs)\n' > "$WORK/corpus/calc.py"
printf 'plain text about chimeras and hybrid databases\n' > "$WORK/corpus/sub/about.txt"
# 1000-doc identity-check timing corpus: duplicates dedup to one document,
# so first ingest embeds once but re-run must identity-check 1000 paths.
for i in $(seq 1 997); do printf 'duplicate content body\n' > "$WORK/corpus/dup_$i.txt"; done

echo "--- first ingest (slow: real extraction) ..."
"$BIN" ingest "$WORK/corpus" > "$WORK/ingest1.log" 2>&1
grep -q "graph ok" "$WORK/ingest1.log" && ok "3a: ingest completes" || { bad "3a: ingest completes"; cat "$WORK/ingest1.log"; }

t0=$(date +%s%N)
"$BIN" ingest "$WORK/corpus" > "$WORK/ingest2.log" 2>&1
t1=$(date +%s%N)
ms=$(( (t1 - t0) / 1000000 ))
grep -q "0 new, 1000 skipped" "$WORK/ingest2.log" && ok "3b: re-run 100% skipped" || { bad "3b: re-run skipped"; cat "$WORK/ingest2.log"; }
[ "$ms" -lt 2000 ] && ok "3c: re-run in ${ms}ms (<2s for 1000 docs)" || bad "3c: re-run took ${ms}ms"

# -- 4. middle edit → supersession; --paranoid catches blind-spot edits ------
sed -i 's/March 2024/April 2024/' "$WORK/corpus/notes.md"
"$BIN" ingest "$WORK/corpus" > "$WORK/ingest3.log" 2>&1
grep -q "1 superseded" "$WORK/ingest3.log" && ok "4a: edit superseded old doc" || { bad "4a: supersession"; cat "$WORK/ingest3.log"; }

python3 - "$WORK/corpus/blind.bin.txt" <<'EOF'
import sys
# 400 KiB of 'a' — bigger than the 192 KiB whole-file window
open(sys.argv[1], "w").write("a" * 400 * 1024)
EOF
"$BIN" ingest "$WORK/corpus" > /dev/null 2>&1
python3 - "$WORK/corpus/blind.bin.txt" <<'EOF'
import sys
data = bytearray(open(sys.argv[1], "rb").read())
data[100 * 1024] = ord("b")   # between head and middle taps: sampled blind spot
open(sys.argv[1], "wb").write(bytes(data))
import os
os.utime(sys.argv[1], (1700000000, 1700000000))  # defeat mtime fast-path too
EOF
"$BIN" ingest "$WORK/corpus" > "$WORK/ingest4.log" 2>&1
grep -q "0 new" "$WORK/ingest4.log" && ok "4b: triple-tap blind spot (expected miss)" || bad "4b: blind spot unexpectedly caught"
"$BIN" verify --db "$WORK/corpus/.chimera" --paranoid > "$WORK/verify.log" 2>&1
grep -q "drifted" "$WORK/verify.log" && ok "4c: --paranoid catches blind-spot edit" || { bad "4c: paranoid"; cat "$WORK/verify.log"; }

# -- 5. search returns cited verified answer; deletion shows missing ---------
echo "--- search (slow: model load + synthesis) ..."
"$BIN" --search "who leads Project Phoenix?" --db "$WORK/corpus/.chimera" > "$WORK/search1.log" 2>&1
grep -q "✓ verified" "$WORK/search1.log" && ok "5a: ≥1 citation verified" || { bad "5a: verified citation"; tail -20 "$WORK/search1.log"; }
grep -qi "maria\|chen" "$WORK/search1.log" && ok "5b: answer names Maria Chen" || bad "5b: answer content"

rm "$WORK/corpus/notes.md"
"$BIN" --search "who leads Project Phoenix?" --db "$WORK/corpus/.chimera" > "$WORK/search2.log" 2>&1
grep -q "⚠ missing" "$WORK/search2.log" && ok "5c: deleted source marked missing" || { bad "5c: missing mark"; tail -20 "$WORK/search2.log"; }

# -- 6. vacuum shrinks stores; sparql still works -----------------------------
size_before=$(du -sb "$WORK/corpus/.chimera/qlever" | cut -f1)
"$BIN" vacuum --db "$WORK/corpus/.chimera" > "$WORK/vacuum.log" 2>&1
size_after=$(du -sb "$WORK/corpus/.chimera/qlever" | cut -f1)
grep -q "purged" "$WORK/vacuum.log" && ok "6a: vacuum ran" || { bad "6a: vacuum"; cat "$WORK/vacuum.log"; }
"$BIN" sparql 'PREFIX ch: <chimera://ontology#> SELECT (COUNT(*) AS ?n) WHERE { ?d a ch:Document }' \
    --db "$WORK/corpus/.chimera" > "$WORK/sparql.log" 2>&1
grep -q '"n"' "$WORK/sparql.log" && ok "6b: sparql works post-vacuum" || { bad "6b: sparql"; cat "$WORK/sparql.log"; }
echo "    (qlever store: $size_before → $size_after bytes)"

# -- 7. kill -9 mid-ingest, re-run: no duplicates ------------------------------
mkdir -p "$WORK/kcorp"
for i in 1 2 3; do printf 'kill test doc %d with unique content %d\n' "$i" "$i" > "$WORK/kcorp/k$i.txt"; done
"$BIN" ingest "$WORK/kcorp" > "$WORK/k1.log" 2>&1 &
IPID=$!
sleep 100  # mid-pipeline: model is up, somewhere in embed/extract
kill -9 $IPID 2>/dev/null
sleep 2
pkill -9 -f "$WORK/kcorp/.chimera" 2>/dev/null  # orphaned organs, if any
rm -f "$WORK/kcorp/.chimera/lock"
"$BIN" ingest "$WORK/kcorp" > "$WORK/k2.log" 2>&1
grep -q "graph ok" "$WORK/k2.log" && ok "7a: resume completes" || { bad "7a: resume"; cat "$WORK/k2.log"; }
n_chunks=$("$BIN" sparql 'PREFIX ch: <chimera://ontology#> SELECT (COUNT(DISTINCT ?c) AS ?n) WHERE { ?c a ch:Chunk }' --db "$WORK/kcorp/.chimera" 2>/dev/null | python3 -c 'import json,sys; print(json.load(sys.stdin)["results"]["bindings"][0]["n"]["value"])' 2>/dev/null)
n_manifest=$(echo 'SELECT COUNT(*) FROM chunk;' | sqlite3 "$WORK/kcorp/.chimera/manifest.db" 2>/dev/null)
[ -n "$n_chunks" ] && [ "$n_chunks" = "$n_manifest" ] && ok "7b: graph chunks ($n_chunks) == manifest chunks ($n_manifest), zero dupes" || bad "7b: chunk count mismatch (graph=$n_chunks manifest=$n_manifest)"

echo
echo "=== $pass passed, $fail failed ==="
exit $((fail > 0))
