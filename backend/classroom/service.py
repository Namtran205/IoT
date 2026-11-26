import math
import re
import threading
import time
from ipaddress import IPv4Address

from .database import cleared_session_id, connect, now, pending_data_reset


class DeviceState:
    def __init__(self):
        self.lock = threading.Lock()
        self.seen = 0
        self.updated_at = None
        self.telemetry = {}
        self.available = None

    def update(self, payload):
        if not isinstance(payload, dict):
            return
        cleaned = {}
        for key in ("lux", "mq2_raw", "mq2_percent", "led_percent", "uptime_ms"):
            value = payload.get(key)
            if type(value) in {int, float} and math.isfinite(value):
                cleaned[key] = value
        for key in ("gas_alarm", "gas_sensor_ready", "moving"):
            if type(payload.get(key)) is bool:
                cleaned[key] = payload[key]
        if payload.get("door") in {"open", "closed"}:
            cleaned["door"] = payload["door"]
        station_ip = payload.get("station_ip")
        if isinstance(station_ip, str):
            try:
                address = IPv4Address(station_ip)
            except ValueError:
                pass
            else:
                if not address.is_unspecified:
                    cleaned["station_ip"] = str(address)
        with self.lock:
            self.telemetry.update(cleaned)
            self.seen = time.monotonic()
            self.updated_at = now()
            self.available = True

    def snapshot(self):
        with self.lock:
            age = time.monotonic() - self.seen if self.seen else None
            return {"online": bool(self.available and age is not None and age < 15),
                    "updated_at": self.updated_at, "age_seconds": round(age, 1) if age is not None else None,
                    "telemetry": dict(self.telemetry)}


class AttendanceService:
    def __init__(self, database, camera_id):
        self.database = database
        self.camera_id = camera_id

    def log(self, kind, message):
        with connect(self.database) as db:
            db.execute("INSERT INTO events(kind,message,created_at) VALUES(?,?,?)", (kind, message[:300], now()))

    def ingest(self, payload, source="mqtt"):
        if not isinstance(payload, dict):
            raise ValueError("Bản tin phải là JSON object.")
        if payload.get("event") not in {"attendance_result", "attendance_success"}:
            raise ValueError("Loại sự kiện không hợp lệ.")
        event_id = payload.get("event_id")
        if not isinstance(event_id, str) or not re.fullmatch(r"[a-zA-Z0-9_.:-]{1,120}", event_id):
            raise ValueError("Bản tin thiếu event_id hợp lệ; firmware cần gửi định danh sự kiện.")
        camera = payload.get("camera_id")
        if camera != self.camera_id:
            raise ValueError("Camera không khớp CAMERA_ID cấu hình.")
        face_id = payload.get("face_id")
        status = payload.get("status", "recognized")
        session_id = payload.get("session_id")
        generation = payload.get("session_generation", 0)
        if type(generation) is not int or generation < 0:
            raise ValueError("session_generation phải là số nguyên không âm.")
        if type(session_id) is not int or session_id < 0:
            raise ValueError("Bản tin cần session_id số nguyên; cập nhật firmware và nhận buổi học từ backend.")
        if status not in {"recognized", "unknown", "no_face", "not_aligned", "busy", "error", "timeout", "not_enrolled", "multiple_faces", "liveness_failed"}:
            raise ValueError("Trạng thái nhận diện không hợp lệ.")
        if status == "recognized" and (type(face_id) is not int or not 0 <= face_id <= 6):
            raise ValueError("Bản tin thành công cần Face ID từ 0 đến 6.")
        with connect(self.database) as db:
            db.execute("BEGIN IMMEDIATE")
            if pending_data_reset(db) or 0 < session_id <= cleared_session_id(db):
                return {"event_id": event_id, "status": "rejected", "message": "Dữ liệu điểm danh của lượt này đã được reset."}
            receipt = db.execute("SELECT * FROM receipts WHERE event_id=?", (event_id,)).fetchone()
            if receipt:
                return {"event_id": event_id, "status": "duplicate" if receipt["status"] == "accepted" else receipt["status"], "message": receipt["message"]}
            result = "rejected"
            if status != "recognized":
                reason = {
                    "no_face": "Camera chưa phát hiện khuôn mặt trong ảnh đã chụp.",
                    "not_aligned": "Camera đã thấy mặt. Nhìn thẳng và giữ cả mặt trong ảnh rồi thử lại.",
                    "multiple_faces": "Ảnh có nhiều khuôn mặt. Chỉ đứng một người trước camera.",
                    "unknown": "Camera đã thấy mặt nhưng chưa khớp với khuôn mặt đã đăng ký.",
                    "liveness_failed": "Chưa vượt qua xác minh người thật. Nhìn thẳng và làm theo hướng dẫn xoay mặt trên web.",
                    "not_enrolled": "Camera chưa có khuôn mặt đăng ký.",
                    "busy": "Camera đang xử lý lượt khác.",
                    "error": "Camera gặp lỗi khi chụp hoặc xử lý ảnh.",
                    "timeout": "Lượt nhận diện đã quá thời gian chờ.",
                }
                message = f"{reason[status]} ({status}) Chưa ghi điểm danh."
            else:
                student = db.execute("SELECT * FROM students WHERE camera_id=? AND face_id=?", (camera, face_id)).fetchone()
                session = db.execute("SELECT id,generation FROM sessions WHERE id=? AND active=1", (session_id,)).fetchone()
                if student is None:
                    message = f"Chưa liên kết {camera} / Face ID {face_id} với sinh viên."
                elif session is None:
                    message = "Buổi học của bản tin chưa mở hoặc đã kết thúc. Không ghi điểm danh."
                elif generation != session["generation"]:
                    message = "Bản tin thuộc lượt trước khi reset. Đợi thiết bị nhận lượt mới rồi nhấn nút lại."
                else:
                    existing = db.execute("SELECT id,source FROM attendance WHERE session_id=? AND student_id=?", (session["id"], student["id"])).fetchone()
                    if existing:
                        result = "duplicate"
                        message = f"{student['student_code']} đã điểm danh trong buổi này."
                        # A real hardware check-in supersedes a previous software demo.
                        if existing["source"] == "demo" and source != "demo":
                            db.execute("UPDATE attendance SET source=?,event_id=?,checked_in_at=? WHERE id=?", (source, event_id, now(), existing["id"]))
                            result = "accepted"
                            message = f"{student['student_code']} — đã xác nhận bằng thiết bị."
                    else:
                        db.execute("INSERT INTO attendance(session_id,student_id,checked_in_at,source,event_id) VALUES(?,?,?,?,?)", (session["id"], student["id"], now(), source, event_id))
                        result = "accepted"
                        message = f"{student['student_code']} — {student['full_name']} đã điểm danh."
            db.execute("INSERT INTO receipts VALUES(?,?,?)", (event_id, result, message))
            db.execute("INSERT INTO events(kind,message,created_at) VALUES(?,?,?)", (source + ":" + result, message, now()))
        return {"event_id": event_id, "status": result, "message": message}

