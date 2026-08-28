# Environment + shell functions cho SCRFD + MobileFaceNet demo (A733)
#
# CÁCH DÙNG:
#   source env.sh                          (chỉ session hiện tại)
#   echo "source $(pwd)/env.sh" >> ~/.bashrc   (áp vĩnh viễn)
#
# Sau khi source, có các lệnh:
#   face_run [DB]              — chạy realtime match (default: db/faces_all.fdb)
#   face_run_rtsp URL [DB]     — chạy realtime match với RTSP camera
#   face_detect                — detect-only (không recognition)
#   face_capture NAME [COUNT]  — chụp N frames (default 5)
#   face_add NAME IMG [IMG..]  — thêm 1 người vào DB
#   face_enroll [DIR] [OUT]    — rebuild DB từ folder (default: faces/ → db/faces_all.fdb)
#   face_bench [N]             — bench N frames (default 100)
#   face_bench_rtsp URL [N]    — bench RTSP N frames
#   face_ls                    — liệt kê DB và enroll folders
#   face_help                  — in help này

# ---- Path setup ----
export FACE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export FACE_DET_MODEL="$FACE_ROOT/model/face_det/scrfd_2.5g_bnkps640_uint8_a733.nb"
export FACE_RECOG_MODEL="$FACE_ROOT/model/face_recog/w600k_mbf_uint8_a733.nb"
export FACE_DB_DIR="$FACE_ROOT/db"
export FACE_DB_DEFAULT="$FACE_DB_DIR/faces_all.fdb"
export FACE_FACES_DIR="$FACE_ROOT/faces"

# X11 display
[ -z "$DISPLAY" ]    && export DISPLAY=:0.0
[ -z "$XAUTHORITY" ] && export XAUTHORITY="$HOME/.Xauthority"

# Silence benign OpenCV/GStreamer warnings on RTSP
[ -z "$OPENCV_LOG_LEVEL" ] && export OPENCV_LOG_LEVEL=ERROR

# ---- Shell functions ----
face_run() {
    local db="${1:-$FACE_DB_DEFAULT}"
    local thr="${2:-0.35}"
    (cd "$FACE_ROOT" && ./face_recog_app "$FACE_DET_MODEL" \
        --recog-model "$FACE_RECOG_MODEL" \
        --recog-dim 512 --recog-bgr \
        --face-db "$db" --match-thr "$thr")
}

face_run_rtsp() {
    if [ -z "$1" ]; then
        echo "Usage: face_run_rtsp URL [DB] [THR] [LATENCY_MS]"
        echo "Example: face_run_rtsp rtsp://admin:pass@192.168.1.100:554/stream1"
        return 1
    fi
    local url="$1"
    local db="${2:-$FACE_DB_DEFAULT}"
    local thr="${3:-0.35}"
    local lat="${4:-100}"
    (cd "$FACE_ROOT" && ./face_recog_app "$FACE_DET_MODEL" \
        --recog-model "$FACE_RECOG_MODEL" \
        --recog-dim 512 --recog-bgr \
        --face-db "$db" --match-thr "$thr" \
        --source "$url" --gst-latency "$lat")
}

face_detect() {
    (cd "$FACE_ROOT" && ./face_recog_app "$FACE_DET_MODEL")
}

face_capture() {
    if [ -z "$1" ]; then
        echo "Usage: face_capture NAME [COUNT] [MIN_FACE_PX]"
        return 1
    fi
    local name="$1"
    local count="${2:-5}"
    local minface="${3:-100}"
    (cd "$FACE_ROOT" && ./capture_person \
        --name "$name" --count "$count" \
        --min-face-px "$minface" \
        --det-model "$FACE_DET_MODEL")
}

