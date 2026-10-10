#include <Arduino.h>
#include <ArduinoJson.h>
#include <MD5Builder.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <WiFi.h>

#include <array>
#include <vector>

namespace {
constexpr char kServiceUuid[] = "812CA8ED-A177-4D74-AEFA-70098C5416AF";
constexpr char kUnlockUuid[] = "A1D67C01-FE3F-01BF-7D49-7DBDC43B02A6";
constexpr char kClockUuid[] = "337EA83E-744B-4B7C-B23F-45BEAAB11DB7";
constexpr char kPeriodUuid[] = "E5A7E9A0-E2D7-4F46-83D1-0E55D2427780";
constexpr char kCursorUuid[] = "FC3119DA-9B40-414E-A1C4-6C1737718CAE";
constexpr char kRangeUuid[] = "418D1817-F516-45F1-AB64-433AE596613D";
constexpr char kSamplesUuid[] = "A337B4EB-2544-4F94-87C9-F7732E5FE250";
constexpr char kTemperatureUuid[] = "B5634099-97C4-4F37-ADF3-771246E77754";
constexpr char kDevicePrefix[] = "2C:FC:E4";
constexpr size_t kMaxSensors = 8;
constexpr uint32_t kListenDurationMs = 8000;

struct Sensor { String address; String name; String code; };
struct Reading { float voltage = 0; float temperature = 0; bool pending = false; };

Preferences preferences;
WebServer web(80);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
std::vector<Sensor> sensors;
std::array<Reading, kMaxSensors> readings;
portMUX_TYPE readingMux = portMUX_INITIALIZER_UNLOCKED;
volatile int activeSensor = -1;
String wifiSsid, wifiPassword, mqttHost, mqttUsername, mqttPassword, availabilityTopic;
uint16_t mqttPort = 1883;
size_t nextSensor = 0;
uint32_t lastMqttAttempt = 0;

String escapeHtml(const String& value) {
  String output;
  for (char ch : value) {
    if (ch == '&') output += F("&amp;");
    else if (ch == '<') output += F("&lt;");
    else if (ch == '>') output += F("&gt;");
    else if (ch == '\"') output += F("&quot;");
    else output += ch;
  }
  return output;
}

String normalizeAddress(String address) { address.toUpperCase(); return address; }
bool isCtekAddress(const String& address) { return normalizeAddress(address).startsWith(kDevicePrefix); }

void loadConfiguration() {
  preferences.begin("ctek-gw", false);
  wifiSsid = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("wifi_pw", "");
  mqttHost = preferences.getString("mqtt_host", "");
  mqttPort = preferences.getUShort("mqtt_port", 1883);
  mqttUsername = preferences.getString("mqtt_user", "");
  mqttPassword = preferences.getString("mqtt_pw", "");
  JsonDocument stored;
  if (deserializeJson(stored, preferences.getString("sensors", "[]"))) return;
  for (JsonObjectConst item : stored.as<JsonArrayConst>()) {
    if (sensors.size() >= kMaxSensors) break;
    String address = normalizeAddress(item["address"] | "");
    String code = item["code"] | "";
    if (isCtekAddress(address) && code.length() == 16)
      sensors.push_back({address, item["name"] | address, code});
  }
}

void saveSensors() {
  JsonDocument stored;
  JsonArray array = stored.to<JsonArray>();
  for (const Sensor& sensor : sensors) {
    JsonObject item = array.add<JsonObject>();
    item["address"] = sensor.address;
    item["name"] = sensor.name;
    item["code"] = sensor.code;
  }
  String payload;
  serializeJson(stored, payload);
  preferences.putString("sensors", payload);
}

String gatewayId() {
  String id = WiFi.macAddress();
  id.replace(":", "");
  id.toLowerCase();
  return id;
}
String sensorId(size_t index) {
  String id = sensors[index].address;
  id.replace(":", "");
  id.toLowerCase();
  return id;
}
String stateTopic(size_t index) { return "ctek_gateway/" + sensorId(index) + "/state"; }
String discoveryTopic(size_t index, const char* kind) {
  return "homeassistant/sensor/ctek_" + sensorId(index) + "/" + kind + "/config";
}

void publishDiscovery(size_t index, const char* kind, const char* label,
                      const char* field, const char* unit, const char* deviceClass,
                      const char* icon) {
  JsonDocument config;
  String id = sensorId(index) + "_" + kind;
  config["name"] = sensors[index].name + " " + label;
  config["unique_id"] = "ctek_" + id;
  config["state_topic"] = stateTopic(index);
  config["value_template"] = String("{{ value_json.") + field + " }}";
  config["availability_topic"] = availabilityTopic;
  config["payload_available"] = "online";
  config["payload_not_available"] = "offline";
  config["unit_of_measurement"] = unit;
  config["device_class"] = deviceClass;
  config["state_class"] = "measurement";
  config["icon"] = icon;
  JsonObject device = config["device"].to<JsonObject>();
  device["identifiers"].to<JsonArray>().add("ctek_" + sensorId(index));
  device["name"] = sensors[index].name;
  device["manufacturer"] = "CTEK";
  device["model"] = "Battery Sense";
  String payload;
  serializeJson(config, payload);
  mqtt.publish(discoveryTopic(index, kind).c_str(), payload.c_str(), true);
}

void publishAllDiscovery() {
  for (size_t i = 0; i < sensors.size(); ++i) {
    publishDiscovery(i, "voltage", "voltage", "voltage", "V", "voltage", "mdi:car-battery");
    publishDiscovery(i, "temperature", "temperature", "temperature", "°C", "temperature", "mdi:thermometer");
  }
}

void publishPending() {
  if (!mqtt.connected()) return;
  for (size_t i = 0; i < sensors.size(); ++i) {
    Reading reading;
    portENTER_CRITICAL(&readingMux);
    reading = readings[i];
    readings[i].pending = false;
    portEXIT_CRITICAL(&readingMux);
    if (!reading.pending) continue;
    JsonDocument state;
    state["voltage"] = reading.voltage;
    state["temperature"] = reading.temperature;
    String payload;
    serializeJson(state, payload);
    mqtt.publish(stateTopic(i).c_str(), payload.c_str(), true);
  }
}

void connectMqtt() {
  if (mqttHost.isEmpty() || WiFi.status() != WL_CONNECTED || mqtt.connected() || millis() - lastMqttAttempt < 5000) return;
  lastMqttAttempt = millis();
  String clientId = "ctek-gateway-" + gatewayId();
  bool connected = mqttUsername.isEmpty()
      ? mqtt.connect(clientId.c_str(), availabilityTopic.c_str(), 0, true, "offline")
      : mqtt.connect(clientId.c_str(), mqttUsername.c_str(), mqttPassword.c_str(), availabilityTopic.c_str(), 0, true, "offline");
  if (connected) {
    mqtt.publish(availabilityTopic.c_str(), "online", true);
    publishAllDiscovery();
  }
}

String pageStart(const char* title) {
  return String(F("<!doctype html><html lang='nl'><meta name='viewport' content='width=device-width,initial-scale=1'><title>")) + title +
      F("</title><style>body{font:16px system-ui;max-width:760px;margin:2rem auto;padding:0 1rem;color:#192b2a;background:#f4f6f2}h1{color:#12665b}fieldset{border:1px solid #bccac3;margin:1rem 0;padding:1rem}label{display:block;margin:.7rem 0}input{box-sizing:border-box;width:100%;padding:.65rem;border:1px solid #8c9f98;border-radius:4px}button,a.button{display:inline-block;padding:.65rem 1rem;background:#12665b;color:white;border:0;border-radius:4px;text-decoration:none;margin:.4rem 0}small{color:#52615c}</style><body><h1>CTEK Battery Sense gateway</h1>");
}

void handleRoot() {
  String page = pageStart("CTEK gateway configuratie");
  page += F("<form method='post' action='/save'><fieldset><legend>Wi-Fi</legend><label>Netwerknaam<input name='ssid' required value='");
  page += escapeHtml(wifiSsid);
  page += F("'></label><label>Wi-Fi-wachtwoord<input name='wifi_password' type='password' placeholder='Leeg laten om ongewijzigd te laten'></label></fieldset><fieldset><legend>MQTT / Home Assistant</legend><label>Brokeradres<input name='mqtt_host' value='");
  page += escapeHtml(mqttHost);
  page += F("'></label><label>Poort<input name='mqtt_port' type='number' min='1' max='65535' value='");
  page += String(mqttPort);
  page += F("'></label><label>Gebruikersnaam<input name='mqtt_user' value='");
  page += escapeHtml(mqttUsername);
  page += F("'></label><label>Wachtwoord<input name='mqtt_password' type='password' placeholder='Leeg laten om ongewijzigd te laten'></label></fieldset><button type='submit'>Opslaan en herstarten</button></form><h2>Batterijmonitors</h2><p>");
  page += String(sensors.size());
  page += F(" van 8 toegevoegd</p><a class='button' href='/scan'>Scan naar CTEK-apparaten</a><ul>");
  for (size_t i = 0; i < sensors.size(); ++i) {
    page += F("<li>"); page += escapeHtml(sensors[i].name); page += F(" (");
    page += escapeHtml(sensors[i].address); page += F(") <a href='/remove?i=");
    page += String(i); page += F("'>verwijderen</a></li>");
  }
  page += F("</ul><small>Configuratie wordt lokaal op de gateway bewaard.</small></body></html>");
  web.send(200, "text/html; charset=utf-8", page);
}

void handleSave() {
  wifiSsid = web.arg("ssid");
  if (!web.arg("wifi_password").isEmpty()) wifiPassword = web.arg("wifi_password");
  mqttHost = web.arg("mqtt_host");
  long port = web.arg("mqtt_port").toInt();
  mqttPort = port > 0 && port <= 65535 ? static_cast<uint16_t>(port) : 1883;
  mqttUsername = web.arg("mqtt_user");
  if (!web.arg("mqtt_password").isEmpty()) mqttPassword = web.arg("mqtt_password");
  preferences.putString("ssid", wifiSsid);
  preferences.putString("wifi_pw", wifiPassword);
  preferences.putString("mqtt_host", mqttHost);
  preferences.putUShort("mqtt_port", mqttPort);
  preferences.putString("mqtt_user", mqttUsername);
  preferences.putString("mqtt_pw", mqttPassword);
  web.send(200, "text/html; charset=utf-8", pageStart("Opgeslagen") + F("<p>Instellingen opgeslagen. De gateway start opnieuw op.</p></body></html>"));
  delay(800);
  ESP.restart();
}

void handleAdd() {
  String address = normalizeAddress(web.arg("address"));
  String code = web.arg("code");
  String name = web.arg("name");
  if (!isCtekAddress(address) || code.length() != 16 || sensors.size() >= kMaxSensors) {
    web.send(400, "text/plain; charset=utf-8", "Ongeldig CTEK-adres, code moet 16 tekens zijn, of maximum van 8 bereikt.");
    return;
  }
  for (const Sensor& sensor : sensors) if (sensor.address == address) {
    web.send(409, "text/plain; charset=utf-8", "Dit apparaat is al toegevoegd.");
    return;
  }
  if (name.isEmpty()) name = "CTEK " + address.substring(address.length() - 5);
  sensors.push_back({address, name, code});
  saveSensors();
  web.sendHeader("Location", "/");
  web.send(303);
}

void handleRemove() {
  int index = web.arg("i").toInt();
  if (index >= 0 && static_cast<size_t>(index) < sensors.size()) {
    String id = sensorId(index);
    if (mqtt.connected()) {
      mqtt.publish(("homeassistant/sensor/ctek_" + id + "/voltage/config").c_str(), "", true);
      mqtt.publish(("homeassistant/sensor/ctek_" + id + "/temperature/config").c_str(), "", true);
    }
    sensors.erase(sensors.begin() + index);
    saveSensors();
  }
  web.sendHeader("Location", "/");
  web.send(303);
}

void handleScan() {
  NimBLEScan* scanner = NimBLEDevice::getScan();
  scanner->stop();
  scanner->clearResults();
  NimBLEScanResults found = scanner->getResults(5, false);
  String page = pageStart("CTEK-apparaten zoeken");
  page += F("<p>BLE-apparaten gevonden: ");
  page += String(found.getCount());
  page += F(". CTEK-herkenning gebruikt adresprefix 2C:FC:E4.</p><ul>");
  for (int i = 0; i < found.getCount(); ++i) {
    const NimBLEAdvertisedDevice* device = found.getDevice(i);
    String address = normalizeAddress(device->getAddress().toString().c_str());
    String name = device->getName().c_str();
    Serial.printf("[SCAN] %s %s RSSI=%d\n", address.c_str(), name.c_str(), device->getRSSI());
    if (!isCtekAddress(address)) {
      page += F("<li>");
      page += escapeHtml(name.isEmpty() ? String("(geen naam)") : name);
      page += F(" | ");
      page += escapeHtml(address);
      page += F(" | RSSI ");
      page += String(device->getRSSI());
      page += F(" | geen CTEK-adresprefix</li>");
      continue;
    }
    bool exists = false;
    for (const Sensor& sensor : sensors) exists |= sensor.address == address;
    if (exists || sensors.size() >= kMaxSensors) {
      page += F("<li>");
      page += escapeHtml(name.isEmpty() ? String("CTEK Battery Sense") : name);
      page += F(" | ");
      page += escapeHtml(address);
      page += exists ? F(" | al toegevoegd</li>") : F(" | maximum bereikt</li>");
      continue;
    }
    page += F("<li><form method='post' action='/add'><strong>");
    page += escapeHtml(name.isEmpty() ? String("CTEK Battery Sense") : name);
    page += F("</strong> "); page += escapeHtml(address);
    page += F("<input type='hidden' name='address' value='"); page += escapeHtml(address);
    page += F("'><label>Naam<input name='name' value='"); page += escapeHtml(name);
    page += F("'></label><label>Verbindingscode (16 tekens)<input name='code' required minlength='16' maxlength='16' autocomplete='off'></label><button type='submit'>Toevoegen</button></form></li>");
  }
  if (found.getCount() == 0) page += F("<li>Geen BLE-apparaten gevonden. De scan heeft geen advertenties ontvangen.</li>");
  page += F("</ul><a class='button' href='/'>Terug</a></body></html>");
  scanner->clearResults();
  web.send(200, "text/html; charset=utf-8", page);
}

void startWebServer() {
  web.on("/", HTTP_GET, handleRoot);
  web.on("/save", HTTP_POST, handleSave);
  web.on("/add", HTTP_POST, handleAdd);
  web.on("/remove", HTTP_GET, handleRemove);
  web.on("/scan", HTTP_GET, handleScan);
  web.onNotFound([]() { web.send(404, "text/plain", "Niet gevonden"); });
  web.begin();
}

void startProvisioning() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("CTEK-Gateway-Setup", "cteksetup");
  Serial.print("Configuratie-AP: CTEK-Gateway-Setup, open http://");
  Serial.println(WiFi.softAPIP());
}

