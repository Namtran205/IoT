import atexit
import json
import logging
import os
from pathlib import Path

from classroom import create_app

app = create_app()
atexit.register(app.extensions["mqtt"].stop)


def sync_camera_wifi():
    ssid = os.getenv("CAM_WIFI_SSID")
    password = os.getenv("CAM_WIFI_PASSWORD")
    if not ssid or password is None:
        return
    header = Path(__file__).resolve().parents[1] / "firmware/esp32_cam/SmartClassroomCam/camera_secrets.h"
    if not header.parent.is_dir():
        return
    content = (
        "#pragma once\n"
        "// Generated from backend/.env when the dashboard starts.\n"
        f"#define CAM_WIFI_SSID {json.dumps(ssid, ensure_ascii=False)}\n"
        f"#define CAM_WIFI_PASSWORD {json.dumps(password, ensure_ascii=False)}\n"
    )
    try:
        if not header.exists() or header.read_text(encoding="utf-8") != content:
            header.write_text(content, encoding="utf-8")
    except (OSError, UnicodeError):
        logging.warning("Could not update camera_secrets.h. Check the file before uploading in Arduino IDE.")


if __name__ == "__main__":
    sync_camera_wifi()
    checkins = app.extensions["camera_checkins"]
    checkins.start()
    atexit.register(checkins.stopping.set)
    app.run(host=os.getenv("HOST", "127.0.0.1"), port=int(os.getenv("PORT", "5000")), debug=False, use_reloader=False)

