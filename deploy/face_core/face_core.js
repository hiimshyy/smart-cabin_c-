// face_core — quản lý khuôn mặt cư dân thang máy, tách khỏi edge_elevator (06/10).
//
// Luồng giữ như edge_elevator:
//   • ảnh lưu trên đĩa: <FACE_STORAGE_DIR>/floor_<tầng>/<mã cư dân>/<mã>_<ms>_<rand>.<ext>
//     (tên file ASCII tự sinh để add_person/OpenCV đọc không lỗi Unicode)
//   • bản ghi trong `elevator_db.faces` (database cũ của edge_elevator, người dùng chốt 06/10),
//     nối bằng tài khoản MongoDB của iot_core (MONGO_URI của nó) — đổi database qua FACE_MONGO_DB
//   • thêm/sửa có ảnh → spawn add_person (face_recog_app) trích embedding vào SQLite
//     residents.db theo ext_id = mã cư dân; kết quả ghi lại vào bản ghi (enroll_*)
//
// API (mọi phản hồi JSON dạng iot_core: {status:"true", data} | {status:"false", mess}):
//   GET    /api/faces              danh sách
//   POST   /api/faces              thêm  {name, residentId, role, priorityFloor, images:[dataURL]}
//   PUT    /api/faces/:id          sửa   (cùng body; images rỗng = giữ ảnh cũ)
//   DELETE /api/faces/:id          xoá bản ghi + thư mục ảnh
//   GET    /api/faces/:id/image    ảnh đầu tiên trong thư mục cư dân
//   GET    /api/faces/:id/image/:n ảnh thứ n (0-based, xếp theo tên file); số ảnh = imageCount
//   /api/cameras…                   camera thang máy (PostgreSQL embody_devices) — xem cameras.js

const http = require('http')
const fs = require('fs')
const path = require('path')
require('dotenv').config({ path: path.join(__dirname, '.env'), quiet: true })
const { MongoClient, ObjectId } = require('mongodb')
const { enrollFace, deleteResident } = require('./enroll_bridge')
const { handleCameras } = require('./cameras')

const PORT = parseInt(process.env.FACE_PORT, 10) || 3010
const STORAGE_DIR = process.env.FACE_STORAGE_DIR || '/home/orangepi/faces'
const CORS_ORIGINS = String(process.env.FACE_CORS_ORIGINS || '').split(',').map((item) => item.trim()).filter(Boolean)
const BODY_LIMIT = 25 * 1024 * 1024
const IMAGE_RE = /\.(jpg|jpeg|png|bmp|webp)$/i

if (!process.env.MONGO_URI) {
  console.error('[face_core] thieu MONGO_URI trong .env (chep tu /edge/iot_core/.env)')
  process.exit(1)
}

const MIME = { '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg', '.png': 'image/png', '.bmp': 'image/bmp', '.webp': 'image/webp' }

// ── MongoDB ─────────────────────────────────────────────────────────────────
const mongo = new MongoClient(process.env.MONGO_URI)
const MONGO_DB = process.env.FACE_MONGO_DB || 'elevator_db'
let faces = null

// ── Tiện ích ───────────────────────────────────────────────────────────────
// Ký tự an toàn cho tên thư mục: tầng chỉ chữ số/chữ, mã cư dân chữ-số-gạch.
const safeFloor = (floor) => String(floor ?? '').trim().replace(/[^a-zA-Z0-9_-]/g, '') || '1'
const safeResident = (residentId) => String(residentId ?? '').trim().replace(/[^a-zA-Z0-9_-]/g, '_') || 'unknown'
const residentDir = (floor, residentId) => path.join(STORAGE_DIR, `floor_${safeFloor(floor)}`, safeResident(residentId))

// Chỉ thao tác trong STORAGE_DIR — chặn đường dẫn lạ trong face_link cũ.
const insideStorage = (target) => {
  const relative = path.relative(STORAGE_DIR, path.resolve(target))
  return relative && !relative.startsWith('..') && !path.isAbsolute(relative)
}

const imagesIn = (dir) => {
  try {
    return fs.readdirSync(dir).filter((file) => IMAGE_RE.test(file)).sort()
  } catch (error) {
    return []
  }
}

