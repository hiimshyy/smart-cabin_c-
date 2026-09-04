# Environment + shell functions cho SCRFD + MobileFaceNet demo (A733)
#
# CÁCH DÙNG:
#   source env.sh                          (chỉ session hiện tại)
#   echo "source $(pwd)/env.sh" >> ~/.bashrc   (áp vĩnh viễn)
#
# Sau khi source, có các lệnh:
#   --- Chế độ .fdb (test/dev nhận diện đơn giản) ---
#   face_run [DB]              — realtime match full pipeline (default: db/faces_all.fdb)
#   face_run_lite [DB]         — realtime match SCRFD-only (không tracker)
#   face_run_rtsp URL [DB]     — realtime match với RTSP camera
#   face_detect                — detect-only (không recognition)
#   face_capture NAME [COUNT]  — chụp N frames (default 5)
#   face_add NAME IMG [IMG..]  — thêm 1 người vào DB .fdb
#   face_enroll [DIR] [OUT]    — rebuild DB .fdb từ folder
#   face_bench [N]             — bench N frames (default 100)
#
#   --- Chế độ resident-db (SQLite, VẬN HÀNH cabin: tầng + audit log) ---
#   face_migrate [FDB] [DB]    — import .fdb → SQLite (default faces_all.fdb → residents.db)
#   face_cabin [DB] [THR]      — realtime vận hành full pipeline (YOLO+tracker)
#   face_cabin_lite [DB] [THR] — realtime vận hành SCRFD-only
#   face_cabin_rtsp URL [DB]   — realtime vận hành qua RTSP
#   face_residents [DB]        — liệt kê residents trong SQLite
#   face_events [DB] [N]       — xem N match_events gần nhất (default 20)
#   face_set_floor NAME FLOOR [GREETING] [DB] — cập nhật tầng/tên chào
#
#   face_ls                    — liệt kê DB (.fdb + .db) và enroll folders
#   face_help                  — in help này

# ---- Path setup ----
export FACE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export FACE_DET_MODEL="$FACE_ROOT/model/face_det/scrfd_2.5g_bnkps640_uint8_a733.nb"
export FACE_RECOG_MODEL="$FACE_ROOT/model/face_recog/w600k_mbf_uint8_a733.nb"
export FACE_PERSON_MODEL="$FACE_ROOT/model/person_det/yolov5s_rt_uint8_a733.nb"
export FACE_DB_DIR="$FACE_ROOT/db"
export FACE_DB_DEFAULT="$FACE_DB_DIR/faces_all.fdb"
export FACE_RESIDENT_DB="$FACE_DB_DIR/residents.db"
export FACE_SCHEMA="$FACE_DB_DIR/schema.sql"
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
        --face-db "$db" --match-thr "$thr" \
        --person-model "$FACE_PERSON_MODEL")
}

# SCRFD-only mode (không tracker) — nhanh hơn, dùng khi scene không có occlusion
face_run_lite() {
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
        --person-model "$FACE_PERSON_MODEL" \
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
        --person-model "$FACE_PERSON_MODEL" \
        --frames "$n")
}

face_bench_lite() {
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
        --person-model "$FACE_PERSON_MODEL" \
        --source "$url" \
        --frames "$n")
}