void configureMqtt() {
  availabilityTopic = "ctek_gateway/" + gatewayId() + "/availability";
  mqtt.setServer(mqttHost.c_str(), mqttPort);
  mqtt.setBufferSize(1024);
}

bool unlockSensor(NimBLERemoteCharacteristic* unlock, const String& code) {
  static uint8_t salt[] = {225, 218, 113, 149, 236, 237, 64, 228, 188, 22, 85, 158, 157, 231, 59, 164};
  uint8_t input[32];
  memcpy(input, salt, sizeof(salt));
  memcpy(input + sizeof(salt), code.c_str(), code.length());
  MD5Builder md5;
  md5.begin();
  md5.add(input, sizeof(salt) + code.length());
  md5.calculate();
  uint8_t digest[16];
  md5.getBytes(digest);
  return unlock->writeValue(digest, sizeof(digest), true);
}

void onBatterySamples(NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
  int index = activeSensor;
  if (index < 0 || static_cast<size_t>(index) >= kMaxSensors || length < 3) return;
  portENTER_CRITICAL(&readingMux);
  for (size_t offset = 0; offset + 2 < length; offset += 3) {
    readings[index].voltage = (static_cast<uint16_t>(data[offset + 1]) * 256 + data[offset]) / 2048.0f;
    readings[index].temperature = static_cast<int8_t>(data[offset + 2]) / 2.0f;
    readings[index].pending = true;
  }
  portEXIT_CRITICAL(&readingMux);
}

