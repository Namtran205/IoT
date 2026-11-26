import json
import sys
import tempfile
import time
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from classroom import create_app
from classroom.database import connect


class AttendanceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = str(Path(self.temp.name) / "test.sqlite3")
        self.app = create_app({"TESTING": True, "DATABASE": self.path, "MQTT_ENABLED": False, "DEMO_MODE": True, "CAMERA_ID": "CAM01", "DEVICE_ID": "smartgate01"})
        self.client = self.app.test_client()
        self.service = self.app.extensions["attendance"]
        self.headers = {"X-Classroom-Client": "dashboard"}

    def tearDown(self):
        self.temp.cleanup()

    def post(self, path, data=None):
        return self.client.post("/api" + path, json=data or {}, headers=self.headers)

    def student(self, **extra):
        return self.post("/students", {"student_code": "SV001", "full_name": "Nguyễn Văn A", "camera_id": "CAM01", "face_id": 0, **extra})

    def session(self):
        return self.post("/sessions", {"name": "IoT", "date": "2026-11-23"}).get_json()

    def event(self, **extra):
        return {"event": "attendance_result", "status": "recognized", "camera_id": "CAM01", "face_id": 0, "session_id": 1, "event_id": "test-1", **extra}

    def count(self):
        with connect(self.path) as db:
            return db.execute("SELECT COUNT(*) FROM attendance").fetchone()[0]

    def test_health_and_frontend_without_mqtt(self):
        response = self.client.get("/api/health")
        self.assertEqual(response.status_code, 200)
        self.assertFalse(response.json["device"]["online"])
        self.assertEqual(response.json["device"]["telemetry"], {})
        with self.client.get("/") as response:
            self.assertEqual(response.status_code, 200)
        with self.client.get("/assets/app.js") as response:
            self.assertEqual(response.status_code, 200)

    def test_validate_and_enforce_unique_mapping(self):
        self.assertEqual(self.student().status_code, 201)
        self.assertEqual(self.student(student_code="SV002").status_code, 409)
        self.assertEqual(self.student(face_id=True).status_code, 400)
        self.assertEqual(self.student(face_id=7).status_code, 400)

    def test_one_attendance_per_student_per_session(self):
        self.student(); self.session()
        self.assertEqual(self.service.ingest(self.event())["status"], "accepted")
        self.assertEqual(self.service.ingest(self.event(event_id="test-2"))["status"], "duplicate")
        self.assertEqual(self.count(), 1)

    def test_event_redelivery_after_new_session_does_not_check_in_again(self):
        self.student(); self.session(); self.service.ingest(self.event()); self.session()
        self.assertEqual(self.service.ingest(self.event())["status"], "duplicate")
        self.assertEqual(self.count(), 1)

    def test_unknown_face_and_closed_session_are_rejected(self):
        self.student()
        self.assertEqual(self.service.ingest(self.event())["status"], "rejected")
        self.session()
        self.assertEqual(self.service.ingest(self.event(event_id="unknown", face_id=5))["status"], "rejected")
        self.assertEqual(self.service.ingest(self.event(event_id="failed", status="unknown"))["status"], "rejected")
        self.assertEqual(self.count(), 0)

    def test_delayed_event_cannot_be_assigned_to_a_different_session(self):
        self.student(); self.session(); self.session()
        self.assertEqual(self.service.ingest(self.event())["status"], "rejected")
        self.assertEqual(self.count(), 0)

    def test_camera_failure_statuses_are_acknowledged_without_attendance(self):
        self.student(); self.session()
        for status in ("no_face", "not_aligned", "multiple_faces", "busy", "error", "unknown"):
            with self.subTest(status=status):
                event = self.event(event_id="failure-" + status, status=status, face_id=-1)
                result = self.service.ingest(event)
                self.assertEqual(result["status"], "rejected")
                self.assertIn(status, result["message"])
                self.assertEqual(self.service.ingest(event), result)
        self.assertEqual(self.count(), 0)

    def test_face_zero_is_valid_but_boolean_is_not(self):
        self.student(); self.session()
        with self.assertRaises(ValueError): self.service.ingest(self.event(face_id=False))
        with self.assertRaises(ValueError): self.service.ingest(self.event(camera_id="other"))
        with self.assertRaises(ValueError): self.service.ingest({"event": "attendance_success", "camera_data": "SUCCESS"})
        self.assertEqual(self.service.ingest(self.event())["status"], "accepted")

    def test_concurrent_deliveries_only_write_once(self):
        self.student(); self.session()
        with ThreadPoolExecutor(max_workers=5) as pool:
            results = list(pool.map(lambda i: self.service.ingest(self.event(event_id=f"parallel-{i}")), range(12)))
        self.assertEqual(sum(r["status"] == "accepted" for r in results), 1)
        self.assertEqual(self.count(), 1)

    def test_real_hardware_replaces_demo_label(self):
        self.student(); self.session()
        self.assertEqual(self.post("/demo/checkin", {"face_id": 0}).status_code, 200)
        self.assertEqual(self.client.get("/api/attendance").json["students"][0]["source"], "demo")
        self.service.ingest(self.event())
        self.assertEqual(self.client.get("/api/attendance").json["students"][0]["source"], "mqtt")
        self.assertEqual(self.count(), 1)

    def test_mqtt_retained_and_malformed_messages_cannot_mark_present(self):
        self.student(); self.session()
        bridge = self.app.extensions["mqtt"]
        published = []
        broker = SimpleNamespace(publish=lambda *a, **k: published.append((a, k)))
        payload = {"device_id": "smartgate01", **self.event()}
        msg = SimpleNamespace(topic=bridge.base + "/attendance", payload=json.dumps(payload).encode(), retain=True)
        bridge._on_message(broker, None, msg)
        self.assertEqual(self.count(), 0)
        msg.retain=False; msg.payload=b"broken json"; bridge._on_message(broker,None,msg)
        self.assertEqual(self.count(),0)
        msg.payload=json.dumps(payload).encode(); bridge._on_message(broker,None,msg)
        self.assertEqual(self.count(),1)
        args, opts = published[-1]
        self.assertEqual(args[0], bridge.base + "/attendance/ack")
        self.assertEqual(json.loads(args[1]), {"event_id": payload["event_id"], "status": "accepted"})
        self.assertEqual(opts, {"qos": 1, "retain": False})
        self.assertIn("Nguyễn Văn A", self.client.get("/api/events").json[0]["message"])

    def test_retained_online_does_not_unlock_door_controls(self):
        bridge = self.app.extensions["mqtt"]
        bridge._on_message(None,None,SimpleNamespace(topic=bridge.base+"/availability",payload=b"online",retain=True))
        self.assertEqual(self.post("/device/door",{"door":"open"}).status_code,503)
        device=self.app.extensions["device"]; device.update({"door":"closed","gas_alarm":True})
        self.assertEqual(self.post("/device/door",{"door":"close"}).status_code,409)
        device.seen=time.monotonic()-16
        self.assertFalse(device.snapshot()["online"])

    def test_invalid_mutations_and_foreign_origin(self):
        self.assertEqual(self.client.post("/api/students",json={}).status_code,403)
        self.assertEqual(self.client.post("/api/students",json={},headers={**self.headers,"Origin":"https://example.org"}).status_code,403)
        self.assertEqual(self.post("/sessions",{"name":"Bad date","date":"2026-13-01"}).status_code,400)
        self.assertEqual(self.client.get("/api/attendance?session_id=oops").status_code,400)

    def test_local_dashboard_rejects_other_hosts_and_avoids_api_cache(self):
        with patch.dict("os.environ", {"HOST": "127.0.0.1"}):
            client = create_app({"TESTING": True, "MQTT_ENABLED": False, "DATABASE": self.path}).test_client()
        for host in ("localhost:5000", "127.0.0.1:5000"):
            with self.subTest(host=host):
                response = client.get("/api/students", headers={"Host": host})
                self.assertEqual(response.status_code, 200)
                self.assertEqual(response.headers["Cache-Control"], "no-store")
        rejected = client.get("/api/students", headers={"Host": "other.example:5000"})
        self.assertEqual(rejected.status_code, 400)

    def test_export_and_database_survive_restart(self):
        self.student(full_name="=SUM(A1:A2)"); session=self.session(); self.service.ingest(self.event())
        app2=create_app({"TESTING":True,"DATABASE":self.path,"MQTT_ENABLED":False})
        self.assertEqual(app2.test_client().get("/api/attendance").json["present"],1)
        exported=self.client.get(f"/api/attendance/export.csv?session_id={session['id']}")
        self.assertEqual(exported.status_code,200)
        self.assertIn("'=SUM(A1:A2)",exported.data.decode("utf-8-sig"))


if __name__ == "__main__":
    unittest.main()

