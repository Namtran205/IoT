#pragma once
#define WIFI_SSID "YOUR_2_4GHZ_WIFI"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define MQTT_HOST "YOUR_CLUSTER.s1.eu.hivemq.cloud"
#define MQTT_USERNAME "YOUR_MQTT_USERNAME"
#define MQTT_PASSWORD "YOUR_MQTT_PASSWORD"
// Set true only for a controlled demo without a CA. Default validates TLS.
#define MQTT_ALLOW_INSECURE false
// Paste the trusted root CA for your MQTT broker as a PEM string.
static const char MQTT_ROOT_CA[] = R"PEM(
-----BEGIN CERTIFICATE-----
PASTE_BROKER_ROOT_CA_HERE
-----END CERTIFICATE-----
)PEM";

