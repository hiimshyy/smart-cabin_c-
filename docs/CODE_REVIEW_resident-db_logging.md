# Code Review — resident-db-layer + system-logging

**Ngày review**: 2026-09-03
**Phạm vi**: toàn bộ code mới sau khi hoàn thành 2 spec `resident-db-layer` và `system-logging`
(InteractionManager, migrate_fdb, khâu nối `main.cpp`, Phase 2 enroll/add, di trú logger, Makefile).
**Mục đích file**: theo dõi phát hiện + trạng thái sửa, đồng bộ giữa máy dev và Orange Pi.

> Quy ước trạng thái: `[ ]` chưa làm · `[~]` đang làm · `[x]` đã sửa.
> Cập nhật ô "Trạng thái" khi xử lý xong để hai máy biết tiến độ.

---

## Tổng quan

Phần code mới bám sát spec, cấu trúc sạch, không PII, tương thích ngược tốt. Các điểm mạnh:
- `main.cpp`: `--resident-db` ưu tiên, `--face-db` legacy vẫn chạy.
- Resolve `resident_id` từ tên cho cached tracker frame để giữ streak (`main.cpp:730-736`).
- Phase 2 enroll/add ghi multi-embedding, không average (đúng R6).
- Không PII xuyên suốt (dùng `resident_id`/`track_id`).
- Makefile tách `migrate_fdb` không kéo NPU/OpenCV; logger vào `COMMON_SRCS`.

Danh sách vấn đề xếp theo mức độ ưu tiên bên dưới.

---

## 🔴 P1 — Nên sửa sớm (ảnh hưởng đúng đắn / vận hành)

### P1-1. Còn `fprintf(stderr)` làm log thật ở tầng thư viện — không vào file log
**Trạng thái:** `[x]` ĐÃ SỬA — di trú `fprintf(stderr)` (lỗi thật) sang `LOG_*` ở `resident_db.cpp` (tag `db`), `match_engine.cpp` (tag `match`), `face_recog.cpp` (tag `recog`), `face_db.cpp` (tag `facedb`). Giữ `fprintf` usage/help. Các file này include `log/logger.h`; logger.cpp đã có trong link của mọi target. Verify: ResidentDB test in log qua logger (`[db] applied schema`).
**File:** `resident_db.cpp` (vd :20-24, :44-46, :116-118, :245-247), `match_engine.cpp:28-31`,
`face_recog.cpp:15,30,37,69`, `face_db.cpp:97`.
**Vấn đề:** các module này chưa di trú sang `LOG_*`. App chính chạy headless qua systemd →
lỗi DB/match/recog đi ra stderr chứ **không vào `/var/log/face-cabin`**, mất trace đúng lúc cần.
**Đề xuất:** thay `fprintf(stderr, ...)` (lỗi thật) → `LOG_ERROR/WARN` với tag phù hợp
(`db`, `match`, `recog`). Giữ `fprintf` ở phần usage/help.
**Lưu ý build:** các module lib này khi include `log/logger.h` cần logger.cpp đã link (đã có trong COMMON_SRCS).

### P1-2. `resident_id_by_name` ghi đè khi trùng tên / greeting_name
**Trạng thái:** `[x]` ĐÃ SỬA — bỏ hẳn map `name→id`; thêm `std::map<int,int64_t> track_resident_id` (track_id → resident_id) ghi tại điểm match trực tiếp (`rid>=0`). Cached-frame và overlay floor giờ resolve qua `track_id` (không còn suy ngược theo tên). Không còn tham chiếu `resident_id_by_name`.
**File:** `main.cpp:500-503` (nạp map) + `main.cpp:730-736` (resolve cached-frame theo tên).
**Vấn đề:** map `name→id` chỉ giữ id cuối. `greeting_name` rất dễ trùng ("anh Nam", "bác Nga").
Khi resolve cached tracker frame theo tên → có thể trả **nhầm `resident_id`** → ghi sai audit +
sai floor overlay. Frame recog trực tiếp thì đúng (trả thẳng `resident_id`), chỉ frame cached sai
→ không nhất quán trong cùng session.
**Đề xuất:** thay đường "resolve theo tên" bằng map trực tiếp **`track_id → resident_id`** trong
tracker/`main.cpp` (track đã có sẵn danh tính khi record_recognition). Không dựa vào tên để suy ngược id.

