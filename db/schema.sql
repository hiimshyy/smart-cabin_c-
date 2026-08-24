-- ==========================================================================
-- Smart Elevator Cabin — Face Recognition Database Schema
-- SQLite 3, WAL mode
-- ==========================================================================

PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;

-- --------------------------------------------------------------------------
-- residents: identities of building residents
-- --------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS residents (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    name            TEXT    NOT NULL,                    -- full legal name
    apartment       TEXT,                                 -- "12A05"
    home_floor      INTEGER NOT NULL,                    -- destination on match
    language        TEXT    NOT NULL DEFAULT 'vi'
                    CHECK (language IN ('vi','en')),
    greeting_name   TEXT,                                 -- "bác Nga" / "Mr. Smith"
    role            TEXT    NOT NULL DEFAULT 'resident'
                    CHECK (role IN ('resident','staff','vip','guest_regular')),
    active          INTEGER NOT NULL DEFAULT 1,           -- 0 = soft-deleted
    consent_at      TIMESTAMP,                            -- privacy consent time
    created_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_seen_at    TIMESTAMP,                            -- last successful match
    match_count     INTEGER NOT NULL DEFAULT 0,           -- total successful matches
    notes           TEXT
);

CREATE INDEX IF NOT EXISTS idx_residents_active     ON residents(active);
CREATE INDEX IF NOT EXISTS idx_residents_apartment  ON residents(apartment);

-- --------------------------------------------------------------------------
-- embeddings: multiple embedding vectors per resident (self-supervised)
--   source = 'id_photo'   → initial from 3x4 ID card (may age poorly)
--   source = 'cabin'      → captured live from cabin (high quality)
--   source = 'admin'      → uploaded by HR/admin
-- --------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS embeddings (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    resident_id   INTEGER NOT NULL,
    source        TEXT    NOT NULL DEFAULT 'cabin'
                  CHECK (source IN ('id_photo','cabin','admin')),
    dim           INTEGER NOT NULL DEFAULT 512,
    vector        BLOB    NOT NULL,                       -- dim * sizeof(float), L2-normalized
    quality_score REAL,                                    -- 0..1, face size / blur / pose
    cabin_id      INTEGER,                                 -- NULL if not from cabin
    created_at    TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (resident_id) REFERENCES residents(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_embeddings_resident ON embeddings(resident_id);
CREATE INDEX IF NOT EXISTS idx_embeddings_source   ON embeddings(source);

-- --------------------------------------------------------------------------
-- cabins: physical elevator cabin config
-- --------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS cabins (
    id                 INTEGER PRIMARY KEY AUTOINCREMENT,
    name               TEXT    NOT NULL,                  -- 'Block A - Cabin 1'
    location           TEXT,                               -- 'Tower A, ground floor'
    camera_urls        TEXT    NOT NULL,                  -- JSON array ["rtsp://...", ...]
    elevator_endpoint  TEXT,                               -- 'gpio:...' or 'modbus://...'
    floors_min         INTEGER NOT NULL DEFAULT 1,
    floors_max         INTEGER NOT NULL DEFAULT 30,
    created_at         TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
);

-- --------------------------------------------------------------------------
-- match_events: audit log — every recognition attempt (matched or not)
--   retention: 30 days by default (config), except VIP/security → 90 days
-- --------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS match_events (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    ts              TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    cabin_id        INTEGER NOT NULL,
    resident_id     INTEGER,                              -- NULL if unknown
    similarity      REAL,                                  -- top-1 cosine score
    liveness_score  REAL,                                  -- 0..1
    action          TEXT NOT NULL
                    CHECK (action IN ('matched','unknown','cancelled',
                                      'floor_changed','override','error')),
    floor_selected  INTEGER,                               -- 0 = none/cancelled
    latency_ms      INTEGER,                               -- E2E from detect → action
    snapshot        BLOB,                                   -- optional 112x112 face crop
    metadata        TEXT,                                   -- JSON: {angle, brightness, ...}
    FOREIGN KEY (cabin_id)    REFERENCES cabins(id),
    FOREIGN KEY (resident_id) REFERENCES residents(id) ON DELETE SET NULL
);

CREATE INDEX IF NOT EXISTS idx_events_ts       ON match_events(ts);
CREATE INDEX IF NOT EXISTS idx_events_cabin    ON match_events(cabin_id, ts);
CREATE INDEX IF NOT EXISTS idx_events_resident ON match_events(resident_id, ts);
CREATE INDEX IF NOT EXISTS idx_events_action   ON match_events(action);

-- --------------------------------------------------------------------------
-- schema_version: migration tracking
-- --------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS schema_version (
    version INTEGER PRIMARY KEY,
    applied_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
);

INSERT OR IGNORE INTO schema_version (version) VALUES (1);

-- --------------------------------------------------------------------------
-- Default cabin (for single-cabin dev/test)
-- --------------------------------------------------------------------------
INSERT OR IGNORE INTO cabins (id, name, location, camera_urls, floors_min, floors_max)
VALUES (1, 'Dev cabin', 'test building', '["rtsp://127.0.0.1/dev"]', 1, 30);
