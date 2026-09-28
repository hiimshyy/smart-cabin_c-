# Tasks — AI Core ↔ Edge Core qua Unix Domain Socket (edge-core-uds)

**Spec ID**: `edge-core-uds`

---

## 📌 TRẠNG THÁI

**Chưa bắt đầu code.** Tài liệu (requirements + design) đã soạn, **chờ chủ dự án duyệt**. Đây là spec
Đề xuất 5 (giao tiếp nội bộ Edge), phụ thuộc `resident-db-layer` (đã xong) + `cabin-runtime-config`
(AppConfig/precedence). Chỉ triển khai **chiều AI→Edge (đợt a)**; chiều Edge→AI (0x2001/0x2002) để
đợt b.

**Quyết định đã chốt (để không hỏi lại):**
- AI Core = **client**, Edge Core = **server**; reconnect bắt buộc (Edge Core có thể lên sau).
- `resident_id` (int64) **giữ nguyên**; tag `20001` = `Resident.ext_id`, fallback
  `to_string(resident_id)` khi rỗng. KHÔNG đổi khóa chính, KHÔNG phá FK/MatchEngine.
- Đợt a **không gửi `20006`** (tầng bấm tay); nguồn thuộc Edge Core (§3 requirements).
- Cấu hình bằng **CLI `--edge-socket`** (precedence CLI > default; KHÔNG đọc DB, KHÔNG migration).
- **Dải tag v2** (đã chốt): chèn `cabin_id` = `20001`, mọi tag v1 từ `20001` trở đi dời +1 →
  `20000`=ts, `20001`=cabin_id, `20002`=mã NV/ID, `20003`=họ tên, `20004`=chức vụ, `20005`=tầng mặc
  định, `20006`=đường dẫn ảnh, `20007`=tầng bấm tay. `cabin_id` gửi trong `0x1001`/`0x1002` dưới dạng
  tag số `20001` (không dùng key chữ).
- JSON **tự viết** (không dependency); framing header 8 byte, `msg_type`/`payload_len` network byte
  order.
- Gửi **non-blocking** qua thread nền + bounded queue (mirror `ResidentDB::writer_loop`), drop-oldest
  khi đầy.
- Không có `--edge-socket` ⇒ **không hồi quy** (EdgeClient tắt hẳn).

**Đã chốt với chủ dự án (không còn điểm mở):**
- `cabin_id` = tag số `20001` trong dải tag v2 (áp dụng nhất quán cho cả MQTT §2 lẫn UDS §3 của tài
  liệu). Không còn dùng key chữ `"cabin_id"`.
- Cập nhật file `.docx` theo dải v2 **do đội tài liệu thực hiện** (ngoài scope code của spec này) —
  xem task 7.2.

**Ghi chú build/test:** toàn bộ chạy **trực tiếp trên Orange Pi** (Linux, đủ g++ + POSIX socket +
NPU/OpenCV). Module `edge_json` và `edge_client` **không phụ thuộc NPU/OpenCV/sqlite** → unit-test
độc lập bằng `g++ -std=c++17 -Isrc` (framing + JSON + transport qua `socketpair`), không cần load
model. Khâu nối `main.cpp` build đầy đủ bằng `make` trên Orange Pi để xác nhận link. Không có ràng
buộc "blocked-pending-Linux".

---

