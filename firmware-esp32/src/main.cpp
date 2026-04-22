#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "esp_sleep.h"
#include "secrets.h"

// ============================================================
// SMARTPLANT CLOUD - ESP32D WIFI+BT N4XX
// ============================================================

// -------------------- PINOUT --------------------
// Rimosse le definizioni statiche che andavano in conflitto con secrets.h

// -------------------- OLED --------------------
static const uint8_t OLED_W        = SCREEN_WIDTH;
static const uint8_t OLED_H        = SCREEN_HEIGHT;
static const uint8_t OLED_ADDR     = 0x3C;

Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, OLED_RST);

// -------------------- SENSORI --------------------
Adafruit_BMP280 bmp;
Adafruit_AHTX0  aht;

// -------------------- MQTT/TLS --------------------
WiFiClientSecure secureClient;
PubSubClient     mqttClient(secureClient);

// -------------------- COSTANTI LOGICHE --------------------
// Usano i define presenti in secrets.h
static const uint32_t DEFAULT_SLEEP_SEC            = DEFAULT_SLEEP_SECONDS;
static const uint16_t MQTT_WAIT_RETAINED_MS        = RETAINED_STATE_WAIT_MS;
static const uint16_t MQTT_WAIT_AFTER_TELEMETRY_MS = DECISION_WAIT_MS;

// -------------------- TOPIC --------------------
String deviceId = DEVICE_ID;
String topicTelemetry;
String topicState;
String topicEvent;

// -------------------- STRUTTURE --------------------
struct TelemetryData {
  int   rawSoil      = 0;
  float soilMoisture = 0.0f;

  bool  bmpOk          = false;
  float bmpTemperature = NAN;
  float bmpPressure    = NAN;

  bool  ahtOk          = false;
  float ahtTemperature = NAN;
  float airHumidity    = NAN;

  int wifiRssi = 0;
};

struct CloudState {
  bool received           = false;
  uint32_t lastMessageAtMs = 0;

  bool manualWaterPending = false;
  bool shouldWaterNow     = false;
  bool suspendIrrigation  = false;
  bool rainPredicted      = false;
  bool decisionFromCloud  = false;

  int sleepTimeSec         = DEFAULT_SLEEP_SEC;
  int manualDurationSec    = DEFAULT_MANUAL_WATER_SECONDS;
  int autoWaterDurationSec = DEFAULT_AUTO_WATER_SECONDS;

  String commandId = "";
  String reason    = "";
};

TelemetryData telemetry;
CloudState    cloudState;
bool          displayOk = false;

// ============================================================
// FUNZIONI BASE
// ============================================================

void relayOff() { digitalWrite(RELAY_PIN, RELAY_INACTIVE_LEVEL); }
void relayOn()  { digitalWrite(RELAY_PIN, RELAY_ACTIVE_LEVEL);   }

void printLine(uint8_t row, const String& text) {
  if (!displayOk) return;
  display.setCursor(0, row * 10);
  display.print(text);
}

void drawScreen(const String& title,
                const String& line1 = "", const String& line2 = "",
                const String& line3 = "", const String& line4 = "",
                const String& line5 = "") {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  printLine(0, title);
  printLine(1, line1);
  printLine(2, line2);
  printLine(3, line3);
  printLine(4, line4);
  printLine(5, line5);
  display.display();
}

float mapSoilToPercent(int raw) {
  int constrained = constrain(raw, SOIL_RAW_WET, SOIL_RAW_DRY);
  float pct = 100.0f * float(SOIL_RAW_DRY - constrained) / float(SOIL_RAW_DRY - SOIL_RAW_WET);
  return constrain(pct, 0.0f, 100.0f);
}

int readSoilRawFiltered() {
  const int samples = 12;
  long sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(SOIL_PIN);
    delay(20);
  }
  return int(sum / samples);
}

void readSensors() {
  analogReadResolution(12);
  analogSetPinAttenuation(SOIL_PIN, ADC_11db);

  telemetry.rawSoil      = readSoilRawFiltered();
  telemetry.soilMoisture = mapSoilToPercent(telemetry.rawSoil);

  telemetry.bmpOk = bmp.begin(0x76) || bmp.begin(0x77);
  if (telemetry.bmpOk) {
    telemetry.bmpTemperature = bmp.readTemperature();
    telemetry.bmpPressure    = bmp.readPressure() / 100.0f;
  }

  telemetry.ahtOk = aht.begin();
  if (telemetry.ahtOk) {
    sensors_event_t hum, temp;
    aht.getEvent(&hum, &temp);
    telemetry.airHumidity    = hum.relative_humidity;
    telemetry.ahtTemperature = temp.temperature;
  }
}

