/* Smart Classroom central controller.
   Arduino ESP32 core 2.0.0 or 3.x; PubSubClient; ArduinoJson 7.
   BH1750 is read with Wire directly (no extra sensor library required).
   Button requests local camera inference; gas/lighting remain local.
*/
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <esp_system.h>
#include <esp_arduino_version.h>
#include <time.h>
#include <math.h>
#include "config.h"
#if __has_include("secrets.h")
#include "secrets.h"
#define CONTROLLER_HAS_MQTT_CONFIG 1
#else
#include "secrets.example.h"
#define CONTROLLER_HAS_MQTT_CONFIG 0
#endif
#ifndef CONTROLLER_USE_MQTT
#define CONTROLLER_USE_MQTT CONTROLLER_HAS_MQTT_CONFIG
#endif

WiFiClientSecure tls;
PubSubClient mqtt(tls);
HardwareSerial camera(2);
String uartLine, currentRequest;
String enrollmentRequest;
bool enrollmentStarted=false;
unsigned long enrollmentAt=0;
bool uartOverflow=false, busy=false, gasAlarm=false, doorOpen=false, moving=false, lightReady=false;
bool gasLedBlinking=false;
bool buttonLast=HIGH, buttonStable=HIGH;
bool cameraLink=false, cameraAcknowledged=false;
bool cameraLivenessFailed=false;
int gasRaw=0, ledPercent=50, servoCurrent=SERVO_CLOSED, servoFrom=SERVO_CLOSED, servoTarget=SERVO_CLOSED;
int lightLedPercent=50;
float lux=NAN;
unsigned int highCount=0, lowCount=0, requestCounter=0;
unsigned int activeSession=0, requestSession=0;
unsigned int activeGeneration=0, requestGeneration=0;
unsigned long requestAt=0, buttonChanged=0, servoAt=0, sensorAt=0, wifiAt=0, mqttAt=0, retryAt=0;
unsigned long gasAlarmAt=0;
unsigned long lastCameraPing=0, lastCameraPong=0;
char bootId[17];
const int OUTBOX_SIZE=8;
String outbox[OUTBOX_SIZE], outboxIds[OUTBOX_SIZE];

void writeServo(int angle) {
  servoCurrent=constrain(angle,0,180);
  // Match the 1000–2000 microsecond pulse range verified with the servo test sketch.
  uint32_t pulse=1000U+(uint32_t)servoCurrent*1000U/180U;
  uint32_t duty=(uint32_t)((uint64_t)pulse*65535U/20000U);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_SERVO,duty);
#else
  ledcWrite(0,duty);
#endif
}
void writeLED(int percent) {
  ledPercent=constrain(percent,0,100);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_LED,ledPercent*255/100);
#else
  ledcWrite(2,ledPercent*255/100);
#endif
}
void updateLED() {
  const unsigned long elapsed = millis() - gasAlarmAt;
  if (gasLedBlinking && !gasAlarm && elapsed >= GAS_LED_MIN_BLINK_MS) gasLedBlinking = false;
  int percent = lightLedPercent;
  if (gasLedBlinking) {
    const unsigned long phase = elapsed / GAS_LED_TOGGLE_MS;
    percent = phase % 2 == 0 ? 100 : 0;
  }
  if (percent != ledPercent) writeLED(percent);
}
void publishState() {
  if(!mqtt.connected())return;
  JsonDocument doc;doc["device_id"]=DEVICE_ID;doc["door"]=doorOpen?"open":"closed";
  doc["moving"]=moving;doc["gas_alarm"]=gasAlarm;doc["led_percent"]=ledPercent;
  String out;serializeJson(doc,out);mqtt.publish(MQTT_BASE_TOPIC "/state",out.c_str(),true);
}
void setDoor(bool open) {
  if(!open&&gasAlarm){Serial.println("GAS OVERRIDE: close blocked");return;}
  servoFrom=servoCurrent;servoTarget=open?SERVO_OPEN:SERVO_CLOSED;servoAt=millis();moving=true;doorOpen=open;
  Serial.printf("SERVO command | GPIO%d | %d -> %d deg\n", PIN_SERVO, servoFrom, servoTarget);
  publishState();
}
void requestEnrollment() {
  if (busy || enrollmentRequest.length()) {
    Serial.println("ENROLL: busy | wait for the current camera request");
    return;
  }
  enrollmentRequest = String("enroll-") + bootId + "-" + String(++requestCounter);
  enrollmentStarted = false;
  enrollmentAt = millis();
  camera.printf("ENROLL:%s\n", enrollmentRequest.c_str());
  Serial.println("ENROLL request sent | keep one face in front of ESP32-CAM");
}

