#!/usr/bin/env bash
# Build + run dev-machine unit tests for the resident-db-layer spec.
# Run from the repo root:  bash tests/run_tests.sh
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

OUT=/tmp/scabin_tests
mkdir -p "$OUT"
fail=0

echo "== env.sh face_cabin redaction =="
bash tests/test_env_face_cabin.sh || fail=1

echo ""
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
echo "== YoloDecoder animals =="
g++ -std=c++17 -Isrc -I/home/orangepi/ai-sdk -I/home/orangepi/ai-sdk/examples/libawnn_viplite \
    $(pkg-config --cflags opencv4) \
    tests/test_yolo_post.cpp src/yolo_post.cpp \
    $(pkg-config --libs opencv4) -o "$OUT/test_yolo_post" && "$OUT/test_yolo_post" || fail=1

echo ""
echo "== Animal snapshot writer =="
g++ -std=c++17 -Isrc -I/home/orangepi/ai-sdk -I/home/orangepi/ai-sdk/examples/libawnn_viplite \
    $(pkg-config --cflags opencv4) \
    tests/test_animal_snapshot_writer.cpp src/animal_snapshot_writer.cpp src/yolo_post.cpp \
    src/log/logger.cpp $(pkg-config --libs opencv4) -lpthread \
    -o "$OUT/test_animal_snapshot_writer" && "$OUT/test_animal_snapshot_writer" || fail=1

echo ""
echo "== Occupancy config =="
g++ -std=c++17 -Isrc $(pkg-config --cflags opencv4) \
    tests/test_occupancy_config.cpp src/occupancy_config.cpp \
    $(pkg-config --libs opencv4) -o "$OUT/test_occupancy_config" \
    && "$OUT/test_occupancy_config" || fail=1

echo ""
echo "== Occupancy state =="
g++ -std=c++17 -Isrc \
    tests/test_occupancy_state.cpp src/occupancy_state.cpp src/log/logger.cpp \
    -lpthread -o "$OUT/test_occupancy_state" \
    && "$OUT/test_occupancy_state" || fail=1

echo ""
echo "== Occupancy counter =="
g++ -std=c++17 -Isrc $(pkg-config --cflags opencv4) \
    tests/test_occupancy_counter.cpp src/occupancy_counter.cpp \
    $(pkg-config --libs opencv4) -o "$OUT/test_occupancy_counter" \
    && "$OUT/test_occupancy_counter" || fail=1

echo ""
echo "== Occupancy overlay and disabled HUD =="
g++ -std=c++17 -Isrc $(pkg-config --cflags opencv4) \
    tests/test_overlay.cpp src/overlay.cpp src/occupancy_config.cpp \
    $(pkg-config --libs opencv4) -o "$OUT/test_overlay" \
    && "$OUT/test_overlay" || fail=1

echo ""
echo "== Occupancy system metrics gating =="
g++ -std=c++17 -Isrc \
    tests/test_system_metrics.cpp src/system_metrics.cpp src/log/logger.cpp \
    -lpthread -o "$OUT/test_system_metrics" \
    && "$OUT/test_system_metrics" || fail=1

echo ""
echo "== Door inference scheduler =="
g++ -std=c++17 -Isrc \
    tests/test_door_inference.cpp src/door_inference.cpp \
    -o "$OUT/test_door_inference" && "$OUT/test_door_inference" || fail=1

echo ""
echo "== Video frame/deadline wait =="
g++ -std=c++17 -Isrc $(pkg-config --cflags opencv4) \
    tests/test_video_wait.cpp src/video_io.cpp src/event_video_recorder.cpp \
    src/occupancy_counter.cpp src/log/logger.cpp \
    $(pkg-config --libs opencv4) -lpthread \
    -o "$OUT/test_video_wait" && "$OUT/test_video_wait" || fail=1

