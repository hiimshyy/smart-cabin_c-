# Environment + shell functions cho SCRFD + MobileFaceNet demo (A733)
#
# CÁCH DÙNG:
#   source env.sh                          (chỉ session hiện tại)
#   echo "source $(pwd)/env.sh" >> ~/.bashrc   (áp vĩnh viễn)
#
# Sau khi source, có các lệnh:
#   --- VẬN HÀNH cabin thật (SQLite: tầng + tên chào + audit log) ---
#   face_cabin [URL] [DB] [THR]  — realtime vận hành qua RTSP (YOLO+tracker+SQLite)
#                                  URL: tham số > $FACE_CABIN_RTSP_URL (web local config)
#   face_residents [DB]          — liệt kê residents trong SQLite
#   face_events [DB] [N]         — xem N match_events gần nhất (default 20)
#   face_set_resident NAME [--floor N] [--greeting STR] [--role ...] [...] — cập nhật 1 resident
#
#   --- Quản lý dữ liệu (enroll → SQLite residents.db) ---
#   face_capture NAME [COUNT]    — chụp N frames (default 5)
#   face_add NAME IMG [IMG..]    — thêm 1 người vào SQLite (--merge/--replace)
#   face_enroll [DIR] [DB]       — enroll folder → SQLite (mỗi ảnh 1 embedding)
#   face_detect                  — detect-only (không recognition)
#
#   --- DEV / test (đọc residents.db) ---
#   face_usb [DB] [THR]          — realtime trên USB cam (dev; YOLO+tracker)
#   face_run_lite [DB] [THR]     — realtime SCRFD-only (không tracker)
#   face_run_rtsp URL [DB] [THR] — realtime RTSP (dev)
#   face_bench [N]               — bench full pipeline (USB)
#   face_bench_lite [N]          — bench SCRFD-only (USB)
#   face_bench_rtsp URL [N]      — bench RTSP
#
#   --- Legacy / 1 lần ---
#   face_migrate [FDB] [DB]      — import .fdb cũ → SQLite (đã migrate xong; giữ để khôi phục)
#
#   face_ls                      — liệt kê DB (.fdb + .db) và enroll folders
#   face_help                    — in help này

# ---- Path setup ----
export FACE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export FACE_DET_MODEL="$FACE_ROOT/model/face_det/scrfd_2.5g_bnkps640_uint8_a733.nb"
export FACE_RECOG_MODEL="$FACE_ROOT/model/face_recog/w600k_mbf_uint8_a733.nb"
export FACE_PERSON_MODEL="$FACE_ROOT/model/person_det/yolov5s_rt_uint8_a733.nb"
export FACE_DB_DIR="$FACE_ROOT/db"
export FACE_RESIDENT_DB="$FACE_DB_DIR/residents.db"
export FACE_SCHEMA="$FACE_DB_DIR/schema.sql"
export FACE_FACES_DIR="$FACE_ROOT/faces"

# RTSP URL của cabin thật. Web local ghi biến này (hoặc export trong ~/.bashrc).
# face_cabin dùng URL truyền trực tiếp trước, nếu không có thì lấy biến này.
# (Config DB-driven trong bảng `cabins` là kế hoạch tương lai — chưa code.)
export FACE_CABIN_RTSP_URL="${FACE_CABIN_RTSP_URL:-}"

# X11 display
[ -z "$DISPLAY" ]    && export DISPLAY=:0.0
[ -z "$XAUTHORITY" ] && export XAUTHORITY="$HOME/.Xauthority"

# Silence benign OpenCV/GStreamer warnings on RTSP
[ -z "$OPENCV_LOG_LEVEL" ] && export OPENCV_LOG_LEVEL=ERROR

# ============================================================
#  VẬN HÀNH cabin thật (RTSP + SQLite: tầng + tên chào + audit)
# ============================================================

