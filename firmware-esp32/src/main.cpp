#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "secrets.h"

RTC_DATA_ATTR uint32_t bootCount = 0;

Adafruit_BMP280 bmp;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
WiFiClientSecure secureClient;
PubSubClient mqttClient(secureClient);

struct CloudState {
  bool received = false;
  bool manualWaterPending = false;
  bool suspendIrrigation = false;
  bool rainPredicted = false;
  bool shouldWaterNow = false;
  bool decisionFromCloud = false;
  int manualDurationSec = DEFAULT_MANUAL_WATER_SECONDS;
  int autoDurationSec = DEFAULT_AUTO_WATER_SECONDS;
  int sleepTimeSec = DEFAULT_SLEEP_SECONDS;
  String commandId;
  String reason;
  unsigned long updatedAtMs = 0;
};

struct SensorSnapshot {
  float soilMoisture = 0;
  int rawSoil = 0;
  float temperature = NAN;
  float pressure = NAN;
  bool bmpOk = false;
  int wifiRssi = 0;
};

CloudState cloudState;
SensorSnapshot snapshot;

String topicTelemetry;
String topicState;
String topicEvent;
String topicBase;

float clampFloat(float value, float minValue, float maxValue) {
  if (value < minValue) return minValue;
  if (value > maxValue) return maxValue;
  return value;
}

void oledPrint(const String &line1, const String &line2 = "", const String &line3 = "", const String &line4 = "") {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(line1);
  if (line2.length()) display.println(line2);
  if (line3.length()) display.println(line3);
  if (line4.length()) display.println(line4);
  display.display();
}

void ensureDisplay() {
  static bool initialized = false;
  if (!initialized) {
    Wire.begin(I2C_SDA, I2C_SCL);
    display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    initialized = true;
  }
}

float rawToPercent(int raw) {
  // La formula funziona anche se dry > wet oppure wet > dry.
  float percent = ((float)(raw - SOIL_RAW_DRY) / (float)(SOIL_RAW_WET - SOIL_RAW_DRY)) * 100.0f;
  return clampFloat(percent, 0.0f, 100.0f);
}

SensorSnapshot readSensors() {
  SensorSnapshot s;
  s.rawSoil = analogRead(SOIL_SENSOR_PIN);
  s.soilMoisture = rawToPercent(s.rawSoil);

  s.bmpOk = bmp.begin(0x76) || bmp.begin(0x77);
  if (s.bmpOk) {
    s.temperature = bmp.readTemperature();
    s.pressure = bmp.readPressure() / 100.0f; // hPa
  }

  return s;
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  String incomingTopic(topic);
  String body;
  for (unsigned int i = 0; i < length; i++) {
    body += (char)payload[i];
  }

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("JSON non valido da topic %s\n", topic);
    return;
  }

  if (incomingTopic == topicState) {
    cloudState.received = true;
    cloudState.updatedAtMs = millis();
    cloudState.manualWaterPending = doc["manualWaterPending"] | false;
    cloudState.suspendIrrigation = doc["suspendIrrigation"] | false;
    cloudState.rainPredicted = doc["rainPredicted"] | false;
    cloudState.shouldWaterNow = doc["shouldWaterNow"] | false;
    cloudState.decisionFromCloud = doc["decisionFromCloud"] | true;
    cloudState.manualDurationSec = doc["manualDurationSec"] | DEFAULT_MANUAL_WATER_SECONDS;
    cloudState.autoDurationSec = doc["autoWaterDurationSec"] | DEFAULT_AUTO_WATER_SECONDS;
    cloudState.sleepTimeSec = doc["sleepTimeSec"] | DEFAULT_SLEEP_SECONDS;
    cloudState.commandId = String((const char *)(doc["commandId"] | ""));
    cloudState.reason = String((const char *)(doc["reason"] | "Nessuna motivazione cloud"));

    Serial.println("Stato cloud ricevuto:");
    serializeJsonPretty(doc, Serial);
    Serial.println();

    oledPrint(
        "Cloud Sync OK",
        "Soil: " + String(snapshot.soilMoisture, 1) + "%",
        cloudState.suspendIrrigation ? "Pioggia: sospeso" : "Pioggia: libera",
        cloudState.manualWaterPending ? "Manuale in coda" : cloudState.reason);
  }
}

bool connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(300);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi non connesso");
    return false;
  }

  snapshot.wifiRssi = WiFi.RSSI();
  Serial.printf("Wi-Fi OK - IP: %s RSSI: %d dBm\n", WiFi.localIP().toString().c_str(), snapshot.wifiRssi);
  return true;
}

bool connectMqtt() {
  secureClient.setCACert(HIVEMQ_CA_CERT);
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(1024);
  mqttClient.setKeepAlive(20);

  unsigned long start = millis();
  while (!mqttClient.connected() && millis() - start < MQTT_TIMEOUT_MS) {
    String clientId = String("esp32-") + DEVICE_ID + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    bool ok = mqttClient.connect(clientId.c_str(), MQTT_USERNAME, MQTT_PASSWORD);
    if (ok) {
      Serial.println("MQTT TLS connesso");
      mqttClient.subscribe(topicState.c_str(), 1);
      return true;
    }
    Serial.printf("Tentativo MQTT fallito rc=%d\n", mqttClient.state());
    delay(1000);
  }

  return false;
}

void pumpOn() {
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LEVEL);
}

void pumpOff() {
  digitalWrite(RELAY_PIN, RELAY_IDLE_LEVEL);
}

