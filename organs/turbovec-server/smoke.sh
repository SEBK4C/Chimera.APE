#!/bin/bash
# Smoke test for turbovec-server. Exercises every endpoint against a fresh
# index, then persistence round-trip. Exits non-zero on any failed check.
set -u
BIN="$(dirname "$0")/target/release/turbovec-server"
TVIM="$(mktemp -u /tmp/tv-smoke-XXXX.tvim)"
trap 'kill $SRV 2>/dev/null; rm -f "$TVIM"' EXIT

fail=0
check() { # check <label> <expected-substring> <actual>
  if [[ "$3" == *"$2"* ]]; then echo "ok   $1"; else echo "FAIL $1: expected *$2* got: $3"; fail=1; fi
}

"$BIN" --port 0 --path "$TVIM" > /tmp/tv-smoke.out 2>/tmp/tv-smoke.err &
SRV=$!
for i in $(seq 50); do grep -q '^PORT' /tmp/tv-smoke.out 2>/dev/null && break; sleep 0.1; done
PORT=$(awk '/^PORT/{print $2}' /tmp/tv-smoke.out)
[[ -n "$PORT" ]] || { echo "FAIL: no PORT line"; cat /tmp/tv-smoke.err; exit 1; }
U="http://127.0.0.1:$PORT"

check health   '"status":"ok"' "$(curl -s $U/health)"
check info0    '"count":0'     "$(curl -s $U/info)"
check upsert3  '"upserted":3'  "$(curl -s -X POST $U/upsert -d '{"ids":[101,102,103],"vectors":[[1,0,0,0,0,0,0,0],[0,1,0,0,0,0,0,0],[0.9,0.1,0,0,0,0,0,0]]}')"
check query    '101'           "$(curl -s -X POST $U/query -d '{"vectors":[[1,0,0,0,0,0,0,0]],"k":2}')"
allow=$(curl -s -X POST $U/query -d '{"vectors":[[1,0,0,0,0,0,0,0]],"k":2,"allowlist":[102,103]}')
check allowlist '103'          "$allow"
check noleak    '101'          "$(echo "$allow" | grep -q 101 && echo LEAKED101 || echo no-101-ok)"
[[ "$allow" == *101* ]] && { echo "FAIL allowlist leaked 101: $allow"; fail=1; }
check move101  '"upserted":1'  "$(curl -s -X POST $U/upsert -d '{"ids":[101],"vectors":[[0,1,0,0,0,0,0,0]]}')"
q3=$(curl -s -X POST $U/query -d '{"vectors":[[1,0,0,0,0,0,0,0]],"k":1}')
check requery  '103'           "$q3"
check remove   '"removed":1'   "$(curl -s -X POST $U/remove -d '{"ids":[103,999]}')"
check baddim   'error'         "$(curl -s -X POST $U/upsert -d '{"ids":[7],"vectors":[[1,2,3]]}')"
check persist  '"persisted":true' "$(curl -s -X POST $U/persist)"
check shutdown '"shutdown":true'  "$(curl -s -X POST $U/shutdown)"
wait $SRV 2>/dev/null

"$BIN" --port 0 --path "$TVIM" > /tmp/tv-smoke2.out 2>/tmp/tv-smoke2.err &
SRV=$!
for i in $(seq 50); do grep -q '^PORT' /tmp/tv-smoke2.out 2>/dev/null && break; sleep 0.1; done
PORT=$(awk '/^PORT/{print $2}' /tmp/tv-smoke2.out)
U="http://127.0.0.1:$PORT"
check reload   '"count":2'     "$(curl -s $U/info)"
check redim    '"dim":8'       "$(curl -s $U/info)"
curl -s -X POST $U/shutdown > /dev/null

if [[ $fail -eq 0 ]]; then echo "ALL CHECKS PASSED"; else echo "SMOKE FAILED"; exit 1; fi