- [ ] 1. Serializer JSON tối giản (`src/edge_json.{h,cpp}`)
  - [ ] 1.1 `json_escape(s)`: escape `"`, `\`, `\b \f \n \r \t`, control `< 0x20` → `\u00XX`; giữ nguyên byte UTF-8 ≥ 0x80.
    - _Requirements: R3.2_
  - [ ] 1.2 `JsonObj` builder: `kv` (string, có escape), `kvi` (int64), `kvf` (double, `%.2f`, locale-independent); `str()` bọc `{...}`.
    - _Requirements: R3.1, R3.4_
  - [ ] 1.3 Hàm dựng body theo msg_type (dải tag v2): `build_recog(cabin_id,id,name,floor,similarity,ts)` → 0x1001 với `20000/20001/20002/20003/20005/20008` (`20008` = similarity, `%.2f`), KHÔNG có `20007`; `build_unknown(cabin_id,conf,ts)` → 0x1002 với `20000/20001/status/confidence`; `build_heartbeat(ts)` → 0x1003 với `20000`.
    - _Requirements: R3.3, R3.4, R3.5, R7.1, R7.2, R7.4_

- [ ] 2. Framing + transport (`src/edge_client.{h,cpp}` — phần thuần logic, không thread)
  - [ ] 2.1 Hằng `MAGIC0/MAGIC1/HEADER_LEN`, `enum class MsgType`, struct `EdgeConfig`.
    - _Requirements: R2.1_
  - [ ] 2.2 `encode_frame(type, body)`: gộp header 8 byte (magic + `htons(type)` + `htonl(len)`) + body vào một `std::string`.
    - _Requirements: R2.1, R2.2, R2.3_
  - [ ] 2.3 `write_all(fd, p, n)`: lặp `send(MSG_NOSIGNAL)`, xử lý partial write + `EINTR`; trả false khi lỗi (EPIPE/ECONNRESET).
    - _Requirements: R2.4, R4.5_

- [ ] 3. `EdgeClient` — kết nối + thread nền + reconnect (`src/edge_client.cpp`)
  - [ ] 3.1 `connect_locked()`: tạo `AF_UNIX/SOCK_STREAM`, set `SO_SNDTIMEO`; connect hỗ trợ pathname VÀ abstract namespace (`@`, leading NUL + độ dài thực). Trả false + errno khi fail.
    - _Requirements: R1.1, R1.2, R1.3, NFR chống treo_
  - [ ] 3.2 `enqueue(type, body)`: giữ khóa; nếu `q_.size() >= max_queue` → `pop_front` + `++dropped_` (drop-oldest); `push_back`; `notify`.
    - _Requirements: R4.1, R4.3_
  - [ ] 3.3 `worker_loop()`: nếu chưa connect → `connect_locked` + backoff min→max + jitter + log tiết chế; timed-wait `cv_` theo `heartbeat_ms`; drain queue qua `write_all`; lỗi ghi → đóng fd + giữ job còn lại để gửi lại sau reconnect.
    - _Requirements: R4.2, R5.1, R5.2, R5.3_
  - [ ] 3.4 Heartbeat: khi timed-wait hết hạn và đang connected → enqueue `0x1003` (đi chung đường non-blocking).
    - _Requirements: R6.1, R6.2, R6.3_
  - [ ] 3.5 API công khai `send_recog(...)`, `send_unknown(...)`: dựng body (task 1.3) rồi `enqueue`; fire-and-forget.
    - _Requirements: R4.1, R7.1, R7.3_
  - [ ] 3.6 `start()` (spawn thread; false nếu socket_path rỗng) + `close()` (flush best-effort + join; đóng fd). Destructor gọi `close()`.
    - _Requirements: R4.4, R1.4_

- [ ] 4. Cấu hình CLI + nạp `ext_id`
  - [ ] 4.1 `AppConfig`: thêm `const char* edge_socket = nullptr;`. `parse_args`: bắt `--edge-socket <path>`; thêm dòng usage.
    - _Requirements: R8.1, R8.2_
  - [ ] 4.2 `struct Resident` thêm `std::string ext_id;`; `ResidentDB::load_active` SELECT thêm cột `ext_id` (NULL → chuỗi rỗng).
    - _Requirements: R8.3, R7.3_

- [ ] 5. Khâu nối `main.cpp`
  - [ ] 5.1 Khai báo `std::unique_ptr<EdgeClient> edge;`; khởi tạo trong nhánh `use_resident_db` khi `--edge-socket` có giá trị (tái dùng reconnect backoff của `CabinConfig`); log trạng thái; `edge.reset()` nếu `start()` fail.
    - _Requirements: R1.1, R8.4, R5.1_
  - [ ] 5.2 Outcome loop: nhánh `confirmed` → tính `id_str` (ext_id / fallback), `name`, `home_floor` từ `resident_by_id` → `edge->send_recog(id_str, name, floor, oc.similarity, now)` (truyền `oc.similarity` cho tag `20008`). Nhánh `unknown` → `edge->send_unknown(oc.similarity, now)`. Chỉ khi `edge != nullptr`.
    - _Requirements: R7.1, R7.2, R7.3, R3.3, R3.4_
  - [ ] 5.3 Shutdown: `if (edge) edge->close();` ở chỗ dọn resource trước return. Log của module không in PII (chỉ id/đếm).
    - _Requirements: R4.4, NFR PII_

- [ ] 6. Build + test
  - [ ] 6.1 `Makefile`: thêm `edge_client.cpp` + `edge_json.cpp` vào `DB_SRCS` (link vào `face_recog_app`). Không thêm lib.
    - _Requirements: NFR build_
  - [ ] 6.2 `tests/test_edge_json.cpp`: escape (`"`, `\`, `\n`, control, UTF-8 tiếng Việt giữ nguyên); body 0x1001 (đúng tag dải v2 `20000/20001/20002/20003/20005/20008`, có `20001` cabin + `20008` similarity `%.2f`, KHÔNG có `20007`); body 0x1002 (có `20001`, `status`, `confidence` `%.2f` bất kể locale); `kvi` int64 biên; `kvf` `%.2f` locale-independent.
    - _Requirements: R3.1, R3.2, R3.3, R3.4, R7.2_
  - [ ] 6.3 `tests/test_edge_frame.cpp`: `encode_frame` (magic, `ntohs`/`ntohl` round-trip, tổng độ dài); round-trip qua `socketpair` (2 frame liền → tách gói đúng, chống dính gói); (tùy chọn) partial-write.
    - _Requirements: R2.1, R2.2, R2.3, R2.4_
  - [ ] 6.4 Test reconnect/drop: trỏ path không tồn tại → connect fail không crash; bơm > max_queue job → `dropped_ > 0`.
    - _Requirements: R1.3, R4.3, R5.1_
  - [ ] 6.5 `tests/run_tests.sh`: thêm stanza biên dịch + chạy 2 test mới (chỉ `-Isrc`, không NPU).
    - _Requirements: NFR kiểm thử được_

