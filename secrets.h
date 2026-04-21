#pragma once

#define WIFI_SSID       "NOME_WIFI"
#define WIFI_PASSWORD   "PASSWORD_WIFI"

#define MQTT_HOST       "xxxxxx.s1.eu.hivemq.cloud"
#define MQTT_PORT       8883
#define MQTT_USER       "mqtt_username"
#define MQTT_PASSWORD   "mqtt_password"

#define DEVICE_ID       "plant-01"

// Certificato CA del broker / Root CA
static const char* ROOT_CA = R"EOF(
-----BEGIN CERTIFICATE-----
INCOLLA_QUI_IL_CERTIFICATO_CA
-----END CERTIFICATE-----
)EOF";