void pollUsbCommands() {
  static String command;
  static bool overflow = false;
  while (Serial.available()) {
    const char character = (char)Serial.read();
    if (character == '\r') continue;
    if (character != '\n') {
      if (command.length() < 16) command += character;
      else overflow = true;
      continue;
    }
    if (!overflow) {
      if (command == "ENROLL") requestEnrollment();
      else if (!CONTROLLER_USE_MQTT && command == "SERVO_OPEN") setDoor(true);
      else if (!CONTROLLER_USE_MQTT && command == "SERVO_CLOSE") setDoor(false);
    }
    command = "";
    overflow = false;
  }
}
void updateServo() {
  if(!moving)return;
  float progress=fminf(1.0f,(millis()-servoAt)/500.0f);
  writeServo(roundf(servoFrom+(servoTarget-servoFrom)*progress));
  if(progress>=1){moving=false;publishState();}
}
bool queueResult(const String &requestId,const String &status,int faceId) {
  for(int i=0;i<OUTBOX_SIZE;i++)if(!outbox[i].length()) {
    JsonDocument doc;doc["device_id"]=DEVICE_ID;doc["camera_id"]=CAMERA_ID;
    doc["event"]="attendance_result";doc["event_id"]=requestId;doc["request_id"]=requestId;
    doc["status"]=status;if(faceId>=0)doc["face_id"]=faceId;doc["uptime_ms"]=millis();
    doc["session_id"]=requestSession;
    doc["session_generation"]=requestGeneration;
    serializeJson(doc,outbox[i]);outboxIds[i]=requestId;retryAt=0;return true;
  }
  Serial.println("ERROR: outbox full; reconnect backend before next check-in");return false;
}
void callback(char *topic,byte *payload,unsigned int size) {
  JsonDocument doc;if(size>1024||deserializeJson(doc,payload,size))return;
  if(String(topic)==MQTT_BASE_TOPIC "/attendance/ack") {
    String id=doc["event_id"]|"",status=doc["status"]|"";
    if(status!="accepted"&&status!="duplicate"&&status!="rejected")return;
    for(int i=0;i<OUTBOX_SIZE;i++)if(id==outboxIds[i]){outbox[i]="";outboxIds[i]="";Serial.printf("BACKEND ACK: %s %s\n",id.c_str(),status.c_str());}
  } else if(String(topic)==MQTT_BASE_TOPIC "/session") {
    activeSession=doc["active"].as<bool>()?doc["session_id"].as<unsigned int>():0;
    activeGeneration=doc["session_generation"]|0U;
    Serial.printf("SESSION: %u | generation=%u\n",activeSession,activeGeneration);
  } else if(String(topic)==MQTT_BASE_TOPIC "/command") {
    String action=doc["door"]|"";if(action=="open")setDoor(true);else if(action=="close")setDoor(false);
  }
}
void requestCheckin() {
  if(busy||enrollmentRequest.length()){Serial.println("CHECKIN: busy");return;}
#if CONTROLLER_USE_MQTT
  bool space=false;for(int i=0;i<OUTBOX_SIZE;i++)if(!outbox[i].length())space=true;
  if(!space){Serial.println("CHECKIN: queue full, connect backend first");return;}
#endif
  currentRequest=String(DEVICE_ID)+"-"+bootId+"-"+String(++requestCounter);
  requestSession=activeSession;
  requestGeneration=activeGeneration;
  busy=true;requestAt=millis();cameraAcknowledged=false;cameraLivenessFailed=false;
  camera.print(CONTROLLER_USE_MQTT ? "CHECKIN:" : "CHECKIN_LOCAL:");
  camera.println(currentRequest);
  Serial.printf("CHECKIN request=%s\n",currentRequest.c_str());
}
void processEnrollment(const String &line) {
  const int first = line.indexOf(':', 7);
  if (first < 0 || !enrollmentRequest.length() || line.substring(7, first) != enrollmentRequest) return;
  const int second = line.indexOf(':', first + 1);
  if (second < 0) return;

  const String status = line.substring(first + 1, second);
  const String number = line.substring(second + 1);
  if (status == "started" || status == "progress") {
    enrollmentStarted = true;
    Serial.printf("ENROLL Sample=%s/5 | keep one face centered\n", number.c_str());
    return;
  }
  enrollmentRequest = "";
  if (status == "enrolled") {
    Serial.printf("ENROLL SAVED | Face ID=%s | saved on ESP32-CAM\n", number.c_str());
  } else if (status == "enrollment_duplicate") {
    Serial.printf("ENROLL DUPLICATE | existing Face ID=%s\n", number.c_str());
  } else {
    Serial.printf("ENROLL FAILED | %s\n", status.c_str());
  }
}