// Ghi một ảnh data-URL/base64 vào thư mục cư dân, tên file ASCII tự sinh.
const saveImage = (dir, residentId, image) => {
  const match = String(image).match(/^data:image\/(\w+);base64,/)
  const ext = match ? (match[1].toLowerCase() === 'jpeg' ? 'jpg' : match[1].toLowerCase()) : 'png'
  const buffer = Buffer.from(String(image).replace(/^data:image\/\w+;base64,/, ''), 'base64')
  if (!buffer.length) throw new Error('anh rong hoac sai dinh dang')

  fs.mkdirSync(dir, { recursive: true })
  const rand = Math.random().toString(36).slice(2, 6)
  const file = path.join(dir, `${safeResident(residentId)}_${Date.now()}_${rand}.${ext}`)
  fs.writeFileSync(file, buffer)
  return file
}

// Bản ghi Mongo → gói trả UI (tên trường camelCase như edge_elevator).
const toFace = (doc) => {
  const id = doc._id.toString()
  return {
    id,
    name: doc.name || '',
    residentId: doc.resident_id || '',
    role: doc.role || 'Staff',
    priorityFloor: doc.priority_floor !== undefined ? String(doc.priority_floor) : '1',
    faceLink: doc.face_link || '',
    hasImage: Boolean(doc.has_image),
    imageCount: doc.face_link ? imagesIn(doc.face_link).length : 0,
    imageUrl: doc.has_image ? `/api/faces/${id}/image` : null,
    enroll: {
      status: doc.enroll_status || null,
      embeddings: doc.enroll_embeddings ?? 0,
      error: doc.enroll_error ?? null,
      at: doc.enroll_at || null
    },
    updatedAt: doc.updated_at || null
  }
}

// Gọi add_person cho thư mục cư dân rồi ghi kết quả vào bản ghi. Không ném lỗi — enroll hỏng
// thì bản ghi vẫn lưu, trạng thái 'failed' kèm lý do để UI hiện.
const enroll = async (id, doc) => {
  let result
  try {
    result = await enrollFace({
      imagePath: doc.face_link,
      extId: doc.resident_id,
      name: doc.name,
      homeFloor: parseInt(doc.priority_floor, 10) || 0,
      role: doc.role
    })
  } catch (error) {
    result = { ok: false, error: `enroll_exception:${error.message}` }
  }

  await faces.updateOne({ _id: id }, {
    $set: {
      enroll_status: result.ok ? 'enrolled' : 'failed',
      enroll_resident_id: result.resident_id ?? null,
      enroll_embeddings: result.embeddings ?? 0,
      enroll_error: result.ok ? null : (result.error ?? 'unknown'),
      enroll_at: new Date()
    }
  })
  console.log(`[ENROLL] ${doc.resident_id} ${result.ok ? `ok embeddings=${result.embeddings}` : `loi ${result.error}`}`)
}

// Đọc và kiểm body thêm/sửa. Mã cư dân bắt buộc: nó là tên thư mục ảnh và ext_id khi enroll.
const readFaceBody = (body) => {
  const name = String(body.name ?? '').trim()
  const residentId = String(body.residentId ?? body.resident_id ?? '').trim()
  const role = String(body.role ?? '').trim() || 'Staff'
  const priorityFloor = String(body.priorityFloor ?? '').trim()
  const images = Array.isArray(body.images) ? body.images.filter(Boolean)
    : (body.imageBase64 || body.image ? [body.imageBase64 || body.image] : [])

  if (!name) throw new Error('Thieu ten')
  if (!/^[a-zA-Z0-9_-]+$/.test(residentId)) throw new Error('Ma cu dan chi gom chu, so, - hoac _')
  if (!/^\d{1,3}$/.test(priorityFloor) || Number(priorityFloor) < 1) throw new Error('Tang uu tien phai la so nguyen tu 1')
  return { name, residentId, role, priorityFloor, images }
}

// ── HTTP ────────────────────────────────────────────────────────────────────
const corsHeaders = (req) => {
  const origin = req.headers.origin
  return origin && CORS_ORIGINS.includes(origin)
    ? { 'Access-Control-Allow-Origin': origin, 'Access-Control-Allow-Methods': 'GET, POST, PUT, DELETE, OPTIONS', 'Access-Control-Allow-Headers': 'Content-Type', Vary: 'Origin' }
    : {}
}

