# 🌡️ Monitor de Temperatura ESP32 + DHT22 + Sonoff

Projeto de monitoramento de temperatura e umidade com **ESP32**, sensor **DHT22** e **display OLED 0.96"**, com controle de tomada **Sonoff** integrado ao **Home Assistant** e página web própria.

---

## 📦 Componentes

| Componente | Descrição |
|---|---|
| ESP32 | ESP-WROOM-32 WiFi Bluetooth DEVKit V1 30 Pinos |
| DHT22 | Sensor de temperatura e umidade |
| OLED 0.96" | SSD1306 I2C 128×64 px (azul/amarelo) |
| Sonoff | Tomada inteligente (via Home Assistant MQTT) |

## 🔌 Pinagem

| OLED | ESP32 |
|---|---|
| GND | GND |
| VCC | 3.3V |
| SCL | GPIO 22 |
| SDA | GPIO 21 |

| DHT22 | ESP32 |
|---|---|
| VCC | 3.3V |
| DATA | GPIO 4 |
| GND | GND |

## 🚀 Como usar

### 1. Pré-requisitos

- [PlatformIO](https://platformio.org/) instalado no VS Code
- Broker MQTT (ex: Mosquitto ou o integrado ao Home Assistant)
- Home Assistant configurado com MQTT

### 2. Configurar credenciais

Edite `src/config.h` com suas informações:

```cpp
#define WIFI_SSID     "SEU_WIFI"
#define WIFI_PASSWORD "SUA_SENHA"
#define MQTT_BROKER   "192.168.1.XXX"
#define MQTT_USER     "mqtt_user"
#define MQTT_PASSWORD "mqtt_password"
```

### 3. Fazer upload

```bash
# Upload do firmware
pio run --target upload

# Upload da página web (LittleFS)
pio run --target uploadfs

# Monitor serial
pio device monitor
```

### 4. Acessar

Abra o navegador no IP do ESP32 (ex: `http://192.168.1.100`).

## 🏠 Integração com Home Assistant

Adicione no `configuration.yaml`:

```yaml
mqtt:
  switch:
    - name: "Tomada Sonoff ESP32"
      state_topic: "esp32/sonoff/estado"
      command_topic: "esp32/sonoff/comando"
      payload_on: "ON"
      payload_off: "OFF"

  sensor:
    - name: "Temperatura ESP32"
      state_topic: "esp32/temperatura/temperatura"
      unit_of_measurement: "°C"

    - name: "Umidade ESP32"
      state_topic: "esp32/temperatura/umidade"
      unit_of_measurement: "%"
```

## 📡 Tópicos MQTT

| Tópico | Direção | Descrição |
|---|---|---|
| `esp32/temperatura/temperatura` | ESP32 → HA | Temperatura (°C) |
| `esp32/temperatura/umidade` | ESP32 → HA | Umidade (%) |
| `esp32/sonoff/comando` | HA → ESP32 | Comando liga/desliga |
| `esp32/sonoff/estado` | ESP32 → HA | Estado atual da tomada |

## 🌐 API HTTP

| Rota | Método | Descrição |
|---|---|---|
| `/` | GET | Página web |
| `/api/status` | GET | JSON com temperatura, umidade e estado |
| `/api/sonoff?estado=1` | POST | Liga (1/ligado/on) ou desliga (0/desligado/off) |
| `/api/ler` | GET | Força leitura do sensor |
| `/ws` | WebSocket | Atualizações em tempo real |

## 📸 Preview

A página web mostra em tempo real:
- Temperatura e umidade com atualização via WebSocket
- Controle ON/OFF da tomada Sonoff
- Indicador visual de conexão

---

**Autor:** Igor Fronza
