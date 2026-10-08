// ============================================================================
// enroll_bridge.js — cầu nối enroll khuôn mặt: Node (edge_elevator) -> add_person (C++/NPU)
//
// Phương án A (tạm thời, tháo bỏ được khi chuyển sang luồng UDS mobile):
// sau khi POST /api/faces lưu ảnh + ghi Mongo, gọi hàm enrollFace() ở đây để
// spawn binary `add_person` của face_recog_app. add_person detect+align+embedding
// rồi ghi vào SQLite residents.db (keyed by ext_id = resident_id), in 1 dòng JSON
// kết quả. Node đọc JSON đó -> cập nhật trạng thái enroll vào Mongo -> trả UI.
//
// GHÉP VÀO server.js:
//   const { enrollFace } = require('./enroll_bridge');
//   ... trong handler POST /api/faces, SAU khi có savedPath + newDoc + insertedId:
//   const enroll = await enrollFace({
//     imagePath:    savedPath,                         // THƯ MỤC cư dân (add_person quét mọi ảnh)
//     extId:        newDoc.resident_id,                // mã NV (ext_id trong SQLite)
//     name:         newDoc.name,
//     homeFloor:    parseInt(newDoc.priority_floor, 10) || 0,
//     role:         newDoc.role,                       // phải là 1 trong 14 role snake_case
//   });
//   // cập nhật trạng thái vào Mongo
//   await col.updateOne({ _id: result.insertedId }, { $set: {
//     enroll_status: enroll.ok ? 'enrolled' : 'failed',
//     enroll_resident_id: enroll.resident_id ?? null,
//     enroll_embeddings: enroll.embeddings ?? 0,
//     enroll_error: enroll.error ?? null,
//     enroll_at: new Date(),
//   }});
//   // rồi trả UI kèm enroll (sendJson 201 {... , enroll})
// ============================================================================

const { execFile } = require('child_process');
const path = require('path');

// --- Cấu hình đường dẫn (đổi qua env nếu cần) ------------------------------
const FR_ROOT   = process.env.FACE_RECOG_ROOT || '/home/orangepi/face_recog_app';
const SQLITE_BIN = process.env.SQLITE3_BIN || '/usr/bin/sqlite3';
const ADD_BIN   = process.env.ADD_PERSON_BIN  || path.join(FR_ROOT, 'add_person');
const DET_MODEL = process.env.FACE_DET_MODEL  || path.join(FR_ROOT, 'model/face_det/scrfd_2.5g_bnkps640_uint8_a733.nb');
const REC_MODEL = process.env.FACE_RECOG_MODEL|| path.join(FR_ROOT, 'model/face_recog/w600k_mbf_uint8_a733.nb');
const RES_DB    = process.env.FACE_RESIDENT_DB|| path.join(FR_ROOT, 'db/residents.db');
const SCHEMA    = process.env.FACE_SCHEMA     || path.join(FR_ROOT, 'db/schema.sql');
const ENROLL_TIMEOUT_MS = parseInt(process.env.ENROLL_TIMEOUT_MS, 10) || 30000;
const DELETE_TIMEOUT_MS = parseInt(process.env.DELETE_TIMEOUT_MS, 10) || 10000;

// 14 role hợp lệ (khớp CHECK constraint SQLite schema_version 3).
const VALID_ROLES = new Set([
  'chairman','vice_president','deputy_general_director','head','director',
  'manager','specialist','team_leader','sales','engineer','staff','intern',
  'vip','guest_regular',
]);

/**
 * Enroll một khuôn mặt vào SQLite qua add_person.
 * @param {object} p
 * @param {string} p.imagePath  Đường dẫn ảnh HOẶC thư mục cư dân (add_person quét mọi ảnh).
 * @returns {Promise<{ok:boolean, resident_id?:number, ext_id?:string, embeddings?:number, error?:string}>}
 */
