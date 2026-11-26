import json
import logging
import os
import ssl
import uuid
from .database import connect, pending_data_reset


class MQTTBridge:
    def __init__(self, app, service, device):
        self.app, self.service, self.device = app, service, device
        self.client = None
        self.connected = False
        self.error = None
        self.base = os.getenv("MQTT_BASE_TOPIC", "demo/smartgate01").strip("/")
        self.device_id = app.config["DEVICE_ID"]

    def status(self):
        return {"enabled": self.app.config["MQTT_ENABLED"], "connected": self.connected, "error": self.error}

    def start(self):
        host = os.getenv("MQTT_HOST", "").strip()
        try:
            port = int(os.getenv("MQTT_PORT", "8883"))
        except ValueError:
            port = 0

        config_error = None
        if not host or "YOUR_" in host:
            config_error = "Chưa điền MQTT_HOST trong backend/.env."
        elif not self.base or any(c in self.base for c in "+#"):
            config_error = "MQTT_BASE_TOPIC không hợp lệ."
        elif not 1 <= port <= 65535:
            config_error = "MQTT_PORT trong backend/.env phải là số từ 1 đến 65535."

        if config_error:
            self.error = config_error
            logging.warning("%s", self.error)
            return

        try:
            import paho.mqtt.client as mqtt
            self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="classroom-backend-" + uuid.uuid4().hex[:10])
            self.client.username_pw_set(os.getenv("MQTT_USERNAME", ""), os.getenv("MQTT_PASSWORD", ""))
            if os.getenv("MQTT_TLS", "true").lower() == "true":
                self.client.tls_set_context(ssl.create_default_context())
            self.client.on_connect = self._on_connect
            self.client.on_disconnect = self._on_disconnect
            self.client.on_message = self._on_message
            self.client.reconnect_delay_set(min_delay=1, max_delay=30)
            self.client.connect_async(host, port, keepalive=30)
            self.client.loop_start()
        except Exception as exc:
            self.error = f"Không khởi tạo được MQTT: {type(exc).__name__}. Kiểm tra cấu hình và requirements.txt."
            logging.warning(self.error)

    def _on_connect(self, client, userdata, flags, reason_code, properties):
        self.connected = reason_code == 0
        self.error = None if self.connected else f"Broker từ chối kết nối: {reason_code}"
        if self.connected:
            client.subscribe([(self.base + suffix, 1) for suffix in ("/attendance", "/telemetry", "/state", "/availability")])
            self.publish_session()

    def _on_disconnect(self, client, userdata, flags, reason_code, properties):
        self.connected = False
        if reason_code != 0:
            self.error = "Mất kết nối MQTT; đang thử kết nối lại."

    def _on_message(self, client, userdata, message):
        try:
            if len(message.payload) > 8192:
                raise ValueError("Bản tin MQTT quá lớn.")
            suffix = message.topic.removeprefix(self.base)
            if suffix == "/availability":
                if message.payload == b"offline":
                    with self.device.lock:
                        self.device.available = False
                # A retained 'online' alone does not prove that the device is alive.
                return
            payload = json.loads(message.payload)
            if not isinstance(payload, dict):
                raise ValueError("MQTT JSON phải là object.")
            if payload.get("device_id", payload.get("device")) != self.device_id:
                raise ValueError("Sai device_id.")
            if suffix in {"/telemetry", "/state"}:
                if not message.retain:
                    self.device.update(payload)
            elif suffix == "/attendance":
                if message.retain:
                    raise ValueError("Không ghi điểm danh từ bản tin retained.")
                result = self.service.ingest(payload)
                ack = {"event_id": result["event_id"], "status": result["status"]}
                client.publish(self.base + "/attendance/ack", json.dumps(ack), qos=1, retain=False)
        except (ValueError, UnicodeDecodeError) as exc:
            self.service.log("mqtt:invalid", str(exc))
        except Exception:
            logging.exception("Failed to process MQTT message")
            self.error = "Có lỗi xử lý bản tin MQTT. Kiểm tra log backend."

    def command(self, payload):
        if not self.client or not self.connected:
            return False
        result = self.client.publish(self.base + "/command", json.dumps(payload), qos=1, retain=False)
        return result.rc == 0

    def publish_session(self):
        if not self.client or not self.connected:
            return
        with connect(self.app.config["DATABASE"]) as db:
            session = None if pending_data_reset(db) else db.execute("SELECT id,generation FROM sessions WHERE active=1").fetchone()
        payload = {"session_id": session["id"] if session else 0, "session_generation": session["generation"] if session else 0, "active": bool(session)}
        self.client.publish(self.base + "/session", json.dumps(payload), qos=1, retain=True)

    def stop(self):
        if self.client:
            self.client.disconnect()
            self.client.loop_stop()

