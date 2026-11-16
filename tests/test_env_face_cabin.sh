#!/usr/bin/env bash
# Safe shell regression for face_cabin argv wiring and URL redaction. No camera/NPU.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
unset FACE_IOT_WS FACE_EVENT_WS FACE_RECORD_DIR FACE_CABIN_ID FACE_CAMERA_SELECT \
    FACE_CABIN_SOCKETIO FACE_ANIMAL_THR
source ./env.sh >/dev/null

[ "$FACE_CABIN_LOG_DIR" = "$FACE_ROOT/logs" ]
[ -d "$FACE_CABIN_LOG_DIR" ] && [ -w "$FACE_CABIN_LOG_DIR" ]
[ "$FACE_CABIN_ID" = "2" ]
[ "$FACE_CAMERA_SELECT" = "#6" ]
[ "$FACE_CABIN_SOCKETIO" = "0" ]
[ "$FACE_ANIMAL_THR" = "0.5" ]
[ -z "$FACE_IOT_WS" ]
[ -z "$FACE_RECORD_DIR" ]

MOCK=/tmp/face-cabin-env-test
rm -rf "$MOCK"
mkdir -p "$MOCK/config dir" "$MOCK/state dir"
trap 'rm -rf "$MOCK"' EXIT
cat > "$MOCK/face_recog_app" <<'SCRIPT'
#!/bin/sh
printf '%s\n' "$@" > "$FACE_TEST_ARGV"
SCRIPT
chmod +x "$MOCK/face_recog_app"

export FACE_TEST_ARGV="$MOCK/argv"
FACE_ROOT="$MOCK"
FACE_CABIN_ID=7
FACE_OCCUPANCY_CONFIG_DIR="$MOCK/config dir"
FACE_OCCUPANCY_STATE_DIR="$MOCK/state dir"
touch "$FACE_OCCUPANCY_CONFIG_DIR/occupancy_cam7.json"
FACE_CABIN_RTSP_URL=""
FACE_CAMERA_SELECT="#6"
# Simulate a stale service endpoint: the cabin launcher must still keep
# Socket.IO disabled unless FACE_CABIN_SOCKETIO=1 is explicitly selected.
FACE_IOT_WS="ws://127.0.0.1:3000"
node() {
    [ "$2" = "#6" ] || return 1
    printf '%s\n' 'rtsp://fake-user:fake-secret@camera.local/stream?token=fake-token'
}

OUT="$(face_cabin)"
if printf '%s' "$OUT" | grep -Eq 'rtsp://|fake-user|fake-secret|camera\.local|fake-token|@'; then
    echo "FAIL: face_cabin leaked URL/credentials"
    exit 1
fi
mapfile -t ARGS < "$FACE_TEST_ARGV"
found_config=0
found_state=0
found_iot=0
found_record=0
found_animal_thr=0
for ((i = 0; i < ${#ARGS[@]}; ++i)); do
    if [ "${ARGS[$i]}" = "--occupancy-config" ] &&
       [ "${ARGS[$((i + 1))]}" = "$MOCK/config dir/occupancy_cam7.json" ]; then
        found_config=1
    fi
    if [ "${ARGS[$i]}" = "--occupancy-state" ] &&
       [ "${ARGS[$((i + 1))]}" = "$MOCK/state dir/occupancy_state_cam7.json" ]; then
        found_state=1
    fi
    if [ "${ARGS[$i]}" = "--iot-ws" ] &&
       [ "${ARGS[$((i + 1))]}" = "ws://127.0.0.1:3000" ]; then
        found_iot=1
    fi
    if [ "${ARGS[$i]}" = "--record-dir" ]; then
        found_record=1
    fi
    if [ "${ARGS[$i]}" = "--animal-thr" ] &&
       [ "${ARGS[$((i + 1))]}" = "0.5" ]; then
        found_animal_thr=1
    fi
done
[ "$found_config" -eq 1 ] || { echo "FAIL: occupancy config argv missing"; exit 1; }
[ "$found_state" -eq 1 ] || { echo "FAIL: occupancy state argv missing"; exit 1; }
[ "$found_iot" -eq 0 ] || { echo "FAIL: Socket.IO must be disabled by default"; exit 1; }
[ "$found_record" -eq 0 ] || { echo "FAIL: recording should match disabled service default"; exit 1; }
[ "$found_animal_thr" -eq 1 ] || { echo "FAIL: --animal-thr argv missing"; exit 1; }

export FACE_CABIN_SOCKETIO=1
export FACE_TEST_ARGV="$MOCK/service-argv"
SERVICE_OUT="$(face_cabin)"
if printf '%s' "$SERVICE_OUT" | grep -Eq 'rtsp://|fake-user|fake-secret|camera\.local|fake-token|@'; then
    echo "FAIL: service profile leaked URL/credentials"
    exit 1
fi
mapfile -t SERVICE_ARGS < "$FACE_TEST_ARGV"
service_iot=0
for ((i = 0; i < ${#SERVICE_ARGS[@]}; ++i)); do
    if [ "${SERVICE_ARGS[$i]}" = "--iot-ws" ] &&
       [ "${SERVICE_ARGS[$((i + 1))]}" = "ws://127.0.0.1:3000" ]; then
        service_iot=1
    fi
done
[ "$service_iot" -eq 1 ] || { echo "FAIL: production Socket.IO argv missing"; exit 1; }
grep -q '^Environment=FACE_CABIN_SOCKETIO=1$' "$ROOT/face-recog-app.service"
if grep -q '^Environment=FACE_IOT_WS=$' "$ROOT/face-recog-app.service"; then
    echo "FAIL: service unit must not erase EnvironmentFile Socket.IO endpoint"
    exit 1
fi
printf '[test_env_face_cabin] PASS\n'
