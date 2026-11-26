"""Local attendance API. Image inference belongs to the ESP32-CAM."""
import csv
import io
import json
import os
import sqlite3
import threading
import uuid
from datetime import date
from pathlib import Path

from flask import Flask, Response, g, jsonify, request, send_from_directory
from werkzeug.exceptions import HTTPException

from .database import cleared_session_id, connect, initialize, now, pending_data_reset
from .service import AttendanceService, DeviceState
from .mqtt import MQTTBridge
from .camera import register_camera_routes
from .checkin import register_checkin_routes

ROOT = Path(__file__).resolve().parents[2]


def load_env(path):
    if path.exists():
        for line in path.read_text(encoding="utf-8-sig").splitlines():
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                key, value = line.split("=", 1)
                os.environ.setdefault(key.strip(), value.strip().strip("\"'"))


def text_field(body, key, maximum=100):
    value = body.get(key)
    if not isinstance(value, str) or not value.strip() or len(value.strip()) > maximum:
        raise ValueError(f"{key}: cần nhập từ 1 đến {maximum} ký tự.")
    return value.strip()


def face_number(value):
    if type(value) is not int or not 0 <= value <= 6:
        raise ValueError("Face ID phải là số nguyên từ 0 đến 6.")
    return value


def create_app(overrides=None):
    load_env(ROOT / "backend" / ".env")
    app = Flask(__name__, static_folder=None)
    app.json.ensure_ascii = False
    db_path = Path(os.getenv("DATABASE_PATH", "instance/classroom.sqlite3"))
    if not db_path.is_absolute():
        db_path = ROOT / "backend" / db_path
    app.config.update(
        DATABASE=str(db_path), MAX_CONTENT_LENGTH=16 * 1024,
        DEMO_MODE=os.getenv("DEMO_MODE", "true").lower() == "true",
        MQTT_ENABLED=os.getenv("MQTT_ENABLED", "false").lower() == "true",
        DEVICE_ID=os.getenv("DEVICE_ID", "smartgate01"),
        CAMERA_ID=os.getenv("CAMERA_ID", "CAM01"),
        CAMERA_URL=os.getenv("CAMERA_URL", ""),
    )
    if os.getenv("HOST", "127.0.0.1") in {"127.0.0.1", "localhost"}:
        app.config["TRUSTED_HOSTS"] = ["127.0.0.1", "localhost"]
    if overrides:
        app.config.update(overrides)
    initialize(app.config["DATABASE"])
    device = DeviceState()
    service = AttendanceService(app.config["DATABASE"], app.config["CAMERA_ID"])
    bridge = MQTTBridge(app, service, device)
    app.extensions.update(attendance=service, mqtt=bridge, device=device)
    data_write_lock = threading.Lock()

    @app.before_request
    def check_local_writes():
        if request.path.startswith("/api/") and request.method in {"POST", "PUT", "DELETE", "PATCH"}:
            # Browsers cannot add this header cross-origin without a CORS preflight.
            # There are deliberately no permissive CORS headers on this local API.
            if request.headers.get("X-Classroom-Client") != "dashboard":
                return jsonify(error="Thiếu header X-Classroom-Client: dashboard."), 403
            origin = request.headers.get("Origin")
            if origin and origin.rstrip("/") != request.host_url.rstrip("/"):
                return jsonify(error="Nguồn yêu cầu không hợp lệ."), 403
            if request.path not in {"/api/device/door", "/api/camera/resume-preview"}:
                data_write_lock.acquire()
                g.classroom_write_locked = True
                if request.path not in {"/api/camera", "/api/data-reset"}:
                    with connect(app.config["DATABASE"]) as db:
                        if pending_data_reset(db):
                            return jsonify(error="Đang chờ camera xác nhận xóa dữ liệu thử nghiệm."), 409

    @app.teardown_request
    def release_data_write_lock(_exc):
        if g.pop("classroom_write_locked", False):
            data_write_lock.release()

    @app.after_request
    def disable_api_cache(response):
        if request.path.startswith("/api/"):
            response.headers["Cache-Control"] = "no-store"
        return response

    @app.errorhandler(ValueError)
    def bad_input(exc):
        return jsonify(error=str(exc)), 400

    @app.errorhandler(sqlite3.IntegrityError)
    def conflict(_exc):
        return jsonify(error="Mã sinh viên hoặc cặp Camera / Face ID đã tồn tại."), 409

    @app.errorhandler(HTTPException)
    def http_error(exc):
        if request.path.startswith("/api/"):
            return jsonify(error=exc.description), exc.code
        return exc

    @app.get("/")
    def index():
        return send_from_directory(ROOT / "frontend", "index.html")

    @app.get("/assets/<path:name>")
    def assets(name):
        return send_from_directory(ROOT / "frontend" / "assets", name)

    @app.get("/api/health")
    def health():
        return jsonify(status="ok", time=now(), demo_mode=app.config["DEMO_MODE"],
                       camera_id=app.config["CAMERA_ID"], mqtt=bridge.status(), device=device.snapshot())

    @app.get("/api/students")
    def students():
        with connect(app.config["DATABASE"]) as db:
            rows = db.execute("SELECT * FROM students ORDER BY student_code").fetchall()
        return jsonify([dict(row) for row in rows])

    @app.post("/api/students")
    def add_student():
        body = request.get_json()
        if not isinstance(body, dict):
            raise ValueError("Cần gửi một JSON object.")
        values = (text_field(body, "student_code", 30), text_field(body, "full_name"),
                  text_field(body, "camera_id", 40), face_number(body.get("face_id")), now())
        with connect(app.config["DATABASE"]) as db:
            db.execute("BEGIN IMMEDIATE")
            if db.execute("SELECT id FROM enrollments WHERE state IN ('starting','enrolling','uncertain') AND (student_code=? OR (camera_id=? AND expected_face_id=?))", values[:1] + values[2:4]).fetchone():
                return jsonify(error="Sinh viên / Face ID đang được dùng trong lượt đăng ký khuôn mặt."), 409
            cursor = db.execute("INSERT INTO students(student_code,full_name,camera_id,face_id,created_at) VALUES(?,?,?,?,?)", values)
            row = db.execute("SELECT * FROM students WHERE id=?", (cursor.lastrowid,)).fetchone()
        return jsonify(dict(row)), 201

    @app.get("/api/sessions")
    def sessions():
        with connect(app.config["DATABASE"]) as db:
            rows = db.execute("SELECT * FROM sessions ORDER BY id DESC").fetchall()
        return jsonify([dict(row) for row in rows])

    @app.post("/api/sessions")
    def add_session():
        body = request.get_json()
        if not isinstance(body, dict):
            raise ValueError("Cần gửi một JSON object.")
        name = text_field(body, "name")
        day = text_field(body, "date", 10)
        if date.fromisoformat(day).isoformat() != day:
            raise ValueError("Ngày phải có dạng YYYY-MM-DD.")
        with connect(app.config["DATABASE"]) as db:
            db.execute("BEGIN IMMEDIATE")
            db.execute("UPDATE sessions SET active=0 WHERE active=1")
            next_id = db.execute("SELECT MAX(COALESCE(MAX(id), 0), ?) + 1 FROM sessions",
                                 (cleared_session_id(db),)).fetchone()[0]
            cursor = db.execute("INSERT INTO sessions(id,name,date,active,created_at) VALUES(?,?,?,1,?)",
                                (next_id, name, day, now()))
            row = db.execute("SELECT * FROM sessions WHERE id=?", (cursor.lastrowid,)).fetchone()
        bridge.publish_session()
        return jsonify(dict(row)), 201

    @app.post("/api/sessions/<int:session_id>/activate")
    def activate_session(session_id):
        with connect(app.config["DATABASE"]) as db:
            db.execute("BEGIN IMMEDIATE")
            if not db.execute("SELECT id FROM sessions WHERE id=?", (session_id,)).fetchone():
                return jsonify(error="Không tìm thấy buổi học."), 404
            db.execute("UPDATE sessions SET active=0 WHERE active=1")
            db.execute("UPDATE sessions SET active=1 WHERE id=?", (session_id,))
        bridge.publish_session()
        return jsonify(ok=True)

    @app.post("/api/sessions/close")
    def close_session():
        with connect(app.config["DATABASE"]) as db:
            db.execute("UPDATE sessions SET active=0 WHERE active=1")
        bridge.publish_session()
        return jsonify(ok=True)

    @app.post("/api/sessions/<int:session_id>/reset")
    def reset_attendance(session_id):
        body = request.get_json()
        if not isinstance(body, dict) or body.get("confirm") is not True:
            raise ValueError("Cần xác nhận reset kết quả của buổi học.")
        with connect(app.config["DATABASE"]) as db:
            db.execute("BEGIN IMMEDIATE")
            session = db.execute("SELECT * FROM sessions WHERE id=?", (session_id,)).fetchone()
            if not session:
                return jsonify(error="Không tìm thấy buổi học."), 404
            cursor = db.execute("DELETE FROM attendance WHERE session_id=?", (session_id,))
            removed = cursor.rowcount
            db.execute("UPDATE sessions SET generation=generation+1 WHERE id=?", (session_id,))
            # Keep delivery receipts: replaying an old event must not restore a deleted check-in.
            db.execute("INSERT INTO events(kind,message,created_at) VALUES('attendance:reset',?,?)", (f"Reset buổi {session['name']}: đã xóa {removed} kết quả; giữ sinh viên và khuôn mặt.", now()))
        bridge.publish_session()
        return jsonify(ok=True, removed=removed, session_id=session_id,
                       message="Đã reset kết quả buổi học. Đợi thiết bị nhận lượt mới trước khi điểm danh lại.")

    def attendance_rows():
        requested = request.args.get("session_id")
        with connect(app.config["DATABASE"]) as db:
            if requested is not None:
                if not requested.isdigit():
                    raise ValueError("session_id không hợp lệ.")
                session = db.execute("SELECT * FROM sessions WHERE id=?", (int(requested),)).fetchone()
            else:
                session = db.execute("SELECT * FROM sessions WHERE active=1").fetchone()
            if session is None:
                return None, []
            rows = db.execute("""SELECT s.id,s.student_code,s.full_name,s.camera_id,s.face_id,
                       a.checked_in_at,a.source,CASE WHEN a.id IS NULL THEN 'absent' ELSE 'present' END AS status
                       FROM students s LEFT JOIN attendance a ON a.student_id=s.id AND a.session_id=?
                       ORDER BY s.student_code""", (session["id"],)).fetchall()
        return dict(session), [dict(row) for row in rows]

    @app.get("/api/attendance")
    def attendance():
        session, rows = attendance_rows()
        return jsonify(session=session, students=rows, total=len(rows),
                       present=sum(row["status"] == "present" for row in rows))

    @app.get("/api/attendance/export.csv")
    def export():
        session, rows = attendance_rows()
        if session is None:
            return jsonify(error="Chọn một buổi học để xuất CSV."), 400
        output = io.StringIO()
        writer = csv.writer(output)
        writer.writerow(["Mã sinh viên", "Họ tên", "Camera", "Face ID", "Trạng thái", "Thời gian", "Nguồn"])
        for row in rows:
            values = [row[k] or "" for k in ("student_code", "full_name", "camera_id", "face_id", "status", "checked_in_at", "source")]
            values[3] = row["face_id"]
            # Protect spreadsheet formula execution in user-entered cells.
            writer.writerow(["'" + v if isinstance(v, str) and v.startswith(("=", "+", "-", "@")) else v for v in values])
        return Response("\ufeff" + output.getvalue(), mimetype="text/csv; charset=utf-8",
                        headers={"Content-Disposition": f'attachment; filename="attendance-{session["id"]}.csv"'})

    @app.get("/api/events")
    def events():
        with connect(app.config["DATABASE"]) as db:
            rows = db.execute("SELECT id,kind,message,created_at FROM events ORDER BY id DESC LIMIT 30").fetchall()
        return jsonify([dict(row) for row in rows])

    @app.post("/api/demo/checkin")
    def demo():
        if not app.config["DEMO_MODE"]:
            return jsonify(error="Chế độ thử đã tắt."), 403
        body = request.get_json()
        if not isinstance(body, dict):
            raise ValueError("Cần gửi một JSON object.")
        with connect(app.config["DATABASE"]) as db:
            active = db.execute("SELECT id,generation FROM sessions WHERE active=1").fetchone()
        result = service.ingest({"event": "attendance_result", "status": "recognized",
                                 "camera_id": app.config["CAMERA_ID"], "face_id": face_number(body.get("face_id")),
                                 "session_id": active["id"] if active else 0,
                                 "session_generation": active["generation"] if active else 0,
                                 "event_id": "demo-" + uuid.uuid4().hex}, source="demo")
        return jsonify(result), 200 if result["status"] in {"accepted", "duplicate"} else 409

    @app.post("/api/device/door")
    def door():
        body = request.get_json()
        if not isinstance(body, dict) or body.get("door") not in {"open", "close"}:
            raise ValueError("door chỉ nhận open hoặc close.")
        snapshot = device.snapshot()
        if not snapshot["online"]:
            return jsonify(error="ESP32 chưa có dữ liệu trực tuyến mới. Kiểm tra nguồn và MQTT."), 503
        if body["door"] == "close" and snapshot["telemetry"].get("gas_alarm"):
            return jsonify(error="Đang cảnh báo gas; chưa thể đóng cửa."), 409
        if not bridge.command({"door": body["door"]}):
            return jsonify(error="Chưa gửi được lệnh MQTT."), 503
        return jsonify(message="Đã gửi lệnh. Chờ trạng thái phản hồi từ ESP32."), 202

    register_camera_routes(app, text_field)
    register_checkin_routes(app)
    if app.config["MQTT_ENABLED"]:
        bridge.start()
    return app