face_add() {
    if [ -z "$1" ] || [ -z "$2" ]; then
        echo "Usage: face_add NAME IMG [IMG ...] [--replace|--merge]"
        echo "  DB target: $FACE_DB_DEFAULT"
        return 1
    fi
    local name="$1"; shift
    local images=()
    local flags=()
    for arg in "$@"; do
        case "$arg" in
            --replace|--merge) flags+=("$arg") ;;
            *)                 images+=(--image "$arg") ;;
        esac
    done
    (cd "$FACE_ROOT" && ./add_person \
        --name "$name" \
        "${images[@]}" \
        --db "$FACE_DB_DEFAULT" \
        --det-model "$FACE_DET_MODEL" \
        --recog-model "$FACE_RECOG_MODEL" \
        --recog-dim 512 --recog-bgr \
        "${flags[@]}")
}

face_enroll() {
    local dir="${1:-$FACE_FACES_DIR}"
    local out="${2:-$FACE_DB_DEFAULT}"
    (cd "$FACE_ROOT" && ./enroll_faces \
        --dir "$dir" --out "$out" \
        --det-model "$FACE_DET_MODEL" \
        --recog-model "$FACE_RECOG_MODEL" \
        --recog-dim 512 --recog-bgr)
}

face_bench() {
    local n="${1:-100}"
    (cd "$FACE_ROOT" && ./face_recog_app "$FACE_DET_MODEL" \
        --recog-model "$FACE_RECOG_MODEL" \
        --recog-dim 512 --recog-bgr \
        --face-db "$FACE_DB_DEFAULT" --match-thr 0.35 \
        --frames "$n")
}

face_bench_rtsp() {
    if [ -z "$1" ]; then
        echo "Usage: face_bench_rtsp URL [N_FRAMES] [DB]"
        return 1
    fi
    local url="$1"
    local n="${2:-100}"
    local db="${3:-$FACE_DB_DEFAULT}"
    (cd "$FACE_ROOT" && ./face_recog_app "$FACE_DET_MODEL" \
        --recog-model "$FACE_RECOG_MODEL" \
        --recog-dim 512 --recog-bgr \
        --face-db "$db" --match-thr 0.35 \
        --source "$url" \
        --frames "$n")
}

face_ls() {
    echo "DB files in $FACE_DB_DIR:"
    ls -la "$FACE_DB_DIR"/*.fdb 2>/dev/null
    echo ""
    echo "Enroll folders in $FACE_FACES_DIR:"
    ls -1 "$FACE_FACES_DIR" 2>/dev/null | while read n; do
        [ -d "$FACE_FACES_DIR/$n" ] && printf "  %-20s  %d ảnh\n" \
            "$n" "$(ls "$FACE_FACES_DIR/$n"/*.jpg 2>/dev/null | wc -l)"
    done
}

face_help() {
    cat <<'EOF'
==============================================================
 SCRFD + MobileFaceNet — Face Recognition Shortcuts (A733)
==============================================================
 face_run [DB] [THR]           Live match (default DB: db/faces_all.fdb)
 face_run_rtsp URL [DB] [THR]  Live match từ RTSP camera (GStreamer)
 face_detect                   Detect-only (bỏ recognition)
 face_capture NAME [N] [MIN]   Chụp N frames của 1 người
 face_add NAME IMG [IMG...]    Thêm người từ ảnh (--replace/--merge)
 face_enroll [DIR] [OUT]       Rebuild toàn bộ DB
 face_bench [N]                Chạy N frames + in benchmark
 face_bench_rtsp URL [N]       Bench trên RTSP stream
 face_ls                       Liệt kê DB và enroll folders
 face_help                     In help này

 Model default:
   detect: model/face_det/scrfd_2.5g_bnkps640_uint8_a733.nb (SCRFD)
   recog : model/face_recog/w600k_mbf_uint8_a733.nb (MobileFaceNet 512-D)

 Ví dụ:
   face_capture alice 8
   face_add bob /photos/bob.jpg /photos/bob2.jpg
   face_add alice new.jpg --merge
   face_run
   face_enroll                 # rebuild sau khi có folder mới
==============================================================
EOF
}

echo "[face] loaded shortcuts — gõ 'face_help' để xem"