bool initDisplay() {
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) return false;
  display.clearDisplay();
  display.display();
  return true;
}

void initTopics() {
  topicTelemetry = "smartplant/" + deviceId + "/telemetry";
  topicState     = "smartplant/" + deviceId + "/state";
  topicEvent     = "smartplant/" + deviceId + "/event";
}

// ============================================================
// WIFI + MQTT
// ============================================================

bool connectWiFi(uint32_t timeoutMs = WIFI_TIMEOUT_MS) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  drawScreen("WiFi connect", WIFI_SSID);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(300);
  }

  if (WiFi.status() == WL_CONNECTED) {
    telemetry.wifiRssi = WiFi.RSSI();
    drawScreen("WiFi OK", WiFi.localIP().toString(), "RSSI " + String(telemetry.wifiRssi));
    return true;
  }

  drawScreen("WiFi FAIL");
  return false;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (String(topic) != topicState) return;

  StaticJsonDocument<768> doc;
  if (deserializeJson(doc, payload, length)) {
    Serial.println("JSON state parse error");
    return;
  }

  cloudState.received           = true;
  cloudState.lastMessageAtMs   = millis();
  cloudState.manualWaterPending  = doc["manualWaterPending"]  | false;
  cloudState.shouldWaterNow      = doc["shouldWaterNow"]      | false;
  cloudState.suspendIrrigation   = doc["suspendIrrigation"]   | false;
  cloudState.rainPredicted       = doc["rainPredicted"]       | false;
  cloudState.decisionFromCloud   = doc["decisionFromCloud"]   | false;
  cloudState.sleepTimeSec        = doc["sleepTimeSec"]        | (int)DEFAULT_SLEEP_SEC;
  cloudState.manualDurationSec   = doc["manualDurationSec"]   | (int)DEFAULT_MANUAL_WATER_SECONDS;
  cloudState.autoWaterDurationSec = doc["autoWaterDurationSec"] | (int)DEFAULT_AUTO_WATER_SECONDS;
  cloudState.commandId = String((const char*)(doc["commandId"] | ""));
  cloudState.reason    = String((const char*)(doc["reason"]    | ""));

  Serial.println("Cloud state ricevuto:");
  serializeJsonPretty(doc, Serial);
  Serial.println();
}

bool connectMqtt(uint32_t timeoutMs = MQTT_TIMEOUT_MS) {
  secureClient.setCACert(ROOT_CA);
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(1024);

  String clientId = "esp32-" + deviceId + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);

  uint32_t start = millis();
  while (!mqttClient.connected() && millis() - start < timeoutMs) {
    if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD)) {
      mqttClient.subscribe(topicState.c_str(), 1);
      drawScreen("MQTT OK", deviceId, "sub state");
      return true;
    }
    delay(700);
  }

  drawScreen("MQTT FAIL");
  return false;
}

void pumpMqttLoop(uint32_t durationMs) {
  uint32_t start = millis();
  while (millis() - start < durationMs) {
    mqttClient.loop();
    delay(10);
  }
}

bool publishJson(const String& topic, JsonDocument& doc, bool retained = false) {
  String out;
  serializeJson(doc, out);
  return mqttClient.publish(topic.c_str(), out.c_str(), retained);
}

void publishBootEvent() {
  StaticJsonDocument<256> doc;
  doc["type"]         = "boot";
  doc["deviceId"]     = deviceId;
  doc["soilMoisture"] = telemetry.soilMoisture;
  doc["rawSoil"]      = telemetry.rawSoil;
  publishJson(topicEvent, doc, false);
}

void publishTelemetry() {
  StaticJsonDocument<512> doc;
  doc["deviceId"]     = deviceId;
  doc["rawSoil"]      = telemetry.rawSoil;
  doc["soilMoisture"] = telemetry.soilMoisture;
  doc["bmpOk"]        = telemetry.bmpOk;
  if (telemetry.bmpOk) {
    doc["temperature"] = telemetry.bmpTemperature;
    doc["pressure"]    = telemetry.bmpPressure;
  }
  doc["ahtOk"] = telemetry.ahtOk;
  if (telemetry.ahtOk) {
    doc["airTemperatureAht"] = telemetry.ahtTemperature;
    doc["airHumidity"]       = telemetry.airHumidity;
  }
  doc["wifiRssi"] = telemetry.wifiRssi;
  publishJson(topicTelemetry, doc, false);
}

void publishWateringStarted(bool manualMode, int durationSec) {
  StaticJsonDocument<256> doc;
  doc["type"]        = "watering_started";
  doc["deviceId"]    = deviceId;
  doc["manual"]      = manualMode;
  doc["durationSec"] = durationSec;
  publishJson(topicEvent, doc, false);
}