void readOneSensor(size_t index) {
  if (WiFi.status() == WL_CONNECTED) mqtt.loop();
  NimBLEClient* client = NimBLEDevice::createClient();
  if (!client) return;
  if (!client->connect(NimBLEAddress(std::string(sensors[index].address.c_str()), BLE_ADDR_PUBLIC))) {
    NimBLEDevice::deleteClient(client);
    return;
  }
  NimBLERemoteService* service = client->getService(NimBLEUUID(kServiceUuid));
  if (!service) {
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    return;
  }
  NimBLERemoteCharacteristic* unlock = service->getCharacteristic(NimBLEUUID(kUnlockUuid));
  NimBLERemoteCharacteristic* samples = service->getCharacteristic(NimBLEUUID(kSamplesUuid));
  if (!unlock || !samples || !unlockSensor(unlock, sensors[index].code)) {
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    return;
  }

  uint32_t unlockWaitStarted = millis();
  while (millis() - unlockWaitStarted < 2000) {
    web.handleClient();
    if (WiFi.status() == WL_CONNECTED) mqtt.loop();
    delay(20);
  }
  if (auto* clock = service->getCharacteristic(NimBLEUUID(kClockUuid))) clock->readValue();
  if (auto* period = service->getCharacteristic(NimBLEUUID(kPeriodUuid))) period->readValue();
  uint16_t historyCursor = 0;
  if (auto* cursor = service->getCharacteristic(NimBLEUUID(kCursorUuid))) {
    NimBLEAttValue value = cursor->readValue();
    if (value.size() >= 2)
      historyCursor = static_cast<uint16_t>(value[0]) | (static_cast<uint16_t>(value[1]) << 8);
  }
  if (auto* temperature = service->getCharacteristic(NimBLEUUID(kTemperatureUuid))) {
    NimBLEAttValue value = temperature->readValue();
    if (value.size()) {
      portENTER_CRITICAL(&readingMux);
      readings[index].temperature = static_cast<int8_t>(value[0]) / 2.0f;
      portEXIT_CRITICAL(&readingMux);
    }
  }
  activeSensor = static_cast<int>(index);
  if (samples->canNotify() && samples->subscribe(true, onBatterySamples, true) && historyCursor > 0) {
    if (auto* range = service->getCharacteristic(NimBLEUUID(kRangeUuid))) {
      const uint16_t firstSample = historyCursor - 1;
      const uint8_t request[] = {3, static_cast<uint8_t>(firstSample),
                                 static_cast<uint8_t>(firstSample >> 8), 1, 0};
      range->writeValue(request, sizeof(request), true);
    }
  }
  uint32_t started = millis();
  while (millis() - started < kListenDurationMs && client->isConnected()) {
    web.handleClient();
    if (WiFi.status() == WL_CONNECTED) mqtt.loop();
    publishPending();
    delay(20);
  }
  activeSensor = -1;
  if (client->isConnected()) client->disconnect();
  NimBLEDevice::deleteClient(client);
}
}  // namespace

void setup() {
  Serial.begin(115200);
  loadConfiguration();
  NimBLEDevice::init("CTEK-HA-Gateway");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEScan* scanner = NimBLEDevice::getScan();
  scanner->setActiveScan(true);
  scanner->setInterval(45);
  scanner->setWindow(15);

  if (!wifiSsid.isEmpty()) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
    uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < 15000) delay(250);
  }
  if (WiFi.status() != WL_CONNECTED) startProvisioning();
  else Serial.printf("Wi-Fi verbonden: %s\n", WiFi.localIP().toString().c_str());
  configureMqtt();
  startWebServer();
}

void loop() {
  web.handleClient();
  if (WiFi.status() == WL_CONNECTED) {
    connectMqtt();
    mqtt.loop();
    publishPending();
  }
  if (sensors.empty()) { delay(20); return; }
  if (nextSensor >= sensors.size()) nextSensor = 0;
  readOneSensor(nextSensor++);
}
