import copy
import sqlite3
import sys
import tempfile
import unittest
from contextlib import closing
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from classroom import create_app
from classroom.camera import CameraClient, CameraError
from classroom.database import connect


class FakeCamera:
    def __init__(self):
        self.status = dict(camera_ready=True, storage_ready=True, enrollment_locked=False,
                           enrolling=False, face_count=0, samples=0, result="idle", detection="no_face",
                           enrollment_protocol=2, enrollment_token="", enrollment_result="",
                           enrolled_tokens=[""] * 7)
        self.calls = []
        self.offline = False
        self.lose_enroll_reply = False

    def request(self, base, path, method="GET", token=None, frame=False):
        self.calls.append((base, path, method, token))
        if self.offline:
            raise CameraError("Camera offline")
        if path == "/api/status":
            return copy.deepcopy(self.status)
        if path == "/api/enroll":
            self.status.update(enrolling=True, enrollment_token=token, enrollment_result="enrolling")
            if self.lose_enroll_reply:
                raise CameraError("Reply lost after camera queued enrollment")
        elif path == "/api/cancel":
            self.status.update(enrolling=False, enrollment_result="enrollment_cancelled")
        elif path == "/frame":
            return b"\xff\xd8test-frame\xff\xd9"
        return {"queued": True}

    def complete(self, token, slot=0):
        self.status["enrolled_tokens"][slot] = token
        self.status.update(face_count=slot + 1, samples=5, enrolling=False, enrollment_result="enrolled", result="enrolled")


class EnrollmentResetTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = str(Path(self.temp.name) / "test.sqlite3")
        self.settings = {"TESTING": True, "DATABASE": self.path, "MQTT_ENABLED": False,
                         "DEMO_MODE": True, "CAMERA_ID": "CAM01", "DEVICE_ID": "smartgate01"}
        self.app = create_app(self.settings)
        self.client = self.app.test_client()
        self.headers = {"X-Classroom-Client": "dashboard"}
        self.fake = FakeCamera()
        self.app.extensions["camera_client"].request = self.fake.request
        self.post("/camera", {"camera_url": "192.168.137.165"})

    def tearDown(self):
        self.temp.cleanup()

    def post(self, path, body=None):
        return self.client.post("/api" + path, json=body or {}, headers=self.headers)

    def start(self):
        result = self.post("/enrollments", {"student_code": "SV001", "full_name": "Nguyễn Văn A"})
        self.assertEqual(result.status_code, 202, result.json)
        return result.json

    def students(self):
        return self.client.get("/api/students").json

    def test_enrollment_only_links_exact_saved_token_and_is_idempotent(self):
        job = self.start()
        self.assertEqual(self.students(), [])
        # An unrelated recognition result or count increase is not proof of enrollment.
        self.fake.status.update(enrolling=False, face_count=1, last_face_id=0, result="recognized")
        self.client.get("/api/camera/status")
        self.assertEqual(self.students(), [])
        self.fake.complete(job["id"])
        result = self.client.get("/api/camera/status")
        self.assertEqual(result.json["job"]["state"], "completed")
        self.assertEqual(self.students()[0]["face_id"], 0)
        self.assertEqual(self.students()[0]["full_name"], "Nguyễn Văn A")
        self.client.get("/api/camera/status")
        self.assertEqual(len(self.students()), 1)

    def test_saved_receipt_recovers_after_lost_reply_restart_and_ip_change(self):
        self.fake.lose_enroll_reply = True
        job = self.start()
        self.assertEqual(job["state"], "uncertain")
        self.fake.complete(job["id"])
        # A reboot need not preserve the last UI result; the NVS token is sufficient.
        self.fake.status.update(result="idle", enrollment_token="", enrollment_result="", samples=0)
        app2 = create_app(self.settings)
        app2.extensions["camera_client"].request = self.fake.request
        client2 = app2.test_client()
        self.assertEqual(client2.post("/api/camera", json={"camera_url": "192.168.137.166"}, headers=self.headers).status_code, 200)
        result = client2.get("/api/camera/status")
        self.assertEqual(result.json["job"]["state"], "completed")
        self.assertEqual(len(self.students()), 1)
        self.assertEqual(sum(call[1] == "/api/enroll" for call in self.fake.calls), 1)

    def test_missing_storage_confirmation_never_adds_student(self):
        job = self.start(); self.fake.complete(job["id"])
        self.fake.status["storage_ready"] = False
        self.client.get("/api/camera/status")
        self.assertEqual(self.students(), [])

    def test_matching_an_existing_face_rejects_new_enrollment(self):
        job = self.start()
        self.fake.complete(job["id"])
        self.client.get("/api/camera/status")

        reply = self.post("/enrollments", {"student_code": "SV002", "full_name": "Sinh viên khác"})
        self.assertEqual(reply.status_code, 202, reply.json)
        self.fake.status.update(enrolling=False, enrollment_result="enrollment_duplicate",
                                enrollment_face_id=0, last_face_id=-1, samples=0)
        result = self.client.get("/api/camera/status").json["job"]
        self.assertEqual(result["state"], "failed")
        self.assertIn("Face ID 0", result["error"])
        self.assertIn("SV001 — Nguyễn Văn A", result["error"])
        self.assertEqual([row["student_code"] for row in self.students()], ["SV001"])
        self.assertEqual(self.post("/enrollments", {"student_code": "SV002", "full_name": "Sinh viên khác"}).status_code, 202)

    def test_clear_test_data_requires_ack_from_the_same_camera(self):
        job = self.start()
        self.fake.complete(job["id"])
        self.client.get("/api/camera/status")
        session = self.post("/sessions", {"name": "Buổi thử", "date": "2026-11-24"}).json
        self.assertEqual(self.post("/demo/checkin", {"face_id": 0}).status_code, 200)
        self.assertEqual(self.post("/data-reset", {"confirm": True}).status_code, 400)
        self.assertEqual(self.post("/data-reset", {"confirm": "XÓA"}).status_code, 409)
        self.assertEqual(len(self.students()), 1)

        saved_reset = {}
        original = self.fake.request
        def camera_request(base, path, method="GET", token=None, frame=False, control=None):
            if path == "/api/clear":
                saved_reset.update(control)
                raise CameraError("Reply lost on camera reboot")
            return original(base, path, method, token, frame)

        self.fake.status.update(face_reset_device="0123456789ab", face_reset_token="")
        self.app.extensions["camera_client"].request = camera_request
        self.assertEqual(self.post("/data-reset", {"confirm": "XÓA"}).status_code, 202)
        self.assertTrue(self.client.get("/api/camera").json["reset_pending"])
        self.assertEqual(self.post("/sessions", {"name": "Lượt mới", "date": "2026-11-24"}).status_code, 409)

        self.fake.status.update(face_reset_device="abcdef012345", face_reset_token=saved_reset["X-Face-Reset-Token"], face_count=0)
        self.assertTrue(self.client.get("/api/camera/status").json["reset_pending"])
        self.assertEqual(len(self.students()), 1)

        self.fake.status.update(face_reset_device=saved_reset["X-Face-Reset-Device"], enrolled_tokens=[""] * 7,
                                enrollment_result="", enrollment_token="", samples=0)
        self.assertFalse(self.client.get("/api/camera/status").json["reset_pending"])
        with connect(self.path) as db:
            for table in ("attendance", "enrollments", "events", "receipts", "sessions", "students", "web_checkins"):
                self.assertEqual(db.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0], 0, table)
        self.assertEqual(self.client.get("/api/camera").json["camera_url"], "http://192.168.137.165")

        job = self.start()
        self.fake.complete(job["id"])
        self.client.get("/api/camera/status")
        new_session = self.post("/sessions", {"name": "Lượt mới", "date": "2026-11-24"}).json
        self.assertGreater(new_session["id"], session["id"])
        service = self.app.extensions["attendance"]
        late = service.ingest({"event": "attendance_result", "event_id": "stale-button", "camera_id": "CAM01",
                               "status": "recognized", "face_id": 0, "session_id": session["id"]}, source="button")
        self.assertEqual(late["status"], "rejected")
        self.assertEqual(self.client.get("/api/attendance").json["present"], 0)

    def test_timeout_ends_enrollment_with_retry_guidance_without_creating_student(self):
        self.start()
        self.fake.status.update(enrolling=False, enrollment_result="enrollment_timeout",
                                samples=0, enrollment_locked=False)
        result = self.client.get("/api/camera/status").json["job"]
        self.assertEqual(result["state"], "failed")
        self.assertIn("hết thời gian thu mẫu", result["error"])
        self.assertNotIn("Khởi động lại", result["error"])
        self.assertEqual(self.students(), [])
        self.assertEqual(self.start()["state"], "enrolling")

    def test_profile_created_before_scanning_can_enroll_same_reserved_id(self):
        existing = self.post("/students", {"student_code":"SV001", "full_name":"Nguyễn Văn A", "camera_id":"CAM01", "face_id":0}).json
        self.assertEqual(self.post("/enrollments", {"student_code":"OTHER", "full_name":"Other"}).status_code, 409)
        job = self.start(); self.fake.complete(job["id"])
        self.assertEqual(self.client.get("/api/camera/status").json["job"]["state"], "completed")
        self.assertEqual(len(self.students()), 1)
        self.assertEqual(self.students()[0]["id"], existing["id"])

    def test_wrong_saved_slot_is_not_linked(self):
        job = self.start(); self.fake.complete(job["id"], slot=1)
        response = self.client.get("/api/camera/status")
        self.assertEqual(response.json["job"]["state"], "needs_link")
        self.assertEqual(self.students(), [])

    def test_cancel_and_offline_do_not_create_student(self):
        self.start(); self.fake.offline=True
        self.assertEqual(self.client.get("/api/camera/status").status_code, 502)
        self.assertEqual(self.app.extensions["enrollment"].latest()["state"], "uncertain")
        self.fake.offline=False
        self.assertEqual(self.post("/camera/cancel").status_code, 202)
        self.assertEqual(self.client.get("/api/camera/status").json["job"]["state"], "failed")
        self.assertEqual(self.students(), [])

    def test_old_firmware_duplicate_and_pending_mapping_are_blocked_before_scan(self):
        self.fake.status["enrollment_protocol"] = 1
        self.assertEqual(self.post("/enrollments", {"student_code":"SV001", "full_name":"A"}).status_code, 409)
        self.assertFalse(any(c[1] == "/api/enroll" for c in self.fake.calls))
        self.fake.status["enrollment_protocol"] = 2
        self.start()
        self.assertEqual(self.post("/students", {"student_code":"SV001", "full_name":"A", "camera_id":"CAM01", "face_id":0}).status_code, 409)
        self.assertEqual(self.post("/enrollments", {"student_code":"SV002", "full_name":"B"}).status_code, 409)

    def test_camera_url_rejects_loopback_public_hostnames_redirect_style_and_credentials(self):
        for value in ("http://127.0.0.1:5000", "https://192.168.1.2", "http://example.com", "http://8.8.8.8", "http://user:pass@192.168.1.2", "http://192.168.1.2/api/status", "http://169.254.169.254"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                CameraClient.normalize_url(value)
        self.assertEqual(CameraClient.normalize_url("192.168.137.165/"), "http://192.168.137.165")
        self.assertEqual(self.client.post("/api/enrollments", json={}).status_code, 403)

    def test_snapshot_uses_exact_checkin_id_and_resume_requires_local_post(self):
        calls = []
        jpeg = b"\xff\xd8captured-frame\xff\xd9"

        def camera_request(base, path, method="GET", frame=False, control=None):
            calls.append((path, method, frame, control))
            return jpeg if frame else {"ready": True}

        self.app.extensions["camera_client"].request = camera_request
        shot = self.client.get("/api/camera/checkin-frame/web-followup")
        self.assertEqual(shot.data, jpeg)
        self.assertEqual(shot.content_type, "image/jpeg")
        self.assertEqual(shot.headers["Cache-Control"], "no-store")
        self.assertEqual(self.client.get("/api/camera/checkin-frame/invalid.id").status_code, 400)
        self.assertEqual(self.client.post("/api/camera/resume-preview", json={"event_id":"web-followup"}).status_code, 403)
        self.assertEqual(self.post("/camera/resume-preview", {"event_id":"web-followup"}).status_code, 200)
        self.assertEqual(calls, [
            ("/checkin/frame", "GET", True, {"X-Checkin-Id":"web-followup"}),
            ("/api/checkin/resume-preview", "POST", False, {"X-Checkin-Id":"web-followup"}),
        ])

    def test_reset_preserves_students_other_sessions_and_receipts_and_rejects_stale_events(self):
        self.post("/students", {"student_code":"SV001", "full_name":"A", "camera_id":"CAM01", "face_id":0})
        first = self.post("/sessions", {"name":"Buổi 1", "date":"2026-11-23"}).json
        service = self.app.extensions["attendance"]
        event = {"event":"attendance_result", "event_id":"before-reset", "camera_id":"CAM01", "face_id":0, "status":"recognized", "session_id":first["id"]}
        self.assertEqual(service.ingest(event)["status"], "accepted")
        second = self.post("/sessions", {"name":"Buổi 2", "date":"2026-11-23"}).json
        service.ingest({**event, "event_id":"other-session", "session_id":second["id"]})
        self.assertEqual(self.post(f"/sessions/{first['id']}/reset").status_code, 400)
        response = self.post(f"/sessions/{first['id']}/reset", {"confirm":True})
        self.assertEqual(response.json["removed"], 1)
        self.assertEqual(len(self.students()), 1)
        self.assertEqual(self.client.get(f"/api/attendance?session_id={second['id']}").json["present"], 1)
        self.post(f"/sessions/{first['id']}/activate")
        self.assertEqual(service.ingest(event)["status"], "duplicate")
        self.assertEqual(service.ingest({**event, "event_id":"late-unseen"})["status"], "rejected")
        self.assertEqual(self.client.get("/api/attendance").json["present"], 0)
        self.assertEqual(service.ingest({**event, "event_id":"after-reset", "session_generation":1})["status"], "accepted")
        self.assertEqual(self.client.get("/api/attendance").json["present"], 1)
        self.assertFalse(any(c[2] == "POST" for c in self.fake.calls), "Reset must never command or clear camera faces")

    def test_demo_can_check_in_again_after_reset(self):
        self.post("/students", {"student_code":"SV001", "full_name":"A", "camera_id":"CAM01", "face_id":0})
        session = self.post("/sessions", {"name":"Demo", "date":"2026-11-23"}).json
        self.assertEqual(self.post("/demo/checkin", {"face_id":0}).status_code, 200)
        self.post(f"/sessions/{session['id']}/reset", {"confirm":True})
        self.assertEqual(self.post("/demo/checkin", {"face_id":0}).status_code, 200)
        self.assertEqual(self.client.get("/api/attendance").json["present"], 1)

    def test_web_checkin_requires_matching_camera_result_and_physical_deduplicates(self):
        self.post("/students", {"student_code":"SV001", "full_name":"A", "camera_id":"CAM01", "face_id":0})
        session = self.post("/sessions", {"name":"Hai nút", "date":"2026-11-24"}).json
        pending = []
        sent = []

        def camera_request(base, path, method="GET", control=None):
            if path == "/api/checkin/sync":
                if pending and control["X-Checkin-Ack"] == pending[0]["event_id"]:
                    pending.pop(0)
                return {"result": pending[0] if pending else None}
            self.assertEqual(path, "/api/checkin")
            sent.append(control["X-Checkin-Id"])
            return {"queued": True}

        self.app.extensions["camera_client"].request = camera_request
        checkins = self.app.extensions["camera_checkins"]
        camera_url = "http://192.168.137.165"
        body = {"session_id": session["id"], "generation": 0}
        job = self.post("/checkins", body)
        self.assertEqual(job.status_code, 202)
        event_id = job.json["event_id"]
        self.assertEqual(sent, [event_id])
        self.assertEqual(self.post("/checkins", body).status_code, 409)
        self.assertEqual(self.client.get("/api/attendance").json["present"], 0)
        pending.append({"event_id": event_id, "status": "no_face", "face_id": -1})
        checkins.exchange(camera_url)
        self.assertEqual(self.client.get("/api/checkins/" + event_id).json["status"], "rejected")
        self.assertEqual(self.client.get("/api/attendance").json["present"], 0)
        event_id = self.post("/checkins", body).json["event_id"]
        pending.append({"event_id": event_id, "status": "recognized", "face_id": 0})
        checkins.exchange(camera_url)
        pending.append({"event_id": "devkit-1", "status": "recognized", "face_id": 0,
                        "session_id": session["id"], "session_generation": 0})
        checkins.exchange(camera_url)
        roster = self.client.get("/api/attendance").json
        self.assertEqual(roster["present"], 1)
        self.assertEqual(roster["students"][0]["source"], "web")
        self.assertEqual(self.client.get("/api/events").json[0]["kind"], "button:duplicate")

    def test_web_result_cannot_reappear_after_reset(self):
        self.post("/students", {"student_code":"SV001", "full_name":"A", "camera_id":"CAM01", "face_id":0})
        session = self.post("/sessions", {"name":"Reset", "date":"2026-11-24"}).json
        self.app.extensions["camera_client"].request = lambda *args, **kwargs: {"queued": True}
        job = self.post("/checkins", {"session_id":session["id"], "generation":0}).json
        self.post(f"/sessions/{session['id']}/reset", {"confirm":True})
        event_id = job["event_id"]
        self.app.extensions["camera_checkins"].save_result(
            {"status":"recognized","event_id":event_id,"face_id":0}, event_id)
        self.assertEqual(self.client.get("/api/checkins/" + event_id).json["status"], "rejected")
        self.assertEqual(self.client.get("/api/attendance").json["present"], 0)

    def test_existing_database_migrates_without_losing_students_or_attendance(self):
        legacy = str(Path(self.temp.name) / "legacy.sqlite3")
        with closing(sqlite3.connect(legacy)) as db:
            db.executescript("""
                CREATE TABLE sessions(id INTEGER PRIMARY KEY,name TEXT,date TEXT,active INTEGER,created_at TEXT);
                CREATE TABLE students(id INTEGER PRIMARY KEY,student_code TEXT UNIQUE,full_name TEXT,camera_id TEXT,face_id INTEGER,created_at TEXT,UNIQUE(camera_id,face_id));
                CREATE TABLE attendance(id INTEGER PRIMARY KEY,session_id INTEGER,student_id INTEGER,checked_in_at TEXT,source TEXT,event_id TEXT UNIQUE,UNIQUE(session_id,student_id));
                INSERT INTO sessions VALUES(1,'Buổi cũ','2026-11-23',1,'2026-11-23');
                INSERT INTO students VALUES(1,'SV_OLD','Sinh viên cũ','CAM01',0,'2026-11-23');
                INSERT INTO attendance VALUES(1,1,1,'2026-11-23','mqtt','legacy-event');
            """)
            db.commit()
        app2 = create_app({**self.settings, "DATABASE":legacy})
        roster = app2.test_client().get("/api/attendance").json
        self.assertEqual(roster["present"], 1)
        self.assertEqual(roster["session"]["generation"], 0)
        self.assertEqual(roster["students"][0]["student_code"], "SV_OLD")


if __name__ == "__main__":
    unittest.main()
