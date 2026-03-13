#include <WiFiNINA.h>
#include <ArduinoMqttClient.h>
#include <ArduinoECCX08.h>
#include <ArduinoBearSSL.h>
#include <DFRobot_HX711_I2C.h>
#include <Wire.h>
#include "secrets.h"

const char ssid[]       = WIFI_SSID;
const char pass[]       = WIFI_PASSWORD;
const char broker[]     = SECRET_BROKER;
const int  port         = 8883;
const char* certificate = SECRET_CERTIFICATE;

WiFiClient    wifiClient;
BearSSLClient sslClient(wifiClient);
MqttClient    mqttClient(sslClient);

DFRobot_HX711_I2C MyScale;

const int BUTTON_PIN = 4;
const int SOUND_PIN  = A1;
const int LIGHT_PIN  = A0;
const int LED_PIN    = 3;
const int BUZZER_PIN = 2;

int   light_min    = 100;
int   sound_max    = 200;
float weight_low_g = 60.0;
float weight_out_g = 1.0;

enum State {
  NORMAL, ALERT_LIGHT, ALERT_SOUND,
  LOW_STOCK, OUT_OF_STOCK, MANUAL_ALARM
};

State state     = NORMAL;
State lastState = NORMAL;

const char* stateName(State s) {
  switch (s) {
    case NORMAL:       return "NORMAL";
    case ALERT_LIGHT:  return "ALERT_LIGHT";
    case ALERT_SOUND:  return "ALERT_SOUND";
    case LOW_STOCK:    return "LOW_STOCK";
    case OUT_OF_STOCK: return "OUT_OF_STOCK";
    case MANUAL_ALARM: return "MANUAL_ALARM";
    default:           return "UNKNOWN";
  }
}

// ---------- 非阻塞报警器 ----------
struct BeepPattern {
  int  freq, onMs, offMs, times, step;
  bool isOn, active;
  unsigned long lastMs;
};
BeepPattern beep = {0, 0, 0, 0, 0, false, false, 0};

void startBeep(int freq, int onMs, int offMs, int times) {
  beep = {freq, onMs, offMs, times, 0, true, true, millis()};
  tone(BUZZER_PIN, freq);
  digitalWrite(LED_PIN, HIGH);
}

void updateBeep() {
  if (!beep.active) return;
  unsigned long now = millis();
  if (beep.isOn) {
    if (now - beep.lastMs >= (unsigned long)beep.onMs) {
      noTone(BUZZER_PIN);
      digitalWrite(LED_PIN, LOW);
      beep.isOn   = false;
      beep.lastMs = now;
    }
  } else {
    if (now - beep.lastMs >= (unsigned long)beep.offMs) {
      beep.step++;
      if (beep.step >= beep.times) {
        beep.active = false;
      } else {
        tone(BUZZER_PIN, beep.freq);
        digitalWrite(LED_PIN, HIGH);
        beep.isOn   = true;
        beep.lastMs = now;
      }
    }
  }
}

void applyActuators(State s) {
  noTone(BUZZER_PIN);
  digitalWrite(LED_PIN, LOW);
  beep.active = false;
  switch (s) {
    case NORMAL:       break;
    case ALERT_LIGHT:  digitalWrite(LED_PIN, HIGH); startBeep(800,  250, 100, 1); break;
    case ALERT_SOUND:  startBeep(1200, 120, 80, 1);                               break;
    case LOW_STOCK:    digitalWrite(LED_PIN, HIGH); startBeep(1000, 200, 100, 1); break;
    case OUT_OF_STOCK: digitalWrite(LED_PIN, HIGH); startBeep(800,  250, 150, 2); break;
    case MANUAL_ALARM: startBeep(900, 250, 100, 3);                               break;
  }
}

bool buttonTriggered = false;
int  maxSoundVal     = 0;
int  minLightVal     = 9999;

unsigned long lastPublishMs = 0;
const unsigned long PUBLISH_INTERVAL_MS = 5000;

// ---------- 多次采样取中位数 ----------
float readWeightStable() {
  const int N = 5;
  float samples[N];
  for (int i = 0; i < N; i++) {
    samples[i] = MyScale.readWeight();
    delay(20);
  }
  for (int i = 0; i < N - 1; i++)
    for (int j = 0; j < N - 1 - i; j++)
      if (samples[j] > samples[j + 1]) {
        float tmp    = samples[j];
        samples[j]   = samples[j + 1];
        samples[j+1] = tmp;
      }
  float result = samples[N / 2];
  return (result <= 0.5) ? 0.0 : result;
}

// ---------- 去皮，每次 I2C 冲突后调用 ----------
void doTare() {
  Serial.println("Taring...");
  delay(1000);
  MyScale.peel();
  delay(500);
  Serial.print("Tare check: ");
  Serial.println(MyScale.readWeight());
}