void processCheckinGuide(const String &line) {
  const int start = sizeof("CHECKIN_GUIDE:") - 1;
  const int first = line.indexOf(':', start);
  if (!busy || first < 0 || line.substring(start, first) != currentRequest) return;
  const int second = line.indexOf(':', first + 1);
  if (second < 0) return;
  const int third = line.indexOf(':', second + 1);
  if (third < 0) return;

  const String step = line.substring(first + 1, second);
  const String hint = line.substring(second + 1, third);
  const String score = line.substring(third + 1);
  if (step == "result") {
    cameraLivenessFailed = hint == "liveness_failed";
    if (score != "nan") Serial.printf("CHECKIN score | similarity=%s\n", score.c_str());
    return;
  }

  // camera's unmirrored image shows the person's left at image right.
  if (step == "center") Serial.print("1/3 NHIN THANG");
  else if (step == "turn_left") Serial.print("2/3 XOAY NHE SANG PHAI CUA BAN");
  else if (step == "turn_right") Serial.print("2/3 XOAY NHE SANG TRAI CUA BAN");
  else if (step == "return") Serial.print("3/3 NHIN THANG LAI");
  else return;

  if (hint == "no_face") Serial.print(" | Chua thay mat. Dung giua ong kinh, cach khoang 30-50 cm.");
  else if (hint == "closer") Serial.print(" | Tien lai gan mot chut.");
  else if (hint == "farther") Serial.print(" | Lui lai de camera thay tron mat.");
  else if (hint == "center_face") Serial.print(" | Dua mat ve giua ong kinh.");
  else if (hint == "dark" || hint == "uneven") Serial.print(" | Them anh sang deu tu phia truoc.");
  else if (hint == "bright") Serial.print(" | Giam anh sang gat chieu vao mat.");
  else if (hint == "opposite") Serial.print(" | Dang xoay nguoc huong. Xoay sang phia con lai.");
  else if (hint == "low_match") Serial.print(" | Da thay mat, do khop con thap.");
  else if (hint == "hold") Serial.print(" | Giu tu the cho den khi doi buoc.");
  if (score != "nan") Serial.printf(" | similarity=%s", score.c_str());
  Serial.println();
}