### P1-3. Dim mismatch im lặng làm cả DB "biến mất"
**Trạng thái:** `[x]` ĐÃ SỬA — trước khi `match_engine.build()`, đọc dim thực từ `embeddings.front()` và cross-check với `--recog-dim`; nếu lệch → `LOG_ERROR` nêu rõ dim DB vs dim model + thoát (code 2) thay vì để mọi người thành unknown. Thêm cảnh báo phụ nếu build ra 0 vector dù DB có rows.
**File:** `match_engine.cpp:19-31` (skip khi size != dim), `main.cpp:497` (`build(embeddings, recog_dim)`).
**Vấn đề:** nếu enroll bằng dim khác mà runtime quên `--recog-dim`, `build()` skip TOÀN BỘ embedding,
mọi người thành `unknown`, chỉ có 1 WARN mờ ra stderr (không qua LOG_*).
**Đề xuất:** đọc dim thực từ `embeddings[0].vector.size()` và cross-check với `recog_dim`;
nếu lệch → `LOG_ERROR` rõ ràng (nêu dim DB vs dim model) thay vì skip âm thầm. Cân nhắc auto-dùng dim của DB.

---

## 🟡 P2 — State machine (cần theo hành vi cabin mong muốn)

### P2-1. CONFIRMED "dính" — quyết định: MỖI PHIÊN CHÀO 1 LẦN (giữ nguyên hành vi)
**Trạng thái:** `[x] quyết định` · phần con bên dưới `[ ]` cần sửa
**File:** `interaction.cpp:118-140`.
**Quyết định (đã chốt):** mỗi phiên tương tác chỉ phát 1 event `matched` — KHÔNG chào lại
sau cooldown khi người vẫn ở trong cabin. Hành vi CONFIRMED-dính hiện tại là ĐÚNG Ý ĐỒ.
Cooldown per-resident chỉ để chống flip-flop giữa các phiên/track khác nhau.
**⚠ Phần con:** `[x]` ĐÃ SỬA — khi `in_cd == true`, KHÔNG đặt `state = CONFIRMED` nữa; session
giữ ở MATCHED và retry ở các frame sau, phát đúng 1 event `matched` khi cooldown hết. Vẫn đảm bảo
tối đa 1 event/phiên (guard `state != CONFIRMED` chỉ chặn sau khi ĐÃ phát). Thêm test case #10 trong
`tests/test_interaction.cpp` (suppressed-then-retry) → InteractionManager 50 checks pass.

### P2-2. Reap session sau đúng 1 frame vắng mặt
**Trạng thái:** `[x]` ĐÃ SỬA — thêm `InteractionConfig.reap_grace_ms` (default 500ms). Reap chỉ xóa session khi `now - last_seen_ms > reap_grace_ms`, nên dropout 1-2 frame giữ nguyên streak. Test #11 (`tests/test_interaction.cpp`) phủ: sống qua dropout ngắn → confirm; vắng lâu → reaped. Các test cũ đặt `reap_grace_ms=0` để giữ nghĩa "reap ngay". InteractionManager 54 checks pass.
**File:** `interaction.cpp:41-49`.
**Vấn đề:** tracker off (largest face, key=0) → chỉ 1 frame miss detection là mất toàn bộ streak;
detection flicker khiến khó đạt `confirm_streak=5`.
**Đề xuất:** thêm "grace period" (giữ session sống thêm N frame / T ms sau lần thấy cuối) trước khi reap,
tương tự `max_missed` của tracker.

### P2-3. "Lỗ" audit: match ngắn (chưa đủ streak) rồi mất → không sinh event nào
**Trạng thái:** `[ ]`
**File:** `interaction.cpp:76-97` + logic streak.
**Vấn đề:** subject match 1-2 frame rồi mất hẳn → không `matched` (chưa confirm) lẫn `unknown` (đã từng có ID).
**Đề xuất:** cân nhắc ghi 1 event nhẹ (vd `action='seen'` hoặc để `unknown` bắn kể cả khi từng match ngắn)
tùy nhu cầu audit. Ưu tiên thấp — bàn sau.

---

## 🟢 P3 — Nhỏ / dọn dẹp

### P3-1. `migrate_fdb` + `enroll` thiếu transaction bao ngoài
**Trạng thái:** `[x]` ĐÃ SỬA — thêm `ResidentDB::begin()/commit()/rollback()` (wrap `BEGIN/COMMIT/ROLLBACK`). `migrate_fdb` và `enroll_faces` bọc toàn bộ vòng import trong 1 transaction (`begin()` trước loop, `commit()` khi xong, `rollback()` nếu 0 embedding). Verify: enroll 11 ảnh chạy 1 transaction OK.
**File:** `migrate_fdb.cpp`, `enroll_faces.cpp`.
**Vấn đề:** mỗi INSERT tự commit (WAL fsync) → chậm nhiều lần với ~1000 người; chết giữa chừng để lại DB nửa vời.
**Đề xuất:** bọc `BEGIN;` … `COMMIT;` quanh toàn bộ vòng import.

