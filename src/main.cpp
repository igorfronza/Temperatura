#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <DHT.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include "config.h"

// ============================================================
// Objetos globais
// ============================================================
DHT dht(DHTPIN, DHTTYPE);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
WiFiClient espClient;
PubSubClient mqtt(espClient);

// ============================================================
// Variáveis de estado
// ============================================================
float temperatura = 0.0;
float umidade = 0.0;

// --- 5 Sonoffs ---
bool sonoffEstado[SONOFF_COUNT] = {false, false, false, false, false};

// Nomes (definidos no config.h, copiamos para array em runtime)
const char* sonoffNome[SONOFF_COUNT] = {
  SONOFF_NAME_1, SONOFF_NAME_2, SONOFF_NAME_3, SONOFF_NAME_4, SONOFF_NAME_5
};

// Entity IDs do Home Assistant
const char* sonoffEntity[SONOFF_COUNT] = {
  HA_ENTITY_1, HA_ENTITY_2, HA_ENTITY_3, HA_ENTITY_4, HA_ENTITY_5
};

// --- Botões físicos ---
const int btnPins[SONOFF_COUNT] = {BTN_PIN_1, BTN_PIN_2, BTN_PIN_3, BTN_PIN_4, BTN_PIN_5};
unsigned long lastBtnPress[SONOFF_COUNT] = {0, 0, 0, 0, 0};
bool btnPressionado[SONOFF_COUNT] = {false, false, false, false, false};

// Comandos pendentes (processados sem bloquear os botões)
bool comandoPendente[SONOFF_COUNT] = {false, false, false, false, false};
bool comandoValor[SONOFF_COUNT] = {false, false, false, false, false};

// Timers
unsigned long lastSensor = 0;
unsigned long lastMqtt = 0;
unsigned long lastHaStatus = 0;