# Vận hành đầy đủ qua RTSP: YOLO person + tracker + SCRFD + recog + SQLite audit.
# URL nguồn: tham số $1 > $FACE_CABIN_RTSP_URL (web local config).
face_cabin() {
    local url="${1:-$FACE_CABIN_RTSP_URL}"
    local db="${2:-$FACE_RESIDENT_DB}"
    local thr="${3:-0.35}"
    local lat="${4:-100}"
    if [ -z "$url" ]; then
        echo "Chưa có RTSP URL. Truyền trực tiếp hoặc đặt biến FACE_CABIN_RTSP_URL."
        echo "  face_cabin rtsp://admin:pass@192.168.1.100:554/stream1"
        echo "  export FACE_CABIN_RTSP_URL='rtsp://...'   # web local config"
        echo "(USB chỉ để dev: dùng face_usb)"
        return 1
    fi
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

# Cập nhật thông tin 1 resident theo tên, dùng flag (gộp nhiều tool về 1 lệnh).
# Chỉ các flag được truyền mới bị UPDATE (partial update).
#   --floor N            home_floor (số; 0 = chưa đăng ký tầng)
#   --greeting STR       greeting_name ("bác Nga")
#   --apartment STR      apartment ("12A05")
#   --language vi|en     language
#   --role ROLE          resident|staff|vip|guest_regular
#   --active 0|1         1 = hoạt động, 0 = soft-delete
#   --notes STR          ghi chú
#   --db PATH            DB đích (default residents.db)
# Ví dụ: face_set_resident 'Cao Tien Sy' --floor 7 --greeting 'anh Sy' --role staff
face_set_resident() {
    if [ -z "$1" ]; then
        echo "Usage: face_set_resident NAME [--floor N] [--greeting STR] [--apartment STR]"
        echo "                              [--language vi|en] [--role ROLE] [--active 0|1]"
        echo "                              [--notes STR] [--db PATH]"
        echo "  ROLE: resident|staff|vip|guest_regular"
        echo "  Ví dụ: face_set_resident 'Cao Tien Sy' --floor 7 --greeting 'anh Sy'"
        return 1
    fi
    local name="$1"; shift
    local db="$FACE_RESIDENT_DB"
    local sets=()

    # SQL single-quote escaper.
    _sq() { printf "%s" "${1//\'/\'\'}"; }

    while [ $# -gt 0 ]; do
        case "$1" in
            --floor)
                if ! [[ "$2" =~ ^-?[0-9]+$ ]]; then echo "--floor cần số nguyên"; return 1; fi
                sets+=("home_floor=$2"); shift 2 ;;
            --greeting)
                sets+=("greeting_name='$(_sq "$2")'"); shift 2 ;;
            --apartment)
                sets+=("apartment='$(_sq "$2")'"); shift 2 ;;
            --language)
                if [ "$2" != "vi" ] && [ "$2" != "en" ]; then
                    echo "--language chỉ nhận 'vi' hoặc 'en'"; return 1; fi
                sets+=("language='$2'"); shift 2 ;;
            --role)
                case "$2" in
                    resident|staff|vip|guest_regular) ;;
                    *) echo "--role: resident|staff|vip|guest_regular"; return 1 ;;
                esac
                sets+=("role='$2'"); shift 2 ;;
            --active)
                if [ "$2" != "0" ] && [ "$2" != "1" ]; then echo "--active: 0 hoặc 1"; return 1; fi
                sets+=("active=$2"); shift 2 ;;
            --notes)
                sets+=("notes='$(_sq "$2")'"); shift 2 ;;
            --db)
                db="$2"; shift 2 ;;
            *)
                echo "Flag không hợp lệ: $1"; return 1 ;;
        esac
    done

    if [ ! -f "$db" ]; then echo "Không tìm thấy DB: $db"; return 1; fi
    if [ ${#sets[@]} -eq 0 ]; then
        echo "Chưa có flag nào để cập nhật. Xem: face_set_resident (không tham số)."
        return 1
    fi

    # Always bump updated_at (no trigger in schema).
    local name_sql; name_sql="$(_sq "$name")"
    local set_clause; set_clause="$(IFS=,; echo "${sets[*]}"),updated_at=CURRENT_TIMESTAMP"
    # UPDATE + changes() must run in the SAME sqlite3 session.
    local changed
    changed=$(sqlite3 "$db" \
        "UPDATE residents SET $set_clause WHERE name='$name_sql'; SELECT changes();")
    echo "Đã cập nhật $changed resident (name='$name': ${sets[*]})"
    face_residents "$db"
}

# ============================================================
#  Quản lý dữ liệu (enroll → SQLite residents.db)
# ============================================================

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
        echo "  DB target (SQLite): $FACE_RESIDENT_DB"
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
        --db "$FACE_RESIDENT_DB" --schema "$FACE_SCHEMA" \
        --det-model "$FACE_DET_MODEL" \
        --recog-model "$FACE_RECOG_MODEL" \
        --recog-dim 512 --recog-bgr \
        "${flags[@]}")
}

