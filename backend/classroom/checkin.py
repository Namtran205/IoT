"""Receive real recognition results over the camera's local connection."""
import logging
import re
import threading
import time
import uuid

from flask import jsonify, request

from .camera import CameraConflict, CameraError
from .database import connect, pending_data_reset


class CameraCheckins:
    def __init__(self, app):
        self.path = app.config["DATABASE"]
        self.camera = app.extensions["enrollment"]
        self.attendance = app.extensions["attendance"]
        self.button_fallback = not app.config["MQTT_ENABLED"]
        self.exchange_lock = threading.Lock()
        self.stopping = threading.Event()
        self.ack = ""

    def exchange(self, base):
        with self.exchange_lock:
            with connect(self.path) as db:
                session = None if pending_data_reset(db) else db.execute("SELECT id,generation FROM sessions WHERE active=1").fetchone()
            result = self.camera.client.request(base, "/api/checkin/sync", "POST", control={
                "X-Session-Id": str(session["id"] if session else 0),
                "X-Session-Generation": str(session["generation"] if session else 0),
                "X-Checkin-Ack": self.ack,
                "X-Button-Fallback": "1" if self.button_fallback else "0",
            }).get("result")
            self.ack = ""
            if result is None:
                return
            event_id = result.get("event_id") if isinstance(result, dict) else None
            if not isinstance(event_id, str) or not re.fullmatch(r"[a-zA-Z0-9_-]{1,60}", event_id):
                raise CameraError("Camera trả mã điểm danh không hợp lệ.")
            self.save_result(result, event_id)
            # Ack only after the database committed the result (including rejection).
            self.ack = event_id

    def save_result(self, result, event_id):
        if not isinstance(result.get("status"), str):
            self.attendance.log("camera:invalid", "Camera gửi kết quả thiếu trạng thái nhận diện.")
            return
        source = "button"
        payload = dict(result, event="attendance_result", camera_id=self.camera.camera_id)
        if event_id.startswith("web-"):
            source = "web"
            with connect(self.path) as db:
                job = db.execute("SELECT * FROM web_checkins WHERE id=?", (event_id,)).fetchone()
            if job is None:
                return
            payload.update(session_id=job["session_id"], session_generation=job["generation"])
            if time.time() > job["deadline"]:
                payload["status"] = "timeout"
        try:
            self.attendance.ingest(payload, source=source)
        except ValueError as exc:
            self.attendance.log("camera:invalid", str(exc))

    def start(self):
        threading.Thread(target=self._poll, name="camera-checkins", daemon=True).start()

    def _poll(self):
        while not self.stopping.is_set():
            interval = 1.2
            try:
                self.exchange(self.camera.url())
            except (ValueError, CameraError):
                interval = 4  # Allow more time to recover when the camera is unreachable.
            except Exception:
                logging.exception("Cannot synchronize camera check-ins")
                interval = 4
            self.stopping.wait(interval)

    def submit(self, session_id, generation):
        base = self.camera.url()
        with connect(self.path) as db:
            db.execute("BEGIN IMMEDIATE")
            session = db.execute("SELECT generation FROM sessions WHERE id=? AND active=1", (session_id,)).fetchone()
            if not session or session["generation"] != generation:
                raise CameraConflict("Buổi học đã đổi hoặc vừa reset. Hãy kiểm tra buổi đang mở rồi bấm lại.")
            already_pending = db.execute("""
                SELECT 1 FROM web_checkins w WHERE w.deadline > ?
                AND NOT EXISTS(SELECT 1 FROM receipts r WHERE r.event_id=w.id)
            """, (time.time(),)).fetchone()
            if already_pending:
                raise CameraConflict("Camera đang chờ kết quả của lần bấm trước.")
            event_id = "web-" + uuid.uuid4().hex
            db.execute("INSERT INTO web_checkins VALUES(?,?,?,?)", (event_id, session_id, generation, time.time() + 45))
        try:
            self.exchange(base)
        except CameraError as exc:
            self._discard(event_id)
            if exc.definitive:
                raise CameraConflict("Nạp firmware ESP32-CAM mới để bật điểm danh qua web.")
            raise
        try:
            self.camera.client.request(base, "/api/checkin", "POST", control={"X-Checkin-Id": event_id})
        except CameraError as exc:
            if exc.definitive:
                self._discard(event_id)
                raise CameraConflict(str(exc))
            # The camera may already have received the command. Keep polling this ID.
        return event_id

    def _discard(self, event_id):
        with connect(self.path) as db:
            db.execute("DELETE FROM web_checkins WHERE id=?", (event_id,))


def register_checkin_routes(app):
    checkins = CameraCheckins(app)
    app.extensions["camera_checkins"] = checkins

    @app.post("/api/checkins")
    def start_web_checkin():
        body = request.get_json()
        if (not isinstance(body, dict) or type(body.get("session_id")) is not int
                or type(body.get("generation")) is not int):
            raise ValueError("Chọn buổi học đang mở trước khi điểm danh.")
        event_id = checkins.submit(body["session_id"], body["generation"])
        return jsonify(event_id=event_id), 202

    @app.get("/api/checkins/<event_id>")
    def web_checkin_status(event_id):
        with connect(checkins.path) as db:
            row = db.execute("""
                SELECT w.deadline, r.status, r.message FROM web_checkins w
                LEFT JOIN receipts r ON w.id=r.event_id WHERE w.id=?
            """, (event_id,)).fetchone()
        if row is None:
            return jsonify(error="Không tìm thấy lượt điểm danh."), 404
        if row["status"]:
            return jsonify(status=row["status"], message=row["message"])
        if time.time() > row["deadline"]:
            return jsonify(status="timeout", message="Camera chưa trả kết quả. Kiểm tra kết nối rồi bấm lại.")
        return jsonify(status="pending")
