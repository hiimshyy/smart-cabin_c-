#!/usr/bin/env bash
# Shell-only regression: face_cabin must not echo any PostgreSQL RTSP URL part
# and source env.sh must choose a writable user log directory. No camera/NPU.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
source ./env.sh >/dev/null

[ "$FACE_CABIN_LOG_DIR" = "$FACE_ROOT/logs" ]
[ -d "$FACE_CABIN_LOG_DIR" ] && [ -w "$FACE_CABIN_LOG_DIR" ]

MOCK=/tmp/face-cabin-env-test
rm -rf "$MOCK"; mkdir -p "$MOCK"
trap 'rm -rf "$MOCK"' EXIT
printf '#!/bin/sh\nexit 0\n' > "$MOCK/face_recog_app"
chmod +x "$MOCK/face_recog_app"
FACE_ROOT="$MOCK"
FACE_CABIN_RTSP_URL=""
FACE_CAMERA_SELECT=""
node(){ printf '%s\n' 'rtsp://fake-user:fake-secret@camera.local/stream?token=fake-token'; }

OUT="$(face_cabin)"
if printf '%s' "$OUT" | grep -Eq 'rtsp://|fake-user|fake-secret|camera\.local|fake-token|@'; then
    echo "FAIL: face_cabin leaked URL/credentials"
    exit 1
fi
printf '[test_env_face_cabin] PASS\n'