void publishEvent(const char *type, bool manualMode, int durationSec = 0, const String &extraReason = "") {
  if (!mqttClient.connected()) return;

  StaticJsonDocument<384> doc;
  doc["deviceId"] = DEVICE_ID;
  doc["type"] = type;
  doc["manual"] = manualMode;
  doc["durationSec"] = durationSec;
  doc["soilMoisture"] = snapshot.soilMoisture;
  doc["rawSoil"] = snapshot.rawSoil;
  doc["commandId"] = cloudState.commandId;
  doc["reason"] = extraReason.length() ? extraReason : cloudState.reason;
  doc["bootCount"] = bootCount;
  doc["timestampMs"] = millis();

  char buffer[384];
  size_t len = serializeJson(doc, buffer);
  mqttClient.publish(topicEvent.c_str(), buffer, len, false);
}

void waterPlant(int durationSec, bool manualMode) {
  durationSec = max(durationSec, 1);
  String modeLabel = manualMode ? "Manuale" : "Automatico";

  oledPrint("Irrigazione", modeLabel, "Durata: " + String(durationSec) + " s", "Pompa ON");
  publishEvent("watering_started", manualMode, durationSec, manualMode ? "manual-command" : "cloud-decision");

  pumpOn();
  unsigned long startedAt = millis();
  while ((millis() - startedAt) < (unsigned long)durationSec * 1000UL) {
    mqttClient.loop();
    delay(50);
  }
  pumpOff();

  oledPrint("Irrigazione finita", "Pompa OFF", "Soil: " + String(snapshot.soilMoisture, 1) + "%");
  publishEvent("watering_finished", manualMode, durationSec, manualMode ? "manual-completed" : "auto-completed");
}

void publishTelemetry() {
  if (!mqttClient.connected()) return;

  StaticJsonDocument<512> doc;
  doc["deviceId"] = DEVICE_ID;
  doc["bootCount"] = bootCount;
  doc["soilMoisture"] = roundf(snapshot.soilMoisture * 10.0f) / 10.0f;
  doc["rawSoil"] = snapshot.rawSoil;
  doc["temperature"] = snapshot.bmpOk ? roundf(snapshot.temperature * 10.0f) / 10.0f : nullptr;
  doc["pressure"] = snapshot.bmpOk ? roundf(snapshot.pressure * 10.0f) / 10.0f : nullptr;
  doc["bmpOk"] = snapshot.bmpOk;
  doc["wifiRssi"] = snapshot.wifiRssi;
  doc["threshold"] = SOIL_THRESHOLD_PERCENT;

  char buffer[512];
  size_t len = serializeJson(doc, buffer);
  mqttClient.publish(topicTelemetry.c_str(), buffer, len, false);

  Serial.println("Telemetria pubblicata:");
  serializeJsonPretty(doc, Serial);
  Serial.println();
}

void mqttLoopFor(unsigned long durationMs) {
  unsigned long start = millis();
  while (millis() - start < durationMs) {
    mqttClient.loop();
    delay(20);
  }
}

void goToDeepSleep() {
  int sleepSeconds = cloudState.sleepTimeSec > 0 ? cloudState.sleepTimeSec : DEFAULT_SLEEP_SECONDS;
  oledPrint("Deep Sleep", "Tra " + String(sleepSeconds) + " sec", cloudState.reason);
  Serial.printf("Sleep per %d secondi\n", sleepSeconds);
  delay(1200);
  mqttClient.disconnect();
  WiFi.disconnect(true, true);
  esp_sleep_enable_timer_wakeup((uint64_t)sleepSeconds * 1000000ULL);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(250);
  bootCount++;

  pinMode(RELAY_PIN, OUTPUT);
  pumpOff();
  analogReadResolution(12);
  analogSetPinAttenuation(SOIL_SENSOR_PIN, ADC_11db);

  ensureDisplay();
  oledPrint("SmartPlant Boot", "Wake #" + String(bootCount), String(DEVICE_ID));

  topicBase = String(MQTT_BASE_TOPIC) + "/" + DEVICE_ID;
  topicTelemetry = topicBase + "/telemetry";
  topicState = topicBase + "/state";
  topicEvent = topicBase + "/event";

  snapshot = readSensors();
  oledPrint(
      "Lettura sensori",
      "Soil: " + String(snapshot.soilMoisture, 1) + "%",
      snapshot.bmpOk ? ("Temp: " + String(snapshot.temperature, 1) + " C") : "BMP280 non trovato",
      snapshot.bmpOk ? ("Press: " + String(snapshot.pressure, 1) + " hPa") : "");

  if (!connectWifi()) {
    cloudState.reason = "WiFi KO";
    goToDeepSleep();
  }

  if (!connectMqtt()) {
    cloudState.reason = "MQTT KO";
    goToDeepSleep();
  }

  // 1) Check immediato dello stato retained (manual override / pioggia / ultimo stato DB)
  mqttLoopFor(RETAINED_STATE_WAIT_MS);

  // 2) Invio telemetria corrente per chiedere una decisione cloud aggiornata
  publishEvent("boot", false, 0, "wake-check");
  publishTelemetry();

  // 3) Attendo la decisione aggiornata dal backend
  mqttLoopFor(DECISION_WAIT_MS);

  bool shouldWater = cloudState.received && cloudState.shouldWaterNow;
  bool manualMode = cloudState.received && cloudState.manualWaterPending;
  int durationSec = manualMode ? cloudState.manualDurationSec : cloudState.autoDurationSec;

  if (shouldWater) {
    waterPlant(durationSec, manualMode);
    mqttLoopFor(1000);
  } else {
    String line = cloudState.received ? cloudState.reason : "Nessuna decisione cloud";
    oledPrint("Niente acqua", "Soil: " + String(snapshot.soilMoisture, 1) + "%", line);
    publishEvent("watering_skipped", false, 0, line);
  }

  goToDeepSleep();
}

void loop() {
  // Vuoto: il dispositivo lavora solo al risveglio, poi torna in deep sleep.
}