function enrollFace({ imagePath, extId, name, homeFloor, role }) {
  return new Promise((resolve) => {
    if (!imagePath) return resolve({ ok: false, error: 'no_image_path' });
    if (!extId)     return resolve({ ok: false, error: 'no_ext_id' });

    // Chuẩn hoá role về snake_case lowercase; rỗng -> để add_person tự default 'staff'.
    let roleArg = (role || '').trim().toLowerCase().replace(/\s+/g, '_');
    if (roleArg && !VALID_ROLES.has(roleArg)) {
      return resolve({ ok: false, error: `invalid_role:${roleArg}` });
    }

    // add_person nhận toàn flag (không positional). Dựng mảng flag:
    const argv = [
      '--name',        String(name || ''),
      '--image',       String(imagePath),
      '--ext-id',      String(extId),
      '--home-floor',  String(Number.isFinite(homeFloor) ? homeFloor : 0),
      '--db',          RES_DB,
      '--schema',      SCHEMA,
      '--det-model',   DET_MODEL,
      '--recog-model', REC_MODEL,
      '--recog-dim',   '512',
      '--recog-bgr',
      '--replace',     // enroll lại cùng ext_id -> thay embedding cũ (idempotent)
      '--json',
      '--log-level',   'error',
    ];
    if (roleArg) { argv.push('--role', roleArg); }

    execFile(ADD_BIN, argv, { timeout: ENROLL_TIMEOUT_MS, cwd: FR_ROOT },
      (err, stdout, stderr) => {
        // add_person in JSON ở dòng cuối stdout kể cả khi thất bại (exit != 0).
        const lastLine = String(stdout || '').trim().split('\n').filter(Boolean).pop();
        let parsed = null;
        if (lastLine) { try { parsed = JSON.parse(lastLine); } catch (_) {} }

        if (parsed && typeof parsed.ok === 'boolean') {
          return resolve(parsed);   // {ok, resident_id, ext_id, embeddings[, error]}
        }
        // Không parse được JSON -> lỗi hạ tầng (binary lỗi, timeout, thiếu model...).
        const reason = err
          ? (err.killed ? 'timeout' : `spawn_error:${err.code ?? err.message}`)
          : 'no_json_output';
        return resolve({ ok: false, error: reason, stderr: String(stderr || '').slice(0, 500) });
      });
  });
}

/**
 * Xóa resident và toàn bộ embeddings trong SQLite theo ext_id.
 * SQLite là catalog local mà face_recog_app đọc trực tiếp.
 */
function deleteResident({ extId }) {
  return new Promise((resolve) => {
    const value = String(extId || '').trim();
    if (!value) return resolve({ ok: false, error: 'no_ext_id' });
    if (!/^[A-Za-z0-9_-]+$/.test(value)) {
      return resolve({ ok: false, error: 'invalid_ext_id' });
    }

    // resident_id is restricted to ASCII letters/digits/_/- by face_core.
    // Keep FK cascade enabled so embeddings are deleted with the resident.
    const sql = [
      'PRAGMA foreign_keys=ON;',
      'BEGIN IMMEDIATE;',
      `DELETE FROM residents WHERE ext_id='${value}';`,
      'SELECT changes();',
      'COMMIT;',
    ].join(' ');

    execFile(SQLITE_BIN, ['-batch', '-noheader', RES_DB, sql],
      { timeout: DELETE_TIMEOUT_MS, cwd: FR_ROOT },
      (err, stdout, stderr) => {
        if (err) {
          return resolve({ ok: false, ext_id: value,
            error: err.killed ? 'timeout' : `sqlite_error:${err.code ?? err.message}`,
            stderr: String(stderr || '').slice(0, 500) });
        }
        const deleted = Number.parseInt(String(stdout || '').trim().split(/\s+/).pop(), 10);
        if (!Number.isInteger(deleted)) {
          return resolve({ ok: false, ext_id: value, error: 'invalid_sqlite_output' });
        }
        return resolve({ ok: true, ext_id: value, deleted });
      });
  });
}

module.exports = { enrollFace, deleteResident, VALID_ROLES };