void publishWateringFinished(bool manualMode, int durationSec) {
  StaticJsonDocument<256> doc;
  doc["type"]        = "watering_finished";
  doc["deviceId"]    = deviceId;
  doc["manual"]      = manualMode;
  doc["durationSec"] = durationSec;
  publishJson(topicEvent, doc, false);
}

void publishWateringSkipped(const String& reason) {
  StaticJsonDocument<256> doc;
  doc["type"]     = "watering_skipped";
  doc["deviceId"] = deviceId;
  doc["reason"]   = reason;
  publishJson(topicEvent, doc, false);
}

// ============================================================
// IRRIGAZIONE
// ============================================================

void waterForSeconds(int durationSec, bool manualMode) {
  drawScreen(
    manualMode ? "Manual watering" : "Auto watering",
    "Relay ON",
    "Durata: " + String(durationSec) + " sec"
  );
  publishWateringStarted(manualMode, durationSec);
  relayOn();
  delay((uint32_t)durationSec * 1000UL);
  relayOff();
  publishWateringFinished(manualMode, durationSec);
  drawScreen(manualMode ? "Manual done" : "Auto done", "Relay OFF");
}

void goToDeepSleep(uint32_t sleepSec) {
  drawScreen("Deep sleep", "For " + String(sleepSec) + " sec");
  delay(600);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  btStop();
  esp_sleep_enable_timer_wakeup((uint64_t)sleepSec * 1000000ULL);
  esp_deep_sleep_start();
}

// ============================================================
// SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(400);

  pinMode(RELAY_PIN, OUTPUT);
  relayOff();

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  displayOk = initDisplay();
  initTopics();

  drawScreen("SmartPlant boot", deviceId);
  readSensors();

  drawScreen(
    "Read sensors",
    "Soil: " + String(telemetry.soilMoisture, 1) + "%",
    telemetry.bmpOk ? "BMP: OK" : "BMP: FAIL",
    telemetry.ahtOk ? "AHT: OK" : "AHT: FAIL"
  );

  bool wifiOk = connectWiFi();
  if (!wifiOk) {
    if (telemetry.soilMoisture < SOIL_THRESHOLD_PERCENT) waterForSeconds(DEFAULT_AUTO_WATER_SECONDS, false);
    goToDeepSleep(1800);
  }

  bool mqttOk = connectMqtt();
  if (!mqttOk) {
    if (telemetry.soilMoisture < SOIL_THRESHOLD_PERCENT) waterForSeconds(DEFAULT_AUTO_WATER_SECONDS, false);
    goToDeepSleep(1800);
  }

  pumpMqttLoop(MQTT_WAIT_RETAINED_MS);
  publishBootEvent();
  publishTelemetry();

  uint32_t afterPublish = millis();
  while (millis() - afterPublish < MQTT_WAIT_AFTER_TELEMETRY_MS) {
    mqttClient.loop();
    delay(10);
  }

  bool     doManual        = false;
  bool     doWater         = false;
  int      waterDurationSec = 0;
  uint32_t nextSleepSec    = DEFAULT_SLEEP_SEC;

  if (cloudState.received) {
    doManual         = cloudState.manualWaterPending;
    doWater          = cloudState.shouldWaterNow;
    waterDurationSec = doManual ? cloudState.manualDurationSec : cloudState.autoWaterDurationSec;
    nextSleepSec     = cloudState.sleepTimeSec > 0 ? (uint32_t)cloudState.sleepTimeSec : DEFAULT_SLEEP_SEC;

    drawScreen(
      "Cloud decision",
      "Soil " + String(telemetry.soilMoisture, 1) + "%",
      doWater ? "WATER YES" : "WATER NO",
      cloudState.suspendIrrigation ? "Rain suspend" : "Rain clear",
      cloudState.reason
    );
  } else {
    doWater          = telemetry.soilMoisture < SOIL_THRESHOLD_PERCENT;
    waterDurationSec = DEFAULT_AUTO_WATER_SECONDS;
    nextSleepSec     = doWater ? 1800 : DEFAULT_SLEEP_SEC;
    drawScreen("Local fallback", "No cloud state", doWater ? "WATER YES" : "WATER NO");
  }

  delay(1200);

  if (doWater) {
    waterForSeconds(waterDurationSec, doManual);
  } else {
    publishWateringSkipped(
      cloudState.received ? cloudState.reason : "Cloud state non ricevuto, terreno OK"
    );
  }

  goToDeepSleep(nextSleepSec);
}

void loop() {}
