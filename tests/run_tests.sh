#!/usr/bin/env bash
# Build + run dev-machine unit tests for the resident-db-layer spec.
# Run from the repo root:  bash tests/run_tests.sh
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

OUT=/tmp/scabin_tests
mkdir -p "$OUT"
fail=0

echo "== Logger =="
g++ -std=c++17 -Isrc \
    tests/test_logger.cpp src/log/logger.cpp \
    -lpthread \
    -o "$OUT/test_logger" && "$OUT/test_logger" || fail=1

echo ""
echo "== AppConfig =="
g++ -std=c++17 -Isrc \
    tests/test_app_config.cpp src/app_config.cpp \
    -o "$OUT/test_app_config" && "$OUT/test_app_config" || fail=1

echo ""
echo "== Video frame/deadline wait =="
g++ -std=c++17 -Isrc $(pkg-config --cflags opencv4) \
    tests/test_video_wait.cpp src/video_io.cpp src/log/logger.cpp \
    $(pkg-config --libs opencv4) -lpthread \
    -o "$OUT/test_video_wait" && "$OUT/test_video_wait" || fail=1

echo ""
echo "== MatchEngine =="
g++ -std=c++17 -Isrc \
    tests/test_match_engine.cpp src/match_engine.cpp src/log/logger.cpp \
    -lpthread \
    -o "$OUT/test_match_engine" && "$OUT/test_match_engine" || fail=1

echo ""
echo "== Tracker catalog invalidation =="
g++ -std=c++17 -Isrc $(pkg-config --cflags opencv4) \
    tests/test_tracker.cpp src/tracker.cpp src/log/logger.cpp \
    -lpthread \
    -o "$OUT/test_tracker" && "$OUT/test_tracker" || fail=1

echo ""
echo "== Resident catalog reconciliation =="
g++ -std=c++17 -Isrc $(pkg-config --cflags opencv4) \
    tests/test_resident_reconcile.cpp src/resident_reconcile.cpp \
    src/tracker.cpp src/interaction.cpp src/log/logger.cpp \
    -lpthread \
    -o "$OUT/test_resident_reconcile" && "$OUT/test_resident_reconcile" || fail=1

echo ""
echo "== InteractionManager =="
g++ -std=c++17 -Isrc \
    tests/test_interaction.cpp src/interaction.cpp src/match_engine.cpp src/log/logger.cpp \
    -lpthread \
    -o "$OUT/test_interaction" && "$OUT/test_interaction" || fail=1

echo ""
echo "== EdgeJson =="
g++ -std=c++17 -Isrc \
    tests/test_edge_json.cpp src/edge_json.cpp \
    -o "$OUT/test_edge_json" && "$OUT/test_edge_json" || fail=1

echo ""
echo "== EdgeFrame =="
g++ -std=c++17 -Isrc \
    tests/test_edge_frame.cpp src/edge_client.cpp src/edge_json.cpp src/log/logger.cpp \
    -lpthread \
    -o "$OUT/test_edge_frame" && "$OUT/test_edge_frame" || fail=1

# ResidentDB test needs sqlite3.h + libsqlite3.
# Prefer a system install; else fall back to a no-sudo local extract at
# /tmp/sqlite_local (see tests/_setup_sqlite_local.sh).
SQLITE_INC=""
SQLITE_LIB="-lsqlite3"
if [ -f /usr/include/sqlite3.h ]; then
    :  # system install present
elif [ -f /tmp/sqlite_local/usr/include/sqlite3.h ]; then
    SQLITE_INC="-I/tmp/sqlite_local/usr/include"
    SQLITE_ARCHIVE="$(find /tmp/sqlite_local/usr/lib -name libsqlite3.a -print -quit 2>/dev/null)"
    if [ -n "$SQLITE_ARCHIVE" ]; then SQLITE_LIB="$SQLITE_ARCHIVE -ldl"; else SQLITE_INC="MISSING"; fi
else
    SQLITE_INC="MISSING"
fi

if [ "$SQLITE_INC" != "MISSING" ]; then
    echo ""
    echo "== ResidentDB =="
    g++ -std=c++17 -Isrc $SQLITE_INC \
        tests/test_resident_db.cpp src/resident_db.cpp src/log/logger.cpp \
        $SQLITE_LIB -lpthread \
        -o "$OUT/test_resident_db" \
        && "$OUT/test_resident_db" "$ROOT/db/schema.sql" || fail=1

    echo ""
    echo "== ResidentEnroll atomic batch =="
    g++ -std=c++17 -Isrc $SQLITE_INC \
        tests/test_resident_enroll.cpp src/resident_enroll.cpp src/resident_db.cpp src/log/logger.cpp \
        $SQLITE_LIB -lpthread \
        -o "$OUT/test_resident_enroll" \
        && "$OUT/test_resident_enroll" "$ROOT/db/schema.sql" || fail=1

    echo ""
    echo "== ResidentCatalog hot reload =="
    g++ -std=c++17 -DRESIDENT_DB_TEST_HOOK -Isrc $SQLITE_INC \
        tests/test_resident_catalog.cpp src/resident_catalog.cpp src/resident_enroll.cpp \
        src/resident_db.cpp src/match_engine.cpp src/log/logger.cpp \
        $SQLITE_LIB -lpthread \
        -o "$OUT/test_resident_catalog" \
        && "$OUT/test_resident_catalog" "$ROOT/db/schema.sql" || fail=1

    echo ""
    echo "== Resident reload during no-frame outage =="
    g++ -std=c++17 -Isrc $SQLITE_INC $(pkg-config --cflags opencv4) \
        tests/test_reload_no_frame.cpp src/resident_catalog.cpp src/resident_enroll.cpp \
        src/resident_db.cpp src/match_engine.cpp src/video_io.cpp src/log/logger.cpp \
        $SQLITE_LIB $(pkg-config --libs opencv4) -lpthread \
        -o "$OUT/test_reload_no_frame" \
        && "$OUT/test_reload_no_frame" "$ROOT/db/schema.sql" || fail=1

    echo ""
    echo "== CabinConfig =="
    g++ -std=c++17 -Isrc $SQLITE_INC \
        tests/test_cabin_config.cpp src/cabin_config.cpp src/resident_db.cpp src/log/logger.cpp \
        $SQLITE_LIB -lpthread \
        -o "$OUT/test_cabin_config" \
        && "$OUT/test_cabin_config" || fail=1
else
    echo ""
    echo "== SQLite-dependent tests == FAILED (no sqlite3 headers/library)"
    echo "   ResidentDB, ResidentEnroll, ResidentCatalog and CabinConfig did NOT run."
    echo "   option 1: sudo apt-get install -y libsqlite3-dev"
    echo "   option 2 (no sudo): bash tests/_setup_sqlite_local.sh"
    fail=1
fi

echo ""
if [ "$fail" -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "SOME TESTS FAILED"; fi
exit $fail