void processCamera(String line) {
  if(line=="BUTTON_PONG") {
    lastCameraPong=millis();
    if(!cameraLink)Serial.println("CAMERA UART: connected");
    cameraLink=true;
    return;
  }
  if(busy&&line.startsWith("CHECKIN_ACK:")&&line.substring(12)==currentRequest) {
    cameraAcknowledged=true;
    Serial.println("CAMERA ACK: capture requested");
    return;
  }
  if (line.startsWith("ENROLL:")) {
    processEnrollment(line);
    return;
  }
  if (line.startsWith("CHECKIN_GUIDE:")) {
    processCheckinGuide(line);
    return;
  }
  if(!line.startsWith("RESULT:"))return;
  int first=line.indexOf(':',7),second=line.indexOf(':',first+1);
  if(first<0||second<0)return;
  String id=line.substring(7,first),status=line.substring(first+1,second),face=line.substring(second+1);
  if(!busy||id!=currentRequest){Serial.println("Ignoring late/unrequested camera result");return;}
  bool success=status=="recognized";
  if(success&&(face.length()!=1||face[0]<'0'||face[0]>'6'))return;
  if(!success&&status!="unknown"&&status!="no_face"&&status!="not_aligned"&&status!="error"&&status!="busy"&&status!="not_enrolled"&&status!="multiple_faces")return;
  if (status == "unknown" && cameraLivenessFailed) status = "liveness_failed";
  int faceId=success?face.toInt():-1;
  if((!CONTROLLER_USE_MQTT||queueResult(id,status,faceId))&&success)setDoor(true);
  Serial.printf("CAMERA: %s Face ID=%d\n",status.c_str(),faceId);busy=false;
}
void pollCamera() {
  while(camera.available()) {
    char c=camera.read();
    if(c=='\r')continue;
    if(c=='\n') {if(!uartOverflow&&uartLine.length())processCamera(uartLine);uartLine="";uartOverflow=false;}
    else if(c>=32&&c<=126&&uartLine.length()<150&&!uartOverflow)uartLine+=c;
    else {uartLine="";uartOverflow=true;}
  }
  const unsigned long now=millis();
  if(now-lastCameraPing>=2000UL){lastCameraPing=now;camera.println("BUTTON_PING");}
  if(cameraLink&&now-lastCameraPong>=8000UL){cameraLink=false;Serial.println("CAMERA UART: no response");}
  if(busy&&now-requestAt>CHECKIN_TIMEOUT_MS){
    if(CONTROLLER_USE_MQTT)queueResult(currentRequest,"timeout",-1);
    busy=false;Serial.println(cameraAcknowledged ? "CHECKIN timeout: no camera result" : "CHECKIN timeout: no UART acknowledgement or result");
  }
  if (enrollmentRequest.length() && now - enrollmentAt > (enrollmentStarted ? 135000UL : 8000UL)) {
    enrollmentRequest = "";
    Serial.println(enrollmentStarted ? "ENROLL timeout: no camera result"
      : "ENROLL timeout: no reply; check CAM power, UART and firmware");
  }
}
void pollButton() {
  bool reading=digitalRead(PIN_BUTTON);if(reading!=buttonLast)buttonChanged=millis();
  if(millis()-buttonChanged>35&&reading!=buttonStable){buttonStable=reading;if(reading==LOW)requestCheckin();}
  buttonLast=reading;
}
void readSensors() {
  Wire.requestFrom((uint8_t)0x23,(uint8_t)2);
  if(Wire.available()==2){lux=((Wire.read()<<8)|Wire.read())/1.2f;lightReady=true;}
  else {lux=NAN;lightReady=false;}
  if(lightReady)lightLedPercent=lux<=20?100:lux>=200?0:roundf(100*(200-lux)/180);
  unsigned long sum=0;for(int i=0;i<8;i++)sum+=analogRead(PIN_MQ2);gasRaw=sum/8;
  const bool gasSensorReady = millis() > 30000UL;
  const bool wasGasAlarm = gasAlarm;
  if (gasSensorReady) {
    if (!gasAlarm) {
      highCount = gasRaw >= GAS_ALARM_ADC ? highCount + 1 : 0;
      if (highCount >= 2) {
        gasAlarm = true;
        gasAlarmAt = millis();
        gasLedBlinking = true;
        lowCount = 0;
      }
    } else {
      lowCount = gasRaw <= GAS_CLEAR_ADC ? lowCount + 1 : 0;
      if (lowCount >= 5) {
        gasAlarm = false;
        highCount = 0;
      }
    }
  }
  updateLED();
  if (gasAlarm && !wasGasAlarm) setDoor(true);
  if (!gasAlarm && wasGasAlarm) publishState();
  if (!CONTROLLER_USE_MQTT) {
    Serial.print("SENSORS | BH1750=");
    if (lightReady) Serial.printf("%.1f lux", lux);
    else Serial.print("no_reading");
    Serial.printf(" | LED=%d%% | MQ2=%d ADC | gas_ready=%d | gas_alarm=%d | LED_mode=%s\n",
                  ledPercent, gasRaw, gasSensorReady, gasAlarm, gasLedBlinking ? "blink" : "auto");
  }
  if(!mqtt.connected())return;
  JsonDocument doc;doc["device_id"]=DEVICE_ID;doc["door"]=doorOpen?"open":"closed";doc["moving"]=moving;
  if(lightReady)doc["lux"]=lux;else doc["lux"]=nullptr;
  doc["mq2_raw"]=gasRaw;doc["led_percent"]=ledPercent;doc["gas_alarm"]=gasAlarm;
  doc["gas_sensor_ready"]=gasSensorReady;doc["uptime_ms"]=millis();
  const IPAddress stationIp = WiFi.localIP();
  if ((uint32_t)stationIp) doc["station_ip"] = stationIp.toString();
  String out;serializeJson(doc,out);mqtt.publish(MQTT_BASE_TOPIC "/telemetry",out.c_str(),false);
}
void networkTick() {
  static uint32_t lastStationIp = 0;
  const uint32_t stationIp = WiFi.status() == WL_CONNECTED ? (uint32_t)WiFi.localIP() : 0;
  if (stationIp != lastStationIp) {
    lastStationIp = stationIp;
    if (stationIp) {
      Serial.printf("DEVKIT IP: %s | MAC: %s\n",
                    IPAddress(stationIp).toString().c_str(), WiFi.macAddress().c_str());
    } else {
      Serial.println("DEVKIT WIFI DISCONNECTED");
    }
  }
  if(!stationIp) {
    if(!wifiAt||millis()-wifiAt>10000UL){wifiAt=millis();WiFi.begin(WIFI_SSID,WIFI_PASSWORD);}return;
  }
  if(!mqtt.connected()) {
    if(millis()-mqttAt<5000UL&&mqttAt)return;
    mqttAt=millis();
    // Validate certificates after SNTP has supplied a usable current time.
    if(!MQTT_ALLOW_INSECURE&&time(nullptr)<1700000000)return;
    String id=String(DEVICE_ID)+"-"+bootId;
    if(mqtt.connect(id.c_str(),MQTT_USERNAME,MQTT_PASSWORD,MQTT_BASE_TOPIC "/availability",1,true,"offline")) {
      mqtt.subscribe(MQTT_BASE_TOPIC "/command",1);mqtt.subscribe(MQTT_BASE_TOPIC "/attendance/ack",1);
      mqtt.subscribe(MQTT_BASE_TOPIC "/session",1);
      mqtt.publish(MQTT_BASE_TOPIC "/availability","online",true);publishState();retryAt=0;
    } else Serial.printf("MQTT connect rc=%d\n",mqtt.state());
  }
  mqtt.loop();
  if(mqtt.connected()&&(!retryAt||millis()-retryAt>5000UL)) {
    retryAt=millis();for(int i=0;i<OUTBOX_SIZE;i++)if(outbox[i].length())mqtt.publish(MQTT_BASE_TOPIC "/attendance",outbox[i].c_str(),false);
  }
}
void setup() {
  Serial.begin(115200);snprintf(bootId,sizeof(bootId),"%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random());
  pinMode(PIN_BUTTON,INPUT_PULLUP);pinMode(PIN_MQ2,INPUT);analogReadResolution(12);analogSetPinAttenuation(PIN_MQ2,ADC_11db);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  const bool servoPwmReady = ledcAttachChannel(PIN_SERVO,50,16,0);
  ledcAttachChannel(PIN_LED,5000,8,2);
#else
  const bool servoPwmReady = ledcSetup(0,50,16) > 0;
  ledcAttachPin(PIN_SERVO,0);ledcSetup(2,5000,8);ledcAttachPin(PIN_LED,2);
#endif
  writeServo(SERVO_CLOSED);writeLED(50);Wire.begin(PIN_SDA,PIN_SCL);
  Wire.beginTransmission(0x23);Wire.write(0x10);Wire.endTransmission();
  camera.begin(115200,SERIAL_8N1,PIN_UART_RX,PIN_UART_TX);
#if CONTROLLER_USE_MQTT
  WiFi.mode(WIFI_STA);WiFi.setAutoReconnect(true);configTime(0,0,"pool.ntp.org","time.google.com");
  if(MQTT_ALLOW_INSECURE)tls.setInsecure();else tls.setCACert(MQTT_ROOT_CA);
  tls.setTimeout(1500);tls.setHandshakeTimeout(2);
  mqtt.setServer(MQTT_HOST,8883);mqtt.setCallback(callback);mqtt.setBufferSize(1024);mqtt.setSocketTimeout(2);
#endif
  Serial.println("CONTROLLER READY | press GPIO13 to request face recognition");
  Serial.println(CONTROLLER_USE_MQTT ? "ATTENDANCE TRANSPORT: MQTT" : "ATTENDANCE TRANSPORT: ESP32-CAM HTTP");
  Serial.printf("SERVO PWM | GPIO%d | %s\n", PIN_SERVO, servoPwmReady ? "ready" : "setup failed");
  Serial.println("USB camera command: ENROLL (5 samples, no Wi-Fi required)");
  if (!CONTROLLER_USE_MQTT) Serial.println("USB servo commands: SERVO_CLOSE | SERVO_OPEN");
}
void loop() {
  // Run local safety and UART before each bounded network attempt.
  pollButton();pollCamera();
  if(millis()-sensorAt>=1000UL){sensorAt=millis();readSensors();}
  else updateLED();
  pollUsbCommands();
  updateServo();
  if(CONTROLLER_USE_MQTT) networkTick();
  delay(2);
}