echo ""
echo "== Person-triggered video recorder =="
g++ -std=c++17 -Isrc $(pkg-config --cflags opencv4) \
    tests/test_event_video_recorder.cpp src/event_video_recorder.cpp src/log/logger.cpp \
    $(pkg-config --libs opencv4) -lpthread \
    -o "$OUT/test_event_video_recorder" && "$OUT/test_event_video_recorder" || fail=1

echo ""
echo "== MatchEngine =="
g++ -std=c++17 -Isrc \
    tests/test_match_engine.cpp src/match_engine.cpp src/log/logger.cpp \
    -lpthread \
    -o "$OUT/test_match_engine" && "$OUT/test_match_engine" || fail=1

echo ""
echo "== Linear assignment =="
g++ -std=c++17 -Isrc \
    tests/test_linear_assignment.cpp src/linear_assignment.cpp \
    -o "$OUT/test_linear_assignment" && "$OUT/test_linear_assignment" || fail=1

echo ""
echo "== Kalman box tracker =="
g++ -std=c++17 -Isrc $(pkg-config --cflags opencv4) \
    tests/test_kalman_box_tracker.cpp src/kalman_box_tracker.cpp \
    $(pkg-config --libs opencv4) -o "$OUT/test_kalman_box_tracker" && "$OUT/test_kalman_box_tracker" || fail=1

echo ""
echo "== Greedy fallback threshold and observation reliability =="
g++ -std=c++17 -Isrc $(pkg-config --cflags opencv4) \
    tests/test_greedy_adapter.cpp src/tracker_greedy.cpp src/tracker.cpp src/log/logger.cpp \
    -lpthread -o "$OUT/test_greedy_adapter" && "$OUT/test_greedy_adapter" || fail=1

echo ""
echo "== ByteTrack core =="
g++ -std=c++17 -Isrc $(pkg-config --cflags opencv4) \
    tests/test_byte_tracker.cpp src/byte_tracker.cpp src/kalman_box_tracker.cpp src/linear_assignment.cpp \
    $(pkg-config --libs opencv4) -o "$OUT/test_byte_tracker" && "$OUT/test_byte_tracker" || fail=1

echo ""
echo "== Face-to-track association =="
g++ -std=c++17 -Isrc \
    tests/test_face_track_assoc.cpp src/face_track_assoc.cpp src/linear_assignment.cpp \
    -o "$OUT/test_face_track_assoc" && "$OUT/test_face_track_assoc" || fail=1

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

echo ""
echo "== Socket.IO floor command =="
g++ -std=c++17 -Isrc \
    tests/test_socketio_client.cpp src/socketio_client.cpp src/log/logger.cpp \
    -lpthread -o "$OUT/test_socketio_client" && "$OUT/test_socketio_client" || fail=1

echo ""
echo "== Floor-call coordinator =="
g++ -std=c++17 -Isrc \
    tests/test_floor_call_coordinator.cpp src/floor_call_coordinator.cpp \
    -lpthread -o "$OUT/test_floor_call_coordinator" && "$OUT/test_floor_call_coordinator" || fail=1

echo ""
echo "== Elevator registers =="
g++ -std=c++17 -Isrc \
    tests/test_elevator_registers.cpp src/elevator_registers.cpp src/edge_json.cpp \
    -o "$OUT/test_elevator_registers" && "$OUT/test_elevator_registers" || fail=1

echo ""
echo "== Elevator state =="
g++ -std=c++17 -Isrc \
    tests/test_elevator_state.cpp src/elevator_state.cpp src/elevator_registers.cpp \
    src/edge_json.cpp src/log/logger.cpp -lpthread \
    -o "$OUT/test_elevator_state" && "$OUT/test_elevator_state" || fail=1

echo ""
echo "== Call tracker =="
g++ -std=c++17 -Isrc \
    tests/test_call_tracker.cpp src/call_tracker.cpp src/elevator_state.cpp \
    src/elevator_registers.cpp src/edge_json.cpp src/log/logger.cpp -lpthread \
    -o "$OUT/test_call_tracker" && "$OUT/test_call_tracker" || fail=1