face_enroll() {
    local dir="${1:-$FACE_FACES_DIR}"
    local db="${2:-$FACE_RESIDENT_DB}"
    (cd "$FACE_ROOT" && ./enroll_faces \
        --dir "$dir" --db "$db" --schema "$FACE_SCHEMA" \
        --det-model "$FACE_DET_MODEL" \
        --recog-model "$FACE_RECOG_MODEL" \
        --recog-dim 512 --recog-bgr)
    echo "Nhắc: enroll tạo resident home_floor=0 → dùng face_set_resident để gán tầng."
}

face_detect() {
    (cd "$FACE_ROOT" && ./face_recog_app "$FACE_DET_MODEL")
}

# ============================================================
#  DEV / test — đọc residents.db (SQLite là nguồn dữ liệu duy nhất)
#  USB chỉ để dev; cabin thật dùng face_cabin (RTSP).
# ============================================================

# Realtime trên USB cam (dev): YOLO person + tracker + SCRFD + recog + SQLite.
face_usb() {
    local db="${1:-$FACE_RESIDENT_DB}"
    local thr="${2:-0.35}"
    (cd "$FACE_ROOT" && ./face_recog_app "$FACE_DET_MODEL" \
        --recog-model "$FACE_RECOG_MODEL" \
        --recog-dim 512 --recog-bgr \
        --resident-db "$db" --cabin-id "${FACE_CABIN_ID:-1}" \
        --match-thr "$thr" \
        --person-model "$FACE_PERSON_MODEL")
}

# SCRFD-only (không tracker) — nhanh hơn, scene không occlusion. USB.
face_run_lite() {
    local db="${1:-$FACE_RESIDENT_DB}"
    local thr="${2:-0.35}"
    (cd "$FACE_ROOT" && ./face_recog_app "$FACE_DET_MODEL" \
        --recog-model "$FACE_RECOG_MODEL" \
        --recog-dim 512 --recog-bgr \
        --resident-db "$db" --match-thr "$thr")
}

# Realtime qua RTSP (dev — URL bắt buộc truyền trực tiếp).
face_run_rtsp() {
    if [ -z "$1" ]; then
        echo "Usage: face_run_rtsp URL [DB] [THR] [LATENCY_MS]"
        echo "Example: face_run_rtsp rtsp://admin:pass@192.168.1.100:554/stream1"
        return 1
    fi
    local url="$1"
    local db="${2:-$FACE_RESIDENT_DB}"
    local thr="${3:-0.35}"
    local lat="${4:-100}"
    (cd "$FACE_ROOT" && ./face_recog_app "$FACE_DET_MODEL" \
        --recog-model "$FACE_RECOG_MODEL" \
        --recog-dim 512 --recog-bgr \
        --resident-db "$db" --match-thr "$thr" \
        --person-model "$FACE_PERSON_MODEL" \
        --source "$url" --gst-latency "$lat")
}

face_bench() {
    local n="${1:-100}"
    (cd "$FACE_ROOT" && ./face_recog_app "$FACE_DET_MODEL" \
        --recog-model "$FACE_RECOG_MODEL" \
        --recog-dim 512 --recog-bgr \
        --resident-db "$FACE_RESIDENT_DB" --match-thr 0.35 \
        --person-model "$FACE_PERSON_MODEL" \
        --frames "$n")
}

face_bench_lite() {
    local n="${1:-100}"
    (cd "$FACE_ROOT" && ./face_recog_app "$FACE_DET_MODEL" \
        --recog-model "$FACE_RECOG_MODEL" \
        --recog-dim 512 --recog-bgr \
        --resident-db "$FACE_RESIDENT_DB" --match-thr 0.35 \
        --frames "$n")
}

face_bench_rtsp() {
    if [ -z "$1" ]; then
        echo "Usage: face_bench_rtsp URL [N_FRAMES] [DB]"
        return 1
    fi
    local url="$1"
    local n="${2:-100}"
    local db="${3:-$FACE_RESIDENT_DB}"
    (cd "$FACE_ROOT" && ./face_recog_app "$FACE_DET_MODEL" \
        --recog-model "$FACE_RECOG_MODEL" \
        --recog-dim 512 --recog-bgr \
        --resident-db "$db" --match-thr 0.35 \
        --person-model "$FACE_PERSON_MODEL" \
        --source "$url" \
        --frames "$n")
}