face_ls() {
    echo "DB files in $FACE_DB_DIR:"
    ls -la "$FACE_DB_DIR"/*.fdb 2>/dev/null
    ls -la "$FACE_DB_DIR"/*.db  2>/dev/null
    echo ""
    echo "Enroll folders in $FACE_FACES_DIR:"
    ls -1 "$FACE_FACES_DIR" 2>/dev/null | while read n; do
        [ -d "$FACE_FACES_DIR/$n" ] && printf "  %-20s  %d ảnh\n" \
            "$n" "$(ls "$FACE_FACES_DIR/$n"/*.jpg 2>/dev/null | wc -l)"
    done
}

# ============================================================
#  Chế độ resident-db (SQLite) — VẬN HÀNH cabin thật
#  (tầng, tên chào, audit log match_events)
# ============================================================

# Import .fdb cũ → SQLite residents.db (mọi người home_floor=0, sửa sau bằng face_set_floor)
face_migrate() {
    local fdb="${1:-$FACE_DB_DEFAULT}"
    local db="${2:-$FACE_RESIDENT_DB}"
    shift 2 2>/dev/null
    (cd "$FACE_ROOT" && ./migrate_fdb --fdb "$fdb" --db "$db" \
        --schema "$FACE_SCHEMA" "$@")
}

# Vận hành đầy đủ: YOLO person + tracker + SCRFD + recog + SQLite audit.
face_cabin() {
    local db="${1:-$FACE_RESIDENT_DB}"
    local thr="${2:-0.35}"
    (cd "$FACE_ROOT" && ./face_recog_app "$FACE_DET_MODEL" \
        --recog-model "$FACE_RECOG_MODEL" \
        --recog-dim 512 --recog-bgr \
        --resident-db "$db" --cabin-id "${FACE_CABIN_ID:-1}" \
        --match-thr "$thr" \
        --person-model "$FACE_PERSON_MODEL")
}

# Vận hành SCRFD-only (không tracker) — nhanh hơn, scene không occlusion.
face_cabin_lite() {
    local db="${1:-$FACE_RESIDENT_DB}"
    local thr="${2:-0.35}"
    (cd "$FACE_ROOT" && ./face_recog_app "$FACE_DET_MODEL" \
        --recog-model "$FACE_RECOG_MODEL" \
        --recog-dim 512 --recog-bgr \
        --resident-db "$db" --cabin-id "${FACE_CABIN_ID:-1}" \
        --match-thr "$thr")
}

# Vận hành qua RTSP IP camera.
face_cabin_rtsp() {
    if [ -z "$1" ]; then
        echo "Usage: face_cabin_rtsp URL [DB] [THR] [LATENCY_MS]"
        echo "Example: face_cabin_rtsp rtsp://admin:pass@192.168.1.100:554/stream1"
        return 1
    fi
    local url="$1"
    local db="${2:-$FACE_RESIDENT_DB}"
    local thr="${3:-0.35}"
    local lat="${4:-100}"
    (cd "$FACE_ROOT" && ./face_recog_app "$FACE_DET_MODEL" \
        --recog-model "$FACE_RECOG_MODEL" \
        --recog-dim 512 --recog-bgr \
        --resident-db "$db" --cabin-id "${FACE_CABIN_ID:-1}" \
        --match-thr "$thr" \
        --person-model "$FACE_PERSON_MODEL" \
        --source "$url" --gst-latency "$lat")
}

# Liệt kê residents trong SQLite (id, tên, tầng, tên chào, số lần match).
face_residents() {
    local db="${1:-$FACE_RESIDENT_DB}"
    if [ ! -f "$db" ]; then echo "Không tìm thấy DB: $db"; return 1; fi
    sqlite3 -header -column "$db" \
        "SELECT id,name,home_floor,greeting_name,match_count,last_seen_at
         FROM residents ORDER BY id;"
}

# Xem N match_events gần nhất (audit log).
face_events() {
    local db="${1:-$FACE_RESIDENT_DB}"
    local n="${2:-20}"
    if [ ! -f "$db" ]; then echo "Không tìm thấy DB: $db"; return 1; fi
    sqlite3 -header -column "$db" \
        "SELECT id,ts,cabin_id,resident_id,ROUND(similarity,3) AS sim,
                action,floor_selected AS floor,latency_ms AS lat
         FROM match_events ORDER BY id DESC LIMIT $n;"
}

# Cập nhật tầng (+ tên chào tùy chọn) cho 1 resident theo tên.
face_set_floor() {
    if [ -z "$1" ] || [ -z "$2" ]; then
        echo "Usage: face_set_floor NAME FLOOR [GREETING] [DB]"
        echo "Example: face_set_floor 'Cao Tien Sy' 7 'anh Sy'"
        return 1
    fi
    local name="$1"
    local floor="$2"
    local greeting="$3"
    local db="${4:-$FACE_RESIDENT_DB}"
    if [ ! -f "$db" ]; then echo "Không tìm thấy DB: $db"; return 1; fi
    # Escape single quotes for SQL string literals.
    local name_sql="${name//\'/\'\'}"
    local changed
    if [ -n "$greeting" ]; then
        local greet_sql="${greeting//\'/\'\'}"
        # UPDATE + changes() must run in the SAME sqlite3 session, else
        # changes() reports 0 (a fresh connection has no prior statement).
        changed=$(sqlite3 "$db" \
            "UPDATE residents SET home_floor=$floor, greeting_name='$greet_sql'
             WHERE name='$name_sql'; SELECT changes();")
    else
        changed=$(sqlite3 "$db" \
            "UPDATE residents SET home_floor=$floor
             WHERE name='$name_sql'; SELECT changes();")
    fi
    echo "Đã cập nhật $changed resident (name='$name' → floor=$floor${greeting:+, greeting='$greeting'})"
    face_residents "$db"
}

face_help() {
    cat <<'EOF'
==============================================================
 SCRFD + YOLO + MobileFaceNet — Face Recognition (A733)
==============================================================

 ┌─ VẬN HÀNH CABIN (SQLite: tầng + tên chào + audit log) ─────┐
   face_migrate [FDB] [DB]        Import .fdb → residents.db (1 lần)
   face_set_floor NAME FLOOR [GREETING]
                                  Đặt tầng/tên chào cho 1 người
   face_cabin [DB] [THR]          Live vận hành (YOLO+tracker+SQLite)
   face_cabin_lite [DB] [THR]     Live vận hành SCRFD-only
   face_cabin_rtsp URL [DB]       Live vận hành qua RTSP
   face_residents [DB]            Liệt kê residents
   face_events [DB] [N]           Xem N match_events gần nhất
 └────────────────────────────────────────────────────────────┘
   Quy trình: face_migrate → face_set_floor cho từng người → face_cabin.
   home_floor=0 nghĩa "chưa đăng ký tầng": vẫn chào tên, overlay hiện F?.
   Đặt FACE_CABIN_ID=N để đổi cabin id (default 1).

 ── TEST/DEV (.fdb — nhận diện đơn giản, KHÔNG tầng/audit) ──
   face_run [DB] [THR]            Live match (YOLO+SCRFD+tracker)
   face_run_lite [DB] [THR]       Live match SCRFD-only (nhanh hơn)
   face_run_rtsp URL [DB] [THR]   Live match RTSP với tracker
   face_bench [N]                 Bench full pipeline
   face_bench_lite [N]            Bench SCRFD-only
   face_bench_rtsp URL [N]        Bench RTSP

 ── Quản lý dữ liệu (.fdb) ──
   face_detect                    Detect-only (bỏ recognition)
   face_capture NAME [N] [MIN]    Chụp N frames của 1 người
   face_add NAME IMG [IMG...]     Thêm người vào .fdb (--replace/--merge)
   face_enroll [DIR] [OUT]        Rebuild toàn bộ .fdb
   face_ls                        Liệt kê DB (.fdb+.db) và enroll folders

 ── Model default ──
   detect: model/face_det/scrfd_2.5g_bnkps640_uint8_a733.nb (SCRFD)
   person: model/person_det/yolov5s_rt_uint8_a733.nb (YOLOv5s COCO)
   recog : model/face_recog/w600k_mbf_uint8_a733.nb (MobileFaceNet 512-D)

 ── Ghi chú ──
   Cabin thật PHẢI dùng face_cabin (SQLite). Nhánh .fdb (face_run) chỉ
   để test nhanh: không tầng, không audit log.
   Tracker duy trì ID kể cả khi người quay lưng, cache recog giảm tải NPU
   (~15 FPS). SCRFD-only (_lite) nhanh hơn nhưng mất persistence.
==============================================================
EOF
}

echo "[face] loaded shortcuts — gõ 'face_help' để xem"
