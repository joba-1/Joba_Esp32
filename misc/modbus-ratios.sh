#!/usr/bin/env bash

set -euo pipefail

HOST="${1:-joba-esp32-c40a24}"
USER="${MODBUS_USER:-admin}"
PASS="${MODBUS_PASS:-}"

if [[ -z "$PASS" ]]; then
  echo "Set MODBUS_PASS (and optionally MODBUS_USER) in env." >&2
  echo "Example: MODBUS_PASS='Joba_Esp32-C40A24' $0 $HOST" >&2
  exit 2
fi

URL="http://${HOST}/api/modbus/status"

json="$(curl -sS --max-time 18 --digest -u "${USER}:${PASS}" "$URL")"

python3 - "$URL" <<'PY'
import json, os, sys

try:
  data = json.loads(sys.stdin.read())
except Exception as e:
  print(f"Failed to parse JSON from {sys.argv[1]}: {e}", file=sys.stderr)
  data = {}
  
uptime = (data.get("updated") or {}).get("uptimeMs", 0)

crc = int(data.get("crcErrors") or 0)

other_req = int(data.get("otherRequestsSeen") or 0)
other_resp = int(data.get("otherResponsesSeen") or 0)
other_exc = int(data.get("otherExceptionsSeen") or 0)
other_total = other_req + other_resp + other_exc

den = other_total + crc
crc_pct = (100.0 * crc / den) if den else 0.0

other_pair = data.get("otherPairing") or {}
rp = int(other_pair.get("responsesPaired") or 0)
ru = int(other_pair.get("responsesUnpaired") or 0)
rt = rp + ru
pair_pct = (100.0 * rp / rt) if rt else 0.0

listen_only = data.get("listenOnly", None)

print(f"uptimeMs={uptime} listenOnly={listen_only}")
print(f"otherFrames: requests={other_req} responses={other_resp} exceptions={other_exc} total={other_total}")
print(f"crcErrors={crc} crcErrorRateAllSeen={crc_pct:.2f}%  (crc/(crc+otherFrames))")
print(f"otherResponsePairing: paired={rp} unpaired={ru} pairedPct={pair_pct:.2f}%")
PY