#!/usr/bin/env bash
# Build + run dev-machine unit tests for the resident-db-layer spec.
# Run from the repo root:  bash tests/run_tests.sh
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

OUT=/tmp/scabin_tests
mkdir -p "$OUT"
fail=0

echo "== MatchEngine =="
g++ -std=c++17 -Isrc \
    tests/test_match_engine.cpp src/match_engine.cpp \
    -o "$OUT/test_match_engine" && "$OUT/test_match_engine" || fail=1

# ResidentDB test needs sqlite3.h + libsqlite3.
# Prefer a system install; else fall back to a no-sudo local extract at
# /tmp/sqlite_local (see tests/_setup_sqlite_local.sh).
SQLITE_INC=""
SQLITE_LIB="-lsqlite3"
if [ -f /usr/include/sqlite3.h ]; then
    :  # system install present
elif [ -f /tmp/sqlite_local/usr/include/sqlite3.h ]; then
    SQLITE_INC="-I/tmp/sqlite_local/usr/include"
    SQLITE_LIB="/tmp/sqlite_local/usr/lib/x86_64-linux-gnu/libsqlite3.a -ldl"
else
    SQLITE_INC="MISSING"
fi

if [ "$SQLITE_INC" != "MISSING" ]; then
    echo ""
    echo "== ResidentDB =="
    g++ -std=c++17 -Isrc $SQLITE_INC \
        tests/test_resident_db.cpp src/resident_db.cpp \
        $SQLITE_LIB -lpthread \
        -o "$OUT/test_resident_db" \
        && "$OUT/test_resident_db" "$ROOT/db/schema.sql" || fail=1
else
    echo ""
    echo "== ResidentDB == SKIPPED (no sqlite3.h)"
    echo "   option 1: sudo apt-get install -y libsqlite3-dev"
    echo "   option 2 (no sudo): bash tests/_setup_sqlite_local.sh"
fi

echo ""
if [ "$fail" -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "SOME TESTS FAILED"; fi
exit $fail
