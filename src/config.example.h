#ifndef CONFIG_H
#define CONFIG_H

// ============================================================
// WiFi
// ============================================================
#define WIFI_SSID     "SEU_WIFI"
#define WIFI_PASSWORD "SUA_SENHA"

// ============================================================
// MQTT (Home Assistant)
// ============================================================
#define MQTT_BROKER   "192.168.1.XXX"   // IP do Home Assistant / broker MQTT
#define MQTT_PORT     1883
#define MQTT_USER     "mqtt_user"
#define MQTT_PASSWORD "mqtt_password"

// Tópicos MQTT
#define MQTT_TOPIC_TEMP      "esp32/temperatura/temperatura"
#define MQTT_TOPIC_HUMID     "esp32/temperatura/umidade"
#define MQTT_TOPIC_SONOFF    "esp32/sonoff/comando"     // recebe comandos
#define MQTT_TOPIC_SONOFF_ST "esp32/sonoff/estado"      // publica estado

// ============================================================
// Pinos
// ============================================================
#define DHTPIN          4    // DHT22 no GPIO 4
#define DHTTYPE         DHT22

// OLED I2C (SDA=GPIO21, SCL=GPIO22 — padrão ESP32)
#define OLED_SDA        21
#define OLED_SCL        22
#define OLED_ADDR       0x3C
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64

// LED interno para indicar estado da tomada
#define LED_SONOFF      2    // LED built-in do ESP32

// ============================================================
// Intervalos (ms)
// ============================================================
#define INTERVAL_SENSOR   5000   // leitura do sensor a cada 5s
#define INTERVAL_MQTT     10000  // publicação MQTT a cada 10s

#endif
