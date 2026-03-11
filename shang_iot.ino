#include <WiFiNINA.h>
#include <ArduinoMqttClient.h>
#include <ArduinoECCX08.h>
#include <ArduinoBearSSL.h>
#include <Arduino_MKRENV.h>
#include <DFRobot_HX711_I2C.h>
#include "secrets.h"

// ---------- Network Settings ----------
const char ssid[] = WIFI_SSID;
const char pass[] = WIFI_PASSWORD;
const char broker[] = SECRET_BROKER; // AWS Endpoint
const int  port = 8883;
const char* certificate = SECRET_CERTIFICATE;

WiFiClient    wifiClient;            
BearSSLClient sslClient(wifiClient); 
MqttClient    mqttClient(sslClient);

// ---------- Hardware Pins ----------
const int LIGHT_PIN  = A0;
const int SOUND_PIN  = A1;
const int BUTTON_PIN = 4;    
const int LED_PIN    = 3;
const int BUZZER_PIN = 2;

// ---------- Sensors & State ----------
DFRobot_HX711_I2C MyScale;
enum State { NORMAL, ALERT_ENV, ALERT_LIGHT, ALERT_SOUND, LOW_STOCK, OUT_OF_STOCK, MANUAL_ALARM };
State currentState = NORMAL;
State lastState = NORMAL;

// ---------- Configuration & Timing ----------
unsigned long lastPublishMs = 0;
const unsigned long PUBLISH_INTERVAL = 5000; // 5 seconds
float weight_g = 0.0;

// ---------- Functions ----------

unsigned long getTime() {
  return WiFi.getTime();
}

void connectWiFi() {
  Serial.print("Connecting to WiFi...");
  while (WiFi.begin(ssid, pass) != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected.");
}

void connectMQTT() {
  Serial.print("Attempting AWS IoT connection...");
  mqttClient.setId("SmartShelf_01");
  while (!mqttClient.connect(broker, port)) {
    Serial.print(".");
    delay(5000);
  }
  Serial.println("\nConnected to AWS IoT!");
  // Subscribe to commands if needed in the future
  mqttClient.subscribe("mkr1010/commands");
}

void alarmBeep(int freq, int onMs, int offMs, int times) {
  for (int i = 0; i < times; i++) {
    tone(BUZZER_PIN, freq);
    digitalWrite(LED_PIN, HIGH);
    delay(onMs);
    noTone(BUZZER_PIN);
    digitalWrite(LED_PIN, LOW);
    delay(offMs);
  }
}

void applyActuators(State s) {
  switch (s) {
    case NORMAL:       digitalWrite(LED_PIN, LOW); noTone(BUZZER_PIN); break;
    case ALERT_LIGHT:  digitalWrite(LED_PIN, HIGH); break;
    case ALERT_SOUND:  alarmBeep(1200, 120, 80, 1); break;
    case ALERT_ENV:    alarmBeep(1000, 200, 120, 2); break;
    case LOW_STOCK:    alarmBeep(1000, 200, 0, 1); digitalWrite(LED_PIN, HIGH); break;
    case OUT_OF_STOCK: alarmBeep(800, 250, 150, 2); digitalWrite(LED_PIN, HIGH); break;
    case MANUAL_ALARM: alarmBeep(900, 250, 100, 3); break;
  }
}

void publishData(int light, int sound, float temp, float hum, float weight, bool btn, State s) {
  Serial.println("Publishing to AWS...");
  
  // This is the topic your friend will subscribe to in AWS
  mqttClient.beginMessage("mkr1010/shelf_data"); 
  
  // JSON Formatting
  mqttClient.print("{");
  mqttClient.print("\"light\":");   mqttClient.print(light);   mqttClient.print(",");
  mqttClient.print("\"temp\":");    mqttClient.print(temp);    mqttClient.print(",");
  mqttClient.print("\"weight\":");  mqttClient.print(weight);  mqttClient.print(",");
  mqttClient.print("\"state\":\""); mqttClient.print(s);       mqttClient.print("\"");
  mqttClient.print("}");
  
  mqttClient.endMessage();
}

void setup() {
  Serial.begin(9600);
  while (!Serial);

  pinMode(LIGHT_PIN, INPUT);
  pinMode(SOUND_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  if (!ECCX08.begin()) { Serial.println("ECCX08 failed"); while (1); }
  if (!ENV.begin())    { Serial.println("ENV Shield failed"); while (1); }
  while (!MyScale.begin()) { Serial.println("HX711 failed"); delay(1000); }
  
  MyScale.setCalWeight(100);

  connectWiFi();
  ArduinoBearSSL.onGetTime(getTime);
  sslClient.setEccSlot(0, certificate);
  connectMQTT();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!mqttClient.connected()) connectMQTT();
  mqttClient.poll();

  int lightVal = analogRead(LIGHT_PIN);
  int soundVal = analogRead(SOUND_PIN);
  bool buttonPressed = (digitalRead(BUTTON_PIN) == HIGH);
  float temp = ENV.readTemperature();
  float hum  = ENV.readHumidity();
  float w = MyScale.readWeight();
  if (w <= 0.5) w = 0.0;
  weight_g = w;

  State newState = NORMAL;
  if (buttonPressed) newState = MANUAL_ALARM;
  else if (hum > 65.0 || temp > 30.0) newState = ALERT_ENV;
  else if (weight_g <= 1.0)  newState = OUT_OF_STOCK;
  else if (weight_g <= 60.0) newState = LOW_STOCK;
  else if (lightVal < 50)    newState = ALERT_LIGHT;
  else                       newState = NORMAL;

  if (newState != lastState) {
    currentState = newState;
    applyActuators(currentState);
    publishData(lightVal, soundVal, temp, hum, weight_g, buttonPressed, currentState);
    lastState = newState;
  }

  if (millis() - lastPublishMs > PUBLISH_INTERVAL) {
    lastPublishMs = millis();
    publishData(lightVal, soundVal, temp, hum, weight_g, buttonPressed, currentState);
  }
}