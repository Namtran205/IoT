import json
import sqlite3
from contextlib import contextmanager
from datetime import datetime, timedelta, timezone
from pathlib import Path

VIETNAM = timezone(timedelta(hours=7))


def now():
    return datetime.now(VIETNAM).isoformat(timespec="seconds")


@contextmanager
def connect(path):
    db = sqlite3.connect(path, timeout=10)
    db.row_factory = sqlite3.Row
    db.execute("PRAGMA foreign_keys=ON")
    try:
        yield db
        db.commit()
    except Exception:
        db.rollback()
        raise
    finally:
        db.close()


def pending_data_reset(db):
    row = db.execute("SELECT value FROM settings WHERE key='data_reset_pending'").fetchone()
    return json.loads(row[0]) if row else None


def cleared_session_id(db):
    row = db.execute("SELECT value FROM settings WHERE key='cleared_session_id'").fetchone()
    return int(row[0]) if row else 0


def initialize(path):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    with connect(path) as db:
        db.execute("PRAGMA journal_mode=WAL")
        db.executescript("""
        CREATE TABLE IF NOT EXISTS students(
            id INTEGER PRIMARY KEY, student_code TEXT UNIQUE NOT NULL,
            full_name TEXT NOT NULL, camera_id TEXT NOT NULL,
            face_id INTEGER NOT NULL CHECK(face_id BETWEEN 0 AND 6),
            created_at TEXT NOT NULL, UNIQUE(camera_id,face_id));
        CREATE TABLE IF NOT EXISTS sessions(
            id INTEGER PRIMARY KEY, name TEXT NOT NULL, date TEXT NOT NULL,
            active INTEGER NOT NULL DEFAULT 0, created_at TEXT NOT NULL);
        CREATE UNIQUE INDEX IF NOT EXISTS one_active_session ON sessions(active) WHERE active=1;
        CREATE TABLE IF NOT EXISTS attendance(
            id INTEGER PRIMARY KEY, session_id INTEGER NOT NULL REFERENCES sessions(id),
            student_id INTEGER NOT NULL REFERENCES students(id), checked_in_at TEXT NOT NULL,
            source TEXT NOT NULL, event_id TEXT NOT NULL UNIQUE, UNIQUE(session_id,student_id));
        CREATE TABLE IF NOT EXISTS receipts(event_id TEXT PRIMARY KEY, status TEXT NOT NULL, message TEXT NOT NULL);
        CREATE TABLE IF NOT EXISTS events(id INTEGER PRIMARY KEY,kind TEXT NOT NULL,message TEXT NOT NULL,created_at TEXT NOT NULL);
        CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY,value TEXT NOT NULL);
        CREATE TABLE IF NOT EXISTS web_checkins(
            id TEXT PRIMARY KEY, session_id INTEGER NOT NULL REFERENCES sessions(id),
            generation INTEGER NOT NULL, deadline REAL NOT NULL);
        CREATE TABLE IF NOT EXISTS enrollments(
            id TEXT PRIMARY KEY, student_code TEXT NOT NULL, full_name TEXT NOT NULL,
            camera_id TEXT NOT NULL, camera_url TEXT NOT NULL, expected_face_id INTEGER NOT NULL,
            state TEXT NOT NULL, created_at TEXT NOT NULL, deadline REAL NOT NULL,
            student_id INTEGER REFERENCES students(id), error TEXT NOT NULL DEFAULT '');
        CREATE UNIQUE INDEX IF NOT EXISTS one_pending_enrollment ON enrollments(camera_id)
            WHERE state IN ('starting','enrolling','uncertain');
        """)
        if "generation" not in {row["name"] for row in db.execute("PRAGMA table_info(sessions)")}:
            db.execute("ALTER TABLE sessions ADD COLUMN generation INTEGER NOT NULL DEFAULT 0")

