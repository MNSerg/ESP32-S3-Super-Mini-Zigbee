/*
  ESP32-S3 Super Mini firmware for Yandex Smart Home (MQTT).
  - Controls a load (relay or MOSFET).
  - Sends state/telemetry to the cloud.
  - Receives on/off commands.

  IMPORTANT: Fill in WiFi and MQTT settings below.
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

// ---------------------------
// WiFi settings
// ---------------------------
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// ---------------------------
// Device and GPIO settings
// ---------------------------
#define DEVICE_ID            "esp32-s3-super-mini"
#define RELAY_PIN            4   // Change to the pin you use for the load
#define RELAY_ACTIVE_HIGH    1   // 1: HIGH turns relay ON, 0: LOW turns relay ON

// Optional local button (toggle) - disabled by default
#define USE_BUTTON           0
#define BUTTON_PIN           0
#define BUTTON_ACTIVE_LOW    1

// Optional status LED - disabled by default
#define USE_STATUS_LED       0
#define STATUS_LED_PIN       2
#define STATUS_LED_ACTIVE_HIGH 1

// ---------------------------
// MQTT / Yandex IoT Core settings
// ---------------------------
// For Yandex Cloud IoT Core use:
//   Host: mqtt.cloud.yandex.net
//   Port: 8883 (TLS)
//   Auth: device/registry credentials or certificates
#define MQTT_HOST            "mqtt.cloud.yandex.net"
#define MQTT_PORT            8883
#define MQTT_USERNAME        "YOUR_MQTT_USERNAME"
#define MQTT_PASSWORD        "YOUR_MQTT_PASSWORD"

// Topics (adjust if your backend expects different paths)
// Common Yandex IoT Core topics:
//   $devices/<device_id>/events  - telemetry/events
//   $devices/<device_id>/state   - state (retained)
//   $devices/<device_id>/commands - commands from cloud
#define MQTT_TOPIC_EVENTS    "$devices/" DEVICE_ID "/events"
#define MQTT_TOPIC_STATE     "$devices/" DEVICE_ID "/state"
#define MQTT_TOPIC_COMMANDS  "$devices/" DEVICE_ID "/commands"
#define MQTT_TOPIC_AVAIL     "$devices/" DEVICE_ID "/availability"

// Publish settings
#define PUBLISH_TO_STATE_TOPIC  1
#define PUBLISH_TO_EVENTS_TOPIC 1
#define TELEMETRY_INTERVAL_MS   30000UL

// TLS settings
#define USE_TLS               1
#define ALLOW_INSECURE_TLS    1  // Set to 0 and provide ROOT_CA for production
const char *ROOT_CA =
  ""; // Paste Yandex Cloud root CA certificate here if ALLOW_INSECURE_TLS=0

// ---------------------------
// Globals
// ---------------------------
WiFiClient wifiClient;
WiFiClientSecure wifiClientSecure;
PubSubClient mqttClient;

bool relayOn = false;
unsigned long lastTelemetryAt = 0;
unsigned long lastMqttReconnectAttempt = 0;
unsigned long lastWifiCheckAt = 0;

#if USE_BUTTON
unsigned long lastButtonChangeAt = 0;
bool lastButtonState = false;
#endif

// ---------------------------
// Helpers
// ---------------------------
void setStatusLed(bool on) {
#if USE_STATUS_LED
  digitalWrite(STATUS_LED_PIN,
               (STATUS_LED_ACTIVE_HIGH ? (on ? HIGH : LOW) : (on ? LOW : HIGH)));
#else
  (void)on;
#endif
}

void setRelay(bool on) {
  relayOn = on;
  digitalWrite(RELAY_PIN,
               (RELAY_ACTIVE_HIGH ? (on ? HIGH : LOW) : (on ? LOW : HIGH)));
}

bool parseBoolToken(const String &token, bool *value) {
  if (token == "1" || token == "true" || token == "on") {
    *value = true;
    return true;
  }
  if (token == "0" || token == "false" || token == "off") {
    *value = false;
    return true;
  }
  return false;
}

bool extractBoolFromJson(const String &json, const char *key, bool *value) {
  String keyPattern = String("\"") + key + "\"";
  int keyPos = json.indexOf(keyPattern);
  if (keyPos < 0) {
    return false;
  }
  int colonPos = json.indexOf(':', keyPos + keyPattern.length());
  if (colonPos < 0) {
    return false;
  }
  int i = colonPos + 1;
  while (i < (int)json.length() && isspace(json[i])) {
    i++;
  }
  if (i >= (int)json.length()) {
    return false;
  }
  if (json.startsWith("true", i)) {
    *value = true;
    return true;
  }
  if (json.startsWith("false", i)) {
    *value = false;
    return true;
  }
  if (json.startsWith("\"on\"", i)) {
    *value = true;
    return true;
  }
  if (json.startsWith("\"off\"", i)) {
    *value = false;
    return true;
  }
  if (json.charAt(i) == '1') {
    *value = true;
    return true;
  }
  if (json.charAt(i) == '0') {
    *value = false;
    return true;
  }
  return false;
}

void publishState(const char *reason, bool retained) {
  char payload[256];
  int rssi = WiFi.isConnected() ? WiFi.RSSI() : 0;
  snprintf(payload, sizeof(payload),
           "{\"device_id\":\"%s\",\"state\":\"%s\",\"relay\":%s,"
           "\"rssi\":%d,\"uptime_ms\":%lu,\"reason\":\"%s\"}",
           DEVICE_ID,
           relayOn ? "ON" : "OFF",
           relayOn ? "true" : "false",
           rssi,
           (unsigned long)millis(),
           reason ? reason : "unknown");

#if PUBLISH_TO_STATE_TOPIC
  mqttClient.publish(MQTT_TOPIC_STATE, payload, retained);
#endif
#if PUBLISH_TO_EVENTS_TOPIC
  mqttClient.publish(MQTT_TOPIC_EVENTS, payload, false);
#endif
}

void publishAvailability(const char *state) {
  if (!state) {
    return;
  }
  mqttClient.publish(MQTT_TOPIC_AVAIL, state, true);
}

void handleCommandMessage(const String &msg) {
  String trimmed = msg;
  trimmed.trim();
  trimmed.toLowerCase();

  bool desired = relayOn;
  bool hasDesired = false;

  if (trimmed == "toggle") {
    desired = !relayOn;
    hasDesired = true;
  } else if (parseBoolToken(trimmed, &desired)) {
    hasDesired = true;
  } else {
    if (extractBoolFromJson(trimmed, "state", &desired)) {
      hasDesired = true;
    } else if (extractBoolFromJson(trimmed, "power", &desired)) {
      hasDesired = true;
    } else if (extractBoolFromJson(trimmed, "on", &desired)) {
      hasDesired = true;
    } else if (extractBoolFromJson(trimmed, "value", &desired)) {
      hasDesired = true;
    }
  }

  if (hasDesired) {
    setRelay(desired);
    publishState("command", true);
  }
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  String msg;
  msg.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }
  handleCommandMessage(msg);
}

void connectWiFi() {
  if (WiFi.isConnected()) {
    return;
  }
  setStatusLed(true);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DEVICE_ID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (!WiFi.isConnected() && (millis() - start) < 20000UL) {
    delay(250);
  }
  setStatusLed(false);
}

bool connectMqtt() {
  if (mqttClient.connected()) {
    return true;
  }

  bool connected = mqttClient.connect(
    DEVICE_ID,
    MQTT_USERNAME,
    MQTT_PASSWORD,
    MQTT_TOPIC_AVAIL,
    1,
    true,
    "offline");

  if (!connected) {
    return false;
  }

  mqttClient.subscribe(MQTT_TOPIC_COMMANDS);
  publishAvailability("online");
  publishState("boot", true);
  return true;
}

void configureMqttClient() {
  if (USE_TLS) {
    if (strlen(ROOT_CA) > 0) {
      wifiClientSecure.setCACert(ROOT_CA);
    } else if (ALLOW_INSECURE_TLS) {
      wifiClientSecure.setInsecure();
    } else {
      Serial.println("TLS enabled but no CA cert set. Halting.");
      while (true) {
        delay(1000);
      }
    }
    mqttClient.setClient(wifiClientSecure);
  } else {
    mqttClient.setClient(wifiClient);
  }
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512);
}

#if USE_BUTTON
void handleButton() {
  bool rawState = digitalRead(BUTTON_PIN) ==
                  (BUTTON_ACTIVE_LOW ? LOW : HIGH);
  if (rawState != lastButtonState) {
    lastButtonChangeAt = millis();
    lastButtonState = rawState;
  }
  if (rawState && (millis() - lastButtonChangeAt) > 30) {
    setRelay(!relayOn);
    publishState("button", true);
    while (digitalRead(BUTTON_PIN) ==
           (BUTTON_ACTIVE_LOW ? LOW : HIGH)) {
      delay(10);
    }
  }
}
#endif

void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(RELAY_PIN, OUTPUT);
  setRelay(false);

#if USE_STATUS_LED
  pinMode(STATUS_LED_PIN, OUTPUT);
  setStatusLed(false);
#endif

#if USE_BUTTON
  pinMode(BUTTON_PIN, BUTTON_ACTIVE_LOW ? INPUT_PULLUP : INPUT_PULLDOWN);
#endif

  connectWiFi();
  configureMqttClient();
  connectMqtt();
}

void loop() {
  if (!WiFi.isConnected()) {
    unsigned long now = millis();
    if (now - lastWifiCheckAt > 5000UL) {
      lastWifiCheckAt = now;
      connectWiFi();
    }
    return;
  }

  if (!mqttClient.connected()) {
    unsigned long now = millis();
    if (now - lastMqttReconnectAttempt > 5000UL) {
      lastMqttReconnectAttempt = now;
      connectMqtt();
    }
  } else {
    mqttClient.loop();
  }

#if USE_BUTTON
  handleButton();
#endif

  if (millis() - lastTelemetryAt > TELEMETRY_INTERVAL_MS) {
    lastTelemetryAt = millis();
    publishState("periodic", false);
  }
}