unsigned long getTime() { return WiFi.getTime(); }

void connectWiFi() {
  Serial.print("Connecting to WiFi");
  while (WiFi.begin(ssid, pass) != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");
}

void connectMQTT() {
  Serial.print("Connecting to MQTT");
  while (!mqttClient.connect(broker, port)) {
    Serial.print(".");
    Serial.print(" MQTT err="); Serial.print(mqttClient.connectError());
    Serial.print(" TLS err=");  Serial.println(sslClient.errorCode());
    delay(5000);
  }
  Serial.println("\nMQTT connected");
}

void publishData(float weight, bool btn, int sound, int light,
                 const char* st, unsigned long ms) {
  Serial.print("Weight:"); Serial.print(weight, 1);
  Serial.print(" Btn:");   Serial.print(btn ? 1 : 0);
  Serial.print(" Sound:"); Serial.print(sound);
  Serial.print(" Light:"); Serial.print(light);
  Serial.print(" State:"); Serial.println(st);

  mqttClient.beginMessage("shelf_01/telemetry");
  mqttClient.print("{\"device_id\":\"shelf_01\",");
  mqttClient.print("\"weight_g\":"); mqttClient.print(weight, 1);   mqttClient.print(",");
  mqttClient.print("\"button\":");  mqttClient.print(btn ? 1 : 0); mqttClient.print(",");
  mqttClient.print("\"sound\":");   mqttClient.print(sound);        mqttClient.print(",");
  mqttClient.print("\"light\":");   mqttClient.print(light);        mqttClient.print(",");
  mqttClient.print("\"state\":\""); mqttClient.print(st);           mqttClient.print("\",");
  mqttClient.print("\"ms\":");      mqttClient.print(ms);
  mqttClient.print("}");
  mqttClient.endMessage();
}

void setup() {
  Serial.begin(9600);
  while (!Serial) {}

  Wire.begin();
  Wire.setClock(50000);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(SOUND_PIN,  INPUT);
  pinMode(LIGHT_PIN,  INPUT);
  pinMode(LED_PIN,    OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(LED_PIN,    LOW);
  digitalWrite(BUZZER_PIN, LOW);

  // 1. HX711 初始化
  Serial.println("Init HX711...");
  while (!MyScale.begin()) {
    Serial.println("HX711 init failed");
    delay(1000);
  }
  MyScale.setCalWeight(100);
  MyScale.setThreshold(30);

  // 2. 第一次去皮
  doTare();

  // 3. ECCX08 初始化（I2C 冲突根源）
  Serial.println("Init ECCX08...");
  if (!ECCX08.begin()) {
    Serial.println("ECCX08 not detected!");
    while (1) {}
  }

  // 4. ★ ECCX08 占用 I2C 后立即重新去皮
  doTare();

  // 5. WiFi 连接
  connectWiFi();

  // 6. ★ WiFi 稳定后去皮
  doTare();

  // 7. MQTT/TLS 连接
  ArduinoBearSSL.onGetTime(getTime);
  sslClient.setEccSlot(0, certificate);
  mqttClient.setId("shelf_01_telemetry");
  connectMQTT();

  // 8. ★ TLS 握手完成后去皮
  doTare();

  Serial.println("Ready.");
}

void loop() {
  updateBeep();

  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    doTare();
  }
  if (!mqttClient.connected()) {
    connectMQTT();
    doTare();
  }
  mqttClient.poll();

  if (digitalRead(BUTTON_PIN) == HIGH) buttonTriggered = true;
  int cs = analogRead(SOUND_PIN); if (cs > maxSoundVal) maxSoundVal = cs;
  int cl = analogRead(LIGHT_PIN); if (cl < minLightVal) minLightVal = cl;

  unsigned long now = millis();
  if (now - lastPublishMs >= PUBLISH_INTERVAL_MS) {
    lastPublishMs = now;

    float weight = readWeightStable();

    State newState = NORMAL;
    if      (buttonTriggered)         newState = MANUAL_ALARM;
    else if (minLightVal < light_min) newState = ALERT_LIGHT;
    else if (maxSoundVal > sound_max) newState = ALERT_SOUND;
    else if (weight <= weight_out_g)  newState = OUT_OF_STOCK;
    else if (weight <= weight_low_g)  newState = LOW_STOCK;

    if (newState != lastState) {
      state = newState;
      applyActuators(state);
      lastState = state;
    }

    publishData(weight, buttonTriggered, maxSoundVal, minLightVal, stateName(newState), now);

    buttonTriggered = false;
    maxSoundVal     = 0;
    minLightVal     = 9999;
  }
}