# ============================================================
#  Legacy / 1 lần
# ============================================================

# Import .fdb cũ → SQLite residents.db. ĐÃ migrate xong; giữ lại để khôi phục
# hoặc nhập một .fdb khác. Enroll/add hiện ghi thẳng SQLite, KHÔNG cần .fdb.
face_migrate() {
    if [ -z "$1" ]; then
        echo "Usage: face_migrate FDB [DB]   (legacy — .fdb đã migrate xong)"
        echo "Example: face_migrate db/faces_all.fdb $FACE_RESIDENT_DB"
        return 1
    fi
    local fdb="$1"
    local db="${2:-$FACE_RESIDENT_DB}"
    shift 2 2>/dev/null
    (cd "$FACE_ROOT" && ./migrate_fdb --fdb "$fdb" --db "$db" \
        --schema "$FACE_SCHEMA" "$@")
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

face_help() {
    cat <<'EOF'
==============================================================
 SCRFD + YOLO + MobileFaceNet — Face Recognition (A733)
==============================================================

 ┌─ VẬN HÀNH CABIN THẬT (RTSP + SQLite: tầng + tên chào + audit) ┐
   face_cabin [URL] [DB] [THR]    Live vận hành qua RTSP (YOLO+tracker+SQLite)
                                  URL: tham số > $FACE_CABIN_RTSP_URL (web config)
   face_residents [DB]            Liệt kê residents
   face_events [DB] [N]           Xem N match_events gần nhất
   face_set_resident NAME [FLAGS]
                                  Cập nhật resident (--floor/--greeting/--role/
                                  --apartment/--language/--active/--notes)
 └──────────────────────────────────────────────────────────────┘
   Đặt URL: export FACE_CABIN_RTSP_URL='rtsp://...' (web local ghi biến này),
   hoặc truyền trực tiếp: face_cabin rtsp://admin:pass@ip:554/stream1
   home_floor=0 = "chưa đăng ký tầng": vẫn chào tên, overlay hiện F?.
   Đặt FACE_CABIN_ID=N để đổi cabin id (default 1).

 ── Quản lý dữ liệu (enroll → SQLite residents.db) ──
   face_capture NAME [N] [MIN]    Chụp N frames của 1 người (ra folder)
   face_add NAME IMG [IMG...]     Thêm người vào SQLite (--merge/--replace)
   face_enroll [DIR] [DB]         Enroll folder → SQLite (mỗi ảnh 1 embedding)
   face_detect                    Detect-only (bỏ recognition)
     Enroll tạo resident home_floor=0 → dùng face_set_resident để gán tầng.

 ── DEV / test (đọc residents.db; USB chỉ để dev) ──
   face_usb [DB] [THR]            Live trên USB cam (YOLO+tracker)
   face_run_lite [DB] [THR]       Live SCRFD-only USB (nhanh hơn)
   face_run_rtsp URL [DB] [THR]   Live RTSP (URL truyền trực tiếp)
   face_bench [N]                 Bench full pipeline (USB)
   face_bench_lite [N]            Bench SCRFD-only (USB)
   face_bench_rtsp URL [N]        Bench RTSP

 ── Legacy / 1 lần ──
   face_migrate FDB [DB]          Import .fdb cũ → SQLite (đã migrate xong)
   face_ls                        Liệt kê DB (.fdb+.db) và enroll folders

 ── Model default ──
   detect: model/face_det/scrfd_2.5g_bnkps640_uint8_a733.nb (SCRFD)
   person: model/person_det/yolov5s_rt_uint8_a733.nb (YOLOv5s COCO)
   recog : model/face_recog/w600k_mbf_uint8_a733.nb (MobileFaceNet 512-D)

 ── Ghi chú ──
   SQLite (residents.db) là NGUỒN DỮ LIỆU DUY NHẤT: enroll/add ghi thẳng
   vào đây, KHÔNG còn .fdb. Cabin thật chạy RTSP qua face_cabin; USB chỉ dev.
   Tracker duy trì ID kể cả khi người quay lưng, cache recog giảm tải NPU
   (~15 FPS). SCRFD-only (_lite) nhanh hơn nhưng mất persistence.
==============================================================
EOF
}

echo "[face] loaded shortcuts — gõ 'face_help' để xem"
