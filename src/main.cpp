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
bool sonoffLigado = false;
bool sonoffConectado = false;   // true = tomada respondeu OK

unsigned long lastSensor = 0;
unsigned long lastMqtt = 0;

// ============================================================
// Forward declarations
// ============================================================
void conectarWiFi();
void conectarMQTT();
void callbackMQTT(char* topic, byte* payload, unsigned int length);
void lerSensor();
void atualizarDisplay();
void notificarWebSocket();
void comandarSonoff(bool ligar);

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

  // --- LittleFS (página web) ---
  if (!LittleFS.begin()) {
    Serial.println(F("Erro ao montar LittleFS"));
  }

  // --- WiFi ---
  conectarWiFi();

  // --- MQTT ---
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(callbackMQTT);

  // --- LED indicador ---
  pinMode(LED_SONOFF, OUTPUT);
  digitalWrite(LED_SONOFF, LOW);

  // ==========================================================
  // Rotas do servidor web
  // ==========================================================

  // Página principal
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(LittleFS, "/index.html", "text/html");
  });

  // API: leitura atual (JSON)
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* request) {
    DynamicJsonDocument doc(256);
    doc["temperatura"] = temperatura;
    doc["umidade"] = umidade;
    doc["sonoff"] = sonoffLigado ? "ligado" : "desligado";
    doc["sonoff_ok"] = sonoffConectado;

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // API: ligar/desligar tomada Sonoff
  server.on("/api/sonoff", HTTP_POST, [](AsyncWebServerRequest* request) {
    String estado;
    if (request->hasParam("estado", true)) {
      estado = request->getParam("estado", true)->value();
    } else if (request->hasParam("estado")) {
      estado = request->getParam("estado")->value();
    }

    bool ligar = (estado == "1" || estado == "ligado" || estado == "on");
    comandarSonoff(ligar);

    request->send(200, "application/json",
                  ligar ? "{\"status\":\"ligado\"}" : "{\"status\":\"desligado\"}");
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
      // envia estado atual ao conectar
      notificarWebSocket();
    }
  });
  server.addHandler(&ws);

  // Inicia servidor
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
    notificarWebSocket();
  }

  // Publicação MQTT
  if (agora - lastMqtt >= INTERVAL_MQTT) {
    lastMqtt = agora;
    if (mqtt.connected()) {
      char buf[8];
      dtostrf(temperatura, 4, 1, buf);
      mqtt.publish(MQTT_TOPIC_TEMP, buf);
      dtostrf(umidade, 4, 1, buf);
      mqtt.publish(MQTT_TOPIC_HUMID, buf);
      mqtt.publish(MQTT_TOPIC_SONOFF_ST, sonoffLigado ? "ON" : "OFF");
    }
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
    // Inscreve no tópico de comando do Sonoff
    mqtt.subscribe(MQTT_TOPIC_SONOFF);
  } else {
    Serial.printf("Falha (rc=%d)\n", mqtt.state());
  }
}

void callbackMQTT(char* topic, byte* payload, unsigned int length) {
  // Copia payload para string
  char msg[32];
  unsigned int len = length < 31 ? length : 31;
  memcpy(msg, payload, len);
  msg[len] = '\0';

  Serial.printf("MQTT recebido [%s]: %s\n", topic, msg);

  if (strcmp(topic, MQTT_TOPIC_SONOFF) == 0) {
    bool ligar = (strcmp(msg, "ON") == 0 || strcmp(msg, "1") == 0 ||
                   strcmp(msg, "ligado") == 0);
    comandarSonoff(ligar);
  }
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
// Display OLED
// ============================================================
void atualizarDisplay() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Linha 1: IP
  display.setCursor(0, 0);
  display.print(WiFi.localIP());

  // Linha 2: Temperatura (texto maior)
  display.setTextSize(2);
  display.setCursor(0, 14);
  display.printf("%.1fC", temperatura);

  // Linha 3: Umidade
  display.setTextSize(1);
  display.setCursor(0, 40);
  display.printf("Umid: %.1f%%", umidade);

  // Linha 4: Tomada Sonoff
  display.setCursor(0, 52);
  display.print(sonoffLigado ? "Tomada: LIGADA" : "Tomada: DESLIG");

  display.display();
}

// ============================================================
// WebSocket: notifica todos os clientes
// ============================================================
void notificarWebSocket() {
  DynamicJsonDocument doc(256);
  doc["temperatura"] = temperatura;
  doc["umidade"] = umidade;
  doc["sonoff"] = sonoffLigado ? "ligado" : "desligado";

  String json;
  serializeJson(doc, json);
  ws.textAll(json);
}

// ============================================================
// Comando para a tomada Sonoff (via MQTT)
// ============================================================
void comandarSonoff(bool ligar) {
  sonoffLigado = ligar;
  digitalWrite(LED_SONOFF, ligar ? HIGH : LOW);

  // Publica comando no tópico do Sonoff
  // No Home Assistant, crie uma automação que escute este tópico
  // e acione o switch correspondente
  mqtt.publish(MQTT_TOPIC_SONOFF_ST, ligar ? "ON" : "OFF");

  Serial.printf("Sonoff -> %s\n", ligar ? "LIGADO" : "DESLIGADO");
  atualizarDisplay();
  notificarWebSocket();
}
