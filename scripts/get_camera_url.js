#!/usr/bin/env node
// get_camera_url.js — in ra RTSP URL của camera từ PostgreSQL (elevator_cameras).
//
// PostgreSQL (bảng elevator_cameras) là NGUỒN SỰ THẬT cho camera đã cấu hình.
// face_recog_app (C++) không đọc PostgreSQL trực tiếp; script này cầu nối:
// env.sh gọi nó CHỈ KHI không truyền URL tay, rồi truyền URL vào --source.
// Ưu tiên tổng thể (trong env.sh): CLI URL (test nhiều camera) > PostgreSQL config.
// SQLite cabins CHỈ giữ tham số AI (match_thr, confirm_streak, ...), không giữ URL.
//
// Cách dùng:
//   node get_camera_url.js [cameraId|#dbId]
//     - không tham số  -> camera 'connected' đầu tiên (id nhỏ nhất)
//     - <cameraId>     -> khớp cột camera_id (vd '{SN}_{slaveId}')
//     - #<dbId>        -> khớp khóa chính id (vd '#6')
// In URL ra stdout (1 dòng) nếu tìm thấy; ngược lại exit != 0, không in URL.
//
// Cấu hình PostgreSQL qua env (mặc định trùng server.js của edge_elevator):
//   PG_HOST PG_PORT PG_USER PG_PASSWORD PG_DATABASE

const { Pool } = require('pg');

const pool = new Pool({
  host: process.env.PG_HOST || '127.0.0.1',
  port: parseInt(process.env.PG_PORT, 10) || 5432,
  user: process.env.PG_USER || 'embody_master',
  password: process.env.PG_PASSWORD || 'embodyy\u03006',
  database: process.env.PG_DATABASE || 'elevator_db',
  connectionTimeoutMillis: 4000,   // đừng treo mãi nếu PG không phản hồi
});

(async () => {
  const sel = (process.argv[2] || '').trim();
  let sql, params;
  if (!sel) {
    sql = `SELECT rtsp_url FROM elevator_cameras
           ORDER BY (status = 'connected') DESC, id ASC LIMIT 1`;
    params = [];
  } else if (sel.startsWith('#')) {
    sql = `SELECT rtsp_url FROM elevator_cameras WHERE id = $1 LIMIT 1`;
    params = [parseInt(sel.slice(1), 10)];
  } else {
    sql = `SELECT rtsp_url FROM elevator_cameras WHERE camera_id = $1 ORDER BY id ASC LIMIT 1`;
    params = [sel];
  }
  try {
    const r = await pool.query(sql, params);
    const url = r.rows[0] && r.rows[0].rtsp_url ? String(r.rows[0].rtsp_url).trim() : '';
    if (!url) { process.stderr.write('get_camera_url: không tìm thấy camera phù hợp\n'); process.exit(2); }
    process.stdout.write(url + '\n');
    process.exit(0);
  } catch (e) {
    process.stderr.write('get_camera_url: lỗi PostgreSQL: ' + e.message + '\n');
    process.exit(1);
  } finally {
    await pool.end().catch(() => {});
  }
})();
