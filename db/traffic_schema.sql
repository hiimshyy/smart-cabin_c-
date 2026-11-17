-- ==========================================================================
-- Smart Elevator Cabin — Elevator Traffic Log Database Schema
-- SQLite 3, WAL mode (db/traffic.db)
-- ==========================================================================

PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;

CREATE TABLE IF NOT EXISTS app_sessions (
    session_key    TEXT PRIMARY KEY,
    started_at_ms  INTEGER NOT NULL,
    cabin_id       INTEGER NOT NULL,
    elevator_slave INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS cabin_stops (
    stop_key           TEXT PRIMARY KEY,
    cabin_id           INTEGER NOT NULL,
    floor              INTEGER,
    height_cm          INTEGER,
    arrived_at_ms      INTEGER,
    door_opened_at_ms  INTEGER,
    door_closed_at_ms  INTEGER,
    departed_at_ms     INTEGER,
    boarded            INTEGER NOT NULL DEFAULT 0,
    alighted           INTEGER NOT NULL DEFAULT 0,
    raw_8471_arrive    INTEGER,
    raw_8472_arrive    INTEGER,
    raw_8480_arrive    INTEGER,
    raw_8471_depart    INTEGER,
    raw_8472_depart    INTEGER,
    raw_8480_depart    INTEGER,
    flags              INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS cabin_runs (
    run_key         TEXT PRIMARY KEY,
    cabin_id        INTEGER NOT NULL,
    from_stop_key   TEXT,
    to_stop_key     TEXT,
    from_floor      INTEGER,
    to_floor        INTEGER,
    direction       TEXT NOT NULL CHECK (direction IN ('up','down','unknown')),
    departed_at_ms  INTEGER,
    arrived_at_ms   INTEGER,
    load_count      INTEGER,
    load_confidence TEXT CHECK (load_confidence IN ('certain','uncertain')),
    car_calls_mask  INTEGER,
    flags           INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS passenger_trips (
    trip_key                TEXT PRIMARY KEY,
    cabin_id                INTEGER NOT NULL,
    entered_at_ms           INTEGER,
    exited_at_ms            INTEGER,
    floor_in                INTEGER,
    floor_out               INTEGER,
    identity_kind           TEXT NOT NULL CHECK (identity_kind IN ('resident','visitor','unidentified')),
    resident_id             INTEGER,
    visitor_id              INTEGER,
    identity_sim            REAL,
    suggested_resident_id   INTEGER,
    suggested_resident_sim  REAL,
    called_floor            INTEGER,
    call_state              TEXT,
    call_register_confirmed INTEGER CHECK (call_register_confirmed IN (0,1)),
    car_calls_mask          INTEGER,
    rides                   INTEGER NOT NULL DEFAULT 0,
    boarded_stop_key        TEXT,
    alighted_stop_key       TEXT,
    hall_call_key           TEXT,
    car_call_key            TEXT,
    confidence              TEXT NOT NULL CHECK (confidence IN ('high','low')),
    flags                   INTEGER NOT NULL DEFAULT 0,
    face_image              TEXT
);

CREATE TABLE IF NOT EXISTS car_calls (
    call_key            TEXT PRIMARY KEY,
    cabin_id            INTEGER NOT NULL,
    floor               INTEGER NOT NULL,
    registered_at_ms    INTEGER,
    registered_stop_key TEXT,
    origin_floor        INTEGER,
    cleared_at_ms       INTEGER,
    served_stop_key     TEXT,
    outcome             TEXT CHECK (outcome IN ('served','cancelled','register_gap','shutdown')),
    source              TEXT NOT NULL CHECK (source IN ('auto','button')),
    flags               INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS hall_calls (
    call_key          TEXT PRIMARY KEY,
    floor             INTEGER NOT NULL,
    direction         TEXT NOT NULL CHECK (direction IN ('up','down')),
    registered_at_ms  INTEGER,
    cleared_at_ms     INTEGER,
    served_cabin_id   INTEGER,
    served_stop_key   TEXT,
    door_opened_at_ms INTEGER,
    wait_ms           INTEGER,
    outcome           TEXT CHECK (outcome IN ('served','served_by_car1','cleared_without_stop',
                                              'register_gap','shutdown')),
    flags             INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS run_passengers (
    run_key  TEXT NOT NULL,
    trip_key TEXT NOT NULL,
    PRIMARY KEY (run_key, trip_key)
);

CREATE TABLE IF NOT EXISTS visitors (
    id                   INTEGER PRIMARY KEY AUTOINCREMENT,
    first_seen_ms        INTEGER NOT NULL,
    last_seen_ms         INTEGER NOT NULL,
    trip_count           INTEGER NOT NULL DEFAULT 0,
    status               TEXT NOT NULL DEFAULT 'active'
                         CHECK (status IN ('active','expired','merged','promoted','deleted')),
    merged_into          INTEGER,
    promoted_resident_id INTEGER
);

CREATE TABLE IF NOT EXISTS visitor_embeddings (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    visitor_id    INTEGER NOT NULL REFERENCES visitors(id) ON DELETE CASCADE,
    vector        BLOB NOT NULL,
    quality       REAL NOT NULL,
    created_at_ms INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS admin_audit (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    at_ms       INTEGER NOT NULL,
    action      TEXT NOT NULL,
    visitor_id  INTEGER,
    resident_id INTEGER,
    detail      TEXT
);

CREATE TABLE IF NOT EXISTS schema_version (
    version    INTEGER PRIMARY KEY,
    applied_at INTEGER NOT NULL
);

INSERT OR IGNORE INTO schema_version (version, applied_at) VALUES (1, 0);

CREATE INDEX IF NOT EXISTS idx_trips_entered    ON passenger_trips(entered_at_ms);
CREATE INDEX IF NOT EXISTS idx_trips_exited     ON passenger_trips(exited_at_ms);
CREATE INDEX IF NOT EXISTS idx_trips_visitor    ON passenger_trips(visitor_id);
CREATE INDEX IF NOT EXISTS idx_trips_resident   ON passenger_trips(resident_id);
CREATE INDEX IF NOT EXISTS idx_trips_od         ON passenger_trips(floor_in, floor_out);
CREATE INDEX IF NOT EXISTS idx_runs_departed    ON cabin_runs(departed_at_ms);
CREATE INDEX IF NOT EXISTS idx_stops_arrived    ON cabin_stops(arrived_at_ms);
CREATE INDEX IF NOT EXISTS idx_car_calls_reg    ON car_calls(registered_at_ms);
CREATE INDEX IF NOT EXISTS idx_car_calls_od     ON car_calls(origin_floor, floor);
CREATE INDEX IF NOT EXISTS idx_hall_calls_reg   ON hall_calls(registered_at_ms);
CREATE INDEX IF NOT EXISTS idx_hall_calls_fd    ON hall_calls(floor, direction);
CREATE INDEX IF NOT EXISTS idx_visitor_emb_vid  ON visitor_embeddings(visitor_id);
