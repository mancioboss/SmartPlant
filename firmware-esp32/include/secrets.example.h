#pragma once

// Copia questo file in secrets.h e compila il progetto.

// Wi-Fi
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// HiveMQ Cloud
#define MQTT_HOST "YOUR_CLUSTER.s1.eu.hivemq.cloud"
#define MQTT_PORT 8883
#define MQTT_USERNAME "YOUR_MQTT_USERNAME"
#define MQTT_PASSWORD "YOUR_MQTT_PASSWORD"
#define DEVICE_ID "plant-01"
#define MQTT_BASE_TOPIC "smartplant"

// Inserisci qui il certificato CA del broker.
// Con HiveMQ Cloud in genere si usa la CA pubblica del broker; se il tuo cluster usa Amazon Root CA 1,
// incolla qui il PEM completo.
static const char *HIVEMQ_CA_CERT = R"EOF(
-----BEGIN CERTIFICATE-----
PASTE_YOUR_CA_CERTIFICATE_HERE
-----END CERTIFICATE-----
)EOF";

// Pin ESP32 (modifica in base al tuo wiring)
#define SOIL_SENSOR_PIN 34
#define RELAY_PIN 26
#define I2C_SDA 21
#define I2C_SCL 22
#define OLED_RESET -1
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// Calibrazione sensore capacitivo (IMPORTANTISSIMO)
// Leggi il valore ADC in aria/asciutto e quando il sensore è immerso in terreno bagnato.
#define SOIL_RAW_DRY 3000
#define SOIL_RAW_WET 1400

// Logica relay
#define RELAY_ACTIVE_LEVEL LOW
#define RELAY_IDLE_LEVEL HIGH

// Soglie e tempi
#define SOIL_THRESHOLD_PERCENT 30.0f
#define DEFAULT_AUTO_WATER_SECONDS 4
#define DEFAULT_MANUAL_WATER_SECONDS 5
#define DEFAULT_SLEEP_SECONDS 3600
#define WIFI_TIMEOUT_MS 20000
#define MQTT_TIMEOUT_MS 15000
#define RETAINED_STATE_WAIT_MS 1500
#define DECISION_WAIT_MS 5000