// ============================================================
// Forward declarations
// ============================================================
void conectarWiFi();
void conectarMQTT();
void lerSensor();
void consultarEstadoSonoffs();
void haPost(String service, String entityId);
void atualizarDisplay();
void notificarWebSocket();
void verificarBotoes();
void comandarSonoff(int id, bool ligar);

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  // --- DHT22 ---
  dht.begin();

  // --- OLED ---
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("Falha ao iniciar OLED SSD1306"));
  } else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(F("Inicializando..."));
    display.display();
  }

  // --- LittleFS ---
  if (!LittleFS.begin()) {
    Serial.println(F("Erro ao montar LittleFS"));
  }

  // --- WiFi ---
  conectarWiFi();

  // --- MQTT (apenas para publicar temperatura/umidade) ---
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);

  // --- Botões físicos ---
  for (int i = 0; i < SONOFF_COUNT; i++) {
    pinMode(btnPins[i], INPUT_PULLUP);
  }

  // ==========================================================
  // Rotas do servidor web
  // ==========================================================

  // Página principal
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(LittleFS, "/index.html", "text/html");
  });

  // API: leitura completa (JSON)
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* request) {
    DynamicJsonDocument doc(512);
    doc["temperatura"] = temperatura;
    doc["umidade"] = umidade;
    JsonArray arr = doc.createNestedArray("sonoffs");
    for (int i = 0; i < SONOFF_COUNT; i++) {
      JsonObject s = arr.createNestedObject();
      s["id"] = i + 1;
      s["nome"] = sonoffNome[i];
      s["ligado"] = sonoffEstado[i];
    }
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // API: ligar/desligar Sonoff específico
  // Ex: POST /api/sonoff  body: id=1&estado=1
  server.on("/api/sonoff", HTTP_POST, [](AsyncWebServerRequest* request) {
    int id = 0;
    String estado;

    if (request->hasParam("id", true))
      id = request->getParam("id", true)->value().toInt();
    else if (request->hasParam("id"))
      id = request->getParam("id")->value().toInt();

    if (request->hasParam("estado", true))
      estado = request->getParam("estado", true)->value();
    else if (request->hasParam("estado"))
      estado = request->getParam("estado")->value();

    if (id < 1 || id > SONOFF_COUNT) {
      request->send(400, "application/json", "{\"erro\":\"id invalido\"}");
      return;
    }

    bool ligar = (estado == "1" || estado == "ligado" || estado == "on");
    comandarSonoff(id - 1, ligar);

    DynamicJsonDocument doc(64);
    doc["id"] = id;
    doc["status"] = ligar ? "ligado" : "desligado";
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // API: forçar leitura do sensor
  server.on("/api/ler", HTTP_GET, [](AsyncWebServerRequest* request) {
    lerSensor();
    DynamicJsonDocument doc(128);
    doc["temperatura"] = temperatura;
    doc["umidade"] = umidade;
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // --- WebSocket ---
  ws.onEvent([](AsyncWebSocket* server, AsyncWebSocketClient* client,
                  AwsEventType type, void* arg, uint8_t* data, size_t len) {
    if (type == WS_EVT_CONNECT) {
      Serial.printf("WebSocket cliente #%u conectado\n", client->id());
      notificarWebSocket();
    }
  });
  server.addHandler(&ws);

  server.begin();
  Serial.println(F("Servidor HTTP iniciado"));
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  unsigned long agora = millis();

  // Leitura do sensor
  if (agora - lastSensor >= INTERVAL_SENSOR) {
    lastSensor = agora;
    lerSensor();
    atualizarDisplay();
  }

  // Leitura dos botões físicos
  verificarBotoes();

  // Processa comandos pendentes (verifica botões antes de cada POST)
  for (int i = 0; i < SONOFF_COUNT; i++) {
    if (comandoPendente[i]) {
      verificarBotoes();  // lê botões antes do POST (evita perder press)
      comandoPendente[i] = false;
      haPost(comandoValor[i] ? "turn_on" : "turn_off", sonoffEntity[i]);
      Serial.printf("Sonoff %d (%s) -> %s\n", i + 1, sonoffNome[i],
                    comandoValor[i] ? "LIGADO" : "DESLIGADO");
      break;
    }
  }

  // Publicação MQTT (temperatura + umidade)
  if (agora - lastMqtt >= INTERVAL_MQTT) {
    lastMqtt = agora;
    if (mqtt.connected()) {
      char buf[8];
      dtostrf(temperatura, 4, 1, buf);
      mqtt.publish(MQTT_TOPIC_TEMP, buf);
      dtostrf(umidade, 4, 1, buf);
      mqtt.publish(MQTT_TOPIC_HUMID, buf);
    }
    notificarWebSocket();
  }

  // Consulta estado dos Sonoffs via REST API do Home Assistant
  if (agora - lastHaStatus >= INTERVAL_MQTT) {
    lastHaStatus = agora;
    consultarEstadoSonoffs();
  }

  // Mantém conexões
  if (!mqtt.connected()) {
    conectarMQTT();
  }
  mqtt.loop();
  ws.cleanupClients();
}

// ============================================================
// WiFi
// ============================================================
void conectarWiFi() {
  Serial.printf("Conectando ao WiFi %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 30) {
    delay(500);
    Serial.print(".");
    tentativas++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(F("\nWiFi conectado!"));
    Serial.print(F("IP: "));
    Serial.println(WiFi.localIP());

    if (display.getBuffer() != nullptr) {
      display.clearDisplay();
      display.setCursor(0, 0);
      display.println(F("WiFi OK"));
      display.print(WiFi.localIP());
      display.display();
    }
  } else {
    Serial.println(F("\nFalha no WiFi. Reiniciando..."));
    ESP.restart();
  }
}

// ============================================================
// MQTT
// ============================================================
void conectarMQTT() {
  static unsigned long ultimaTentativa = 0;
  if (millis() - ultimaTentativa < 5000) return;
  ultimaTentativa = millis();

  Serial.print(F("Conectando MQTT... "));
  String clientId = "ESP32_Temp_" + String(random(0xffff), HEX);

  if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD)) {
    Serial.println(F("OK"));
  } else {
    Serial.printf("Falha (rc=%d)\n", mqtt.state());
  }
}

void callbackMQTT(char* topic, byte* payload, unsigned int length) {
  // Callback vazio — comandos e estado dos Sonoffs via REST API
}

// ============================================================
// Sensor DHT22
// ============================================================
void lerSensor() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (!isnan(t)) temperatura = t;
  if (!isnan(h)) umidade = h;

  Serial.printf("Temp: %.1f C | Umid: %.1f %%\n", temperatura, umidade);
}

// ============================================================
// Display OLED — tela única com sensores + 4 Sonoffs
// ============================================================
void atualizarDisplay() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // --- IP no topo ---
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(WiFi.localIP());

  // Linha separadora
  display.drawLine(0, 9, SCREEN_WIDTH, 9, SSD1306_WHITE);

  // --- Temperatura (y=16 para ficar nos LEDs azuis) ---
  display.setTextSize(2);
  display.setCursor(0, 16);
  display.printf("%.1f", temperatura);
  display.setTextSize(1);
  display.print("C");

  // --- Umidade (ao lado, mesma altura) ---
  display.setTextSize(2);
  display.setCursor(70, 16);
  display.printf("%.1f", umidade);
  display.setTextSize(1);
  display.print("%");

  // Linha separadora abaixo dos valores
  display.drawLine(0, 33, SCREEN_WIDTH, 33, SSD1306_WHITE);

  // --- 5 Sonoffs: 2 colunas x 3 linhas (última centralizada) ---
  display.setTextSize(1);
  for (int i = 0; i < SONOFF_COUNT; i++) {
    int col, row, xCirculo, xNome;

    if (i < 4) {
      // Primeiras 4: grid 2x2
      col = i % 2;
      row = i / 2;
      xCirculo = col * 64 + 4;
      xNome = col * 64 + 16;
    } else {
      // 5ª tomada: centralizada na 3ª linha
      col = 0;
      row = 2;
      // Centraliza: (128 - texto_width) / 2, aprox 30px
      xCirculo = 45;
      xNome = 57;
    }

    int yCirculo = 37 + row * 11;
    int yTexto = yCirculo - 3;

    // Círculo: preenchido = LIGADO, vazio = DESLIGADO
    if (sonoffEstado[i]) {
      display.fillCircle(xCirculo, yCirculo, 4, SSD1306_WHITE);
    } else {
      display.drawCircle(xCirculo, yCirculo, 4, SSD1306_WHITE);
    }

    display.setCursor(xNome, yTexto);
    display.print(sonoffNome[i]);
  }

  display.display();
}

