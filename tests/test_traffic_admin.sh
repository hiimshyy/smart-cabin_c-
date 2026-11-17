#!/usr/bin/env bash
set -euo pipefail

BIN="${1:?usage: test_traffic_admin.sh <traffic_admin_bin>}"
TMP="/tmp/scabin_tests/traffic_admin_cli_$$"
rm -rf "$TMP"
mkdir -p "$TMP/out"
trap 'rm -rf "$TMP"' EXIT

DB="$TMP/traffic.db"

# Help flags exit 0 cleanly
"$BIN" --help >/dev/null
"$BIN" --db "$DB" --schema db/traffic_schema.sql visitors --help >/dev/null
"$BIN" --db "$DB" --schema db/traffic_schema.sql export --help >/dev/null

# Seed two active visitors with dummy embeddings + trip via sqlite3
python3 - "$DB" <<'PY'
import sqlite3, struct, sys
db = sqlite3.connect(sys.argv[1])
v1_blob = struct.pack('4f', 1.0, 0.0, 0.0, 0.0)
v2_blob = struct.pack('4f', 0.99, 0.1, 0.0, 0.0)
db.execute("INSERT INTO visitors(id,first_seen_ms,last_seen_ms,trip_count,status) VALUES (1,1700000000000,1700000010000,2,'active')")
db.execute("INSERT INTO visitors(id,first_seen_ms,last_seen_ms,trip_count,status) VALUES (2,1700000020000,1700000030000,1,'active')")
db.execute("INSERT INTO visitor_embeddings(visitor_id,vector,quality,created_at_ms) VALUES (1,?,0.9,1700000000000)", (v1_blob,))
db.execute("INSERT INTO visitor_embeddings(visitor_id,vector,quality,created_at_ms) VALUES (2,?,0.8,1700000020000)", (v2_blob,))
db.execute("INSERT INTO passenger_trips(trip_key,cabin_id,entered_at_ms,exited_at_ms,floor_in,floor_out,identity_kind,visitor_id,rides,confidence,flags) VALUES ('s-t-1',2,1700000000000,1700000005000,0,5,'visitor',2,1,'high',0)")
db.commit()
PY

LIST_OUT="$("$BIN" --db "$DB" visitors list --status active --limit 10)"
echo "$LIST_OUT" | grep -q "^2 "
echo "$LIST_OUT" | grep -q "^1 "

SHOW_OUT="$("$BIN" --db "$DB" visitors show 2)"
echo "$SHOW_OUT" | grep -q "Visitor 2: status=active"
echo "$SHOW_OUT" | grep -q "trip=s-t-1"

"$BIN" --db "$DB" visitors merge 2 1 --yes >/dev/null
"$BIN" --db "$DB" visitors show 1 | grep -q "trips=3"

"$BIN" --db "$DB" visitors promote 1 --name "Promoted Test" --floor 5 --resident-db "$TMP/residents.db" --yes >/dev/null
"$BIN" --db "$DB" visitors show 1 | grep -q "status=promoted"

EXP_OUT="$("$BIN" --db "$DB" export --out-dir "$TMP/out")"
echo "$EXP_OUT" | grep -q "passenger_trips.csv: 1 rows"
grep -q ",B,5," "$TMP/out/passenger_trips.csv"

echo "[test_traffic_admin] PASS"