- [ ] 7. Không hồi quy + tài liệu
  - [ ] 7.1 Xác nhận build `make` không có `--edge-socket` chạy y như cũ (không thread mới, không kết nối). Chạy full `run_tests.sh` pass.
    - _Requirements: NFR không hồi quy_
  - [ ] 7.2 Cập nhật `docs/DEVELOPMENT_PLAN.md`: ghi mục "Phân tách trách nhiệm AI Core vs Edge Core về gọi tầng" (§3 requirements) + tham chiếu spec này.
    - _Requirements: R7 (§3), liên-team_
  - [ ] 7.3 **[Đội tài liệu]** Cập nhật `docs/Smart_cabin_communicate.docx` sang dải tag v2 (chèn `cabin_id`=`20001`, dời +1 toàn bộ §2 MQTT + §3 UDS + từ điển §3.4 + các payload mẫu §2.1/§2.2/§3.4.1–3.4.4). Ngoài scope code; ghi ở đây để không mất dấu quyết định liên-team.
    - _Requirements: §4 requirements (dải tag v2), liên-team_

---

## Thứ tự triển khai đề xuất

1 (edge_json) → 2 (framing thuần) → 6.2/6.3 (test ngay, TDD) → 3 (EdgeClient thread) → 6.4 → 4
(config + ext_id) → 5 (nối main) → 6.1/6.5 (build + run_tests) → 7 (regression + docs).

Task 1–2 và test 6.2–6.3 làm được **độc lập, không cần Orange Pi** (chỉ g++). Task 3–5 cần verify
link đầy đủ trên Orange Pi.