### P3-2. `enroll` giữ resident rỗng (không embedding) trong DB
**Trạng thái:** `[x]` ĐÃ SỬA — dùng `find_resident` để biết resident mới tạo trong lần chạy này; nếu person không có embedding usable VÀ là mới tạo → `remove_resident(id)`. Resident tồn tại từ trước (đã có embedding) không bị đụng. Verify: folder không mặt → resident bị xóa, chỉ giữ person hợp lệ.
**File:** `enroll_faces.cpp`.
**Vấn đề:** resident active nhưng 0 vector → nạp vào `resident_by_id` nhưng không bao giờ match, chiếm chỗ.
**Đề xuất:** nếu person không có embedding usable → xóa resident vừa tạo.

### P3-3. `migrate_fdb` target thiếu `-lstdc++fs`
**Trạng thái:** `[x]` ĐÃ SỬA — thêm `-lstdc++fs` vào rule `migrate_fdb` trong Makefile cho đồng nhất với các target khác.
**File:** `Makefile`.
**Vấn đề:** logger dùng `<filesystem>`; trên GCC <9 có thể link fail.
**Đề xuất:** thêm `-lstdc++fs` vào rule `migrate_fdb`.

### P3-4. `enroll` chạy lại cùng folder → thêm embedding trùng (không dedup)
**Trạng thái:** `[x]` ĐÃ SỬA — thêm cờ `--replace` cho `enroll_faces`: với resident đã tồn tại, `delete_embeddings(id)` trước khi add lại. Verify: run 2 lần không cờ → 22 embeddings (dup); thêm `--replace` → về 11.
**File:** `enroll_faces.cpp`.
**Vấn đề:** chạy enroll nhiều lần làm DB phình dần embedding trùng.
**Đề xuất:** thêm cờ `--replace` cho enroll.

### P3-5. `source` luôn `'id_photo'` kể cả ảnh cabin
**Trạng thái:** `[x]` ĐÃ SỬA — thêm cờ `--source <id_photo|cabin|admin>` (default `id_photo`) cho cả `enroll_faces` và `add_person`. Verify: `add_person --source cabin` ghi embedding source='cabin', cùng DB với id_photo cũ.
**File:** `enroll_faces.cpp`, `add_person.cpp`.
**Vấn đề:** schema phân biệt `id_photo/cabin/admin` nhưng tool không cho chọn.
**Đề xuất:** thêm cờ `--source` (default `id_photo`).

### P3-6. `ResidentDB` writer thread + API đồng bộ chia sẻ `sqlite3*` không khóa
**Trạng thái:** `[x]` ĐÃ SỬA — chọn phương án (a): `open(path, schema, bool spawn_writer=true)`. App chính giữ default (spawn). Tool offline (`enroll_faces`/`add_person`/`migrate_fdb`) mở với `spawn_writer=false` → không có writer thread, chỉ dùng sync API → không có 2 thread chia sẻ handle.
**File:** `resident_db.cpp` (`open`), 3 tool.
**Vấn đề:** bẫy nếu gọi API sync khi writer thread active → 2 thread dùng chung 1 handle.
**Đề xuất:** (a) không spawn writer ở chế độ tool offline.

---

## Thứ tự xử lý đề xuất

1. ✅ **P1-1** (di trú logger tầng lib) — ĐÃ SỬA.
2. ✅ **P1-2** (name collision → dùng `track_id→resident_id`) — ĐÃ SỬA.
3. ✅ **P1-3** (dim cross-check) — ĐÃ SỬA.
4. ✅ **P2-1 phần con** (cooldown chặn confirm đầu → mất event) — ĐÃ SỬA.
5. ✅ **P2-2** (grace period) + toàn bộ **P3-1..P3-6** — ĐÃ SỬA.

**Còn lại (chờ quyết định):**
- **P2-3** (lỗ audit match ngắn) — `[ ]` review đánh dấu "ưu tiên thấp, bàn sau". Việc thêm
  `action='seen'` thay đổi ngữ nghĩa `match_events` (schema CHECK cho phép các action cố định) →
  cần chốt nhu cầu audit + cập nhật schema CHECK trước khi làm.

Tất cả mục đã sửa test được trên máy dev qua `tests/` (104 checks pass:
logger 14 + match 14 + interaction 54 + resident_db 22).
