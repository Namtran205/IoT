"""Camera transport and recoverable enrollment; no image inference on the PC."""
import ipaddress
import json
import re
import sqlite3
import time
import uuid
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import HTTPRedirectHandler, ProxyHandler, Request, build_opener

from flask import Response, jsonify, request

from .database import cleared_session_id, connect, now, pending_data_reset

PENDING = ("starting", "enrolling", "uncertain")
PRIVATE_NETWORKS = tuple(ipaddress.ip_network(n) for n in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16"))


class CameraError(Exception):
    def __init__(self, message, definitive=False):
        super().__init__(message)
        self.definitive = definitive


class CameraConflict(Exception):
    pass


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class CameraClient:
    @staticmethod
    def normalize_url(value):
        if not isinstance(value, str):
            raise ValueError("Nhập IP / địa chỉ HTTP của ESP32-CAM.")
        value = value.strip()
        if "://" not in value:
            value = "http://" + value
        try:
            parsed = urlsplit(value)
            address = ipaddress.IPv4Address(parsed.hostname or "")
            port = parsed.port or 80
        except ValueError as exc:
            raise ValueError("Camera cần IP IPv4 nội bộ, ví dụ http://192.168.137.165.") from exc
        if (parsed.scheme != "http" or parsed.username is not None or parsed.password is not None
                or parsed.path not in {"", "/"} or parsed.query or parsed.fragment
                or not 1 <= port <= 65535 or not any(address in network for network in PRIVATE_NETWORKS)):
            raise ValueError("Chỉ dùng HTTP tới IP camera trong mạng nội bộ, không kèm đường dẫn hoặc tài khoản.")
        return "http://" + str(address) + (f":{port}" if port != 80 else "")

    def request(self, base, path, method="GET", token=None, frame=False, control=None):
        base = self.normalize_url(base)
        allowed = {"/api/status", "/frame", "/checkin/frame", "/api/enroll", "/api/cancel", "/api/rotate",
                   "/api/test", "/api/restart", "/api/clear", "/api/checkin", "/api/checkin/sync", "/api/checkin/resume-preview"}
        if path not in allowed:
            raise ValueError("Lệnh camera không hợp lệ.")
        headers = {"User-Agent": "SmartClassroom/1", "Accept": "image/jpeg" if frame else "application/json"}
        if method == "POST":
            headers["X-Camera-Control"] = "local"
        if token:
            headers["X-Enrollment-Token"] = token
        if control:
            headers.update(control)
        req = Request(base + path, data=b"" if method == "POST" else None, headers=headers, method=method)
        try:
            # Never forward local camera requests through a system proxy or redirects.
            with build_opener(ProxyHandler({}), NoRedirect()).open(req, timeout=2.5) as response:
                limit = 256 * 1024 if frame else 8192
                data = response.read(limit + 1)
                if len(data) > limit:
                    raise CameraError("Phản hồi camera vượt giới hạn.")
                if frame:
                    if response.headers.get_content_type() != "image/jpeg":
                        raise CameraError("Camera chưa trả ảnh JPEG hợp lệ.")
                    return data
                result = json.loads(data)
                if not isinstance(result, dict):
                    raise ValueError("not an object")
                return result
        except HTTPError as exc:
            message = "Camera đang bận / có lệnh chờ." if exc.code == 409 else "Camera chưa sẵn sàng, thử lại sau."
            raise CameraError(f"{message} HTTP {exc.code}.", definitive=400 <= exc.code < 500) from exc
        except (URLError, TimeoutError, OSError) as exc:
            raise CameraError("Chưa kết nối được ESP32-CAM. Kiểm tra nguồn, hotspot và đúng IP camera.") from exc
        except (ValueError, UnicodeError) as exc:
            raise CameraError("Camera trả dữ liệu không hợp lệ. Kiểm tra IP và firmware.") from exc


class EnrollmentService:
    def __init__(self, app, client):
        self.app, self.client = app, client
        self.path = app.config["DATABASE"]
        self.camera_id = app.config["CAMERA_ID"]

    def url(self):
        with connect(self.path) as db:
            row = db.execute("SELECT value FROM settings WHERE key='camera_url'").fetchone()
        value = row[0] if row else self.app.config.get("CAMERA_URL", "")
        if not value:
            raise ValueError("Nhập IP camera và bấm Kết nối camera trước.")
        return self.client.normalize_url(value)

    def latest(self):
        with connect(self.path) as db:
            row = db.execute("SELECT * FROM enrollments ORDER BY rowid DESC LIMIT 1").fetchone()
        return dict(row) if row else None

    def reset_pending(self):
        with connect(self.path) as db:
            return pending_data_reset(db) is not None

    def sync_data_reset(self, status):
        with connect(self.path) as db:
            reset = pending_data_reset(db)
            if not reset:
                return False
            if (status.get("face_reset_device") != reset["device"] or status.get("face_reset_token") != reset["token"]
                    or status.get("storage_ready") is not True or type(status.get("face_count")) is not int
                    or status["face_count"] != 0 or status.get("enrolling")):
                return True
            db.execute("BEGIN IMMEDIATE")
            if pending_data_reset(db) != reset:
                return pending_data_reset(db) is not None
            last_session = db.execute("SELECT COALESCE(MAX(id), 0) FROM sessions").fetchone()[0]
            db.execute("INSERT OR REPLACE INTO settings(key,value) VALUES('cleared_session_id',?)",
                       (str(max(last_session, cleared_session_id(db))),))
            db.execute("DELETE FROM attendance")
            db.execute("DELETE FROM web_checkins")
            db.execute("DELETE FROM enrollments")
            db.execute("DELETE FROM students")
            db.execute("DELETE FROM sessions")
            db.execute("DELETE FROM receipts")
            db.execute("DELETE FROM events")
            db.execute("DELETE FROM settings WHERE key='data_reset_pending'")
        self.app.extensions["mqtt"].publish_session()
        return False

    def request_data_reset(self):
        base = self.url()
        status = self.client.request(base, "/api/status")
        device = status.get("face_reset_device")
        if not isinstance(device, str) or not re.fullmatch(r"[0-9a-f]{12}", device) or "face_reset_token" not in status:
            raise CameraConflict("Nạp SmartClassroomCam mới trước khi xóa dữ liệu thử nghiệm.")
        if status.get("enrolling") or status.get("result") == "recognizing":
            raise CameraConflict("Camera đang xử lý. Chờ camera hoàn tất rồi thử lại.")
        if self.reset_pending() and not self.sync_data_reset(status):
            return False
        with connect(self.path) as db:
            db.execute("BEGIN IMMEDIATE")
            reset = pending_data_reset(db)
            starting = reset is None
            if not starting and reset["device"] != device:
                raise CameraConflict("Kết nối lại đúng ESP32-CAM đã bắt đầu xóa dữ liệu.")
            if starting:
                reset = dict(device=device, token=uuid.uuid4().hex)
                db.execute("INSERT INTO settings(key,value) VALUES('data_reset_pending',?)", (json.dumps(reset),))
        self.app.extensions["mqtt"].publish_session()
        try:
            self.client.request(base, "/api/clear", "POST", control={
                "X-Face-Reset-Token": reset["token"], "X-Face-Reset-Device": device,
            })
        except CameraError as exc:
            if starting and exc.definitive:
                with connect(self.path) as db:
                    db.execute("DELETE FROM settings WHERE key='data_reset_pending'")
                self.app.extensions["mqtt"].publish_session()
                raise
            # The command may already be queued. Keep the same token for recovery.
        return self.reset_pending()

    def start(self, code, name):
        base = self.url()
        status = self.client.request(base, "/api/status")
        if status.get("enrollment_protocol") != 2 or not isinstance(status.get("enrolled_tokens"), list):
            raise CameraConflict("Cần Upload firmware camera mới có đăng ký liên kết sinh viên (protocol 2).")
        if not status.get("camera_ready") or not status.get("storage_ready"):
            raise CameraConflict("Camera / bộ nhớ khuôn mặt chưa sẵn sàng.")
        if status.get("enrolling") or status.get("result") == "recognizing":
            raise CameraConflict("Camera đang xử lý; đợi hoàn tất trước khi đăng ký.")
        if status.get("enrollment_locked"):
            raise CameraConflict("Lượt đăng ký trước bị dở. Khởi động lại camera trước khi thử tiếp.")
        slot = status.get("face_count")
        if type(slot) is not int or not 0 <= slot < 7:
            raise CameraConflict("Camera đã đủ 7 người hoặc trả số người không hợp lệ.")
        token = uuid.uuid4().hex
        with connect(self.path) as db:
            db.execute("BEGIN IMMEDIATE")
            if db.execute("SELECT id FROM enrollments WHERE state IN ('starting','enrolling','uncertain')").fetchone():
                raise CameraConflict("Đã có lượt đăng ký đang chờ; kiểm tra trạng thái lượt đó trước.")
            student = db.execute("SELECT * FROM students WHERE student_code=?", (code,)).fetchone()
            owner = db.execute("SELECT id FROM students WHERE camera_id=? AND face_id=?", (self.camera_id, slot)).fetchone()
            if student:
                if student["full_name"] != name or student["camera_id"] != self.camera_id or student["face_id"] != slot:
                    raise CameraConflict("Sinh viên đã có hồ sơ. Nhập đúng họ tên; Face ID đã liên kết phải trùng ô tiếp theo của camera. Nếu đã quét mặt, không cần đăng ký lại.")
            elif owner:
                raise CameraConflict("Face ID tiếp theo đã được liên kết với sinh viên khác. Kiểm tra danh sách trước khi quét.")
            db.execute("INSERT INTO enrollments(id,student_code,full_name,camera_id,camera_url,expected_face_id,state,created_at,deadline,student_id) VALUES(?,?,?,?,?,?,'starting',?,?,?)",
                       (token, code, name, self.camera_id, base, slot, now(), time.time() + 180, student["id"] if student else None))
        try:
            self.client.request(base, "/api/enroll", "POST", token=token)
            state, error = "enrolling", ""
        except CameraError as exc:
            # A lost response does not prove that the camera rejected the command.
            state, error = ("failed" if exc.definitive else "uncertain"), str(exc)
        with connect(self.path) as db:
            db.execute("UPDATE enrollments SET state=?,error=? WHERE id=? AND state='starting'", (state, error, token))
        return self.latest()

    def sync(self, status):
        with connect(self.path) as db:
            db.execute("BEGIN IMMEDIATE")
            if pending_data_reset(db):
                return
            row = db.execute("SELECT * FROM enrollments WHERE state IN ('starting','enrolling','uncertain') ORDER BY rowid DESC LIMIT 1").fetchone()
            if not row:
                return
            tokens = status.get("enrolled_tokens", [])
            if (status.get("storage_ready") and isinstance(tokens, list) and row["id"] in tokens):
                face_id = tokens.index(row["id"])
                count = status.get("face_count")
                if face_id != row["expected_face_id"] or type(count) is not int or face_id >= count:
                    db.execute("UPDATE enrollments SET state='needs_link',error=? WHERE id=?", ("Camera đã lưu nhưng Face ID không khớp lượt yêu cầu. Kiểm tra thủ công, không quét lại ngay.", row["id"]))
                    return
                if row["student_id"] is not None:
                    existing = db.execute("SELECT * FROM students WHERE id=?", (row["student_id"],)).fetchone()
                    if not existing or any(existing[key] != row[key] for key in ("student_code", "full_name", "camera_id")) or existing["face_id"] != face_id:
                        db.execute("UPDATE enrollments SET state='needs_link',error=? WHERE id=?", ("Hồ sơ sinh viên đã thay đổi. Khuôn mặt đã lưu, cần đối chiếu thủ công trước khi dùng.", row["id"]))
                        return
                    student_id = existing["id"]
                else:
                    try:
                        cursor = db.execute("INSERT INTO students(student_code,full_name,camera_id,face_id,created_at) VALUES(?,?,?,?,?)",
                                            (row["student_code"], row["full_name"], row["camera_id"], face_id, now()))
                        student_id = cursor.lastrowid
                    except sqlite3.IntegrityError:
                        db.execute("UPDATE enrollments SET state='needs_link',error=? WHERE id=?", ("Khuôn mặt đã lưu, nhưng mã sinh viên / Face ID đã có trong danh sách. Dùng liên kết ID có sẵn để xử lý, không quét lại ngay.", row["id"]))
                        return
                db.execute("UPDATE enrollments SET state='completed',student_id=?,error='' WHERE id=?", (student_id, row["id"]))
                db.execute("INSERT INTO events(kind,message,created_at) VALUES('enrollment',?,?)", (f"{row['student_code']} — {row['full_name']}: đã lưu khuôn mặt và liên kết Face ID {face_id}.", now()))
            elif status.get("enrollment_token") == row["id"] and status.get("enrollment_result") not in {None, "", "enrolling", "enrolled"}:
                outcome = status["enrollment_result"]
                messages = {
                    "enrollment_timeout": "Camera hết thời gian thu mẫu. Bật xem trước, để một người trong ảnh và căn mặt đến khi camera báo đã phát hiện khuôn mặt.",
                    "enrollment_cancelled": "Đã dừng thu mẫu.",
                    "enrollment_locked": "Camera đang khóa lượt thu mẫu sau lần đăng ký dở.",
                    "enrollment_error": "Camera gặp lỗi khi xử lý mẫu.",
                    "enrollment_duplicate": "Camera phát hiện khuôn mặt này đã được đăng ký.",
                    "memory_error": "Camera thiếu bộ nhớ để thu mẫu, thử khởi động lại camera.",
                    "storage_error": "Camera chưa lưu được khuôn mặt. Kiểm tra bộ nhớ của camera.",
                    "full": "Camera đã lưu đủ 7 người.",
                }
                message = messages.get(outcome, "Camera chưa hoàn tất lượt thu mẫu.")
                matched = status.get("enrollment_face_id")
                if outcome == "enrollment_duplicate" and type(matched) is int and 0 <= matched <= 6:
                    existing = db.execute("SELECT student_code,full_name FROM students WHERE camera_id=? AND face_id=?",
                                          (row["camera_id"], matched)).fetchone()
                    message += f" Face ID {matched}."
                    if existing:
                        message += f" Đã liên kết với {existing['student_code']} — {existing['full_name']}."
                if status.get("enrollment_locked"):
                    message += " Khởi động lại camera trước khi thử tiếp."
                db.execute("UPDATE enrollments SET state='failed',error=? WHERE id=?", (message, row["id"]))
            elif time.time() > row["deadline"] and not status.get("enrolling"):
                db.execute("UPDATE enrollments SET state='failed',error=? WHERE id=?", ("Chưa có xác nhận lưu đúng lượt đăng ký. Kiểm tra camera / Face ID trước khi đăng ký lại.", row["id"]))
            elif status.get("enrollment_token") == row["id"] and status.get("enrolling"):
                db.execute("UPDATE enrollments SET state='enrolling',error='' WHERE id=?", (row["id"],))


def register_camera_routes(app, text_field):
    client = CameraClient()
    service = EnrollmentService(app, client)
    app.extensions.update(camera_client=client, enrollment=service)

    @app.errorhandler(CameraError)
    def camera_error(exc):
        return jsonify(error=str(exc)), 502

    @app.errorhandler(CameraConflict)
    def camera_conflict(exc):
        return jsonify(error=str(exc)), 409

    @app.get("/api/camera")
    def camera_config():
        try:
            url = service.url()
        except ValueError:
            url = ""
        return jsonify(camera_url=url, camera_id=service.camera_id, job=service.latest(), reset_pending=service.reset_pending())

    @app.post("/api/data-reset")
    def reset_data():
        body = request.get_json()
        if not isinstance(body, dict) or body.get("confirm") != "XÓA":
            raise ValueError('Nhập XÓA để xác nhận xóa toàn bộ dữ liệu thử nghiệm.')
        pending = service.request_data_reset()
        return jsonify(reset_pending=pending), 202 if pending else 200

    @app.post("/api/camera")
    def save_camera():
        body = request.get_json()
        if not isinstance(body, dict):
            raise ValueError("Cần gửi một JSON object.")
        url = client.normalize_url(body.get("camera_url"))
        with connect(service.path) as db:
            db.execute("BEGIN IMMEDIATE")
            db.execute("INSERT INTO settings(key,value) VALUES('camera_url',?) ON CONFLICT(key) DO UPDATE SET value=excluded.value", (url,))
            # DHCP may change the IP during recovery. Completion still requires
            # the exact saved token, never just a face count or last Face ID.
            db.execute("UPDATE enrollments SET camera_url=? WHERE state IN ('starting','enrolling','uncertain')", (url,))
        return jsonify(camera_url=url)

    @app.get("/api/camera/status")
    def camera_status():
        try:
            status = client.request(service.url(), "/api/status")
        except CameraError as exc:
            with connect(service.path) as db:
                db.execute("UPDATE enrollments SET state='uncertain',error=? WHERE state IN ('starting','enrolling','uncertain')", (str(exc),))
            raise
        reset_pending = service.sync_data_reset(status)
        if not reset_pending:
            service.sync(status)
        return jsonify(camera=status, job=service.latest(), reset_pending=reset_pending)

    @app.get("/api/camera/frame")
    def camera_frame():
        data = client.request(service.url(), "/frame", frame=True)
        return Response(data, mimetype="image/jpeg", headers={"Cache-Control": "no-store"})

    def snapshot_header(event_id):
        if not isinstance(event_id, str) or not re.fullmatch(r"[a-zA-Z0-9_-]{1,60}", event_id):
            raise ValueError("Mã ảnh điểm danh không hợp lệ.")
        return {"X-Checkin-Id": event_id}

    @app.get("/api/camera/checkin-frame/<event_id>")
    def camera_checkin_frame(event_id):
        data = client.request(service.url(), "/checkin/frame", frame=True, control=snapshot_header(event_id))
        return Response(data, mimetype="image/jpeg", headers={"Cache-Control": "no-store"})

    @app.post("/api/camera/resume-preview")
    def camera_resume_preview():
        body = request.get_json()
        if not isinstance(body, dict):
            raise ValueError("Cần mã ảnh hiện tại để mở lại camera.")
        result = client.request(service.url(), "/api/checkin/resume-preview", "POST",
                                control=snapshot_header(body.get("event_id")))
        return jsonify(result)

    @app.post("/api/enrollments")
    def start_enrollment():
        body = request.get_json()
        if not isinstance(body, dict):
            raise ValueError("Cần gửi một JSON object.")
        job = service.start(text_field(body, "student_code", 30), text_field(body, "full_name"))
        return jsonify(job), 202

    @app.post("/api/camera/<action>")
    def camera_action(action):
        if action not in {"cancel", "rotate", "test", "restart"}:
            raise ValueError("Lệnh camera không hợp lệ.")
        result = client.request(service.url(), "/api/" + action, "POST")
        return jsonify(result), 202
