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
#define MQTT_BROKER   "192.168.1.XXX"   // IP do broker MQTT
#define MQTT_PORT     1883
#define MQTT_USER     "mqtt_user"
#define MQTT_PASSWORD "mqtt_password"

// Tópicos MQTT
#define MQTT_TOPIC_TEMP      "esp32/temperatura/temperatura"
#define MQTT_TOPIC_HUMID     "esp32/temperatura/umidade"

// ============================================================
// Home Assistant — REST API
// ============================================================
#define HA_HOST         "192.168.1.XXX"
#define HA_PORT         8123
#define HA_TOKEN        "SEU_LONG_LIVED_ACCESS_TOKEN"

// ============================================================
// Sonoffs (5 tomadas)
// Controle direto via REST API do Home Assistant
// ============================================================
#define SONOFF_COUNT 5

// Nomes que aparecem no display e na página web
#define SONOFF_NAME_1 "Sala"
#define SONOFF_NAME_2 "Quarto"
#define SONOFF_NAME_3 "Cozinha"
#define SONOFF_NAME_4 "Escrit."
#define SONOFF_NAME_5 "Jardim"

// Entity ID de cada Sonoff no Home Assistant
#define HA_ENTITY_1 "switch.sonoff_10011ccb43"
#define HA_ENTITY_2 "switch.sonoff_10011bdfac"
#define HA_ENTITY_3 "switch.sonoff_10011c3e2c"
#define HA_ENTITY_4 "switch.sonoff_10011c39cb"
#define HA_ENTITY_5 "switch.sonoff_10011ccb9a"

// ============================================================
// Pinos
// ============================================================
#define DHTPIN          4    // DHT22 no GPIO 4
#define DHTTYPE         DHT22

// Botões físicos (um para cada Sonoff)
// Pull-up interno: pressionar = LOW
#define BTN_PIN_1       32
#define BTN_PIN_2       33
#define BTN_PIN_3       25
#define BTN_PIN_4       26
#define BTN_PIN_5       27
#define DEBOUNCE_MS     300

// OLED I2C (SDA=GPIO21, SCL=GPIO22 — padrão ESP32)
// Alimentação: VCC no pino 3.3V do ESP32
#define OLED_SDA        21
#define OLED_SCL        22
#define OLED_ADDR       0x3C
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64

#endif