echo ""
echo "== Trip assembler =="
g++ -std=c++17 -Isrc $(pkg-config --cflags opencv4) \
    tests/test_trip_assembler.cpp src/trip_assembler.cpp src/elevator_state.cpp \
    src/elevator_registers.cpp src/edge_json.cpp src/call_tracker.cpp \
    src/floor_call_coordinator.cpp src/log/logger.cpp \
    $(pkg-config --libs opencv4) -lpthread \
    -o "$OUT/test_trip_assembler" && "$OUT/test_trip_assembler" || fail=1

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
        src/resident_db.cpp src/match_engine.cpp src/video_io.cpp src/event_video_recorder.cpp \
        src/log/logger.cpp \
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

    echo ""
    echo "== Visitor gallery =="
    g++ -std=c++17 -Isrc $SQLITE_INC $(pkg-config --cflags opencv4) \
        tests/test_visitor_gallery.cpp src/visitor_gallery.cpp src/traffic_db.cpp \
        src/resident_db.cpp src/match_engine.cpp src/trip_assembler.cpp \
        src/elevator_state.cpp src/elevator_registers.cpp src/edge_json.cpp \
        src/call_tracker.cpp src/floor_call_coordinator.cpp src/log/logger.cpp \
        $SQLITE_LIB $(pkg-config --libs opencv4) -lpthread \
        -o "$OUT/test_visitor_gallery" && "$OUT/test_visitor_gallery" || fail=1

    echo ""
    echo "== TrafficDB =="
    g++ -std=c++17 -Isrc $SQLITE_INC $(pkg-config --cflags opencv4) \
        tests/test_traffic_db.cpp src/traffic_db.cpp src/resident_db.cpp \
        src/resident_enroll.cpp src/match_engine.cpp src/trip_assembler.cpp \
        src/elevator_state.cpp src/elevator_registers.cpp src/edge_json.cpp \
        src/call_tracker.cpp src/floor_call_coordinator.cpp src/log/logger.cpp \
        $SQLITE_LIB -lopencv_core -lpthread \
        -o "$OUT/test_traffic_db" \
        && "$OUT/test_traffic_db" "$ROOT/db/traffic_schema.sql" "$ROOT/db/schema.sql" || fail=1

    echo ""
    echo "== TrafficLogWriter =="
    g++ -std=c++17 -Isrc $SQLITE_INC $(pkg-config --cflags opencv4) \
        tests/test_traffic_log_writer.cpp src/traffic_log_writer.cpp src/visitor_gallery.cpp \
        src/traffic_db.cpp src/resident_db.cpp src/match_engine.cpp src/trip_assembler.cpp \
        src/elevator_state.cpp src/elevator_registers.cpp src/edge_json.cpp \
        src/call_tracker.cpp src/floor_call_coordinator.cpp src/log/logger.cpp \
        $SQLITE_LIB $(pkg-config --libs opencv4) -lpthread \
        -o "$OUT/test_traffic_log_writer" \
        && "$OUT/test_traffic_log_writer" "$ROOT/db/traffic_schema.sql" || fail=1

    echo ""
    echo "== traffic_admin CLI =="
    g++ -std=c++17 -Isrc $SQLITE_INC $(pkg-config --cflags opencv4) \
        src/traffic_admin.cpp src/traffic_db.cpp src/trip_assembler.cpp \
        src/elevator_state.cpp src/elevator_registers.cpp src/call_tracker.cpp \
        src/floor_call_coordinator.cpp src/edge_json.cpp src/resident_db.cpp \
        src/match_engine.cpp src/log/logger.cpp \
        $SQLITE_LIB -lpthread \
        -o "$OUT/traffic_admin" \
        && bash tests/test_traffic_admin.sh "$OUT/traffic_admin" || fail=1
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