const send = (req, res, code, payload) => {
  res.writeHead(code, { 'Content-Type': 'application/json; charset=UTF-8', ...corsHeaders(req) })
  res.end(JSON.stringify(payload))
}
const ok = (req, res, data, code = 200) => send(req, res, code, { status: 'true', data })
const fail = (req, res, code, mess) => send(req, res, code, { status: 'false', mess })

const readJson = (req) => new Promise((resolve, reject) => {
  let raw = ''
  req.on('data', (chunk) => {
    raw += chunk
    if (raw.length > BODY_LIMIT) {
      reject(new Error('Anh qua lon (toi da 25 MB moi lan gui)'))
      req.destroy()
    }
  })
  req.on('end', () => {
    try {
      resolve(raw ? JSON.parse(raw) : {})
    } catch (error) {
      reject(new Error('JSON khong hop le'))
    }
  })
  req.on('error', reject)
})

const parseId = (text) => (ObjectId.isValid(text) ? new ObjectId(text) : null)

const routes = async (req, res) => {
  const url = req.url.split('?')[0]

  if (req.method === 'OPTIONS') {
    res.writeHead(204, corsHeaders(req))
    return res.end()
  }
  // Camera (PostgreSQL embody_devices) không cần MongoDB — xử lý trước.
  if (await handleCameras(req, res, url, { ok, fail, readJson })) return undefined
  if (!faces) return fail(req, res, 503, 'face_core chua noi duoc MongoDB')

  if (url === '/api/faces' && req.method === 'GET') {
    const docs = await faces.find({}).sort({ _id: 1 }).toArray()
    return ok(req, res, docs.map(toFace))
  }

  if (url === '/api/faces' && req.method === 'POST') {
    const input = readFaceBody(await readJson(req))
    if (!input.images.length) return fail(req, res, 400, 'Can it nhat mot anh khuon mat')
    if (await faces.findOne({ resident_id: input.residentId })) {
      return fail(req, res, 409, `Ma cu dan ${input.residentId} da ton tai`)
    }

    const dir = residentDir(input.priorityFloor, input.residentId)
    input.images.forEach((image) => saveImage(dir, input.residentId, image))

    const doc = {
      name: input.name,
      resident_id: input.residentId,
      role: input.role,
      priority_floor: input.priorityFloor,
      face_link: dir,
      has_image: true,
      image_count: imagesIn(dir).length,
      created_at: new Date(),
      updated_at: new Date()
    }
    const { insertedId } = await faces.insertOne(doc)
    console.log(`[FACE] them ${input.residentId} (${input.name}) ${input.images.length} anh -> ${dir}`)
    await enroll(insertedId, doc)
    return ok(req, res, toFace(await faces.findOne({ _id: insertedId })), 201)
  }

  // /image = ảnh đầu tiên; /image/<n> = ảnh thứ n (0-based, theo tên file) — popup xem nhiều ảnh.
  const imageMatch = url.match(/^\/api\/faces\/([a-f0-9]{24})\/image(?:\/(\d+))?$/)
  if (imageMatch && (req.method === 'GET' || req.method === 'HEAD')) {
    const doc = await faces.findOne({ _id: parseId(imageMatch[1]) })
    const dir = doc?.face_link
    const name = dir && insideStorage(dir) ? imagesIn(dir)[Number(imageMatch[2] || 0)] : null
    if (!name) return fail(req, res, 404, 'Khong tim thay anh')

    const file = path.join(dir, name)
    res.writeHead(200, {
      'Content-Type': MIME[path.extname(file).toLowerCase()] || 'application/octet-stream',
      'Content-Length': fs.statSync(file).size,
      'Cache-Control': 'no-cache',
      ...corsHeaders(req)
    })
    if (req.method === 'HEAD') return res.end()
    return fs.createReadStream(file).pipe(res)
  }

  const itemMatch = url.match(/^\/api\/faces\/([a-f0-9]{24})$/)
  if (itemMatch && req.method === 'PUT') {
    const id = parseId(itemMatch[1])
    const existing = await faces.findOne({ _id: id })
    if (!existing) return fail(req, res, 404, 'Khong tim thay khuon mat')

    const input = readFaceBody(await readJson(req))
    if (input.residentId !== existing.resident_id && await faces.findOne({ resident_id: input.residentId })) {
      return fail(req, res, 409, `Ma cu dan ${input.residentId} da ton tai`)
    }

    // Đổi tầng/mã cư dân → chuyển cả thư mục ảnh sang vị trí mới (như edge_elevator).
    const dir = residentDir(input.priorityFloor, input.residentId)
    const oldDir = existing.face_link
    if (oldDir && oldDir !== dir && insideStorage(oldDir) && fs.existsSync(oldDir)) {
      fs.mkdirSync(path.dirname(dir), { recursive: true })
      if (fs.existsSync(dir)) {
        imagesIn(oldDir).forEach((file) => fs.renameSync(path.join(oldDir, file), path.join(dir, file)))
        fs.rmSync(oldDir, { recursive: true, force: true })
      } else {
        fs.renameSync(oldDir, dir)
      }
    }
    // Ảnh mới gửi kèm được THÊM vào thư mục (một cư dân nhiều ảnh), ảnh cũ giữ nguyên.
    input.images.forEach((image) => saveImage(dir, input.residentId, image))

    const fields = {
      name: input.name,
      resident_id: input.residentId,
      role: input.role,
      priority_floor: input.priorityFloor,
      face_link: dir,
      has_image: imagesIn(dir).length > 0,
      image_count: imagesIn(dir).length,
      updated_at: new Date()
    }
    await faces.updateOne({ _id: id }, { $set: fields })
    console.log(`[FACE] sua ${input.residentId} (${input.name}) them ${input.images.length} anh -> ${dir}`)

    // Khác edge_elevator: sửa cũng enroll lại (add_person --replace) để SQLite nhận đúng tầng,
    // tên, chức vụ mới — trước đây sửa không enroll nên residents.db lệch với Mongo.
    if (fields.has_image) await enroll(id, { ...existing, ...fields })
    return ok(req, res, toFace(await faces.findOne({ _id: id })))
  }

  if (itemMatch && req.method === 'DELETE') {
    const id = parseId(itemMatch[1])
    const existing = await faces.findOne({ _id: id })
    if (!existing) return fail(req, res, 404, 'Khong tim thay khuon mat')

    // Keep MongoDB and the local face_recog_app catalog consistent. Delete
    // SQLite first; if it fails, keep the Mongo document so the operation can
    // be retried safely instead of leaving a one-sided deletion.
    const localDelete = await deleteResident({ extId: existing.resident_id })
    if (!localDelete.ok) {
      console.error(`[FACE] xoa SQLite that bai ${existing.resident_id}: ${localDelete.error}`)
      return fail(req, res, 500, `Khong the xoa resident khoi SQLite: ${localDelete.error}`)
    }

    // Xoá cả thư mục ảnh của cư dân (edge_elevator định xoá nhưng gọi unlink lên thư mục nên ảnh
    // còn nằm lại). SQLite resident + embeddings đã được xóa ở bước trên.
    if (existing.face_link && insideStorage(existing.face_link)) {
      fs.rmSync(existing.face_link, { recursive: true, force: true })
    }
    await faces.deleteOne({ _id: id })
    console.log(`[FACE] xoa ${existing.resident_id} (${existing.name}) sqlite_deleted=${localDelete.deleted}`)
    return ok(req, res, { id: id.toString(), sqliteDeleted: localDelete.deleted })
  }

  return fail(req, res, 404, 'Khong co API nay')
}

const server = http.createServer((req, res) => {
  routes(req, res).catch((error) => {
    console.error('[API]', req.method, req.url, error.message)
    if (!res.headersSent) fail(req, res, 400, error.message)
  })
})

mongo.connect()
  .then(async (client) => {
    faces = client.db(MONGO_DB).collection('faces')
    await faces.createIndex({ resident_id: 1 }, { unique: true })
    console.log(`[face_core] MongoDB ${MONGO_DB}.faces`)
  })
  .catch((error) => {
    console.error('[face_core] khong noi duoc MongoDB:', error.message)
    process.exit(1)
  })

server.listen(PORT, '0.0.0.0', () => {
  console.log(`[face_core] http://0.0.0.0:${PORT} · anh: ${STORAGE_DIR} · CORS: ${CORS_ORIGINS.join(', ') || '(khong)'}`)
})