// ============================================================
// WebSocket: notifica todos os clientes
// ============================================================
void notificarWebSocket() {
  DynamicJsonDocument doc(512);
  doc["temperatura"] = temperatura;
  doc["umidade"] = umidade;

  JsonArray arr = doc.createNestedArray("sonoffs");
  for (int i = 0; i < SONOFF_COUNT; i++) {
    JsonObject s = arr.createNestedObject();
    s["id"] = i + 1;
    s["nome"] = sonoffNome[i];
    s["ligado"] = sonoffEstado[i];
  }

  String json;
  serializeJson(doc, json);
  ws.textAll(json);
}

// ============================================================
// Comando para o Sonoff (chamado pela API web)
// Marca pendência — o POST é feito no loop sem bloquear
void comandarSonoff(int id, bool ligar) {
  if (id < 0 || id >= SONOFF_COUNT) return;

  sonoffEstado[id] = ligar;
  comandoPendente[id] = true;
  comandoValor[id] = ligar;

  Serial.printf("Sonoff %d (%s) -> %s\n", id + 1, sonoffNome[id],
                ligar ? "LIGADO" : "DESLIGADO");

  atualizarDisplay();
  notificarWebSocket();
}

// ============================================================
// POST para Home Assistant REST API
// Ex: /api/services/switch/turn_on  body: {"entity_id": "switch.xxx"}
// ============================================================
void haPost(String service, String entityId) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = "http://" + String(HA_HOST) + ":" + String(HA_PORT) +
               "/api/services/switch/" + service;

  http.begin(url);
  http.addHeader("Authorization", "Bearer " + String(HA_TOKEN));
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(1500);  // timeout curto para não travar o loop

  String body = "{\"entity_id\":\"" + entityId + "\"}";

  int httpCode = http.POST(body);
  Serial.printf("HA POST %s [%s] -> %d\n", service.c_str(), entityId.c_str(),
                httpCode);

  if (httpCode > 0 && httpCode < 400) {
    String resp = http.getString();
    Serial.println(resp);
  } else {
    Serial.printf("  ERRO: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
}

// ============================================================
// Consulta estado de TODOS os Sonoffs via REST API
// GET /api/states/switch.sonoff_XXXXXXXXXXXX
// ============================================================
void consultarEstadoSonoffs() {
  if (WiFi.status() != WL_CONNECTED) return;

  for (int i = 0; i < SONOFF_COUNT; i++) {
    HTTPClient http;
    String url = "http://" + String(HA_HOST) + ":" + String(HA_PORT) +
                 "/api/states/" + sonoffEntity[i];

    http.begin(url);
    http.addHeader("Authorization", "Bearer " + String(HA_TOKEN));

    int httpCode = http.GET();
    if (httpCode == 200) {
      String resp = http.getString();

      // Parse JSON: {"state": "on", ...} ou {"state": "off", ...}
      DynamicJsonDocument doc(512);
      DeserializationError err = deserializeJson(doc, resp);
      if (!err) {
        const char* state = doc["state"];
        bool ligado = (strcmp(state, "on") == 0);
        if (sonoffEstado[i] != ligado) {
          sonoffEstado[i] = ligado;
          Serial.printf("HA Estado %s = %s\n", sonoffEntity[i], state);
          atualizarDisplay();
          notificarWebSocket();
        }
      }
    } else {
      Serial.printf("HA GET %s -> erro %d\n", sonoffEntity[i], httpCode);
    }

    http.end();
    delay(50);  // pequena pausa entre requisições
  }
}

// ============================================================
// Leitura dos botões físicos (debounce com detecção de borda)
// NÃO bloqueia — apenas alterna estado local e marca pendência
// ============================================================
void verificarBotoes() {
  unsigned long agora = millis();

  for (int i = 0; i < SONOFF_COUNT; i++) {
    bool estadoAtual = (digitalRead(btnPins[i]) == LOW);

    if (estadoAtual && !btnPressionado[i]) {
      // Borda de descida: botão acabou de ser pressionado
      if (agora - lastBtnPress[i] >= DEBOUNCE_MS) {
        lastBtnPress[i] = agora;
        btnPressionado[i] = true;

        // Alterna estado e marca comando pendente (NÃO bloqueia)
        sonoffEstado[i] = !sonoffEstado[i];
        comandoPendente[i] = true;
        comandoValor[i] = sonoffEstado[i];

        atualizarDisplay();
        notificarWebSocket();

        Serial.printf("Botao %d pressionado -> %s %s\n",
                      i + 1, sonoffNome[i],
                      sonoffEstado[i] ? "LIGADO" : "DESLIGADO");
      }
    } else if (!estadoAtual && btnPressionado[i]) {
      // Botão foi solto → libera para próximo acionamento
      btnPressionado[i] = false;
    }
  }
}
