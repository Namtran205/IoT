"""Temporary UI QA server. Uses a fresh test DB and a fake camera, never hardware."""
import base64
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "backend"))
sys.path.insert(0, str(ROOT / "backend" / "tests"))
from flask import jsonify, request
from classroom import create_app
from test_enrollment_reset import FakeCamera

temp_root = (ROOT / "artifacts" / "tmp").resolve()
temp_root.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="enrollment-ui-", dir=temp_root) as folder:
    assert Path(folder).resolve().is_relative_to(temp_root)
    app = create_app({"DATABASE": str(Path(folder) / "test.sqlite3"), "MQTT_ENABLED": False, "DEMO_MODE": True})
    fake = FakeCamera()
    fake.status.update(face_reset_device="0123456789ab", face_reset_token="")
    original_request = fake.request
    frame = b""
    frames = 0
    enrolled_at = None
    checkin_results = []
    session_headers = {}
    shot = {}
    button_link = {"online": False, "last_id": "", "last_status": ""}
    submitted_checkins = []
    liveness = None

    def capture_checkin(result, delay):
        shot.clear()
        if liveness:
            delay = 6
            shot.update(id=result["event_id"], captured=False, ready=False, status="challenge", liveness_step="center")
            if liveness == "reject":
                result.update(status="liveness_failed", face_id=-1)
        else:
            shot.update(id=result["event_id"], captured=True, ready=True, status="recognizing")
        submitted_checkins.append((time.monotonic()+delay, result))

    def camera_request(base, path, method="GET", token=None, frame=False, control=None):
        global frames, enrolled_at
        if path == "/frame" and not fake.offline:
            frames += 1
            return globals()["frame"]
        if path == "/api/status":
            if shot.get("status") == "challenge" and submitted_checkins:
                remaining = submitted_checkins[0][0] - time.monotonic()
                shot["liveness_step"] = "center" if remaining > 4 else "turn_right" if remaining > 2 else "return"
            fake.status["checkin_snapshot"] = dict(shot)
            fake.status["button_link"] = dict(button_link)
            fake.status.update(checkin_protocol=1, uptime_ms=int(time.monotonic()*1000)%10000000, reset_reason="power_on", wifi_rssi=-48,
                               wifi_disconnects=0, wifi_disconnect_reason=0)
            if fake.status["enrolling"] and enrolled_at and not fake.offline:
                samples = min(5, int((time.monotonic()-enrolled_at)/1.3))
                fake.status["samples"] = samples
                fake.status["detection"] = "ok"
                if samples == 5:
                    fake.complete(fake.status["enrollment_token"], slot=fake.status["face_count"])
        if not fake.offline and path == "/api/checkin/sync":
            while submitted_checkins and time.monotonic() >= submitted_checkins[0][0]:
                result = submitted_checkins.pop(0)[1]
                if shot.get("id") == result["event_id"]:
                    if shot["status"] == "challenge":
                        shot["captured"] = shot["ready"] = result["status"] == "recognized"
                    shot["status"] = result["status"]
                checkin_results.append(result)
            session_headers.update(control)
            if checkin_results and checkin_results[0]["event_id"] == control["X-Checkin-Ack"]:
                checkin_results.pop(0)
            return {"result":checkin_results[0] if checkin_results else None}
        if not fake.offline and path == "/api/checkin":
            capture_checkin(dict(event_id=control["X-Checkin-Id"], status="recognized", face_id=0), 3.5)
            return {"queued":True}
        if not fake.offline and path == "/api/clear":
            assert control["X-Face-Reset-Device"] == fake.status["face_reset_device"]
            face_reset_token = control["X-Face-Reset-Token"]
            fake.status.update(face_reset_token=face_reset_token, face_count=0, samples=0, enrolling=False,
                               enrollment_token="", enrollment_result="", enrolled_tokens=[""] * 7)
            shot.clear()
            checkin_results.clear()
            submitted_checkins.clear()
            return {"queued": True}
        if not fake.offline and path in {"/checkin/frame", "/api/checkin/resume-preview"}:
            if control["X-Checkin-Id"] != shot.get("id"):
                raise ValueError("Snapshot does not match")
            if path == "/checkin/frame":
                return globals()["frame"]
            shot.clear()
            return {"ready":True}
        result = original_request(base, path, method, token, frame)
        if path == "/api/enroll":
            enrolled_at=time.monotonic();fake.status["samples"]=0
        return result

    app.extensions["camera_client"].request = camera_request
    checkins = app.extensions["camera_checkins"]
    checkins.start()

    @app.post("/__qa/frame")
    def set_frame():
        global frame
        frame=base64.b64decode(request.get_data())
        return jsonify(ok=True)

    @app.post("/__qa/state")
    def set_state():
        global liveness
        body=request.get_json()
        if "liveness" in body:
            liveness = body["liveness"]
        if "offline" in body:
            fake.offline=body["offline"]
        if "button_online" in body:
            button_link["online"] = body["button_online"]
        if "direct_button_gpio" in body:
            fake.status["direct_button_gpio"] = body["direct_button_gpio"]
        if "frame_light" in body:
            fake.status["frame_light"] = body["frame_light"]
        if body.get("capture_failure"):
            shot.update(id="fake-capture-failure", captured=False, ready=False, status="error")
        if body.get("physical") or body.get("direct_button"):
            base = app.extensions["enrollment"].url()
            checkins.exchange(base)
            assert session_headers["X-Button-Fallback"] == "1"
            if body.get("direct_button"):
                event_id = "cam-button-fixture-1"
                button_link.update(online=False, last_id="", last_status="")
                fake.status["direct_button_gpio"] = 13
            else:
                event_id = "fake-devkit-liveness" if liveness else "fake-devkit-button"
                button_link.update(online=True, last_id=event_id, last_status="recognized")
            capture_checkin(dict(event_id=event_id, status="recognized", face_id=0,
                                 session_id=int(session_headers["X-Session-Id"]),
                                 session_generation=int(session_headers["X-Session-Generation"])), 1.5)
        return jsonify(frames=frames, face_count=fake.status["face_count"])

    app.run(host="127.0.0.1", port=5001, use_reloader=False)
