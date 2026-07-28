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

// --- 5 Sonoffs (ordem física: Muro, Garagem, Container, Quarto, Cuscas) ---
bool sonoffEstado[SONOFF_COUNT] = {false, false, false, false, false};

const char* sonoffNome[SONOFF_COUNT] = {
  SONOFF_NAME_3, SONOFF_NAME_2, SONOFF_NAME_5, SONOFF_NAME_4, SONOFF_NAME_1
};
// Muro         Garagem      Container     Quarto        Cuscas

const char* sonoffEntity[SONOFF_COUNT] = {
  HA_ENTITY_3, HA_ENTITY_2, HA_ENTITY_5, HA_ENTITY_4, HA_ENTITY_1
};

// --- Botões físicos ---
const int btnPins[SONOFF_COUNT] = {BTN_PIN_3, BTN_PIN_2, BTN_PIN_5, BTN_PIN_4, BTN_PIN_1};
//                                 GPIO25    GPIO33    GPIO27    GPIO26    GPIO32
unsigned long lastBtnPress[SONOFF_COUNT] = {0, 0, 0, 0, 0};
bool btnPressionado[SONOFF_COUNT] = {false, false, false, false, false};

// Flags de interrupção (voláteis)
volatile bool btnIrq[SONOFF_COUNT] = {false, false, false, false, false};

// ISRs mapeadas para a posição física correta
// GPIO32=Cuscas(1), GPIO33=Garagem(4), GPIO25=Muro(0), GPIO26=Quarto(3), GPIO27=Container(2)
void IRAM_ATTR isrBtn1() { btnIrq[1] = true; }  // GPIO32 → Cuscas (pos 1)
void IRAM_ATTR isrBtn2() { btnIrq[4] = true; }  // GPIO33 → Garagem (pos 4)
void IRAM_ATTR isrBtn3() { btnIrq[0] = true; }  // GPIO25 → Muro (pos 0)
void IRAM_ATTR isrBtn4() { btnIrq[3] = true; }  // GPIO26 → Quarto (pos 3)
void IRAM_ATTR isrBtn5() { btnIrq[2] = true; }  // GPIO27 → Container (pos 2)

// Comandos pendentes
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

  // --- Botões físicos (interrupção na borda de descida) ---
  pinMode(btnPins[0], INPUT_PULLUP);
  pinMode(btnPins[1], INPUT_PULLUP);
  pinMode(btnPins[2], INPUT_PULLUP);
  pinMode(btnPins[3], INPUT_PULLUP);
  pinMode(btnPins[4], INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(btnPins[0]), isrBtn3, FALLING); // GPIO25→Muro
  attachInterrupt(digitalPinToInterrupt(btnPins[1]), isrBtn2, FALLING); // GPIO33→Garagem
  attachInterrupt(digitalPinToInterrupt(btnPins[2]), isrBtn5, FALLING); // GPIO27→Container
  attachInterrupt(digitalPinToInterrupt(btnPins[3]), isrBtn4, FALLING); // GPIO26→Quarto
  attachInterrupt(digitalPinToInterrupt(btnPins[4]), isrBtn1, FALLING); // GPIO32→Cuscas

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

  // Nomes para display (versões curtas)
  const char* nomesDisplay[SONOFF_COUNT] = {
    "Muro", "Garagem", "Cont", "Qrto", "Cusca"
  };

  // --- 5 Sonoffs: linha 1 com 2, linha 2 com 3 ---
  display.setTextSize(1);
  for (int i = 0; i < SONOFF_COUNT; i++) {
    int xCirculo, xNome, row;

    if (i < 2) {
      row = 0;
      xCirculo = i * 64 + 4;
      xNome    = i * 64 + 14;
    } else {
      row = 1;
      int col = i - 2;
      xCirculo = col * 42 + 2;
      xNome    = col * 42 + 12;
    }

    int yCirculo = 37 + row * 13;
    int yTexto = yCirculo - 3;

    // Círculo: preenchido = LIGADO, vazio = DESLIGADO
    if (sonoffEstado[i]) {
      display.fillCircle(xCirculo, yCirculo, 4, SSD1306_WHITE);
    } else {
      display.drawCircle(xCirculo, yCirculo, 4, SSD1306_WHITE);
    }

    display.setCursor(xNome, yTexto);
    display.print(nomesDisplay[i]);
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
// Leitura dos botões físicos (via interrupção + debounce)
// A ISR captura o pressionamento — NÃO precisa confirmar com digitalRead
// ============================================================
void verificarBotoes() {
  unsigned long agora = millis();

  for (int i = 0; i < SONOFF_COUNT; i++) {
    if (btnIrq[i]) {
      btnIrq[i] = false;

      // Debounce: ignora disparos com menos de DEBOUNCE_MS entre si
      if (agora - lastBtnPress[i] >= DEBOUNCE_MS) {
        lastBtnPress[i] = agora;

        sonoffEstado[i] = !sonoffEstado[i];
        comandoPendente[i] = true;
        comandoValor[i] = sonoffEstado[i];

        atualizarDisplay();
        notificarWebSocket();

        Serial.printf("Botao %d -> %s %s\n",
                      i + 1, sonoffNome[i],
                      sonoffEstado[i] ? "LIGADO" : "DESLIGADO");
      }
    }
  }
}
